/* 动态壁纸页：桌面挂载与播放由原生服务管理，网页负责开关、资源选择和历史记录。 */
(function installWallpaperPage() {
    "use strict";
    // 页面状态，监听刷新不覆盖正在提交的对话框。
    const state = {
        items: [], // 后端返回的全部历史记录。
        enabled: false, // 动态壁纸功能开关。
        activeId: "", // 当前使用记录的标识。
        screens: [], // 按实际物理矩形排列的已连接显示器。
        selectedScreenId: "", // 当前编辑的显示器，刷新时保留用户选择。
        engineError: "", // 原生壁纸挂载失败说明。
        modeBusy: false, // 显示方式保存期间禁用重复提交。
        revision: 0, // 查询序号，忽略过期响应。
        timer: null, // 后端通知合并定时器。
        initialized: false, // 控件和信号只绑定一次。
        available: false, // 历史读取是否正常。
        options: null, // 主页面提供的提示函数。
        confirm: null, // 当前对话框提交操作。
        dialogBusy: false, // 对话框提交期间阻止重复操作。
        dialogRevision: 0 // 防止过期响应覆盖新对话框。
    };
    // 获取固定页面控件。
    function byId(id) { return document.getElementById(id); }
    // 以纯文本创建节点，壁纸数据绝不作为 HTML 执行。
    function node(tag, className, text) {
        const result = document.createElement(tag);
        if (className)
            result.className = className;
        if (text !== undefined)
            window.desktopI18n.text(result, window.desktopI18n.live(() => text));
        return result;
    }
    // 统一处理按钮的异步错误和重复点击。
    function action(label, handler, className = "button quiet small") {
        const result = node("button", className, window.desktopI18n.live(() => label));
        result.type = "button";
        result.addEventListener("click", async function clicked() {
            result.disabled = true;
            try {
                await handler();
            }
            catch (error) {
                state.options.reportError(error);
            }
            finally {
                result.disabled = false;
            }
        });
        return result;
    }
    // 将时间显示为本机中文日期，空值显示占位线。
    function date(value) { return value ? new Date(value).toLocaleDateString("zh-CN") : "—"; }
    // 媒体类型的中文标签。
    function typeLabel(item) { return item.type === "video" ? window.desktopI18n.t("视频") : (item.animated ? window.desktopI18n.t("动图") : window.desktopI18n.t("图片")); }
    // 当前状态行的说明文字。
    function statusText() {
        if (!state.available)
            return window.desktopI18n.t("历史记录读取失败");
        if (!selectedScreen())
            return window.desktopI18n.t("未检测到可用显示器");
        if (selectedScreen().playbackError)
            return selectedScreen().playbackError;
        if (state.engineError)
            return state.engineError;
        const active = state.items.find(function isCurrent(item) { return item.id === state.activeId; });
        if (state.enabled && active)
            return (active.exists ? (selectedScreen().running ? window.desktopI18n.t("正在显示：") : window.desktopI18n.t("已选择，等待显示：")) + active.name : window.desktopI18n.t("当前资源已丢失：") + active.name);
        if (state.enabled)
            return window.desktopI18n.t("此屏使用系统壁纸，可为它单独选择资源");
        if (active)
            return window.desktopI18n.t("未开启，已保存的选择：") + active.name;
        return window.desktopI18n.t("未开启，选择本地资源后即可显示");
    }
    // 创建历史卡片，显示预览、类型、路径和操作。
    function card(item) {
        const result = node("article", "emoji-card wallpaper-card" + (item.id === state.activeId ? " current" : ""));
        result.dataset.id = item.id;
        let thumb;
        if (item.type === "video") {
            thumb = node("div", "wallpaper-thumb");
            thumb.setAttribute("aria-hidden", "true");
            window.desktopI18n.text(thumb, window.desktopI18n.live(() => "▶"));
            window.desktopI18n.attr(thumb, "title", window.desktopI18n.live(() => window.desktopI18n.t("视频不生成缩略图")));
        }
        else {
            thumb = node("img", "emoji-thumbnail");
            thumb.src = item.thumbnail;
            window.desktopI18n.attr(thumb, "alt", window.desktopI18n.live(() => window.desktopI18n.t("壁纸预览 ") + item.name));
            thumb.loading = "lazy";
        }
        const name = node("p", "emoji-name", window.desktopI18n.live(() => item.name));
        window.desktopI18n.attr(name, "title", window.desktopI18n.live(() => item.name));
        const path = node("p", "emoji-path", window.desktopI18n.live(() => item.path));
        window.desktopI18n.attr(path, "title", window.desktopI18n.live(() => item.path));
        const badges = node("div", "wallpaper-badges");
        badges.append(node("span", "wallpaper-badge", window.desktopI18n.live(() => typeLabel(item))));
        if (item.id === state.activeId)
            badges.append(node("span", "wallpaper-badge current", window.desktopI18n.live(() => window.desktopI18n.t("此屏已选"))));
        const assigned = state.screens.filter(function usesItem(screen) { return screen.activeId === item.id; });
        if (assigned.length)
            badges.append(node("span", "wallpaper-badge", window.desktopI18n.live(() => window.desktopI18n.t("屏幕 ") + assigned.map(function screenNumber(screen) { return screen.number; }).join("、"))));
        if (!item.exists)
            badges.append(node("span", "wallpaper-badge missing", window.desktopI18n.live(() => window.desktopI18n.t("资源丢失"))));
        const footer = node("div", "emoji-card-meta");
        footer.append(node("span", "", window.desktopI18n.live(() => window.desktopI18n.t("添加 ") + date(item.addedAt))), node("span", "muted", window.desktopI18n.live(() => window.desktopI18n.t("使用 ") + date(item.lastUsedAt))));
        const actions = node("div", "emoji-card-actions");
        const useButton = action(window.desktopI18n.live(() => window.desktopI18n.t("应用到此屏")), function useWallpaperClick() { return useItem(item); }, "button secondary small");
        useButton.disabled = !selectedScreen();
        actions.append(useButton);
        actions.append(action(window.desktopI18n.live(() => window.desktopI18n.t("打开位置")), async function openWallpaperLocation() {
            await window.desktopBridge.call("openWallpaperDirectory", item.id);
        }, "button secondary small"));
        actions.append(action(window.desktopI18n.live(() => window.desktopI18n.t("删除")), function deleteWallpaperClick() { confirmRemove([item.id]); }, "button danger quiet small"));
        result.append(thumb, name, path, badges, footer, actions);
        return result;
    }
    // 绘制历史列表和状态行。
    function render() {
        state.activeId = selectedScreen()?.activeId || "";
        renderScreens();
        window.desktopI18n.text(byId("wallpaper-count"), window.desktopI18n.live(() => state.items.length));
        byId("wallpaper-list").replaceChildren(...state.items.map(card));
        window.desktopI18n.text(byId("wallpaper-status"), window.desktopI18n.live(() => statusText()));
        byId("wallpaper-enabled").checked = state.enabled;
        byId("wallpaper-empty").hidden = state.items.length > 0 || !state.available;
        byId("add-wallpaper").disabled = !selectedScreen() || !state.available;
        byId("add-wallpaper-clipboard").disabled = !selectedScreen() || !state.available;
    }
    // 获取当前正在编辑的屏幕，断开时不会隐式返回其他屏幕。
    function selectedScreen() {
        return state.screens.find(function matchesScreen(screen) { return screen.id === state.selectedScreenId; });
    }
    // 同步当前屏幕的互斥勾选项，说明裁切、留边和原尺寸规则。
    function renderDisplayModes() {
        const selected = selectedScreen();
        const active = state.items.find(function currentItem(item) { return item.id === selected?.activeId; });
        const isVideo = active?.type === "video";
        byId("wallpaper-display-modes").disabled = !state.available || !selected || state.modeBusy || isVideo;
        const mode = selected?.displayMode || "fill";
        for (const input of byId("wallpaper-display-modes").querySelectorAll("input"))
            input.checked = input.value === mode;
        // 每种方式给出直观说明；视频不冒充具有静态图片预览。
        const descriptions = {
            get fill() { return window.desktopI18n.t("等比放大铺满屏幕，超出部分裁切。"); }, // 填充模式提示。
            get fit() { return window.desktopI18n.t("完整显示图片并保持比例，空余区域留黑。"); }, // 适应模式提示。
            get stretch() { return window.desktopI18n.t("拉伸到屏幕大小，可能改变图片比例。"); }, // 拉伸模式提示。
            get tile() { return window.desktopI18n.t("按原图像素大小，从左上角重复铺满。"); }, // 平铺模式提示。
            get center() { return window.desktopI18n.t("按原图像素大小居中，空余区域留黑，超出部分裁切。"); } // 居中模式提示。
        };
        window.desktopI18n.text(byId("wallpaper-mode-hint"), window.desktopI18n.live(() => isVideo
            ? window.desktopI18n.t("当前是视频壁纸，按填充播放；以上选项用于图片和动图。") : (descriptions[mode] || descriptions.fill) + window.desktopI18n.t(" 仅设置当前屏幕，示意图使用静态预览。")));
    }
    // 先预览再原子保存，失败后重新读取已保存的状态，避免界面与桌面不同步。
    async function changeDisplayMode(mode) {
        const selected = selectedScreen();
        if (!selected || state.modeBusy)
            return;
        const screenId = selected.id;
        selected.displayMode = mode;
        state.modeBusy = true;
        ++state.revision;
        renderScreens();
        try {
            await window.desktopBridge.call("setWallpaperDisplayMode", screenId, mode);
        }
        catch (error) {
            state.options.reportError(error);
        }
        finally {
            state.modeBusy = false;
            await refresh();
        }
    }
    // 以同一比例投影整个物理桌面，保留负坐标、竖屏和不齐平的排列。
    function renderScreens() {
        renderDisplayModes();
        const container = byId("wallpaper-displays");
        container.replaceChildren();
        window.desktopI18n.text(byId("wallpaper-screen-count"), window.desktopI18n.live(() => window.desktopI18n.t("已连接 ") + state.screens.length + window.desktopI18n.t(" 个屏幕")));
        byId("clear-screen-wallpaper").disabled = !state.activeId || !state.available;
        const selected = selectedScreen();
        window.desktopI18n.text(byId("wallpaper-screen-detail"), window.desktopI18n.live(() => selected
            ? window.desktopI18n.t("屏幕 ") + selected.number + (selected.primary ? window.desktopI18n.t(" \u00B7 主显示器") : "") + " · " + selected.width + " × " + selected.height
            : window.desktopI18n.t("暂无显示器")));
        if (!state.screens.length || !container.clientWidth)
            return;
        const left = Math.min(...state.screens.map(function leftEdge(screen) { return screen.x; }));
        const top = Math.min(...state.screens.map(function topEdge(screen) { return screen.y; }));
        const right = Math.max(...state.screens.map(function rightEdge(screen) { return screen.x + screen.width; }));
        const bottom = Math.max(...state.screens.map(function bottomEdge(screen) { return screen.y + screen.height; }));
        const scale = Math.min((container.clientWidth - 32) / (right - left), (container.clientHeight - 32) / (bottom - top));
        const offsetX = (container.clientWidth - (right - left) * scale) / 2;
        const offsetY = (container.clientHeight - (bottom - top) * scale) / 2;
        for (const screen of state.screens) {
            const button = node("button", "wallpaper-monitor");
            button.type = "button";
            button.dataset.screenId = screen.id;
            button.setAttribute("aria-pressed", String(screen.id === state.selectedScreenId));
            window.desktopI18n.attr(button, "aria-label", window.desktopI18n.live(() => window.desktopI18n.t("屏幕 ") + screen.number + (screen.primary ? window.desktopI18n.t("，主显示器") : "") + "，" + screen.width + " × " + screen.height));
            window.desktopI18n.attr(button, "title", window.desktopI18n.live(() => button.getAttribute("aria-label")));
            button.style.left = offsetX + (screen.x - left) * scale + "px";
            button.style.top = offsetY + (screen.y - top) * scale + "px";
            button.style.width = screen.width * scale + "px";
            button.style.height = screen.height * scale + "px";
            const item = state.items.find(function previewItem(item) { return item.id === screen.activeId; });
            if (item?.type === "image" && item.thumbnail) {
                const preview = node("span", "wallpaper-monitor-preview");
                preview.setAttribute("aria-hidden", "true");
                // 使用原图尺寸计算平铺周期，缩略图只提供颜色内容，动图在此处保持静止。
                window.desktopWallpaperLayout.apply(preview, item.thumbnail, screen.displayMode || "fill", item.width, item.height, screen.width, screen.height, Math.max(0, screen.width * scale - 6), Math.max(0, screen.height * scale - 6));
                button.classList.add("has-preview");
                button.append(preview);
                button.title += " · " + item.name + window.desktopI18n.t("（静态预览）");
            }
            button.append(node("span", "wallpaper-monitor-number", window.desktopI18n.live(() => screen.number)));
            // 切换的只是编辑目标，只有选择资源后才改变壁纸。
            button.addEventListener("click", function selectMonitor() { state.selectedScreenId = screen.id; render(); });
            container.append(button);
        }
    }
    // 查询后端快照，错误仅显示在壁纸页。
    async function refresh() {
        const revision = ++state.revision;
        try {
            const data = await window.desktopBridge.call("getWallpaper");
            if (revision !== state.revision)
                return;
            state.items = data.items;
            state.enabled = data.enabled === true;
            state.activeId = data.activeId || "";
            state.screens = data.screens || [];
            state.engineError = data.engineError || "";
            if (!selectedScreen())
                state.selectedScreenId = (state.screens.find(function primary(screen) { return screen.primary; }) || state.screens[0])?.id || "";
            state.available = true;
            window.desktopI18n.text(byId("wallpaper-error"), window.desktopI18n.live(() => ""));
            byId("wallpaper-error").hidden = true;
        }
        catch (error) {
            if (revision !== state.revision)
                return;
            state.available = false;
            window.desktopI18n.text(byId("wallpaper-error"), window.desktopI18n.live(() => error.message));
            byId("wallpaper-error").hidden = false;
        }
        render();
    }
    // 合并紧邻的后端通知，降低刷新频率。
    function scheduleRefresh() {
        clearTimeout(state.timer);
        state.timer = setTimeout(refresh, 100);
    }
    // 准备对话框，返回本次打开的序号。
    function openDialog(title, submitText, danger = false) {
        state.dialogRevision += 1;
        state.confirm = null;
        window.desktopI18n.text(byId("wallpaper-dialog-title"), window.desktopI18n.live(() => title));
        byId("wallpaper-dialog-body").replaceChildren();
        byId("wallpaper-dialog-error").hidden = true;
        window.desktopI18n.text(byId("wallpaper-dialog-confirm"), window.desktopI18n.live(() => submitText));
        byId("wallpaper-dialog-confirm").className = "button primary" + (danger ? " danger" : "");
        byId("wallpaper-dialog").showModal();
        return state.dialogRevision;
    }
    // 关闭对话框时使未完成操作失效。
    function closeDialog() {
        if (state.dialogBusy)
            return;
        state.dialogRevision += 1;
        byId("wallpaper-dialog").close();
    }
    // 切换到历史中的指定资源；源文件丢失时弹窗说明而不是短暂提示。
    async function useItem(item) {
        try {
            const screenId = state.selectedScreenId;
            if (!screenId)
                throw new Error(window.desktopI18n.t("请先选择显示器"));
            await window.desktopBridge.call("useWallpaperOnScreen", item.id, screenId);
            await refresh();
            state.options.toast(window.desktopI18n.live(() => state.enabled ? window.desktopI18n.t("已切换桌面壁纸") : window.desktopI18n.t("已设为当前壁纸；开启动态壁纸后显示")));
        }
        catch (error) {
            await refresh();
            if (!String(error.message || "").includes(window.desktopI18n.t("丢失")))
                throw error;
            const revision = openDialog(window.desktopI18n.live(() => window.desktopI18n.t("资源已丢失")), window.desktopI18n.live(() => window.desktopI18n.t("知道了")), true);
            byId("wallpaper-dialog-body").append(node("p", "dialog-description", window.desktopI18n.live(() => error.message + window.desktopI18n.t("\n可以恢复该文件后重试，或删除这条记录后重新选择资源。"))));
            if (revision !== state.dialogRevision)
                return;
        }
    }
    // 把资源路径加入历史并设为当前壁纸，按开关状态提示。
    async function addWallpaperPath(path, screenId) {
        await window.desktopBridge.call("addWallpaper", { path: path || "", screenId });
        await refresh();
        state.options.toast(window.desktopI18n.live(() => state.enabled ? window.desktopI18n.t("壁纸已更新") : window.desktopI18n.t("已加入历史记录；开启动态壁纸后显示")));
    }
    // 选择本地媒体文件，添加历史并设为当前壁纸。
    async function addWallpaper() {
        const screenId = state.selectedScreenId;
        const result = await window.desktopBridge.call("chooseWallpaperResource");
        if (!result || result.cancelled)
            return;
        await addWallpaperPath(result.target || "", screenId);
    }
    // 从剪贴板添加：复制的文件直接引用，截屏等图像内容先保存到数据目录。
    async function addWallpaperFromClipboard() {
        const screenId = state.selectedScreenId;
        const result = await window.desktopBridge.call("pasteWallpaperResource");
        await addWallpaperPath(result.target || "", screenId);
    }
    // 删除确认固定本次选中的标识，原文件不会被删除。
    function confirmRemove(ids) {
        if (!ids.length)
            return;
        openDialog(window.desktopI18n.live(() => window.desktopI18n.t("删除壁纸记录")), window.desktopI18n.live(() => window.desktopI18n.t("确认删除")), true);
        byId("wallpaper-dialog-body").append(node("p", "dialog-description", window.desktopI18n.live(() => window.desktopI18n.t("确定删除这 ") + ids.length + window.desktopI18n.t(" 条壁纸记录吗？\n只移除历史记录，不会删除原文件。"))));
        state.confirm = async function removeConfirmedWallpapers() {
            await window.desktopBridge.call("removeWallpapers", ids);
            await refresh();
        };
    }
    // 提交对话框操作，失败保留对话框并显示原因。
    async function submitDialog() {
        if (state.dialogBusy || !state.confirm)
            return;
        state.dialogBusy = true;
        byId("wallpaper-dialog-confirm").disabled = true;
        try {
            await state.confirm();
            state.dialogBusy = false;
            closeDialog();
        }
        catch (error) {
            window.desktopI18n.text(byId("wallpaper-dialog-error"), window.desktopI18n.live(() => error.message));
            byId("wallpaper-dialog-error").hidden = false;
        }
        finally {
            state.dialogBusy = false;
            byId("wallpaper-dialog-confirm").disabled = false;
        }
    }
    // 切换开关；失败时恢复勾选状态。
    async function toggleEnabled(checkbox) {
        checkbox.disabled = true;
        try {
            await window.desktopBridge.call("setWallpaperEnabled", checkbox.checked);
            await refresh();
            state.options.toast(window.desktopI18n.live(() => checkbox.checked ? window.desktopI18n.t("动态壁纸已开启") : window.desktopI18n.t("动态壁纸已关闭")));
        }
        catch (error) {
            checkbox.checked = !checkbox.checked;
            throw error;
        }
        finally {
            checkbox.disabled = false;
        }
    }
    // 绑定固定控件和原生通知，初始化失败不影响其他功能。
    async function initialize(options) {
        state.options = options;
        if (!state.initialized) {
            state.initialized = true;
            byId("wallpaper-display-modes").addEventListener("change", function modeChanged(event) {
                if (event.target.matches("input[name=wallpaper-mode]") && event.target.checked)
                    changeDisplayMode(event.target.value).catch(state.options.reportError);
            });
            // 页面首次从隐藏切换为可见及窗口缩放时，重新计算示意图尺寸。
            new ResizeObserver(renderScreens).observe(byId("wallpaper-displays"));
            byId("clear-screen-wallpaper").addEventListener("click", async function clearScreen() {
                try {
                    await window.desktopBridge.call("clearScreenWallpaper", state.selectedScreenId);
                    await refresh();
                    state.options.toast(window.desktopI18n.live(() => window.desktopI18n.t("此屏已恢复系统壁纸")));
                }
                catch (error) {
                    state.options.reportError(error);
                }
            });
            byId("add-wallpaper").addEventListener("click", function addClicked() { addWallpaper().catch(state.options.reportError); });
            byId("add-wallpaper-clipboard").addEventListener("click", function addFromClipboardClicked() { addWallpaperFromClipboard().catch(state.options.reportError); });
            byId("wallpaper-enabled").addEventListener("change", function enabledChanged(event) { toggleEnabled(event.target).catch(state.options.reportError); });
            byId("wallpaper-dialog-close").addEventListener("click", closeDialog);
            byId("wallpaper-dialog-cancel").addEventListener("click", closeDialog);
            byId("wallpaper-dialog-form").addEventListener("submit", function dialogSubmitted(event) { event.preventDefault(); submitDialog(); });
            byId("wallpaper-dialog").addEventListener("cancel", function escapeDialog(event) { event.preventDefault(); closeDialog(); });
            await window.desktopBridge.on("wallpaperChanged", scheduleRefresh);
        }
        await refresh();
    }
    // 对主页面仅暴露初始化入口。
    window.desktopWallpaper = Object.freeze({ initialize });
})();
