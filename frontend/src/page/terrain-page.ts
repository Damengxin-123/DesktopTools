/**
 * 主窗口地形页：左侧项目列表、右侧三维缩略预览（只读）与项目信息。
 * 支持新建、重命名、删除与打开；双击列表项或预览模型打开独立编辑窗口。
 */
import * as THREE from "three";
/** 后端项目摘要。 */
export interface TerrainItem {
    id: string;
    title: string;
    updatedAt: string;
    gridWidth: number;
    gridDepth: number;
    spacing: number;
    sizeWidth: number;
    sizeDepth: number;
    undoSteps: number;
}
interface PageOptions {
    toast: (message: string, type?: string) => void;
    reportError: (error: unknown) => void;
}
/** 主窗口统一桥（bridge.js）：整个页面共享一个 QWebChannel，不能再新建第二个。 */
interface SharedBridge {
    connect: () => Promise<unknown>;
    call: <T>(method: string, ...args: unknown[]) => Promise<T>;
    on: (signalName: string, callback: (...args: unknown[]) => void) => Promise<void>;
}
/** 三维缩略预览：只允许观察、旋转、缩放，不修改地形。 */
class TerrainPreview {
    private renderer: THREE.WebGLRenderer | null = null;
    private scene: THREE.Scene | null = null;
    private camera: THREE.PerspectiveCamera | null = null;
    private mesh: THREE.Mesh | null = null;
    private yaw = Math.PI * 0.3;
    private pitch = 0.62;
    private distance = 100;
    private target = new THREE.Vector3();
    private drag: {
        x: number;
        y: number;
    } | null = null;
    private canvas: HTMLCanvasElement | null = null;
    private framePending = false;
    failed = false;
    mount(canvas: HTMLCanvasElement): void {
        this.canvas = canvas;
        try {
            this.renderer = new THREE.WebGLRenderer({ canvas, antialias: true });
            if (!this.renderer.capabilities.isWebGL2)
                throw new Error("WebGL2");
        }
        catch {
            this.failed = true;
            this.renderer = null;
            return;
        }
        this.scene = new THREE.Scene();
        this.scene.background = new THREE.Color(0x9db8d2);
        this.scene.add(new THREE.HemisphereLight(0xdfeaf5, 0x8a7a5a, 0.9));
        const sun = new THREE.DirectionalLight(0xfff2dd, 1.3);
        sun.position.set(-300, 420, 240);
        this.scene.add(sun);
        this.camera = new THREE.PerspectiveCamera(50, 1, 0.5, 40000);
        canvas.addEventListener("pointerdown", (event) => {
            this.drag = { x: event.clientX, y: event.clientY };
            try {
                canvas.setPointerCapture(event.pointerId);
            }
            catch { /* 无活动指针时忽略 */ }
        });
        canvas.addEventListener("pointermove", (event) => {
            if (!this.drag)
                return;
            // 与主流三维查看器一致的“抓住模型拖动”方向。
            this.yaw += (event.clientX - this.drag.x) * 0.006;
            this.pitch = THREE.MathUtils.clamp(this.pitch - (event.clientY - this.drag.y) * 0.006, 0.08, 1.5);
            this.drag = { x: event.clientX, y: event.clientY };
        });
        canvas.addEventListener("pointerup", () => { this.drag = null; });
        canvas.addEventListener("wheel", (event) => {
            event.preventDefault();
            this.distance = THREE.MathUtils.clamp(this.distance * (event.deltaY < 0 ? 0.88 : 1 / 0.88), 4, 60000);
        }, { passive: false });
        this.startLoop();
    }
    /** 用降采样的预览高度构建只读网格。 */
    show(heights: Float32Array, gridWidth: number, gridDepth: number, spacing: number): void {
        if (!this.scene || this.failed)
            return;
        if (this.mesh) {
            this.scene.remove(this.mesh);
            this.mesh.geometry.dispose();
            this.mesh = null;
        }
        const field = { gridWidth, gridDepth, spacing, heights, sizeWidth: (gridWidth - 1) * spacing,
            sizeDepth: (gridDepth - 1) * spacing };
        const positions = new Float32Array(gridWidth * gridDepth * 3);
        let min = Number.POSITIVE_INFINITY;
        let max = Number.NEGATIVE_INFINITY;
        for (let index = 0; index < heights.length; ++index) {
            if (heights[index] < min)
                min = heights[index];
            if (heights[index] > max)
                max = heights[index];
        }
        if (!Number.isFinite(min)) {
            min = 0;
            max = 1;
        }
        for (let z = 0; z < gridDepth; ++z) {
            for (let x = 0; x < gridWidth; ++x) {
                const index = z * gridWidth + x;
                positions[index * 3] = x * spacing;
                positions[index * 3 + 1] = heights[index];
                positions[index * 3 + 2] = z * spacing;
            }
        }
        const normals = new Float32Array(gridWidth * gridDepth * 3);
        for (let z = 0; z < gridDepth; ++z) {
            for (let x = 0; x < gridWidth; ++x) {
                const index = z * gridWidth + x;
                const left = heights[z * gridWidth + Math.max(0, x - 1)];
                const right = heights[z * gridWidth + Math.min(gridWidth - 1, x + 1)];
                const up = heights[Math.max(0, z - 1) * gridWidth + x];
                const down = heights[Math.min(gridDepth - 1, z + 1) * gridWidth + x];
                const normal = new THREE.Vector3(left - right, 2 * spacing, up - down).normalize();
                normals[index * 3] = normal.x;
                normals[index * 3 + 1] = normal.y;
                normals[index * 3 + 2] = normal.z;
            }
        }
        const quads = (gridWidth - 1) * (gridDepth - 1);
        const indices = new Uint32Array(quads * 6);
        let cursor = 0;
        for (let z = 0; z < gridDepth - 1; ++z) {
            for (let x = 0; x < gridWidth - 1; ++x) {
                const a = z * gridWidth + x;
                const b = a + 1;
                const c = a + gridWidth;
                const d = c + 1;
                indices[cursor++] = a;
                indices[cursor++] = c;
                indices[cursor++] = b;
                indices[cursor++] = b;
                indices[cursor++] = c;
                indices[cursor++] = d;
            }
        }
        const geometry = new THREE.BufferGeometry();
        geometry.setAttribute("position", new THREE.BufferAttribute(positions, 3));
        geometry.setAttribute("normal", new THREE.BufferAttribute(normals, 3));
        // 按高度着色帮助观察形状；与编辑器保持一致的配色。
        const colors = new Float32Array(gridWidth * gridDepth * 3);
        const tint = new THREE.Color();
        for (let index = 0; index < heights.length; ++index) {
            const t = max > min ? (heights[index] - min) / (max - min) : 0.5;
            if (t < 0.5)
                tint.setRGB(0.32 + t * 0.5, 0.45 + t * 0.25, 0.2 + t * 0.12);
            else {
                const u = (t - 0.5) * 2;
                tint.setRGB(0.57 + u * 0.33, 0.57 - u * 0.2, 0.32 - u * 0.14);
            }
            colors[index * 3] = tint.r;
            colors[index * 3 + 1] = tint.g;
            colors[index * 3 + 2] = tint.b;
        }
        geometry.setAttribute("color", new THREE.BufferAttribute(colors, 3));
        geometry.setIndex(new THREE.BufferAttribute(indices, 1));
        this.mesh = new THREE.Mesh(geometry, new THREE.MeshStandardMaterial({ vertexColors: true, roughness: 0.95, metalness: 0 }));
        this.scene.add(this.mesh);
        void field;
        const size = Math.max((gridWidth - 1) * spacing, (gridDepth - 1) * spacing);
        this.target.set((gridWidth - 1) * spacing / 2, (min + max) / 2, (gridDepth - 1) * spacing / 2);
        this.distance = size * 1.4 + spacing * 10;
    }
    clear(): void {
        if (this.scene && this.mesh) {
            this.scene.remove(this.mesh);
            this.mesh.geometry.dispose();
            this.mesh = null;
        }
    }
    private resize(): void {
        if (!this.renderer || !this.camera || !this.canvas)
            return;
        const wrap = this.canvas.parentElement;
        if (!wrap)
            return;
        const width = wrap.clientWidth;
        const height = wrap.clientHeight;
        if (width <= 0 || height <= 0)
            return;
        this.renderer.setSize(width, height, false);
        this.camera.aspect = width / height;
        this.camera.updateProjectionMatrix();
    }
    // 与编辑器一致：定时器驱动的按需渲染，不依赖 requestAnimationFrame
    // （部分启动环境下 rAF 不被调度，画面会冻结在首帧）。
    private loopTimer = 0;
    private startLoop(): void {
        if (this.loopTimer)
            return;
        this.loopTimer = window.setInterval(() => this.tick(), 33);
    }
    private tick = (): void => {
        if (!this.renderer || !this.scene || !this.camera || !this.mesh)
            return;
        this.resize();
        if (!this.drag)
            this.yaw += 0.005; // 缓慢自转便于观察。
        const sinPitch = Math.sin(this.pitch);
        this.camera.position.set(this.target.x + Math.cos(this.yaw) * sinPitch * this.distance, this.target.y + Math.cos(this.pitch) * this.distance, this.target.z + Math.sin(this.yaw) * sinPitch * this.distance);
        this.camera.lookAt(this.target);
        this.renderer.render(this.scene, this.camera);
    };
}
/** 页面注册的全局接口（与 app.js 的加载器约定一致）。 */
declare global {
    interface Window {
        desktopTerrain: {
            initialize: (options: PageOptions) => Promise<void>;
            canLeave: () => boolean;
        };
        desktopBridge: SharedBridge;
    }
}
/** 地形页面控制器。 */
class TerrainPage {
    private options: PageOptions = { toast: () => undefined, reportError: () => undefined };
    private get bridge(): SharedBridge {
        return window.desktopBridge;
    }
    private projects: TerrainItem[] = [];
    private selected: TerrainItem | null = null;
    private revision = 0;
    private preview = new TerrainPreview();
    private initialized = false;
    get canLeave(): boolean {
        // 主页面为只读预览，无未保存状态。
        return true;
    }
    async initialize(options: PageOptions): Promise<void> {
        this.options = options;
        if (!this.initialized) {
            this.initialized = true;
            const canvas = document.getElementById("terrain-preview-canvas") as HTMLCanvasElement | null;
            if (canvas)
                this.preview.mount(canvas);
            document.getElementById("add-terrain")?.addEventListener("click", () => this.createDialog());
            document.getElementById("terrain-rename")?.addEventListener("click", () => {
                if (this.selected)
                    this.renameDialog(this.selected);
            });
            document.getElementById("terrain-delete")?.addEventListener("click", () => {
                if (this.selected)
                    this.deleteDialog(this.selected);
            });
            document.getElementById("terrain-open")?.addEventListener("click", () => {
                if (this.selected)
                    void this.openEditor(this.selected.id);
            });
            document.getElementById("terrain-list")?.addEventListener("click", (event) => {
                const row = (event.target as HTMLElement).closest("[data-terrain-id]");
                if (!row)
                    return;
                const item = this.projects.find((candidate) => candidate.id === row.getAttribute("data-terrain-id"));
                if (item)
                    void this.select(item);
            });
            document.getElementById("terrain-list")?.addEventListener("dblclick", (event) => {
                const row = (event.target as HTMLElement).closest("[data-terrain-id]");
                if (!row)
                    return;
                void this.openEditor(row.getAttribute("data-terrain-id") || "");
            });
            // 双击预览模型同样进入编辑。
            document.getElementById("terrain-preview-canvas")?.addEventListener("dblclick", () => {
                if (this.selected)
                    void this.openEditor(this.selected.id);
            });
            document.getElementById("terrain-dialog-form")?.addEventListener("submit", (event) => {
                event.preventDefault();
                void this.submitDialog();
            });
            document.getElementById("terrain-dialog-cancel")?.addEventListener("click", () => this.closeDialog());
            document.getElementById("terrain-dialog-close")?.addEventListener("click", () => this.closeDialog());
            await this.bridge.connect();
            await this.bridge.on("terrainChanged", () => this.scheduleRefresh());
        }
        await this.refresh();
    }
    private refreshTimer = 0;
    private scheduleRefresh(): void {
        window.clearTimeout(this.refreshTimer);
        this.refreshTimer = window.setTimeout(() => { void this.refresh(); }, 120);
    }
    private async refresh(): Promise<void> {
        const revision = ++this.revision;
        try {
            const data = await this.bridge.call<{
                items: TerrainItem[];
            }>("getTerrains");
            if (revision !== this.revision)
                return;
            this.projects = data.items;
            document.getElementById("terrain-error")!.hidden = true;
            if (this.selected && !this.projects.some((item) => item.id === this.selected!.id)) {
                this.selected = null;
                this.renderPreview();
            }
        }
        catch (error) {
            if (revision !== this.revision)
                return;
            const target = document.getElementById("terrain-error")!;
            window.desktopI18n.text(target, window.desktopI18n.live(() => error instanceof Error ? error.message : String(error)));
            target.hidden = false;
        }
        this.renderList();
        this.renderInfo();
    }
    private renderList(): void {
        window.desktopI18n.text(document.getElementById("terrain-count")!, window.desktopI18n.live(() => this.projects.length));
        const list = document.getElementById("terrain-list")!;
        list.replaceChildren();
        if (!this.projects.length) {
            const empty = document.createElement("div");
            empty.className = "empty-state";
            window.desktopI18n.text(empty, window.desktopI18n.live(() => window.desktopI18n.t("还没有地形项目，点击右上角\u201C新建地形项目\u201D开始。")));
            list.append(empty);
            return;
        }
        for (const item of this.projects) {
            const row = document.createElement("article");
            row.className = "terrain-row" + (this.selected?.id === item.id ? " active" : "");
            row.dataset.terrainId = item.id;
            const title = document.createElement("button");
            title.type = "button";
            title.className = "terrain-row-title";
            window.desktopI18n.text(title, window.desktopI18n.live(() => item.title));
            window.desktopI18n.attr(title, "title", window.desktopI18n.live(() => item.title + window.desktopI18n.t("（双击打开编辑窗口）")));
            const meta = document.createElement("div");
            meta.className = "terrain-row-meta";
            window.desktopI18n.text(meta, window.desktopI18n.live(() => window.desktopI18n.format("{0}\u00D7{1} 米 \u00B7 间距 {2} 米 \u00B7 {3}\u00D7{4}", item.sizeWidth, item.sizeDepth, item.spacing, item.gridWidth, item.gridDepth)));
            row.append(title, meta);
            list.append(row);
        }
    }
    private renderInfo(): void {
        const info = document.getElementById("terrain-info")!;
        const actions = document.getElementById("terrain-actions")!;
        const empty = document.getElementById("terrain-preview-empty")!;
        const panel = document.getElementById("terrain-preview-panel")!;
        if (!this.selected) {
            window.desktopI18n.text(info, window.desktopI18n.live(() => window.desktopI18n.t("选择一个项目查看预览与基本信息。")));
            actions.hidden = true;
            empty.hidden = false;
            panel.hidden = true;
            return;
        }
        empty.hidden = true;
        panel.hidden = false;
        actions.hidden = false;
        const item = this.selected;
        window.desktopI18n.text(info, window.desktopI18n.live(() => window.desktopI18n.format("{0}：实际尺寸 {1}\u00D7{2} 米，", item.title, item.sizeWidth, item.sizeDepth) + window.desktopI18n.format("采样间距 {0} 米，网格 {1}\u00D7{2}，", item.spacing, item.gridWidth, item.gridDepth)
            + window.desktopI18n.format("高度数据约 {0} MB。", (item.gridWidth * item.gridDepth * 4 / 1048576).toFixed(1))));
    }
    private async select(item: TerrainItem): Promise<void> {
        this.selected = item;
        this.renderList();
        this.renderInfo();
        await this.renderPreview();
    }
    private async renderPreview(): Promise<void> {
        const notice = document.getElementById("terrain-preview-note")!;
        if (!this.selected) {
            this.preview.clear();
            window.desktopI18n.text(notice, window.desktopI18n.live(() => ""));
            return;
        }
        if (this.preview.failed) {
            window.desktopI18n.text(notice, window.desktopI18n.live(() => window.desktopI18n.t("当前环境不支持 WebGL 2，无法显示三维缩略预览。")));
            return;
        }
        window.desktopI18n.text(notice, window.desktopI18n.live(() => window.desktopI18n.t("正在加载预览\u2026")));
        try {
            const data = await this.bridge.call<{
                gridWidth: number;
                gridDepth: number;
                spacing: number;
                heights: string;
            }>("getTerrainPreview", this.selected.id);
            const heights = decodeBase64Floats(data.heights);
            this.preview.show(heights, data.gridWidth, data.gridDepth, data.spacing);
            window.desktopI18n.text(notice, window.desktopI18n.live(() => ""));
        }
        catch (error) {
            window.desktopI18n.text(notice, window.desktopI18n.live(() => window.desktopI18n.t("预览加载失败：") + (error instanceof Error ? error.message : String(error))));
        }
    }
    private async openEditor(id: string): Promise<void> {
        try {
            await this.bridge.call("openTerrainEditor", id);
        }
        catch (error) {
            this.options.reportError(error);
        }
    }
    // ---------- 对话框 ----------
    private dialogMode: "create" | "rename" | "delete" | null = null;
    private dialogTarget: TerrainItem | null = null;
    private openDialog(title: string, submitText: string, body: HTMLElement[], danger = false): void {
        window.desktopI18n.text(document.getElementById("terrain-dialog-title")!, window.desktopI18n.live(() => title));
        const bodyNode = document.getElementById("terrain-dialog-body")!;
        bodyNode.replaceChildren(...body);
        const error = document.getElementById("terrain-dialog-error")!;
        error.hidden = true;
        const submit = document.getElementById("terrain-dialog-submit") as HTMLButtonElement;
        window.desktopI18n.text(submit, window.desktopI18n.live(() => submitText));
        submit.className = "button primary" + (danger ? " danger" : "");
        (document.getElementById("terrain-dialog") as HTMLDialogElement).showModal();
    }
    private closeDialog(): void {
        (document.getElementById("terrain-dialog") as HTMLDialogElement).close();
        this.dialogMode = null;
    }
    private formField(label: string, id: string, value: string, attrs: Record<string, string> = {}): HTMLLabelElement {
        const labelNode = document.createElement("label");
        labelNode.className = "form-field";
        const span = document.createElement("span");
        window.desktopI18n.text(span, window.desktopI18n.live(() => label));
        const input = document.createElement("input");
        input.id = id;
        input.value = value;
        for (const [key, attr] of Object.entries(attrs))
            input.setAttribute(key, attr);
        labelNode.append(span, input);
        return labelNode;
    }
    private createDialog(): void {
        this.dialogMode = "create";
        const name = this.formField(window.desktopI18n.live(() => window.desktopI18n.t("项目名称")), "terrain-name", "", { maxlength: "200", required: "", placeholder: window.desktopI18n.t("例如：开局平原") });
        const width = this.formField(window.desktopI18n.live(() => window.desktopI18n.t("地形宽度（米）")), "terrain-width", "512", { type: "number", min: "1", step: "any" });
        const length = this.formField(window.desktopI18n.live(() => window.desktopI18n.t("地形长度（米）")), "terrain-length", "512", { type: "number", min: "1", step: "any" });
        const spacing = this.formField(window.desktopI18n.live(() => window.desktopI18n.t("采样间距（米）")), "terrain-spacing", "2", { type: "number", min: "0.1", step: "any" });
        const height = this.formField(window.desktopI18n.live(() => window.desktopI18n.t("初始地面高度（米）")), "terrain-height", "0", { type: "number", step: "any" });
        const hint = document.createElement("p");
        hint.className = "dialog-description";
        hint.id = "terrain-create-hint";
        window.desktopI18n.text(hint, window.desktopI18n.live(() => window.desktopI18n.t("尺寸不能被采样间距整除时，将向下取整并显示实际尺寸。")));
        this.openDialog(window.desktopI18n.live(() => window.desktopI18n.t("新建地形项目")), window.desktopI18n.live(() => window.desktopI18n.t("创建")), [name, width, length, spacing, height, hint]);
        const updateHint = () => {
            const w = Number((width.querySelector("input") as HTMLInputElement).value);
            const l = Number((length.querySelector("input") as HTMLInputElement).value);
            const s = Number((spacing.querySelector("input") as HTMLInputElement).value);
            const target = document.getElementById("terrain-create-hint")!;
            if (!(w > 0 && l > 0 && s > 0)) {
                window.desktopI18n.text(target, window.desktopI18n.live(() => window.desktopI18n.t("请输入正数尺寸与采样间距。")));
                return;
            }
            const gw = Math.max(2, Math.floor(w / s + 1e-9) + 1);
            const gd = Math.max(2, Math.floor(l / s + 1e-9) + 1);
            const cells = gw * gd;
            window.desktopI18n.text(target, window.desktopI18n.live(() => window.desktopI18n.format("实际尺寸 {0}\u00D7{1} 米，网格 {2}\u00D7{3}，", (gw - 1) * s, (gd - 1) * s, gw, gd) + window.desktopI18n.format("高度数据约 {0} MB。", (cells * 4 / 1048576).toFixed(1))
                + (cells > 4194304 ? window.desktopI18n.t(" 超过 2049\u00D72049 采样点上限，请增大采样间距或缩小地形。") : "")));
        };
        for (const field of [width, length, spacing]) {
            (field.querySelector("input") as HTMLInputElement).addEventListener("input", updateHint);
        }
        updateHint();
    }
    private renameDialog(item: TerrainItem): void {
        this.dialogMode = "rename";
        this.dialogTarget = item;
        const name = this.formField(window.desktopI18n.live(() => window.desktopI18n.t("项目名称")), "terrain-name", item.title, { maxlength: "200", required: "" });
        this.openDialog(window.desktopI18n.live(() => window.desktopI18n.t("重命名地形项目")), window.desktopI18n.live(() => window.desktopI18n.t("保存")), [name]);
    }
    private deleteDialog(item: TerrainItem): void {
        this.dialogMode = "delete";
        this.dialogTarget = item;
        const text = document.createElement("p");
        text.className = "dialog-description";
        window.desktopI18n.text(text, window.desktopI18n.live(() => window.desktopI18n.format("确定删除地形项目\u201C{0}\u201D吗？其全部地形数据将一并删除，此操作无法撤销。", item.title)));
        this.openDialog(window.desktopI18n.live(() => window.desktopI18n.t("删除地形项目")), window.desktopI18n.live(() => window.desktopI18n.t("确认删除")), [text], true);
    }
    private async submitDialog(): Promise<void> {
        if (!this.dialogMode)
            return;
        const error = document.getElementById("terrain-dialog-error")!;
        error.hidden = true;
        try {
            if (this.dialogMode === "create") {
                const title = (document.getElementById("terrain-name") as HTMLInputElement).value.trim();
                const width = Number((document.getElementById("terrain-width") as HTMLInputElement).value);
                const length = Number((document.getElementById("terrain-length") as HTMLInputElement).value);
                const spacing = Number((document.getElementById("terrain-spacing") as HTMLInputElement).value);
                const height = Number((document.getElementById("terrain-height") as HTMLInputElement).value);
                const created = await this.bridge.call<TerrainItem>("createTerrain", title, width, length, spacing, height);
                await this.refresh();
                const item = this.projects.find((candidate) => candidate.id === created.id) || null;
                if (item)
                    await this.select(item);
                this.options.toast(window.desktopI18n.live(() => created.sizeWidth < width - 1e-9 || created.sizeDepth < length - 1e-9
                    ? window.desktopI18n.format("项目已创建：实际尺寸调整为 {0}\u00D7{1} 米", created.sizeWidth, created.sizeDepth) : window.desktopI18n.t("地形项目已创建")));
            }
            else if (this.dialogMode === "rename" && this.dialogTarget) {
                const title = (document.getElementById("terrain-name") as HTMLInputElement).value.trim();
                await this.bridge.call("renameTerrain", this.dialogTarget.id, title);
                await this.refresh();
                this.options.toast(window.desktopI18n.live(() => window.desktopI18n.t("名称已更新")));
            }
            else if (this.dialogMode === "delete" && this.dialogTarget) {
                await this.bridge.call("deleteTerrains", [this.dialogTarget.id]);
                await this.refresh();
                this.options.toast(window.desktopI18n.live(() => window.desktopI18n.t("地形项目已删除")));
            }
            this.closeDialog();
        }
        catch (dialogError) {
            window.desktopI18n.text(error, window.desktopI18n.live(() => dialogError instanceof Error ? dialogError.message : String(dialogError)));
            error.hidden = false;
        }
    }
}
/** 解析后端返回的 Base64 Float32 数据。 */
function decodeBase64Floats(base64: string): Float32Array {
    const binary = atob(base64);
    const bytes = new Uint8Array(binary.length);
    for (let index = 0; index < binary.length; ++index)
        bytes[index] = binary.charCodeAt(index);
    const view = new DataView(bytes.buffer);
    const floats = new Float32Array(bytes.length / 4);
    for (let index = 0; index < floats.length; ++index)
        floats[index] = view.getFloat32(index * 4, true);
    return floats;
}
const page = new TerrainPage();
window.desktopTerrain = {
    initialize: (options: PageOptions) => page.initialize(options),
    canLeave: () => page.canLeave
};
