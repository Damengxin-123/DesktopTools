/* 文件检索页：全盘文件名即时检索，呈现索引状态并提供打开、定位与复制操作。 */
(function installFileSearchPage() {
    "use strict";
    const state = {
        initialized: false,
        options: null, // 主页面提供的提示函数。
        status: null, // 后端索引状态快照。
        requestId: 0, // 最近一次检索编号，过期结果直接丢弃。
        searchPath: false, // 是否匹配完整路径。
        rows: [], // 已加载的结果行。
        total: 0, // 全部命中数。
        collected: 0, // 实际参与排序的命中数。
        elapsedMs: 0, // 最近一次检索耗时。
        searching: false,
        pendingTimer: null // 输入防抖。
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
    // 文件大小显示为 B / KB / MB / GB。
    function readableSize(bytes) {
        if (!Number.isFinite(Number(bytes)) || Number(bytes) < 0)
            return "";
        const value = Number(bytes);
        if (value < 1024)
            return value + " B";
        if (value < 1024 * 1024)
            return (value / 1024).toFixed(0) + " KB";
        if (value < 1024 * 1024 * 1024)
            return (value / (1024 * 1024)).toFixed(1) + " MB";
        return (value / (1024 * 1024 * 1024)).toFixed(2) + " GB";
    }
    // 修改时间显示为短日期；索引未提供时留空。
    function readableDate(mtimeMs) {
        if (!Number(mtimeMs))
            return "";
        const date = new Date(Number(mtimeMs));
        if (Number.isNaN(date.getTime()))
            return "";
        return date.toLocaleDateString("zh-CN", { year: "numeric", month: "2-digit", day: "2-digit" });
    }
    // 侧栏角标：大数字改写为“万”，避免撑破导航宽度。
    function compactCount(value) {
        const count = Number(value) || 0;
        return count >= 100000 ? Math.round(count / 10000) + window.desktopI18n.t("万") : String(count);
    }
    // 索引状态行：阶段、模式与文件计数；构建中持续刷新进度。
    function renderStatus() {
        const status = state.status || {};
        const target = byId("filesearch-status");
        const rebuild = byId("filesearch-rebuild");
        const enable = byId("filesearch-enable-service");
        if (enable)
            enable.hidden = !status.serviceRequired;
        const modeText = status.mode === "mft" ? window.desktopI18n.t("NTFS 高速模式") : (status.mode === "scan" ? window.desktopI18n.t("目录扫描模式") : "");
        if (status.serviceRequired) {
            window.desktopI18n.text(target, window.desktopI18n.live(() => window.desktopI18n.t("启用高速索引服务后，通过 NTFS 日志更新文件列表。首次启用需 Windows 管理员授权。")));
            rebuild.disabled = true;
        }
        else if (status.phase === "disabled") {
            window.desktopI18n.text(target, window.desktopI18n.live(() => window.desktopI18n.t("文件检索未启用：请从桌面程序正常启动后使用。")));
            rebuild.disabled = true;
        }
        else if (status.phase === "building") {
            window.desktopI18n.text(target, window.desktopI18n.live(() => window.desktopI18n.t("正在建立索引（") + (modeText || window.desktopI18n.t("准备中")) + window.desktopI18n.t("）\u00B7 已索引 ")
                + (Number(status.total) || 0).toLocaleString() + window.desktopI18n.t(" 项，期间即可试用检索。")));
            rebuild.disabled = true;
        }
        else if (status.phase === "ready") {
            window.desktopI18n.text(target, window.desktopI18n.live(() => window.desktopI18n.t("索引就绪 \u00B7 ") + modeText + window.desktopI18n.t(" \u00B7 共 ")
                + (Number(status.fileCount) || 0).toLocaleString() + window.desktopI18n.t(" 个文件、")
                + (Number(status.directoryCount) || 0).toLocaleString() + window.desktopI18n.t(" 个文件夹")));
            rebuild.disabled = false;
        }
        else if (status.phase === "error") {
            window.desktopI18n.text(target, window.desktopI18n.live(() => window.desktopI18n.t("索引不可用：") + (status.lastError || window.desktopI18n.t("未知原因，可尝试重建索引。"))));
            rebuild.disabled = false;
        }
        else {
            window.desktopI18n.text(target, window.desktopI18n.live(() => window.desktopI18n.t("正在读取索引状态\u2026")));
            rebuild.disabled = false;
        }
        if (status.lastError && (status.serviceRequired || status.phase !== "error"))
            target.textContent += " · " + status.lastError;
        window.desktopI18n.text(byId("filesearch-count"), window.desktopI18n.live(() => compactCount(status.fileCount)));
    }
    // 绘制结果行；append 为真时保留既有列表（加载更多）。
    function renderResults(append) {
        const target = byId("filesearch-results");
        if (!append)
            target.replaceChildren();
        const visibleRows = append ? state.rows.slice(target.childElementCount) : state.rows;
        for (const row of visibleRows) {
            const card = node("article", "filesearch-row");
            card.append(node("span", "filesearch-kind " + (row.isDir ? "dir" : "file"), window.desktopI18n.live(() => row.isDir ? "▤" : "▱")));
            const main = node("div", "filesearch-row-main");
            main.append(node("span", "filesearch-row-name", window.desktopI18n.live(() => row.name)));
            const path = node("span", "filesearch-row-path", window.desktopI18n.live(() => row.path));
            window.desktopI18n.attr(path, "title", window.desktopI18n.live(() => row.path));
            main.append(path);
            card.append(main);
            if (!row.isDir) {
                const meta = [readableSize(row.size), readableDate(row.mtimeMs)].filter(Boolean).join(" · ");
                if (meta)
                    card.append(node("span", "filesearch-row-meta", window.desktopI18n.live(() => meta)));
            }
            const actions = node("div", "filesearch-row-actions");
            const open = node("button", "button quiet", window.desktopI18n.live(() => window.desktopI18n.t("打开")));
            open.type = "button";
            window.desktopI18n.attr(open, "aria-label", window.desktopI18n.live(() => window.desktopI18n.t("打开\u201C") + row.name + "”"));
            open.addEventListener("click", function openResult(event) {
                event.stopPropagation();
                open.disabled = true;
                window.desktopBridge.call("openFilePath", row.path)
                    .catch(report)
                    .finally(function restoreOpen() { if (open.isConnected)
                    open.disabled = false; });
            });
            const locate = node("button", "button quiet", window.desktopI18n.live(() => window.desktopI18n.t("位置")));
            locate.type = "button";
            window.desktopI18n.attr(locate, "aria-label", window.desktopI18n.live(() => window.desktopI18n.t("在资源管理器中显示\u201C") + row.name + "”"));
            locate.addEventListener("click", function locateResult(event) {
                event.stopPropagation();
                locate.disabled = true;
                window.desktopBridge.call("openFileLocation", row.path)
                    .catch(report)
                    .finally(function restoreLocate() { if (locate.isConnected)
                    locate.disabled = false; });
            });
            const copy = node("button", "button quiet", window.desktopI18n.live(() => window.desktopI18n.t("复制路径")));
            copy.type = "button";
            window.desktopI18n.attr(copy, "aria-label", window.desktopI18n.live(() => window.desktopI18n.t("复制\u201C") + row.path + "”"));
            copy.addEventListener("click", function copyResult(event) {
                event.stopPropagation();
                copy.disabled = true;
                window.desktopBridge.call("copyText", row.path)
                    .then(function copied() { state.options.toast(window.desktopI18n.live(() => window.desktopI18n.t("已复制路径"))); })
                    .catch(report)
                    .finally(function restoreCopy() { if (copy.isConnected)
                    copy.disabled = false; });
            });
            actions.append(open, locate, copy);
            card.append(actions);
            // 双击整行同样打开。
            card.addEventListener("dblclick", function openByDoubleClicked() {
                window.desktopBridge.call("openFilePath", row.path).catch(report);
            });
            target.append(card);
        }
        const isEmpty = state.rows.length === 0;
        const emptyBox = byId("filesearch-empty");
        if (isEmpty && !state.searching) {
            // 无关键词时展示语法提示，有关键词时展示无结果说明。
            window.desktopI18n.text(emptyBox.querySelector("h2"), window.desktopI18n.live(() => state.query ? window.desktopI18n.t("没有匹配的文件") : window.desktopI18n.t("输入关键词开始检索")));
            window.desktopI18n.text(emptyBox.querySelector("p"), window.desktopI18n.live(() => state.query
                ? window.desktopI18n.t("试试更短的关键词，或取消勾选\u201C匹配完整路径\u201D。") : window.desktopI18n.t("空格分隔多个关键词（全部包含）；*.docx 通配整个名称；ext:pdf 限定扩展名；folder:/file: 只看文件夹/文件。")));
            emptyBox.hidden = false;
        }
        else {
            emptyBox.hidden = true;
        }
        const shownLimit = Math.min(state.total, state.collected);
        const more = byId("filesearch-more");
        more.hidden = isEmpty || state.rows.length >= shownLimit;
        const summary = byId("filesearch-summary");
        if (state.searching) {
            window.desktopI18n.text(summary, window.desktopI18n.live(() => window.desktopI18n.t("正在检索\u2026")));
        }
        else if (!state.query) {
            window.desktopI18n.text(summary, window.desktopI18n.live(() => ""));
        }
        else {
            let text = window.desktopI18n.t("共 ") + state.total.toLocaleString() + window.desktopI18n.t(" 个结果，耗时 ")
                + state.elapsedMs + window.desktopI18n.t(" 毫秒");
            if (state.collected < state.total)
                text += window.desktopI18n.t("，仅列出前 ") + state.collected.toLocaleString() + window.desktopI18n.t(" 个，可增加关键词缩小范围");
            window.desktopI18n.text(summary, window.desktopI18n.live(() => text));
        }
    }
    // 提交一次检索；后端通过 fileSearchResults 信号回传，这里记录最新编号。
    async function runSearch(offset) {
        const raw = byId("filesearch-input").value.trim();
        state.query = raw;
        if (!raw) {
            state.rows = [];
            state.total = 0;
            state.collected = 0;
            renderResults(false);
            return;
        }
        const requestId = ++state.requestId;
        state.searching = true;
        if (offset > 0) {
            byId("filesearch-more").disabled = true;
        }
        else {
            // 新检索先清空旧列表，等待信号回传最新结果。
            state.rows = [];
            state.total = 0;
            state.collected = 0;
            renderResults(false);
        }
        try {
            const ack = await window.desktopBridge.call("searchFiles", raw, offset, 200, state.searchPath);
            if (Number(ack && ack.requestId) !== requestId)
                return;
        }
        catch (error) {
            state.searching = false;
            byId("filesearch-more").disabled = false;
            report(error);
            return;
        }
        if (offset === 0)
            renderResults(false);
        window.desktopI18n.text(byId("filesearch-summary"), window.desktopI18n.live(() => window.desktopI18n.t("正在检索\u2026")));
    }
    // 信号回传：仅应用最新一次检索的结果，加载更多时追加。
    function applyResults(result) {
        if (!result || Number(result.requestId) !== state.requestId)
            return;
        const rows = Array.isArray(result.rows) ? result.rows : [];
        state.rows = Number(result.offset) > 0 ? state.rows.concat(rows) : rows;
        state.total = Number(result.total) || 0;
        state.collected = Number(result.collected) || 0;
        state.elapsedMs = Number(result.elapsedMs) || 0;
        state.searching = false;
        byId("filesearch-more").disabled = false;
        renderResults(Number(result.offset) > 0);
    }
    // 输入防抖后检索首页。
    function scheduleSearch() {
        window.clearTimeout(state.pendingTimer);
        state.pendingTimer = window.setTimeout(function executeScheduledSearch() {
            state.pendingTimer = null;
            runSearch(0).catch(report);
        }, 80);
    }
    // 读取索引状态快照。
    async function refreshStatus() {
        try {
            state.status = await window.desktopBridge.call("getFileSearchStatus");
        }
        catch (error) {
            return;
        }
        renderStatus();
    }
    async function rebuild() {
        await window.desktopBridge.call("rebuildFileSearchIndex");
        state.options.toast(window.desktopI18n.live(() => window.desktopI18n.t("已开始重建索引")));
    }
    async function initialize(options) {
        if (state.initialized)
            return;
        state.initialized = true;
        state.options = options;
        await window.desktopBridge.on("fileSearchStatusChanged", refreshStatus);
        await window.desktopBridge.on("fileSearchResults", applyResults);
        // 服务安装由用户明确触发，普通搜索不会弹出管理员授权。
        const enable = byId("filesearch-enable-service");
        enable.addEventListener("click", async function enableIndexService() {
            enable.disabled = true;
            try {
                await window.desktopBridge.call("enableFileSearchService");
                state.options.toast(window.desktopI18n.live(() => window.desktopI18n.t("正在启用高速索引服务，完成后自动建立索引。")));
            }
            catch (error) {
                report(error);
            }
            finally {
                enable.disabled = false;
            }
        });
        byId("filesearch-input").addEventListener("input", scheduleSearch);
        byId("filesearch-path").addEventListener("change", function searchPathChanged(event) {
            state.searchPath = event.target.checked;
            if (state.query)
                runSearch(0).catch(report);
        });
        byId("filesearch-input").addEventListener("keydown", function searchEnterPressed(event) {
            if (event.key === "Enter") {
                window.clearTimeout(state.pendingTimer);
                state.pendingTimer = null;
                runSearch(0).catch(report);
            }
        });
        byId("filesearch-rebuild").addEventListener("click", function rebuildClicked() {
            rebuild().catch(report);
        });
        byId("filesearch-more").addEventListener("click", function loadMoreClicked() {
            runSearch(state.rows.length).catch(report);
        });
        // 初始呈现语法提示空状态。
        state.query = "";
        renderResults(false);
        await refreshStatus();
    }
    window.desktopFileSearch = Object.freeze({ initialize });
})();
