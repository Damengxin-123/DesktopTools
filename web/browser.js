/* 资源浏览器页：打开图片/视频与文件夹清单，管理两类浏览器的系统默认关联。 */
(function installBrowserPage() {
    "use strict";
    const state = {
        initialized: false,
        options: null, // 主页面提供的提示函数。
        association: null, // 后端关联状态快照。
        directory: "", // 正在浏览的文件夹。
        manualKind: "" // 格式选择弹窗当前显示的媒体类别。
    };
    function byId(id) { return document.getElementById(id); }
    // 以纯文本创建节点，路径与文件名不作为 HTML 执行。
    function node(tag, className, text) {
        const result = document.createElement(tag);
        if (className)
            result.className = className;
        if (text !== undefined)
            window.desktopI18n.text(result, window.desktopI18n.live(() => text));
        return result;
    }
    function report(error) {
        state.options.reportError(error);
    }
    // 关联操作的失败原因需要较长篇幅，显示在页面常驻提示区而不是短暂气泡。
    function showNotice(message) {
        const target = byId("browser-error");
        window.desktopI18n.text(target, window.desktopI18n.live(() => message));
        target.hidden = false;
    }
    function clearNotice() {
        byId("browser-error").hidden = true;
    }
    // 事件里的 Promise 异常统一报告，并保持按钮可再次点击。
    function action(handler) {
        return async function invoked(event) {
            const source = event.currentTarget;
            if (source)
                source.disabled = true;
            try {
                await handler(event);
            }
            catch (error) {
                report(error);
            }
            finally {
                if (source && source.isConnected)
                    source.disabled = false;
            }
        };
    }
    // 文件大小显示为 KB / MB / GB。
    function readableSize(bytes) {
        if (!Number.isFinite(bytes) || bytes < 0)
            return "";
        if (bytes < 1024 * 1024)
            return (bytes / 1024).toFixed(0) + " KB";
        if (bytes < 1024 * 1024 * 1024)
            return (bytes / (1024 * 1024)).toFixed(1) + " MB";
        return (bytes / (1024 * 1024 * 1024)).toFixed(2) + " GB";
    }
    function extensionList(extensions) {
        return (extensions || []).map(function dot(name) { return "." + name; }).join("、");
    }
    // 回读每种格式的实际关联；勾选仅代表本次要设置的范围。
    async function refreshAssociation() {
        const data = await window.desktopBridge.call("getMediaAssociation");
        state.association = data;
        for (const kind of ["image", "video"]) {
            const extensions = data[kind + "Extensions"] || [];
            const count = extensions.filter(function isOurs(ext) { return data.handlers && data.handlers[ext].ours; }).length;
            window.desktopI18n.text(byId("browser-" + kind + "-default-state"), window.desktopI18n.live(() => data.supported
                ? window.desktopI18n.t("当前由独立工具打开 ") + count + " / " + extensions.length + window.desktopI18n.t(" 种格式") : window.desktopI18n.t("当前环境不支持修改系统默认程序")));
        }
        document.querySelectorAll("#browser-openwith-grid label").forEach(function updateHandler(label) {
            const ext = label.querySelector("input").value;
            const handler = (data.handlers || {})[ext];
            window.desktopI18n.text(label.querySelector("span"), window.desktopI18n.live(() => "." + ext + " · " + (handler ? handler.name : window.desktopI18n.t("未关联"))));
        });
        return data;
    }
    // 打开单个文件；路径来自后端清单或文件选择器，不进入网页再加工。
    async function openFile(kind, path) {
        if (kind === "image")
            await window.desktopBridge.call("openImageBrowser", path);
        else
            await window.desktopBridge.call("openVideoPlayer", path);
    }
    // 渲染文件夹清单；仅使用后端返回的数据。
    async function refreshFiles() {
        const list = byId("browser-file-list");
        const summary = byId("browser-folder-summary");
        const error = byId("browser-folder-error");
        list.replaceChildren();
        if (!state.directory) {
            window.desktopI18n.text(summary, window.desktopI18n.live(() => window.desktopI18n.t("未选择文件夹")));
            window.desktopI18n.attr(summary, "title", window.desktopI18n.live(() => ""));
            byId("browser-folder-empty").hidden = false;
            return;
        }
        const data = await window.desktopBridge.call("listMediaDirectory", state.directory);
        window.desktopI18n.text(summary, window.desktopI18n.live(() => data.directory + window.desktopI18n.t("（图片 ") + data.imageCount + window.desktopI18n.t("，视频 ") + data.videoCount
            + (data.truncated ? window.desktopI18n.t("，仅显示前 ") + data.items.length + window.desktopI18n.t(" 项") : "") + "）"));
        window.desktopI18n.attr(summary, "title", window.desktopI18n.live(() => data.directory));
        if (!data.items.length) {
            byId("browser-folder-empty").hidden = false;
            return;
        }
        byId("browser-folder-empty").hidden = true;
        for (const item of data.items) {
            const row = node("button", "browser-file-row");
            row.type = "button";
            window.desktopI18n.attr(row, "title", window.desktopI18n.live(() => item.path));
            const icon = node("span", "browser-file-kind " + (item.kind === "image" ? "image" : "video"), window.desktopI18n.live(() => item.kind === "image" ? "▨" : "▶"));
            icon.setAttribute("aria-hidden", "true");
            const name = node("span", "browser-file-name", window.desktopI18n.live(() => item.name));
            const meta = node("span", "browser-file-meta", window.desktopI18n.live(() => readableSize(item.bytes)));
            row.append(icon, name, meta);
            row.addEventListener("click", action(function openRowClicked() {
                return openFile(item.kind, item.path);
            }));
            list.append(row);
        }
    }
    // 选择文件夹并刷新清单。
    async function chooseFolder() {
        const result = await window.desktopBridge.call("chooseTarget", 0);
        if (!result || result.cancelled || !result.target)
            return;
        state.directory = result.target;
        await refreshFiles();
    }
    // 每种格式独立勾选；未勾选项不会提交给系统设置。
    function openManualDialog(kind) {
        state.manualKind = kind;
        const data = state.association || {};
        const grid = byId("browser-openwith-grid");
        grid.replaceChildren();
        for (const ext of data[kind + "Extensions"] || []) {
            const label = node("label", "browser-openwith-chip");
            const input = node("input");
            input.type = "checkbox";
            input.value = ext;
            input.checked = Boolean(data.handlers && data.handlers[ext].ours);
            label.append(input, node("span", "", window.desktopI18n.live(() => "." + ext)));
            grid.append(label);
        }
        window.desktopI18n.text(byId("browser-openwith-title"), window.desktopI18n.live(() => kind === "image" ? window.desktopI18n.t("选择图片默认打开格式") : window.desktopI18n.t("选择视频默认打开格式")));
        window.desktopI18n.text(byId("browser-openwith-progress"), window.desktopI18n.live(() => window.desktopI18n.t("勾选需要设置的格式。取消勾选只会排除本次设置；更换已有默认程序需在系统设置中完成。")));
        byId("browser-openwith-dialog").showModal();
        refreshAssociation().catch(report);
    }
    // 关闭格式选择弹窗。
    function closeManualDialog() { byId("browser-openwith-dialog").close(); }
    // 一次提交选中格式；等待用户在系统设置确认后主动或自动刷新。
    async function configureSelected() {
        const extensions = Array.from(document.querySelectorAll("#browser-openwith-grid input:checked"), function value(input) { return input.value; });
        if (!extensions.length) {
            window.desktopI18n.text(byId("browser-openwith-progress"), window.desktopI18n.live(() => window.desktopI18n.t("请至少勾选一种文件类型。")));
            return;
        }
        try {
            await window.desktopBridge.call("configureMediaDefaults", extensions);
            clearNotice();
            window.desktopI18n.text(byId("browser-openwith-progress"), window.desktopI18n.live(() => window.desktopI18n.t("已打开系统设置，请将 ") + extensionList(extensions) + window.desktopI18n.t(" 设为 ") + window.desktopI18n.t(state.manualKind === "image" ? "DesktopTool 图像浏览器" : "DesktopTool 视频浏览器") + window.desktopI18n.t("。完成后点击“刷新状态”；若只显示默认应用首页，请搜索对应工具名称或扩展名。")));
        }
        catch (error) {
            window.desktopI18n.text(byId("browser-openwith-progress"), window.desktopI18n.live(() => error.message));
            showNotice(error.message);
        }
    }
    // 通过原生选择器打开单个文件。
    async function chooseFile(kind) {
        const result = await window.desktopBridge.call("chooseMediaFile", kind);
        if (!result || result.cancelled || !result.target)
            return;
        await openFile(kind, result.target);
    }
    async function initialize(options) {
        state.options = options;
        if (!state.initialized) {
            state.initialized = true;
            byId("browser-open-image").addEventListener("click", action(function openImageClicked() {
                return chooseFile("image");
            }));
            byId("browser-open-video").addEventListener("click", action(function openVideoClicked() {
                return chooseFile("video");
            }));
            byId("browser-open-folder").addEventListener("click", action(chooseFolder));
            byId("browser-image-openwith").addEventListener("click", function imageManualClicked() {
                openManualDialog("image");
            });
            byId("browser-video-openwith").addEventListener("click", function videoManualClicked() {
                openManualDialog("video");
            });
            byId("browser-openwith-close").addEventListener("click", closeManualDialog);
            byId("browser-openwith-cancel").addEventListener("click", closeManualDialog);
            byId("browser-openwith-wizard").addEventListener("click", action(configureSelected));
            byId("browser-openwith-resume").addEventListener("click", action(refreshAssociation));
            // 从 Windows 设置返回时重新查询，不将打开设置页当作关联成功。
            window.addEventListener("focus", function refreshOnFocus() {
                if (state.association)
                    refreshAssociation().catch(report);
            });
            byId("browser-openwith-all").addEventListener("click", function selectAll() {
                document.querySelectorAll("#browser-openwith-grid input").forEach(function check(input) { input.checked = true; });
            });
            byId("browser-openwith-none").addEventListener("click", function selectNone() {
                document.querySelectorAll("#browser-openwith-grid input").forEach(function uncheck(input) { input.checked = false; });
            });
        }
        const association = refreshAssociation();
        const formats = association.then(function fillFormats(data) {
            window.desktopI18n.text(byId("browser-image-formats"), window.desktopI18n.live(() => extensionList(data.imageExtensions)));
            window.desktopI18n.text(byId("browser-video-formats"), window.desktopI18n.live(() => extensionList(data.videoExtensions)));
        });
        await Promise.all([association, formats]);
    }
    // 对主页面仅暴露初始化入口。
    window.desktopBrowser = Object.freeze({ initialize });
})();
