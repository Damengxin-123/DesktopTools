/* 录屏页：触发原生录屏流程，管理与录制结果对应的记录。 */
(function installRecordingPage() {
    "use strict";
    const state = {
        items: [], // 后端历史摘要。
        revision: 0, // 列表刷新序号，丢弃过期响应。
        timer: null, // 后端通知合并定时器。
        audioSaving: false, // 音频选项保存中，避免保存期间的信号刷新覆盖输入。
        initialized: false,
        options: null // 主页面提供的提示函数。
    };
    function byId(id) { return document.getElementById(id); }
    // 以纯文本创建节点，路径与时间不作为 HTML 执行。
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
            }
        };
    }
    // 时间显示为本地完整日期时间。
    function readableTime(value) {
        const date = new Date(value);
        if (Number.isNaN(date.getTime()))
            return "";
        return date.toLocaleString("zh-CN", { year: "numeric", month: "2-digit", day: "2-digit",
            hour: "2-digit", minute: "2-digit", second: "2-digit", hour12: false });
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
    // 有效录制时长显示为 h:mm:ss 或 m:ss。
    function readableDuration(milliseconds) {
        const total = Math.max(0, Math.round(milliseconds / 1000));
        const hours = Math.floor(total / 3600);
        const minutes = Math.floor((total % 3600) / 60);
        const seconds = total % 60;
        if (hours > 0)
            return hours + ":" + String(minutes).padStart(2, "0") + ":" + String(seconds).padStart(2, "0");
        return minutes + ":" + String(seconds).padStart(2, "0");
    }
    function updateState() {
        window.desktopI18n.text(byId("recording-count"), window.desktopI18n.live(() => state.items.length));
        window.desktopI18n.text(byId("recording-summary"), window.desktopI18n.live(() => state.items.length + window.desktopI18n.t(" 段")));
        byId("recording-clear").disabled = !state.items.length;
    }
    function renderList() {
        updateState();
        const target = byId("recording-list");
        target.replaceChildren();
        if (!state.items.length) {
            byId("recording-empty").hidden = false;
            return;
        }
        byId("recording-empty").hidden = true;
        for (const item of state.items) {
            const row = node("article", "recording-row");
            const thumbWrap = node("div", "recording-thumb");
            if (item.thumbnail) {
                const thumb = node("img");
                thumb.src = item.thumbnail;
                window.desktopI18n.attr(thumb, "alt", window.desktopI18n.live(() => window.desktopI18n.t("录屏首帧缩略图")));
                thumb.loading = "lazy";
                thumbWrap.append(thumb);
            }
            else {
                thumbWrap.append(node("span", "recording-thumb-fallback", window.desktopI18n.live(() => "▶")));
            }
            const content = node("div", "recording-row-content");
            content.append(node("p", "recording-row-title", window.desktopI18n.live(() => window.desktopI18n.t("录制于 ") + readableTime(item.recordedAt))));
            const meta = node("div", "recording-row-meta");
            meta.append(node("span", "", window.desktopI18n.live(() => window.desktopI18n.t("时长 ") + readableDuration(item.durationMs))), node("span", "", window.desktopI18n.live(() => item.width + " × " + item.height + " px")), node("span", "", window.desktopI18n.live(() => readableSize(item.bytes))));
            content.append(meta);
            const actions = node("div", "recording-row-actions");
            // 播放走资源浏览器的视频窗口，可调节进度、倍速与音量。
            const play = node("button", "button primary", window.desktopI18n.live(() => window.desktopI18n.t("播放")));
            play.type = "button";
            play.addEventListener("click", action(async function playClicked() {
                await window.desktopBridge.call("openRecordingPlayer", item.id);
            }));
            const copy = node("button", "button secondary", window.desktopI18n.live(() => window.desktopI18n.t("复制文件")));
            copy.type = "button";
            copy.addEventListener("click", action(async function copyClicked() {
                await window.desktopBridge.call("copyRecording", item.id);
                state.options.toast(window.desktopI18n.live(() => window.desktopI18n.t("视频文件已复制到剪贴板")));
            }));
            const open = node("button", "button secondary", window.desktopI18n.live(() => window.desktopI18n.t("打开位置")));
            open.type = "button";
            open.addEventListener("click", action(async function openClicked() {
                await window.desktopBridge.call("openRecordingDirectory", item.id);
            }));
            // 删除前弹窗确认；确认后后端会连同原始视频文件一起删除。
            const remove = node("button", "button danger quiet", window.desktopI18n.live(() => window.desktopI18n.t("删除")));
            remove.type = "button";
            remove.addEventListener("click", function removeClicked() {
                if (!window.confirm(window.desktopI18n.t("确定删除这段录屏记录吗？\n对应的视频文件将从磁盘一并删除，此操作无法撤销。")))
                    return;
                window.desktopBridge.call("deleteRecordings", [item.id])
                    .then(function removed() { state.options.toast(window.desktopI18n.live(() => window.desktopI18n.t("已删除录屏记录"))); })
                    .catch(report);
            });
            actions.append(play, copy, open, remove);
            row.append(thumbWrap, content, actions);
            target.append(row);
        }
    }
    async function refresh() {
        const revision = ++state.revision;
        try {
            const data = await window.desktopBridge.call("getRecordingHistory");
            if (revision !== state.revision)
                return;
            state.items = data.items;
            byId("recording-error").hidden = true;
        }
        catch (error) {
            if (revision !== state.revision)
                return;
            window.desktopI18n.text(byId("recording-error"), window.desktopI18n.live(() => error.message));
            byId("recording-error").hidden = false;
        }
        renderList();
    }
    function scheduleRefresh() {
        clearTimeout(state.timer);
        state.timer = setTimeout(function runScheduledRefresh() { refresh().catch(report); }, 100);
    }
    // 读取音频选项并同步开关显示；关闭总开关时禁用来源勾选。
    async function refreshAudio() {
        if (state.audioSaving)
            return;
        const settings = await window.desktopBridge.call("getSettings");
        const enabled = settings.recordingAudioEnabled === true;
        byId("recording-audio-enabled").checked = enabled;
        byId("recording-audio-microphone").checked = settings.recordingAudioMicrophone !== false;
        byId("recording-audio-system").checked = settings.recordingAudioSystem !== false;
        updateAudioState();
    }
    function updateAudioState() {
        const enabled = byId("recording-audio-enabled").checked;
        byId("recording-audio-microphone").disabled = !enabled;
        byId("recording-audio-system").disabled = !enabled;
        window.desktopI18n.text(byId("recording-audio-state"), window.desktopI18n.live(() => enabled ? "" : window.desktopI18n.t("已关闭，仅录制画面")));
    }
    // 勾选即生效：保存到设置；失败时回读后端值并提示。
    function installAudioEvents() {
        const keys = [
            ["recording-audio-enabled", "recordingAudioEnabled"],
            ["recording-audio-microphone", "recordingAudioMicrophone"],
            ["recording-audio-system", "recordingAudioSystem"]
        ];
        for (const [elementId, settingKey] of keys) {
            byId(elementId).addEventListener("change", async function audioChanged(event) {
                const source = event.currentTarget;
                source.disabled = true;
                state.audioSaving = true;
                try {
                    await window.desktopBridge.call("saveSettings", { [settingKey]: source.checked });
                    state.options.toast(window.desktopI18n.live(() => window.desktopI18n.t("录屏音频设置已保存")));
                }
                catch (error) {
                    report(error);
                }
                finally {
                    state.audioSaving = false;
                    source.disabled = false;
                    try {
                        await refreshAudio();
                    }
                    catch (refreshError) {
                        report(refreshError);
                    }
                }
            });
        }
    }
    async function initialize(options) {
        state.options = options;
        if (!state.initialized) {
            state.initialized = true;
            byId("recording-start").addEventListener("click", action(async function startClicked() {
                await window.desktopBridge.call("startRecording");
            }));
            byId("recording-clear").addEventListener("click", function clearClicked() {
                if (!state.items.length)
                    return;
                if (!window.confirm(window.desktopI18n.t("确定清空全部录屏记录吗？\n对应的视频文件将从磁盘删除，此操作无法撤销。")))
                    return;
                window.desktopBridge.call("clearRecordingHistory")
                    .then(function cleared() { state.options.toast(window.desktopI18n.live(() => window.desktopI18n.t("已清空录屏记录"))); })
                    .catch(report);
            });
            installAudioEvents();
            await window.desktopBridge.on("recordingsChanged", scheduleRefresh);
        }
        await Promise.all([refresh(), refreshAudio()]);
    }
    // 对主页面仅暴露初始化入口。
    window.desktopRecording = Object.freeze({ initialize });
})();
