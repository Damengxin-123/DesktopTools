/* 二维码页：打开本地或剪贴板图片并识别内容，历史记录由原生服务持久化。 */
(function installQrPage() {
    "use strict";
    const state = {
        items: [], // 后端返回的历史摘要。
        image: "", // 正在查看的图片数据地址。
        text: "", // 当前识别结果。
        currentId: "", // 与历史记录关联的标识；新加载的图片为空。
        source: "", // 当前图片来源说明。
        decoding: false,
        revision: 0, // 历史刷新序号，丢弃过期响应。
        timer: null, // 后端通知合并定时器。
        initialized: false,
        options: null // 主页面提供的提示函数。
    };
    function byId(id) { return document.getElementById(id); }
    // 以纯文本创建节点，识别内容绝不作为 HTML 执行。
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
                updateState();
            }
        };
    }
    // 把时间显示为简短本地日期。
    function readableDate(value) {
        if (!value)
            return "";
        const date = new Date(value);
        return Number.isNaN(date.getTime()) ? "" : date.toLocaleDateString("zh-CN", { month: "2-digit", day: "2-digit" });
    }
    // 根据当前状态刷新工具按钮与提示。
    function updateState() {
        byId("qr-decode").disabled = !state.image || state.decoding;
        byId("qr-clear-image").disabled = !state.image;
        byId("qr-copy").disabled = !state.text;
        window.desktopI18n.text(byId("qr-image-state"), window.desktopI18n.live(() => state.decoding
            ? window.desktopI18n.t("正在识别\u2026") : (state.image ? window.desktopI18n.t("已加载图片，点击\u201C识别\u201D解码") : "")));
    }
    // 显示新加载的图片；上一次的识别结果一并清空。
    function showImage(image, source) {
        state.image = image;
        state.source = source;
        state.text = "";
        state.currentId = "";
        byId("qr-image").src = image;
        byId("qr-image").hidden = false;
        byId("qr-image-empty").hidden = true;
        byId("qr-result").value = "";
        updateState();
        renderHistory();
    }
    function clearImage() {
        state.image = "";
        state.text = "";
        state.currentId = "";
        state.source = "";
        byId("qr-image").removeAttribute("src");
        byId("qr-image").hidden = true;
        byId("qr-image-empty").hidden = false;
        byId("qr-result").value = "";
        updateState();
        renderHistory();
    }
    // 把一条完整记录应用到查看区；识别结果与历史回看共用。
    function showRecord(record) {
        if (record.image) {
            state.image = record.image;
            byId("qr-image").src = record.image;
            byId("qr-image").hidden = false;
            byId("qr-image-empty").hidden = true;
        }
        state.text = record.text;
        state.currentId = record.id;
        state.source = record.source || "";
        byId("qr-result").value = record.text;
        updateState();
        renderHistory();
    }
    async function decodeCurrent() {
        if (!state.image || state.decoding)
            return;
        state.decoding = true;
        updateState();
        try {
            const record = await window.desktopBridge.call("decodeQr", state.image, state.source);
            showRecord(record);
            state.options.toast(window.desktopI18n.live(() => window.desktopI18n.t("识别成功")));
        }
        finally {
            state.decoding = false;
            updateState();
        }
    }
    async function openHistory(id) {
        const record = await window.desktopBridge.call("readQr", id);
        showRecord(record);
    }
    function renderHistory() {
        window.desktopI18n.text(byId("qr-count"), window.desktopI18n.live(() => state.items.length));
        window.desktopI18n.text(byId("qr-history-count"), window.desktopI18n.live(() => state.items.length + window.desktopI18n.t(" 条")));
        const target = byId("qr-history");
        target.replaceChildren();
        if (!state.items.length) {
            const empty = node("div", "empty-state");
            empty.append(node("div", "empty-icon", window.desktopI18n.live(() => "▩")), node("h2", "", window.desktopI18n.live(() => window.desktopI18n.t("还没有识别记录"))), node("p", "", window.desktopI18n.live(() => window.desktopI18n.t("识别成功的二维码会出现在这里，点击可回看。"))));
            target.append(empty);
            return;
        }
        for (const item of state.items) {
            const row = node("article", "qr-row" + (state.currentId === item.id ? " active" : ""));
            const thumb = node("img", "qr-thumb");
            thumb.src = item.thumbnail;
            window.desktopI18n.attr(thumb, "alt", window.desktopI18n.live(() => window.desktopI18n.t("二维码缩略图")));
            thumb.loading = "lazy";
            const content = node("div", "qr-row-content");
            const preview = node("button", "qr-row-preview", window.desktopI18n.live(() => item.preview));
            preview.type = "button";
            window.desktopI18n.attr(preview, "title", window.desktopI18n.live(() => window.desktopI18n.t("查看这条记录")));
            window.desktopI18n.attr(preview, "aria-label", window.desktopI18n.live(() => window.desktopI18n.t("查看识别记录：") + item.preview));
            content.append(preview);
            const meta = node("div", "qr-row-meta");
            meta.append(node("span", "qr-row-source", window.desktopI18n.live(() => item.source || "")), node("span", "", window.desktopI18n.live(() => readableDate(item.decodedAt))));
            content.append(meta);
            const remove = node("button", "icon-button", window.desktopI18n.live(() => "×"));
            remove.type = "button";
            window.desktopI18n.attr(remove, "aria-label", window.desktopI18n.live(() => window.desktopI18n.t("删除识别记录")));
            remove.addEventListener("click", action(async function removeClicked() {
                await window.desktopBridge.call("removeQrHistory", [item.id]);
                state.options.toast(window.desktopI18n.live(() => window.desktopI18n.t("已删除记录")));
            }));
            row.append(thumb, content, remove);
            preview.addEventListener("click", function previewClicked() { openHistory(item.id).catch(report); });
            row.addEventListener("click", function rowClicked(event) {
                if (event.target.closest("button"))
                    return;
                openHistory(item.id).catch(report);
            });
            target.append(row);
        }
    }
    async function refresh() {
        const revision = ++state.revision;
        try {
            const data = await window.desktopBridge.call("getQrHistory");
            if (revision !== state.revision)
                return;
            state.items = data.items;
            byId("qr-error").hidden = true;
        }
        catch (error) {
            if (revision !== state.revision)
                return;
            window.desktopI18n.text(byId("qr-error"), window.desktopI18n.live(() => error.message));
            byId("qr-error").hidden = false;
        }
        renderHistory();
    }
    function scheduleRefresh() {
        clearTimeout(state.timer);
        state.timer = setTimeout(function runScheduledRefresh() { refresh().catch(report); }, 100);
    }
    async function initialize(options) {
        state.options = options;
        if (!state.initialized) {
            state.initialized = true;
            byId("qr-open").addEventListener("click", action(async function openClicked() {
                const result = await window.desktopBridge.call("chooseQrImage");
                if (!result || result.cancelled || !result.image)
                    return;
                showImage(result.image, window.desktopI18n.t("本地文件"));
            }));
            byId("qr-paste").addEventListener("click", action(async function pasteClicked() {
                const result = await window.desktopBridge.call("pasteQrImage");
                if (!result || !result.image)
                    return;
                showImage(result.image, window.desktopI18n.t("剪贴板"));
                state.options.toast(window.desktopI18n.live(() => window.desktopI18n.t("已从剪贴板加载图片")));
            }));
            byId("qr-decode").addEventListener("click", action(decodeCurrent));
            byId("qr-copy").addEventListener("click", action(async function copyClicked() {
                await window.desktopBridge.call("copyText", state.text);
                state.options.toast(window.desktopI18n.live(() => window.desktopI18n.t("已复制识别结果")));
            }));
            byId("qr-clear-image").addEventListener("click", action(async function clearClicked() {
                clearImage();
            }));
            await window.desktopBridge.on("qrHistoryChanged", scheduleRefresh);
        }
        await refresh();
    }
    // 对主页面仅暴露初始化入口。
    window.desktopQr = Object.freeze({ initialize });
})();
