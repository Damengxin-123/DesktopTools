/* 书签窗口左侧面板：书签增删查改、网址检测、排序与右侧浏览请求。 */
(function installBookmarkPanel() {
    "use strict";
    const state = {
        items: [], // 后端返回的书签列表。
        filter: "", // 搜索关键字。
        selected: new Set(), // 已勾选的书签标识，便于批量删除。
        activeId: "", // 最近一次在右侧打开的书签。
        editingId: "", // 正在编辑的书签标识；空表示新建。
        dragId: "", // 正在拖动排序的书签标识。
        currentUrl: "", // 右侧浏览区当前网址，用于一键收藏。
        probePending: null, // 等待中的检测令牌与 Promise 解析函数。
        initialized: false, // 是否已安装事件。
        saving: false, // 保存期间阻止重复提交和关闭。
        probeVersion: 0, // 忽略旧地址或已关闭对话框的检测结果。
        refreshVersion: 0, // 丢弃晚到的旧列表快照。
        probeResults: new Map() // 缓存先于桥接回调到达的检测结果。
    };
    function byId(id) { return document.getElementById(id); }
    // 以纯文本创建节点，网址与名称都不会作为 HTML 执行。
    function node(tag, className, text) {
        const result = document.createElement(tag);
        if (className)
            result.className = className;
        if (text !== undefined)
            window.desktopI18n.text(result, window.desktopI18n.live(() => text));
        return result;
    }
    function showError(message) {
        const target = byId("bookmark-error");
        window.desktopI18n.text(target, window.desktopI18n.live(() => message));
        target.hidden = false;
    }
    function clearError() { byId("bookmark-error").hidden = true; }
    // 面板内的轻提示，操作成功或失败都使用同一区域，自动消失。
    let toastTimer = null;
    function toast(message, kind) {
        const target = byId("bookmark-toast");
        window.desktopI18n.text(target, window.desktopI18n.live(() => message));
        target.className = "bookmark-toast" + (kind ? " " + kind : "");
        target.hidden = false;
        window.clearTimeout(toastTimer);
        toastTimer = window.setTimeout(function hideToast() { target.hidden = true; }, 2600);
    }
    function showDialogError(message) {
        const target = byId("bookmark-dialog-error");
        window.desktopI18n.text(target, window.desktopI18n.live(() => message));
        target.hidden = false;
    }
    function clearDialogError() { byId("bookmark-dialog-error").hidden = true; }
    function setProbeState(message) { window.desktopI18n.text(byId("bookmark-probe-state"), window.desktopI18n.live(() => message)); }
    function report(error) { toast(window.desktopI18n.live(() => error && error.message ? error.message : window.desktopI18n.t("操作未能完成，请重试。")), "error"); }
    // 事件里的 Promise 异常统一提示，并保持按钮可再次点击。
    function action(handler) {
        return async function invoked(event) {
            if (event)
                event.preventDefault();
            if (event)
                event.stopPropagation();
            const source = event ? event.currentTarget : null;
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
    function hostOf(address) {
        try {
            return new URL(address).hostname.replace(/^www\./i, "");
        }
        catch (error) {
            return String(address || "");
        }
    }
    function visibleItems() {
        const keyword = state.filter.trim().toLowerCase();
        if (!keyword)
            return state.items;
        return state.items.filter(function matches(item) {
            return item.name.toLowerCase().includes(keyword) || item.url.toLowerCase().includes(keyword);
        });
    }
    function selectionCount() {
        let count = 0;
        for (const item of visibleItems())
            if (state.selected.has(item.id))
                count += 1;
        return count;
    }
    function updateSelectionUi() {
        const count = selectionCount();
        const visible = visibleItems();
        const all = byId("bookmark-select-all");
        all.checked = visible.length > 0 && count === visible.length;
        all.indeterminate = count > 0 && count < visible.length;
        window.desktopI18n.text(byId("bookmark-selection-info"), window.desktopI18n.live(() => count ? window.desktopI18n.t("已选 ") + count + window.desktopI18n.t(" 项") : visible.length + window.desktopI18n.t(" 个书签")));
        byId("bookmark-delete-selected").disabled = count === 0;
    }
    // 渲染列表；空状态提示会根据是否已有书签切换文案。
    function render() {
        const list = byId("bookmark-list");
        const empty = byId("bookmark-empty");
        list.replaceChildren();
        const items = visibleItems();
        empty.hidden = false;
        if (!state.items.length) {
            window.desktopI18n.text(empty.querySelector("h2"), window.desktopI18n.live(() => window.desktopI18n.t("还没有书签")));
            window.desktopI18n.text(empty.querySelector("p"), window.desktopI18n.live(() => window.desktopI18n.t("点击\u201C新建书签\u201D，输入网址后会自动读取网页标题。")));
            byId("bookmark-empty-add").hidden = false;
        }
        else if (!items.length) {
            window.desktopI18n.text(empty.querySelector("h2"), window.desktopI18n.live(() => window.desktopI18n.t("没有匹配的书签")));
            window.desktopI18n.text(empty.querySelector("p"), window.desktopI18n.live(() => window.desktopI18n.t("试试其他名称或网址关键字。")));
            byId("bookmark-empty-add").hidden = true;
        }
        else {
            empty.hidden = true;
        }
        for (const item of items) {
            const row = node("div", "bookmark-row");
            row.dataset.id = item.id;
            row.draggable = true;
            if (state.activeId === item.id)
                row.classList.add("active");
            if (state.selected.has(item.id))
                row.classList.add("selected");
            const checkbox = document.createElement("input");
            checkbox.type = "checkbox";
            checkbox.checked = state.selected.has(item.id);
            window.desktopI18n.attr(checkbox, "aria-label", window.desktopI18n.live(() => window.desktopI18n.t("选择书签 ") + item.name));
            checkbox.addEventListener("change", function selectionChanged(event) {
                if (event.target.checked)
                    state.selected.add(item.id);
                else
                    state.selected.delete(item.id);
                updateSelectionUi();
                render();
            });
            const content = node("div", "bookmark-content");
            content.addEventListener("click", action(function openClicked() {
                return openItem(item);
            }));
            const name = node("button", "bookmark-name", window.desktopI18n.live(() => item.name));
            name.type = "button";
            name.addEventListener("click", action(function nameClicked() {
                return openItem(item);
            }));
            const url = node("span", "bookmark-url", window.desktopI18n.live(() => item.url));
            content.append(name, url);
            const actions = node("div", "bookmark-actions");
            const edit = node("button", "icon-button");
            edit.append(window.desktopIcons.create("edit"));
            edit.type = "button";
            window.desktopI18n.attr(edit, "title", window.desktopI18n.live(() => window.desktopI18n.t("编辑书签")));
            window.desktopI18n.attr(edit, "aria-label", window.desktopI18n.live(() => window.desktopI18n.t("编辑书签 ") + item.name));
            edit.addEventListener("click", action(function editClicked() {
                return Promise.resolve(openDialog(item));
            }));
            const remove = node("button", "icon-button");
            remove.append(window.desktopIcons.create("delete"));
            remove.type = "button";
            window.desktopI18n.attr(remove, "title", window.desktopI18n.live(() => window.desktopI18n.t("删除书签")));
            window.desktopI18n.attr(remove, "aria-label", window.desktopI18n.live(() => window.desktopI18n.t("删除书签 ") + item.name));
            remove.addEventListener("click", action(function removeClicked() {
                return deleteItems([item.id]);
            }));
            actions.append(edit, remove);
            row.append(checkbox, content, actions);
            row.addEventListener("dragstart", function rowDragStarted(event) {
                state.dragId = item.id;
                row.classList.add("dragging");
                event.dataTransfer.effectAllowed = "move";
                event.dataTransfer.setData("text/plain", item.url);
            });
            row.addEventListener("dragend", function rowDragEnded() {
                state.dragId = "";
                row.classList.remove("dragging");
                list.querySelectorAll(".drag-over").forEach(function clearMark(target) { target.classList.remove("drag-over"); });
            });
            row.addEventListener("dragover", function rowDraggedOver(event) {
                if (!state.dragId || state.dragId === item.id)
                    return;
                event.preventDefault();
                event.stopPropagation();
                event.dataTransfer.dropEffect = "move";
                row.classList.add("drag-over");
            });
            row.addEventListener("dragleave", function rowDraggedAway(event) {
                if (!row.contains(event.relatedTarget))
                    row.classList.remove("drag-over");
            });
            row.addEventListener("drop", function rowDropped(event) {
                row.classList.remove("drag-over");
                if (!state.dragId || state.dragId === item.id)
                    return;
                event.preventDefault();
                event.stopPropagation();
                const dragged = state.dragId;
                state.dragId = "";
                moveItem(dragged, item.id).catch(report);
            });
            list.append(row);
        }
        updateSelectionUi();
    }
    // 列表末尾的放置区：移动到最末。
    function installListDropTarget() {
        const list = byId("bookmark-list");
        list.addEventListener("dragover", function listDraggedOver(event) {
            if (!state.dragId)
                return;
            event.preventDefault();
            event.dataTransfer.dropEffect = "move";
        });
        list.addEventListener("drop", function listDropped(event) {
            if (!state.dragId)
                return;
            event.preventDefault();
            const dragged = state.dragId;
            state.dragId = "";
            moveItem(dragged, "").catch(report);
        });
    }
    async function refresh() {
        const version = ++state.refreshVersion;
        const data = await window.desktopBridge.call("getBookmarks");
        if (version !== state.refreshVersion)
            return;
        state.items = Array.isArray(data.items) ? data.items : [];
        for (const id of Array.from(state.selected)) {
            const exists = state.items.some(function sameId(item) { return item.id === id; });
            if (!exists)
                state.selected.delete(id);
        }
        render();
    }
    async function openItem(item) {
        await window.desktopBridge.call("openUrl", item.url);
        state.activeId = item.id;
        render();
    }
    async function moveItem(id, beforeId) {
        await window.desktopBridge.call("moveBookmark", id, beforeId);
        await refresh();
    }
    async function deleteItems(ids) {
        if (!ids.length)
            return;
        const message = ids.length === 1 ? window.desktopI18n.t("确定删除这个书签吗？") : window.desktopI18n.t("确定删除选中的 ") + ids.length + window.desktopI18n.t(" 个书签吗？");
        if (!window.confirm(message))
            return;
        await window.desktopBridge.call("deleteBookmarks", ids);
        for (const id of ids)
            state.selected.delete(id);
        await refresh();
        toast(window.desktopI18n.live(() => ids.length === 1 ? window.desktopI18n.t("已删除书签") : window.desktopI18n.t("已删除 ") + ids.length + window.desktopI18n.t(" 个书签")), "success");
    }
    // 等待指定令牌的检测结果；结果由 urlChecked 信号带回。
    function waitProbe(token) {
        token = String(token);
        if (state.probeResults.has(token)) {
            const result = state.probeResults.get(token);
            state.probeResults.delete(token);
            return Promise.resolve(result);
        }
        return new Promise(function waiting(resolve) {
            const timer = window.setTimeout(function probeTimedOut() {
                if (state.probePending && state.probePending.token === token)
                    state.probePending = null;
                resolve({ ok: false, get error() { return window.desktopI18n.t("检测超时，请手动填写名称。"); } });
            }, 20000);
            state.probePending = { token, resolve: result => { window.clearTimeout(timer); resolve(result); } };
        });
    }
    // 释放旧检测的等待者，防止关闭对话框后遗留 Promise 或覆盖新输入。
    function cancelProbe() {
        ++state.probeVersion;
        if (state.probePending)
            state.probePending.resolve({ ok: false });
        state.probePending = null;
        state.probeResults.clear();
        byId("bookmark-probe").disabled = false;
    }
    // 检测网址：成功时用网页标题自动填充名称，失败时保留用户手动输入。
    async function probeAddress() {
        cancelProbe();
        const version = state.probeVersion;
        const address = byId("bookmark-url").value.trim();
        const originalName = byId("bookmark-name").value;
        if (!address) {
            showDialogError(window.desktopI18n.live(() => window.desktopI18n.t("请先输入网页地址。")));
            return;
        }
        const button = byId("bookmark-probe");
        button.disabled = true;
        setProbeState(window.desktopI18n.live(() => window.desktopI18n.t("正在打开网页并读取标题\u2026")));
        clearDialogError();
        try {
            const started = await window.desktopBridge.call("checkUrl", address);
            if (version !== state.probeVersion)
                return;
            const result = await waitProbe(started.token);
            if (version !== state.probeVersion || !byId("bookmark-dialog").open || byId("bookmark-url").value.trim() !== address)
                return;
            if (!result.ok) {
                setProbeState(window.desktopI18n.live(() => window.desktopI18n.t("无法自动获取名称，请手动输入。")));
                showDialogError(window.desktopI18n.live(() => result.error || window.desktopI18n.t("无法读取网页标题。")));
                return;
            }
            const data = result.data || {};
            if (!data.available) {
                setProbeState(window.desktopI18n.live(() => window.desktopI18n.t("暂时打不开这个网页，请手动输入名称。") + (data.error ? "（" + data.error + "）" : "")));
                return;
            }
            const title = data.title || hostOf(data.url || address);
            if (byId("bookmark-name").value === originalName)
                byId("bookmark-name").value = title;
            setProbeState(window.desktopI18n.live(() => window.desktopI18n.t("已读取网页标题：") + title));
        }
        catch (error) {
            if (version !== state.probeVersion)
                return;
            showDialogError(window.desktopI18n.live(() => error.message || window.desktopI18n.t("检测失败，请手动输入名称。")));
            setProbeState(window.desktopI18n.live(() => window.desktopI18n.t("检测失败，请手动输入名称。")));
        }
        finally {
            if (version === state.probeVersion)
                button.disabled = false;
        }
    }
    function openDialog(item) {
        if (state.saving || byId("bookmark-dialog").open)
            return;
        cancelProbe();
        state.editingId = item ? item.id : "";
        state.probePending = null;
        window.desktopI18n.text(byId("bookmark-dialog-title"), window.desktopI18n.live(() => item ? window.desktopI18n.t("编辑书签") : window.desktopI18n.t("新建书签")));
        byId("bookmark-url").value = item ? item.url : (state.currentUrl || "");
        byId("bookmark-name").value = item ? item.name : "";
        window.desktopI18n.text(byId("bookmark-probe-state"), window.desktopI18n.live(() => item ? window.desktopI18n.t("修改后仍可点击\u201C检测标题\u201D重新读取。") : ""));
        clearDialogError();
        byId("bookmark-dialog").showModal();
        // 新建且已填入网址时自动检测，读取成功则自动补全名称。
        if (!item && byId("bookmark-url").value.trim())
            probeAddress().catch(report);
    }
    function closeDialog() {
        if (state.saving)
            return;
        cancelProbe();
        const dialog = byId("bookmark-dialog");
        state.probePending = null;
        if (dialog.open)
            dialog.close();
    }
    async function submitDialog(event) {
        event.preventDefault();
        if (state.saving)
            return;
        const payload = {
            id: state.editingId,
            url: byId("bookmark-url").value.trim(),
            name: byId("bookmark-name").value.trim()
        };
        if (!payload.name) {
            showDialogError(window.desktopI18n.live(() => window.desktopI18n.t("请填写书签名称。")));
            return;
        }
        const button = byId("bookmark-dialog-submit");
        state.saving = true;
        cancelProbe();
        clearDialogError();
        byId("bookmark-dialog-form").inert = true;
        button.disabled = true;
        try {
            await window.desktopBridge.call("saveBookmark", payload);
            state.saving = false;
            closeDialog();
            await refresh();
            toast(window.desktopI18n.live(() => window.desktopI18n.t("书签已保存")), "success");
        }
        catch (error) {
            showDialogError(window.desktopI18n.live(() => error.message || window.desktopI18n.t("保存失败，请重试。")));
        }
        finally {
            state.saving = false;
            byId("bookmark-dialog-form").inert = false;
            button.disabled = false;
        }
    }
    // 右侧浏览区推送当前网址，用于“收藏当前网页”。
    function setCurrentUrl(url) {
        state.currentUrl = url || "";
        const footer = byId("bookmark-current");
        footer.hidden = !state.currentUrl;
        const label = byId("bookmark-current-url");
        window.desktopI18n.text(label, window.desktopI18n.live(() => state.currentUrl ? window.desktopI18n.t("当前网页：") + state.currentUrl : window.desktopI18n.t("当前网页：\u2014")));
        window.desktopI18n.attr(label, "title", window.desktopI18n.live(() => state.currentUrl));
    }
    function installEvents() {
        if (state.initialized)
            return;
        state.initialized = true;
        byId("bookmark-add").addEventListener("click", function addClicked() { openDialog(null); });
        byId("bookmark-empty-add").addEventListener("click", function emptyAddClicked() { openDialog(null); });
        byId("bookmark-add-current").addEventListener("click", function addCurrentClicked() { openDialog(null); });
        byId("bookmark-search").addEventListener("input", function searchChanged(event) {
            state.filter = event.target.value;
            render();
        });
        byId("bookmark-select-all").addEventListener("change", function selectAllChanged(event) {
            for (const item of visibleItems()) {
                if (event.target.checked)
                    state.selected.add(item.id);
                else
                    state.selected.delete(item.id);
            }
            render();
        });
        byId("bookmark-delete-selected").addEventListener("click", action(function deleteSelectedClicked() {
            return deleteItems(visibleItems().filter(item => state.selected.has(item.id)).map(item => item.id));
        }));
        byId("bookmark-dialog-form").addEventListener("submit", submitDialog);
        byId("bookmark-probe").addEventListener("click", function probeClicked() { probeAddress().catch(report); });
        byId("bookmark-url").addEventListener("input", cancelProbe);
        byId("bookmark-url").addEventListener("change", function addressChanged() {
            // 输入完网址离开输入框且名称为空时自动检测一次。
            if (!byId("bookmark-name").value.trim() && byId("bookmark-url").value.trim())
                probeAddress().catch(report);
        });
        byId("bookmark-dialog-cancel").addEventListener("click", closeDialog);
        byId("bookmark-dialog-close").addEventListener("click", closeDialog);
        byId("bookmark-dialog").addEventListener("cancel", function dialogCancelled(event) {
            event.preventDefault();
            closeDialog();
        });
        installListDropTarget();
    }
    async function initialize() {
        try {
            await window.desktopBridge.connect();
        }
        catch (error) {
            showError(window.desktopI18n.live(() => error.message || window.desktopI18n.t("无法连接桌面程序。")));
            return;
        }
        installEvents();
        await window.desktopBridge.on("urlChecked", function urlChecked(result) {
            const data = result && result.data ? result.data : {};
            if (!state.probePending || String(data.token) !== state.probePending.token) {
                if (state.probeResults.size > 20)
                    state.probeResults.clear();
                state.probeResults.set(String(data.token), result);
                return;
            }
            const resolve = state.probePending.resolve;
            state.probePending = null;
            resolve(result);
        });
        await window.desktopBridge.on("bookmarksChanged", function bookmarksChanged() {
            refresh().catch(report);
        });
        clearError();
        await refresh();
    }
    window.desktopBookmarkPanel = Object.freeze({ setCurrentUrl: setCurrentUrl, refresh: refresh });
    initialize().catch(function startupFailed(error) { report(error); });
})();
