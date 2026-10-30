/**
 * 地形编辑器主控制器：三维显示、笔划调度、撤销、保存、导出与设置。
 * 界面事件只做输入采集；所有数据操作在 core/ 与 render/ 模块中完成。
 */
import * as THREE from "three";
import { applyStamp, BrushSettings, defaultBrushSettings, ToolId } from "../core/brushes";
import { HeightField } from "../core/heightfield";
import { computeMapping, encode16Bit, resolveExportGrid, resample, verticesForSize } from "../core/resample";
import { StrokeTransaction, UndoStack } from "../core/undo";
import { TerrainCamera } from "../render/controls";
import { buildTerrainGeometry, GridRect, refreshAllColors, refreshRegion } from "../render/mesh";
import { pickHeightField, rayFromPointer } from "../render/picker";
import { QtBridge } from "./bridge";
import { HEIGHT_HEADER_BYTES, readHeightChunks, writeExportChunks, writeHeightChunks } from "./transfer";
/** 后端返回的项目元数据。 */
interface ProjectMeta {
    id: string;
    title: string;
    gridWidth: number;
    gridDepth: number;
    spacing: number;
    sizeWidth: number;
    sizeDepth: number;
    undoSteps: number;
}
/** 页面状态汇总。 */
class TerrainEditor {
    readonly bridge = new QtBridge();
    projectId = "";
    projectName = "";
    field: HeightField = new HeightField(2, 2, 1);
    undoStack = new UndoStack(30);
    settings: BrushSettings = defaultBrushSettings();
    tool: ToolId = "detail";
    dirty = false;
    saving = false;
    // 三维场景相关。
    private renderer: THREE.WebGLRenderer | null = null;
    private scene: THREE.Scene | null = null;
    private camera: THREE.PerspectiveCamera | null = null;
    private controls = new TerrainCamera();
    private geometry: THREE.BufferGeometry | null = null;
    private mesh: THREE.Mesh | null = null;
    private brushRing: THREE.LineLoop | null = null;
    private seaPlane: THREE.Mesh | null = null;
    private webglAvailable = true;
    private heightMin = 0;
    private heightMax = 1;
    // 笔划与渲染调度。
    private stroke: {
        transaction: StrokeTransaction;
        scratch: Map<number, number>;
        lastX: number;
        lastZ: number;
        lastTime: number;
        flattenTarget: number;
        lastHintAt: number;
    } | null = null;
    private pendingRect: GridRect | null = null;
    private cameraMode: "rotate" | "pan" | null = null;
    private lastPointer = { x: 0, y: 0 };
    // 页面元素引用。
    private canvas!: HTMLCanvasElement;
    /** 启动：解析项目、建立连接、加载高度数据并初始化界面。 */
    async boot(): Promise<void> {
        this.projectId = new URLSearchParams(window.location.search).get("id") || "";
        if (!this.projectId) {
            this.showFatal(window.desktopI18n.live(() => window.desktopI18n.t("缺少项目标识，请从地形页面双击项目打开编辑窗口。")));
            return;
        }
        this.canvas = document.getElementById("terrain-canvas") as HTMLCanvasElement;
        try {
            const meta = await this.bridge.call<ProjectMeta>("getInitialData");
            this.projectName = meta.title;
            this.undoStack.setMaxSteps(meta.undoSteps || 30);
            await this.loadHeights();
            this.initScene();
            this.wireUi();
            this.updateStatus(window.desktopI18n.live(() => window.desktopI18n.t("已加载项目：") + this.projectName));
            this.exposeTestApi();
            this.markReady();
        }
        catch (error) {
            this.showFatal(error instanceof Error ? error.message : String(error));
        }
    }
    private showFatal(message: string): void {
        const overlay = document.getElementById("terrain-fatal");
        if (overlay) {
            overlay.hidden = false;
            window.desktopI18n.text(overlay, window.desktopI18n.live(() => message));
        }
        (window as unknown as {
            terrainBootError?: string;
        }).terrainBootError = message;
        this.updateStatus(window.desktopI18n.live(() => message));
    }
    private markReady(): void {
        const page = document.getElementById("terrain-root");
        if (page)
            page.dataset.ready = "true";
        (window as unknown as {
            terrainReady?: boolean;
        }).terrainReady = true;
    }
    /** 通过分块流读取高度数据并构建高度场。 */
    private async loadHeights(): Promise<void> {
        const info = await this.bridge.call<{
            token: string;
            bytes: number;
            chunkSize: number;
            gridWidth: number;
            gridDepth: number;
            spacing: number;
            sizeWidth: number;
            sizeDepth: number;
            undoSteps: number;
        }>("openHeightStream");
        const heights = await readHeightChunks(this.bridge, info, HEIGHT_HEADER_BYTES);
        this.field = new HeightField(info.gridWidth, info.gridDepth, info.spacing, heights);
        // 服务端的实际尺寸 = (顶点数-1)×间距，与高度场自身推导一致。
        this.recomputeHeightRange();
        this.undoStack.clear();
        this.dirty = false;
        this.syncDirty();
    }
    // ---------- 三维场景 ----------
    private initScene(): void {
        try {
            this.renderer = new THREE.WebGLRenderer({ canvas: this.canvas, antialias: true });
            if (!this.renderer.capabilities.isWebGL2) {
                throw new Error(window.desktopI18n.t("当前环境的浏览器内核不支持 WebGL 2。"));
            }
        }
        catch (error) {
            this.webglAvailable = false;
            this.showFatal(window.desktopI18n.live(() => window.desktopI18n.t("三维视图不可用：") + (error instanceof Error ? error.message : String(error))
                + window.desktopI18n.t("。数据操作（保存、导出、撤销）仍可使用。")));
            return;
        }
        this.renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
        this.scene = new THREE.Scene();
        this.scene.background = new THREE.Color(0x9db8d2);
        this.camera = new THREE.PerspectiveCamera(52, 1, 0.5, 40000);
        const hemisphere = new THREE.HemisphereLight(0xdfeaf5, 0x8a7a5a, 0.85);
        this.scene.add(hemisphere);
        const sun = new THREE.DirectionalLight(0xfff2dd, 1.35);
        sun.position.set(-0.6, 1, 0.45).multiplyScalar(600);
        this.scene.add(sun);
        this.geometry = buildTerrainGeometry(this.field, this.heightMin, this.heightMax);
        const material = new THREE.MeshStandardMaterial({ vertexColors: true, roughness: 0.95, metalness: 0 });
        this.mesh = new THREE.Mesh(this.geometry, material);
        this.scene.add(this.mesh);
        // 画笔范围预览环：跟随悬停位置贴合地面。
        const ringGeometry = new THREE.BufferGeometry();
        const segments = 64;
        const ringPositions = new Float32Array((segments + 1) * 3);
        ringGeometry.setAttribute("position", new THREE.BufferAttribute(ringPositions, 3));
        this.brushRing = new THREE.LineLoop(ringGeometry, new THREE.LineBasicMaterial({ color: 0x12305e }));
        this.brushRing.visible = false;
        this.brushRing.frustumCulled = false;
        this.scene.add(this.brushRing);
        // 海平面预览平面：仅观察辅助，不参与地形数据。
        this.seaPlane = new THREE.Mesh(new THREE.PlaneGeometry(1, 1), new THREE.MeshBasicMaterial({ color: 0x2f6fe0, transparent: true, opacity: 0.24,
            side: THREE.DoubleSide, depthWrite: false }));
        this.seaPlane.visible = false;
        this.scene.add(this.seaPlane);
        this.controls.fitTerrain(this.field);
        this.resize();
        window.addEventListener("resize", () => { this.resize(); this.requestRender(); });
        // 容器布局晚于初始化变化时（例如首次布局、面板折叠）也能正确重建画布。
        const wrap = this.canvas.parentElement;
        if (wrap && typeof ResizeObserver === "function") {
            new ResizeObserver(() => this.resize()).observe(wrap);
        }
        this.requestRender();
    }
    private resize(): void {
        if (!this.renderer || !this.camera)
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
        this.requestRender();
    }
    // 渲染采用“按需渲染 + 定时器驱动”：requestAnimationFrame 在部分环境
    // （如由后台服务代启的进程）会被暂停，导致首帧之后画面完全冻结；
    // setInterval 不依赖合成器帧调度，只要页面存活就会运行。
    // 空闲时不发起渲染，GPU 零负载；交互（笔划、相机、悬停）置脏后下一拍刷新。
    private needsRender = false;
    private renderTimer = 0;
    private startRenderLoop(): void {
        if (this.renderTimer)
            return;
        this.renderTimer = window.setInterval(() => this.renderTick(), 16);
    }
    private renderTick(): void {
        if (!this.needsRender || !this.renderer || !this.scene || !this.camera)
            return;
        this.needsRender = false;
        try {
            this.camera.updateProjectionMatrix();
            this.controls.apply(this.camera, this.camera.aspect);
            this.flushRegion();
            this.renderer.render(this.scene, this.camera);
        }
        catch (error) {
            // 渲染异常时停止循环并给出可见提示，避免每帧重复抛错。
            window.clearInterval(this.renderTimer);
            this.renderTimer = 0;
            const message = error instanceof Error ? error.message : String(error);
            this.updateStatus(window.desktopI18n.live(() => window.desktopI18n.t("三维渲染已停止：") + message));
        }
    }
    private requestRender(): void {
        this.needsRender = true;
        this.startRenderLoop();
    }
    /** 把累积的脏矩形刷新到网格属性。 */
    private flushRegion(): void {
        if (!this.geometry || !this.pendingRect)
            return;
        const rect = this.pendingRect;
        this.pendingRect = null;
        refreshRegion(this.field, this.geometry, rect, this.heightMin, this.heightMax);
        this.updateRegionRange(rect);
    }
    /** 全域重着色：高度范围变化后调用，保持整幅地形颜色连续。 */
    private refreshAllColors(): void {
        if (!this.geometry)
            return;
        refreshAllColors(this.field, this.geometry, this.heightMin, this.heightMax);
        this.requestRender();
    }
    /** 用刷新区域的实际高度增量更新全局范围（供颜色映射与拾取使用）。 */
    private updateRegionRange(rect: GridRect): void {
        for (let z = Math.max(0, rect.z0); z <= Math.min(this.field.gridDepth - 1, rect.z1); ++z) {
            for (let x = Math.max(0, rect.x0); x <= Math.min(this.field.gridWidth - 1, rect.x1); ++x) {
                const value = this.field.heights[z * this.field.gridWidth + x];
                if (value < this.heightMin)
                    this.heightMin = value;
                if (value > this.heightMax)
                    this.heightMax = value;
            }
        }
    }
    private recomputeHeightRange(): void {
        let min = Number.POSITIVE_INFINITY;
        let max = Number.NEGATIVE_INFINITY;
        for (let index = 0; index < this.field.heights.length; ++index) {
            const value = this.field.heights[index];
            if (value < min)
                min = value;
            if (value > max)
                max = value;
        }
        if (Number.isFinite(min)) {
            this.heightMin = min;
            this.heightMax = max;
        }
        else {
            this.heightMin = 0;
            this.heightMax = 1;
        }
    }
    // ---------- 界面交互 ----------
    private wireUi(): void {
        // 工具栏按钮。
        const tools: Array<[
            ToolId,
            string
        ]> = [
            ["detail", "tool-detail"], ["adjust", "tool-adjust"], ["mountain", "tool-mountain"],
            ["river", "tool-river"], ["road", "tool-road"], ["lake", "tool-lake"], ["coast", "tool-coast"]
        ];
        for (const [tool, id] of tools) {
            const button = document.getElementById(id);
            button?.addEventListener("click", () => this.selectTool(tool));
        }
        document.getElementById("action-undo")?.addEventListener("click", () => this.undo());
        document.getElementById("action-redo")?.addEventListener("click", () => this.redo());
        document.getElementById("action-save")?.addEventListener("click", () => {
            void this.save().catch((error) => this.updateStatus(window.desktopI18n.live(() => error.message)));
        });
        document.getElementById("action-export")?.addEventListener("click", () => {
            void this.runExport().catch((error) => this.updateStatus(window.desktopI18n.live(() => error.message)));
        });
        document.getElementById("action-resample")?.addEventListener("click", () => this.openResampleDialog());
        document.getElementById("action-undo-steps")?.addEventListener("click", () => this.applyUndoSteps());
        document.getElementById("action-close")?.addEventListener("click", () => {
            void this.bridge.call("requestClose").catch(() => undefined);
        });
        // 设置块常驻参数面板底部；“设置”按钮仅滚动定位。
        document.getElementById("action-settings")?.addEventListener("click", () => {
            document.querySelector(".terrain-params")
                ?.scrollTo({ top: 999999, behavior: "smooth" });
        });
        document.getElementById("resample-confirm")?.addEventListener("click", () => {
            void this.confirmResample().catch((error) => this.updateStatus(window.desktopI18n.live(() => error.message)));
        });
        document.getElementById("resample-cancel")?.addEventListener("click", () => this.closeDialog("resample-dialog"));
        document.getElementById("resample-spacing")?.addEventListener("input", () => this.updateResamplePreview());
        document.getElementById("export-spacing")?.addEventListener("input", () => this.updateExportPreview());
        document.getElementById("coast-sea-level")?.addEventListener("input", () => this.updateSeaPlane());
        // “显示海平面”勾选后，即使切换到其他画笔也持续显示海平面预览。
        document.getElementById("coast-show-sea-level")?.addEventListener("change", () => this.updateSeaPlane());
        this.installPointerHandlers();
        this.installKeyboard();
        const currentSpacing = document.getElementById("resample-current") as HTMLInputElement | null;
        if (currentSpacing)
            currentSpacing.value = `${this.field.spacing} 米（网格 ${this.field.gridWidth}×${this.field.gridDepth}）`;
        const exportSpacing = document.getElementById("export-spacing") as HTMLInputElement | null;
        if (exportSpacing)
            exportSpacing.value = String(this.field.spacing);
        this.selectTool("detail");
        this.updateExportPreview();
        this.updateUndoMemory();
    }
    /** 切换当前工具并同步右侧参数面板与状态提示。 */
    private selectTool(tool: ToolId): void {
        this.tool = tool;
        document.querySelectorAll("[data-tool-panel]").forEach((node) => {
            (node as HTMLElement).hidden = (node as HTMLElement).dataset.toolPanel !== tool;
        });
        document.querySelectorAll("[data-tool-button]").forEach((node) => {
            node.classList.toggle("active", (node as HTMLElement).dataset.toolButton === tool);
        });
        this.updateSeaPlane();
        this.updateStatus(window.desktopI18n.live(() => this.toolDescription(tool)));
    }
    private toolDescription(tool: ToolId): string {
        switch (tool) {
            case "detail": return window.desktopI18n.t("细节画笔：左键抬高，按住 Alt 左键降低；右键旋转、中键平移、滚轮缩放。");
            case "adjust": return window.desktopI18n.t("调整画笔：平滑接缝、平整到落笔时的高度、锐化地形特征。");
            case "mountain": return window.desktopI18n.t("山画笔：拖动逐渐形成所选类型的山地。");
            case "river": return window.desktopI18n.t("河流画笔：沿路径生成河床与河岸；同一笔划内重复覆盖不会持续加深。");
            case "road": return window.desktopI18n.t("道路画笔：路面横向平整、纵向随原地形；跨越斜坡时自动放坡。");
            case "lake": return window.desktopI18n.t("湖泊画笔：拖动形成连续湖盆与岸坡。");
            case "coast": return window.desktopI18n.t("海岸画笔：以海平面为参考重塑岸坡；未检测到岸线时不修改地形。");
        }
    }
    /**
     * 海平面预览平面：默认只在海岸工具下显示；
     * 勾选“显示海平面”后跨工具持续显示。仅作观察辅助，不参与地形数据。
     */
    private updateSeaPlane(): void {
        if (!this.seaPlane || !this.scene)
            return;
        const checkbox = document.getElementById("coast-show-sea-level") as HTMLInputElement | null;
        const pinned = checkbox ? checkbox.checked : false;
        if (this.tool !== "coast" && !pinned) {
            this.seaPlane.visible = false;
            this.requestRender();
            return;
        }
        const level = this.readNumber("coast-sea-level", 0);
        this.seaPlane.visible = true;
        this.seaPlane.position.set(this.field.sizeWidth / 2, level, this.field.sizeDepth / 2);
        this.seaPlane.rotation.x = -Math.PI / 2;
        this.seaPlane.scale.set(this.field.sizeWidth * 1.02, this.field.sizeDepth * 1.02, 1);
        this.requestRender();
    }
    private installPointerHandlers(): void {
        const canvas = this.canvas;
        canvas.addEventListener("contextmenu", (event) => event.preventDefault());
        canvas.addEventListener("pointerdown", (event) => {
            try {
                canvas.setPointerCapture(event.pointerId);
            }
            catch { /* 无活动指针时忽略 */ }
            if (event.button === 2 || event.button === 1) {
                this.cameraMode = event.button === 2 ? "rotate" : "pan";
                this.lastPointer = { x: event.clientX, y: event.clientY };
                return;
            }
            if (event.button !== 0 || !this.webglAvailable)
                return;
            const hit = this.pick(event);
            if (!hit)
                return;
            this.beginStroke(hit.x, hit.z, event.altKey, event.timeStamp);
        });
        canvas.addEventListener("pointermove", (event) => {
            if (this.cameraMode) {
                const dx = event.clientX - this.lastPointer.x;
                const dy = event.clientY - this.lastPointer.y;
                this.lastPointer = { x: event.clientX, y: event.clientY };
                if (this.cameraMode === "rotate")
                    this.controls.rotate(dx, dy);
                else {
                    this.controls.pan(dx, dy, this.canvas.clientHeight);
                    this.controls.clampTarget(this.field);
                }
                this.requestRender();
                return;
            }
            const hit = this.pick(event);
            this.updateBrushRing(hit);
            if (this.stroke && hit)
                this.stampAlong(hit.x, hit.z, event.altKey, event.timeStamp);
            this.requestRender();
        });
        const endPointer = (event: PointerEvent): void => {
            if (this.cameraMode && (event.button === 2 || event.button === 1))
                this.cameraMode = null;
            if (this.stroke)
                this.endStroke();
        };
        canvas.addEventListener("pointerup", endPointer);
        canvas.addEventListener("pointercancel", endPointer);
        // 鼠标移出画布或窗口失焦时立即结束笔划，避免绘制状态卡住。
        canvas.addEventListener("pointerleave", () => {
            this.updateBrushRing(null);
            if (this.stroke)
                this.endStroke();
        });
        window.addEventListener("blur", () => {
            this.cameraMode = null;
            if (this.stroke)
                this.endStroke();
        });
        canvas.addEventListener("wheel", (event) => {
            event.preventDefault();
            this.controls.zoom(event.deltaY < 0 ? 0.88 : 1 / 0.88);
            this.requestRender();
        }, { passive: false });
    }
    private installKeyboard(): void {
        const typing = (target: EventTarget | null): boolean => {
            if (!(target instanceof HTMLElement))
                return false;
            return target.tagName === "INPUT" || target.tagName === "SELECT"
                || target.tagName === "TEXTAREA" || target.isContentEditable;
        };
        document.addEventListener("keydown", (event) => {
            if (typing(event.target))
                return;
            if ((event.ctrlKey || event.metaKey) && !event.altKey) {
                const key = event.key.toLowerCase();
                if (key === "z" && !event.shiftKey) {
                    event.preventDefault();
                    this.undo();
                }
                else if (key === "y" || (key === "z" && event.shiftKey)) {
                    event.preventDefault();
                    this.redo();
                }
                else if (key === "s") {
                    event.preventDefault();
                    void this.save().catch((error) => this.updateStatus(window.desktopI18n.live(() => error.message)));
                }
                return;
            }
            if (event.key === "[" || event.key === "]") {
                const delta = event.key === "[" ? -2 : 2;
                this.adjustBrushSize(delta);
            }
        });
    }
    private adjustBrushSize(delta: number): void {
        const key = this.tool as keyof BrushSettings;
        const params = this.settings[key] as {
            size?: number;
            width?: number;
        };
        if (params && typeof params.size === "number") {
            params.size = Math.max(2, Math.min(600, params.size + delta));
            this.syncParamInputs();
            this.updateStatus(window.desktopI18n.live(() => window.desktopI18n.format("画笔大小 {0} 米", params.size)));
        }
        else if (params && typeof params.width === "number") {
            params.width = Math.max(2, Math.min(200, params.width + delta));
            this.syncParamInputs();
            this.updateStatus(window.desktopI18n.live(() => window.desktopI18n.format("画笔宽度 {0} 米", params.width)));
        }
    }
    /** 把当前参数同步到输入框（快捷键调整后）。 */
    private syncParamInputs(): void {
        const values: Array<[
            string,
            number
        ]> = [
            ["detail-size", this.settings.detail.size], ["detail-strength", this.settings.detail.strength],
            ["adjust-size", this.settings.adjust.size], ["adjust-strength", this.settings.adjust.strength],
            ["mountain-size", this.settings.mountain.size], ["mountain-strength", this.settings.mountain.strength],
            ["river-width", this.settings.river.width], ["river-depth", this.settings.river.depth],
            ["road-width", this.settings.road.width],
            ["lake-size", this.settings.lake.size], ["lake-depth", this.settings.lake.depth],
            ["coast-size", this.settings.coast.size], ["coast-strength", this.settings.coast.strength],
            ["coast-sea-level", this.settings.coast.seaLevel]
        ];
        for (const [id, value] of values) {
            const input = document.getElementById(id) as HTMLInputElement | null;
            if (input)
                input.value = String(value);
        }
    }
    /** 从界面读取设置。 */
    private captureSettings(): void {
        const number = (id: string, fallback: number, min: number, max: number): number => {
            const value = this.readNumber(id, fallback);
            return Math.min(max, Math.max(min, value));
        };
        this.settings.detail.size = number("detail-size", 20, 2, 600);
        this.settings.detail.strength = number("detail-strength", 24, 1, 200);
        this.settings.adjust.size = number("adjust-size", 24, 2, 600);
        this.settings.adjust.strength = number("adjust-strength", 0.6, 0.05, 2);
        this.settings.mountain.size = number("mountain-size", 60, 4, 600);
        this.settings.mountain.strength = number("mountain-strength", 0.7, 0.05, 1);
        this.settings.river.width = number("river-width", 12, 2, 200);
        this.settings.river.depth = number("river-depth", 4, 0.5, 200);
        this.settings.road.width = number("road-width", 8, 2, 200);
        this.settings.lake.size = number("lake-size", 60, 4, 1200);
        this.settings.lake.depth = number("lake-depth", 6, 0.5, 300);
        this.settings.coast.size = number("coast-size", 80, 8, 1200);
        this.settings.coast.strength = number("coast-strength", 0.7, 0.05, 1);
        this.settings.coast.seaLevel = number("coast-sea-level", 0, -1000, 1000);
        const select = (id: string): string => {
            const node = document.getElementById(id) as HTMLSelectElement | null;
            return node ? node.value : "";
        };
        const adjustMode = select("adjust-mode");
        if (adjustMode === "smooth" || adjustMode === "flatten" || adjustMode === "sharpen") {
            this.settings.adjust.mode = adjustMode;
        }
        const mountainType = select("mountain-type");
        if (mountainType === "hill" || mountainType === "peak" || mountainType === "ridge"
            || mountainType === "mesa") {
            this.settings.mountain.type = mountainType;
        }
        const stratum = (id: string): "soft" | "normal" | "hard" => {
            const value = select(id);
            return value === "soft" || value === "hard" ? value : "normal";
        };
        this.settings.river.stratum = stratum("river-stratum");
        this.settings.lake.stratum = stratum("lake-stratum");
        const bed = select("road-bed");
        this.settings.road.bed = bed === "steep" ? "steep" : "gentle";
        const coastType = select("coast-type");
        if (coastType === "sand" || coastType === "dirt" || coastType === "rock") {
            this.settings.coast.type = coastType;
        }
    }
    private readNumber(id: string, fallback: number): number {
        const input = document.getElementById(id) as HTMLInputElement | null;
        if (!input)
            return fallback;
        const value = Number(input.value);
        return Number.isFinite(value) ? value : fallback;
    }
    // ---------- 拾取与笔划 ----------
    private pick(event: PointerEvent | MouseEvent): {
        x: number;
        z: number;
    } | null {
        if (!this.camera)
            return null;
        const rect = this.canvas.getBoundingClientRect();
        if (rect.width <= 0 || rect.height <= 0)
            return null;
        const ndcX = ((event.clientX - rect.left) / rect.width) * 2 - 1;
        const ndcY = -((event.clientY - rect.top) / rect.height) * 2 + 1;
        const ray = rayFromPointer(this.camera, ndcX, ndcY);
        const hit = pickHeightField(ray, this.field, this.heightMin, this.heightMax);
        return hit ? { x: hit.point.x, z: hit.point.z } : null;
    }
    /** 画笔半径（世界米），供预览环使用。 */
    private previewRadius(): number {
        switch (this.tool) {
            case "detail": return this.settings.detail.size / 2;
            case "adjust": return this.settings.adjust.size / 2;
            case "mountain": return this.settings.mountain.size / 2 * (this.settings.mountain.type === "ridge" ? 2.6 : 1);
            case "river": return this.settings.river.width * 1.15;
            case "road": return this.settings.road.width * (this.settings.road.bed === "gentle" ? 0.975 : 0.64);
            case "lake": return this.settings.lake.size / 2 * 1.8;
            case "coast": return this.settings.coast.size / 2;
        }
    }
    private updateBrushRing(hit: {
        x: number;
        z: number;
    } | null): void {
        if (!this.brushRing || !this.webglAvailable)
            return;
        if (!hit) {
            this.brushRing.visible = false;
            return;
        }
        this.brushRing.visible = true;
        const attribute = this.brushRing.geometry.getAttribute("position") as THREE.BufferAttribute;
        const radius = this.previewRadius();
        const segments = attribute.count - 1;
        for (let index = 0; index <= segments; ++index) {
            const angle = index / segments * Math.PI * 2;
            const px = hit.x + Math.cos(angle) * radius;
            const pz = hit.z + Math.sin(angle) * radius;
            const within = px >= 0 && pz >= 0 && px <= this.field.sizeWidth && pz <= this.field.sizeDepth;
            attribute.setXYZ(index, px, within ? this.field.sampleWorld(px, pz) + 0.25 : -9999, pz);
        }
        attribute.needsUpdate = true;
    }
    private beginStroke(x: number, z: number, altKey: boolean, timeStamp: number): void {
        this.captureSettings();
        if (this.tool === "detail")
            this.settings.detail.invert = altKey;
        const transaction = new StrokeTransaction();
        this.stroke = {
            transaction,
            scratch: new Map(),
            lastX: x,
            lastZ: z,
            lastTime: timeStamp,
            flattenTarget: this.field.sampleWorld(x, z),
            lastHintAt: 0
        };
        this.stampAt(x, z, altKey, 0.016);
    }
    /** 从上一个位置沿线插值落笔，保证快速拖动时笔划连续。 */
    private stampAlong(x: number, z: number, altKey: boolean, timeStamp: number): void {
        const stroke = this.stroke;
        if (!stroke)
            return;
        const distance = Math.hypot(x - stroke.lastX, z - stroke.lastZ);
        const dt = Math.min(0.12, Math.max(0.001, (timeStamp - stroke.lastTime) / 1000));
        stroke.lastTime = timeStamp;
        const radius = this.previewRadius() || this.field.spacing;
        const step = Math.max(this.field.spacing * 0.6, radius * 0.3);
        const steps = Math.max(1, Math.min(64, Math.ceil(distance / step)));
        for (let index = 1; index <= steps; ++index) {
            const t = index / steps;
            this.stampAt(stroke.lastX + (x - stroke.lastX) * t, stroke.lastZ + (z - stroke.lastZ) * t, altKey, dt / steps);
        }
        stroke.lastX = x;
        stroke.lastZ = z;
    }
    /** 执行一次落笔并把影响区域加入刷新队列。 */
    private stampAt(x: number, z: number, altKey: boolean, dt: number): void {
        const stroke = this.stroke;
        if (!stroke)
            return;
        if (this.tool === "detail")
            this.settings.detail.invert = altKey;
        const transaction = stroke.transaction;
        const scratch = stroke.scratch;
        applyStamp(this.tool, this.settings, {
            field: this.field,
            transaction,
            flattenTarget: stroke.flattenTarget,
            scratch,
            hint: (message) => this.showHint(message, stroke)
        }, { centerX: x, centerZ: z, dt, dirX: this.strokeDirectionX(), dirZ: this.strokeDirectionZ() });
        this.mergePendingRect(transaction);
    }
    /** 笔划方向：由最近两次落笔位置估计；静止时使用上次方向或默认 +X。 */
    private strokeDirectionX(): number {
        return this.direction.x;
    }
    private strokeDirectionZ(): number {
        return this.direction.z;
    }
    private direction = { x: 1, z: 0 };
    private mergePendingRect(transaction: StrokeTransaction): void {
        // 事务的矩形在 finish 时才知道；这里用临时探针读取内部范围。
        const bounds = transactionBounds(transaction);
        if (!bounds)
            return;
        if (!this.pendingRect) {
            this.pendingRect = { ...bounds };
        }
        else {
            this.pendingRect.x0 = Math.min(this.pendingRect.x0, bounds.x0);
            this.pendingRect.z0 = Math.min(this.pendingRect.z0, bounds.z0);
            this.pendingRect.x1 = Math.max(this.pendingRect.x1, bounds.x1);
            this.pendingRect.z1 = Math.max(this.pendingRect.z1, bounds.z1);
        }
        this.requestRender();
    }
    private endStroke(): void {
        const stroke = this.stroke;
        if (!stroke)
            return;
        this.stroke = null;
        const data = stroke.transaction.finish(this.field);
        let pushed = false;
        if (data && this.undoStack.push(data)) {
            pushed = true;
            this.dirty = true;
            this.syncDirty();
            this.mergeFinishedRect(data);
            this.updateUndoMemory();
        }
        // 先刷新矩形并据此更新全局高度范围，再整幅重着色，保证颜色连续。
        this.flushRegion();
        if (pushed)
            this.refreshAllColors();
    }
    private mergeFinishedRect(data: {
        x0: number;
        z0: number;
        width: number;
        depth: number;
    }): void {
        const bounds = { x0: data.x0, z0: data.z0, x1: data.x0 + data.width - 1, z1: data.z0 + data.depth - 1 };
        if (!this.pendingRect)
            this.pendingRect = bounds;
        else {
            this.pendingRect.x0 = Math.min(this.pendingRect.x0, bounds.x0);
            this.pendingRect.z0 = Math.min(this.pendingRect.z0, bounds.z0);
            this.pendingRect.x1 = Math.max(this.pendingRect.x1, bounds.x1);
            this.pendingRect.z1 = Math.max(this.pendingRect.z1, bounds.z1);
        }
    }
    private showHint(message: string, stroke: {
        lastHintAt: number;
    }): void {
        const now = performance.now();
        if (now - stroke.lastHintAt < 1500)
            return;
        stroke.lastHintAt = now;
        this.updateStatus(window.desktopI18n.live(() => message));
    }
    // ---------- 撤销 / 重做 ----------
    undo(): void {
        if (this.stroke)
            return;
        const rect = this.undoStack.undo(this.field);
        if (!rect) {
            this.updateStatus(window.desktopI18n.live(() => window.desktopI18n.t("没有可撤销的操作。")));
            return;
        }
        this.mergeFinishedRect(rect);
        this.flushRegion();
        this.refreshAllColors();
        this.updateStatus(window.desktopI18n.live(() => window.desktopI18n.format("已撤销（剩余 {0} 步）", this.undoStack.undoCount)));
        this.updateUndoMemory();
    }
    redo(): void {
        if (this.stroke)
            return;
        const rect = this.undoStack.redo(this.field);
        if (!rect) {
            this.updateStatus(window.desktopI18n.live(() => window.desktopI18n.t("没有可重做的操作。")));
            return;
        }
        this.mergeFinishedRect(rect);
        this.flushRegion();
        this.refreshAllColors();
        this.updateStatus(window.desktopI18n.live(() => window.desktopI18n.format("已重做（剩余 {0} 步）", this.undoStack.redoCount)));
        this.updateUndoMemory();
    }
    // ---------- 保存 ----------
    async save(): Promise<void> {
        if (this.saving)
            return;
        this.saving = true;
        this.syncDirty();
        try {
            const begin = await this.bridge.call<{
                token: string;
                bytes: number;
                chunkSize: number;
            }>("beginSave", this.field.gridWidth, this.field.gridDepth, this.field.spacing, this.undoStack.maxSteps);
            await writeHeightChunks(this.bridge, begin, this.field.heights);
            await this.bridge.call("commitSave", begin.token);
            this.dirty = false;
            this.syncDirty();
            this.updateStatus(window.desktopI18n.live(() => window.desktopI18n.t("已保存 ") + new Date().toLocaleTimeString("zh-CN")));
        }
        finally {
            this.saving = false;
            this.syncDirty();
        }
    }
    /** 供原生关闭流程调用的同步入口：异步保存成功后请求关闭窗口。 */
    requestSaveAndClose(): boolean {
        void (async () => {
            try {
                if (this.dirty)
                    await this.save();
                await this.bridge.call("requestClose");
            }
            catch (error) {
                this.updateStatus(window.desktopI18n.live(() => window.desktopI18n.t("保存失败：") + (error instanceof Error ? error.message : String(error))));
            }
        })();
        return true;
    }
    // ---------- 设置：重采样与撤销步数 ----------
    private openResampleDialog(): void {
        this.updateResamplePreview();
        this.openDialog("resample-dialog");
    }
    private updateResamplePreview(): void {
        const spacing = this.readNumber("resample-spacing", this.field.spacing);
        const target = document.getElementById("resample-preview");
        if (!target)
            return;
        if (!(spacing >= 0.1 && spacing <= 100)) {
            window.desktopI18n.text(target, window.desktopI18n.live(() => window.desktopI18n.t("采样间距需在 0.1 \u2013 100 米之间。")));
            return;
        }
        const width = verticesForSize(this.field.sizeWidth, spacing);
        const depth = verticesForSize(this.field.sizeDepth, spacing);
        const memory = width * depth * 4 / (1024 * 1024);
        window.desktopI18n.text(target, window.desktopI18n.live(() => window.desktopI18n.format("这是重采样操作：新网格 {0}\u00D7{1}（{2} 万采样点），", width, depth, (width * depth / 10000).toFixed(1)) + window.desktopI18n.format("实际尺寸 {0}\u00D7{1} 米，", (width - 1) * spacing, (depth - 1) * spacing)
            + window.desktopI18n.format("高度数据约 {0} MB。", memory.toFixed(1))
            + (spacing < this.field.spacing ? window.desktopI18n.t("加密只提高后续编辑密度，不会恢复已有细节。") : "")
            + window.desktopI18n.t(" 成功后撤销历史将被清空。")));
    }
    private async confirmResample(): Promise<void> {
        const spacing = this.readNumber("resample-spacing", this.field.spacing);
        if (!(spacing >= 0.1 && spacing <= 100) || Math.abs(spacing - this.field.spacing) < 1e-9) {
            this.updateStatus(window.desktopI18n.live(() => window.desktopI18n.t("请输入与当前不同的有效采样间距（0.1 \u2013 100 米）。")));
            return;
        }
        // 重采样只影响内存数据；不保存或保存失败时磁盘数据保持原样。
        this.field = resample(this.field, spacing);
        this.undoStack.clear();
        this.dirty = true;
        this.recomputeHeightRange();
        this.rebuildScene();
        this.closeDialog("resample-dialog");
        this.updateStatus(window.desktopI18n.live(() => window.desktopI18n.format("已重采样为 {0}\u00D7{1}，采样间距 {2} 米。请保存项目。", this.field.gridWidth, this.field.gridDepth, spacing)));
        this.syncDirty();
        this.updateUndoMemory();
    }
    private applyUndoSteps(): void {
        const steps = Math.round(this.readNumber("undo-steps", 30));
        if (!(steps >= 1 && steps <= 200)) {
            this.updateStatus(window.desktopI18n.live(() => window.desktopI18n.t("撤销步数需在 1 \u2013 200 之间；过大的配置会增加内存占用。")));
            return;
        }
        this.undoStack.setMaxSteps(steps);
        this.dirty = true;
        this.syncDirty();
        this.updateUndoMemory();
        this.updateStatus(window.desktopI18n.live(() => window.desktopI18n.format("撤销步数已设为 {0} 步，将在下次保存时随项目保存。", steps)));
    }
    private updateUndoMemory(): void {
        const target = document.getElementById("undo-memory");
        if (target)
            window.desktopI18n.text(target, window.desktopI18n.live(() => window.desktopI18n.format("当前撤销历史占用约 {0} MB。", (this.undoStack.memoryBytes() / (1024 * 1024)).toFixed(2))));
    }
    // ---------- 导出 ----------
    private updateExportPreview(): void {
        const target = document.getElementById("export-preview");
        if (!target)
            return;
        try {
            const requested = this.readNumber("export-spacing", this.field.spacing);
            const grid = resolveExportGrid(this.field, requested);
            const mapping = computeMapping(this.field);
            window.desktopI18n.text(target, window.desktopI18n.live(() => window.desktopI18n.format("导出分辨率 {0}\u00D7{1}，实际间距 {2} 米。", grid.width, grid.depth, grid.spacing.toFixed(3)) + (grid.note ? " " + grid.note : "")
                + window.desktopI18n.format(" 高度映射 {0} \u2013 {1} 米。", mapping.min.toFixed(2), mapping.max.toFixed(2))));
        }
        catch (error) {
            window.desktopI18n.text(target, window.desktopI18n.live(() => error instanceof Error ? error.message : String(error)));
        }
    }
    /** 完整导出流程：解析网格 → 重采样映射 → 选择路径 → 分块传输 → 服务端编码校验。 */
    async runExport(targetPath?: string): Promise<void> {
        if (this.saving)
            throw new Error(window.desktopI18n.t("正在保存，请稍候再导出。"));
        this.saving = true;
        try {
            const requested = this.readNumber("export-spacing", this.field.spacing);
            const grid = resolveExportGrid(this.field, requested);
            const data = resample(this.field, grid.spacing);
            if (data.gridWidth !== grid.width || data.gridDepth !== grid.depth) {
                throw new Error(window.desktopI18n.t("导出网格解析不一致，已取消导出。"));
            }
            const mapping = computeMapping(data);
            const samples = encode16Bit(data, mapping);
            let path = targetPath;
            if (!path) {
                const chosen = await this.bridge.call<{
                    cancelled?: boolean;
                    target?: string;
                }>("chooseExportTarget", this.projectName + window.desktopI18n.t("_高度图"));
                if (chosen.cancelled || !chosen.target) {
                    this.updateStatus(window.desktopI18n.live(() => window.desktopI18n.t("已取消导出。")));
                    return;
                }
                path = chosen.target;
            }
            this.updateStatus(window.desktopI18n.live(() => window.desktopI18n.format("正在导出 {0}\u00D7{1} 高度图\u2026", grid.width, grid.depth)));
            const begin = await this.bridge.call<{
                token: string;
                chunkSize: number;
            }>("beginExport", path, grid.width, grid.depth, grid.spacing, mapping.min, mapping.max);
            await writeExportChunks(this.bridge, begin, samples);
            const result = await this.bridge.call<{
                pngPath: string;
                jsonPath: string;
                bitDepth: number;
                colorType: number;
                spacing: number;
            }>("commitExport", begin.token);
            this.updateStatus(window.desktopI18n.live(() => window.desktopI18n.format("已导出：{0}（16 位灰度 PNG，实际间距 {1} 米），说明文件 {2}", result.pngPath, result.spacing.toFixed(3), result.jsonPath)));
        }
        finally {
            this.saving = false;
        }
    }
    // ---------- 通用界面 ----------
    private openDialog(id: string): void {
        const dialog = document.getElementById(id) as HTMLDialogElement | null;
        dialog?.showModal();
    }
    private closeDialog(id: string): void {
        const dialog = document.getElementById(id) as HTMLDialogElement | null;
        dialog?.close();
    }
    private rebuildScene(): void {
        if (!this.scene || !this.mesh)
            return;
        this.scene.remove(this.mesh);
        this.mesh.geometry.dispose();
        this.geometry = buildTerrainGeometry(this.field, this.heightMin, this.heightMax);
        this.mesh = new THREE.Mesh(this.geometry, new THREE.MeshStandardMaterial({ vertexColors: true, roughness: 0.95, metalness: 0 }));
        this.scene.add(this.mesh);
        this.controls.fitTerrain(this.field);
        this.requestRender();
    }
    private syncDirty(): void {
        const indicator = document.getElementById("status-dirty");
        if (indicator) {
            window.desktopI18n.text(indicator, window.desktopI18n.live(() => this.saving ? window.desktopI18n.t("正在保存\u2026") : (this.dirty ? window.desktopI18n.t("未保存的修改") : window.desktopI18n.t("已保存"))));
            indicator.classList.toggle("dirty", this.dirty && !this.saving);
        }
        const saveButton = document.getElementById("action-save") as HTMLButtonElement | null;
        if (saveButton)
            saveButton.disabled = this.saving;
        void this.bridge.call("setDirty", this.dirty).catch(() => undefined);
    }
    private updateStatus(message: string): void {
        const target = document.getElementById("status-message");
        if (target)
            window.desktopI18n.text(target, window.desktopI18n.live(() => message));
        const size = document.getElementById("status-size");
        if (size)
            window.desktopI18n.text(size, window.desktopI18n.live(() => window.desktopI18n.format("{0}\u00D7{1} 米", this.field.sizeWidth, this.field.sizeDepth)));
        const spacing = document.getElementById("status-spacing");
        if (spacing)
            window.desktopI18n.text(spacing, window.desktopI18n.live(() => window.desktopI18n.format("采样间距 {0} 米", this.field.spacing)));
        const grid = document.getElementById("status-grid");
        if (grid)
            window.desktopI18n.text(grid, window.desktopI18n.live(() => window.desktopI18n.format("网格 {0}\u00D7{1}", this.field.gridWidth, this.field.gridDepth)));
    }
    // ---------- 测试与自动化接口（不进入渲染路径） ----------
    private exposeTestApi(): void {
        const api = {
            projectName: this.projectName,
            getStats: () => ({
                gridWidth: this.field.gridWidth, gridDepth: this.field.gridDepth,
                spacing: this.field.spacing, sizeWidth: this.field.sizeWidth,
                sizeDepth: this.field.sizeDepth, undoSteps: this.undoStack.maxSteps,
                canUndo: this.undoStack.canUndo, canRedo: this.undoStack.canRedo,
                dirty: this.dirty, undoMemoryBytes: this.undoStack.memoryBytes(),
                webgl: this.webglAvailable,
                canvasWidth: this.canvas.width, canvasHeight: this.canvas.height,
                canvasClientWidth: this.canvas.clientWidth, canvasClientHeight: this.canvas.clientHeight
            }),
            // 逐字节 FNV-1a（小端字节序），与 C++ 测试端的校验方式一致。
            checksum: () => {
                const bytes = new Uint8Array(this.field.heights.buffer, this.field.heights.byteOffset, this.field.heights.byteLength);
                let hash = 2166136261;
                for (let index = 0; index < bytes.length; ++index) {
                    hash = Math.imul(hash ^ bytes[index], 16777619) >>> 0;
                }
                return hash >>> 0;
            },
            sampleHeight: (x: number, z: number) => this.field.sampleWorld(x, z),
            /** 程序化笔划：按给定点序列执行一次完整笔划（等价于一次拖动）。 */
            applyStroke: (tool: ToolId, points: Array<{
                x: number;
                z: number;
            }>, dtMs = 16) => {
                if (!points.length)
                    return { changed: false };
                this.captureSettings();
                this.settings.detail.invert = false;
                const previousTool = this.tool;
                this.tool = tool;
                this.stroke = {
                    transaction: new StrokeTransaction(),
                    scratch: new Map(),
                    lastX: points[0].x,
                    lastZ: points[0].z,
                    lastTime: 0,
                    flattenTarget: this.field.sampleWorld(points[0].x, points[0].z),
                    lastHintAt: 0
                };
                this.direction = { x: 1, z: 0 };
                this.stampAt(points[0].x, points[0].z, false, dtMs / 1000);
                for (let index = 1; index < points.length; ++index) {
                    const from = points[index - 1];
                    const to = points[index];
                    const next = { x: to.x - from.x, z: to.z - from.z };
                    const length = Math.hypot(next.x, next.z) || 1;
                    this.direction = { x: next.x / length, z: next.z / length };
                    this.stampAlong(to.x, to.z, false, index * dtMs);
                }
                const changed = this.stroke.transaction.touched;
                this.endStroke();
                this.tool = previousTool;
                return { changed };
            },
            undo: () => { this.undo(); return this.undoStack.canUndo; },
            redo: () => { this.redo(); return this.undoStack.canRedo; },
            canUndo: () => this.undoStack.canUndo,
            canRedo: () => this.undoStack.canRedo,
            save: async () => { await this.save(); },
            exportHeightmap: async (path: string, spacing: number) => {
                const input = document.getElementById("export-spacing") as HTMLInputElement | null;
                if (input)
                    input.value = String(spacing);
                await this.runExport(path);
            },
            resampleTo: async (spacing: number) => {
                const input = document.getElementById("resample-spacing") as HTMLInputElement | null;
                if (input)
                    input.value = String(spacing);
                await this.confirmResample();
            },
            setUndoSteps: (steps: number) => {
                const input = document.getElementById("undo-steps") as HTMLInputElement | null;
                if (input)
                    input.value = String(steps);
                this.applyUndoSteps();
            },
            requestSaveAndClose: () => this.requestSaveAndClose(),
            /** 已提交的渲染帧数；用于验证渲染循环确实在响应交互。 */
            frameCount: () => this.renderer ? this.renderer.info.render.frame : -1,
            /** 海平面预览平面当前是否可见。 */
            seaPlaneVisible: () => this.seaPlane ? this.seaPlane.visible : false,
            /** 同步渲染一帧并导出画布 PNG 数据地址（仅用于冒烟验证，不在交互路径上）。 */
            captureView: () => {
                if (!this.renderer || !this.scene || !this.camera)
                    return "";
                this.controls.apply(this.camera, this.camera.aspect);
                this.flushRegion();
                this.renderer.render(this.scene, this.camera);
                return this.canvas.toDataURL("image/png");
            }
        };
        (window as unknown as {
            terrainEditor: unknown;
        }).terrainEditor = api;
    }
}
/** 读取事务的当前影响范围（进行中也可调用）。 */
function transactionBounds(transaction: StrokeTransaction): GridRect | null {
    return transaction.bounds();
}
// 编辑器单例与启动。
const editor = new TerrainEditor();
void editor.boot();
export { editor as terrainEditor };
