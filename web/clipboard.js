/* 剪贴板页面：原生服务负责监听和持久化，网页只管理筛选、分页和操作。 */
(function installClipboardPage() {
  "use strict";
  // 内容类型的中文名称。
  const names = { text: "文本", image: "图像", files: "文件 / 文件夹", links: "链接", other: "其他" };
  // 页面状态，监听刷新不覆盖正在提交的设置。
  const state = {
    items: [], // 当前关键词匹配的轻量历史摘要。
    types: [], // 后端已保存的监听类型。
    total: 0, // 全部历史数量，与搜索结果数量分开。
    selected: new Set(), // 当前筛选结果中勾选的记录标识。
    page: 0, // 当前分页，从零开始。
    revision: 0, // 查询序号，忽略过期响应。
    timer: null, // 搜索与后端通知合并定时器。
    savingTypes: false, // 正在保存监听类型。
    initialized: false, // 控件和信号只绑定一次。
    available: false, // 历史读取是否正常。
    options: null, // 主页面提供的提示函数。
    confirm: null, // 当前删除确认操作。
    dialogBusy: false, // 删除提交期间阻止重复操作和关闭。
    dialogRevision: 0 // 防止过期详情响应覆盖新对话框。
  };
  // 每页卡片数量，限制同时显示的图片数量。
  const pageSize = 24;
  // 获取固定页面控件。
  function byId(id) { return document.getElementById(id); }
  // 以纯文本创建节点，复制内容绝不作为 HTML 执行。
  function node(tag, className, text) {
    const result = document.createElement(tag);
    if (className) result.className = className;
    if (text !== undefined) result.textContent = String(text);
    return result;
  }
  // 统一处理按钮的异步错误和重复点击。
  function action(label, handler, className = "button secondary small") {
    const result = node("button", className, label);
    result.type = "button";
    result.addEventListener("click", async function clicked() {
      result.disabled = true;
      try { await handler(); } catch (error) { state.options.reportError(error); }
      finally { result.disabled = false; }
    });
    return result;
  }
  // 将 UTC 时间显示为本机中文日期时间，包括秒。
  function date(value) { return new Date(value).toLocaleString("zh-CN", { hour12: false }); }
  // 筛选历史类型并将置顶记录排在前面，同组保持最新复制在前。
  function filtered() {
    const type = byId("clipboard-filter").value;
    return state.items.filter(item => !type || item.type === type).sort((a, b) => Number(b.pinned) - Number(a.pinned));
  }
  // 更新勾选数量、全选状态和批量操作按钮。
  function selectionState(items) {
    const valid = new Set(items.map(item => item.id));
    state.selected = new Set([...state.selected].filter(id => valid.has(id)));
    byId("clipboard-selection-info").textContent = items.length + " 条结果 · 已选 " + state.selected.size;
    byId("clipboard-select-all").checked = items.length > 0 && state.selected.size === items.length;
    byId("clipboard-select-all").indeterminate = state.selected.size > 0 && state.selected.size < items.length;
    byId("clipboard-select-all").disabled = !state.available || !items.length;
    byId("clipboard-delete").disabled = !state.available || !state.selected.size;
    byId("clipboard-clear").disabled = !state.available || !state.total;
  }
  // 创建卡片，显示类型、精确时间和文件引用；图片仅加载缩略图。
  function card(item) {
    const result = node("article", "clipboard-card" + (item.pinned ? " pinned" : ""));
    result.dataset.id = item.id;
    const heading = node("div", "clipboard-card-heading");
    const check = node("input");
    check.type = "checkbox";
    check.checked = state.selected.has(item.id);
    check.setAttribute("aria-label", "选择" + names[item.type] + "记录 " + date(item.copiedAt));
    check.addEventListener("change", function selectRecord() {
      if (check.checked) state.selected.add(item.id); else state.selected.delete(item.id);
      selectionState(filtered());
    });
    const time = node("time", "muted", date(item.copiedAt));
    time.dateTime = item.copiedAt;
    heading.append(check, node("span", "clipboard-kind", names[item.type]), time);
    if (item.pinned) heading.append(node("span", "clipboard-pin", "置顶"));
    result.append(heading);
    if (item.type === "image") {
      const image = node("img", "clipboard-thumbnail");
      image.src = item.thumbnail;
      image.alt = "复制的图像，" + item.preview;
      image.loading = "lazy";
      result.append(image, node("p", "muted", item.preview));
    } else if (item.files) {
      const files = node("div", "clipboard-files-preview");
      for (const file of item.files.slice(0, 3)) {
        files.append(node("strong", "", file.name || "未命名"), node("p", "clipboard-path", file.path));
      }
      if (item.files.length > 3) files.append(node("p", "muted", "共 " + item.files.length + " 项，查看详情显示全部"));
      result.append(files);
    } else {
      result.append(node("pre", "clipboard-text-preview", item.preview));
    }
    const actions = node("div", "clipboard-card-actions");
    actions.append(action("查看", () => details(item.id)));
    if (item.type !== "other") actions.append(action("再次复制", async function copyRecord() {
      await window.desktopBridge.call("copyClipboardItem", item.id);
      state.options.toast("已复制到剪贴板");
    }));
    actions.append(action(item.pinned ? "取消置顶" : "置顶", async function pinRecord() {
      await window.desktopBridge.call("pinClipboardItem", item.id, !item.pinned);
      await refresh();
    }, "button quiet small"));
    actions.append(action("删除", () => confirmDelete([item.id]), "button danger quiet small"));
    result.append(actions);
    return result;
  }
  // 绘制当前页，筛选时保留仍在结果中的选择。
  function render() {
    const items = filtered();
    const pages = Math.max(1, Math.ceil(items.length / pageSize));
    state.page = Math.min(state.page, pages - 1);
    selectionState(items);
    byId("clipboard-list").replaceChildren(...items.slice(state.page * pageSize, (state.page + 1) * pageSize).map(card));
    byId("clipboard-count").textContent = state.total;
    byId("clipboard-empty").hidden = items.length > 0 || !state.available;
    byId("clipboard-empty").querySelector("h2").textContent = state.total ? "没有匹配的记录" : "还没有剪贴板记录";
    byId("clipboard-empty").querySelector("p").textContent = state.total ? "试试其他关键词或内容类型。" : "勾选上方内容类型后，新的复制内容会自动出现在这里。";
    byId("clipboard-page-info").textContent = "第 " + (state.page + 1) + " / " + pages + " 页";
    byId("clipboard-previous").disabled = state.page === 0;
    byId("clipboard-next").disabled = state.page >= pages - 1;
  }
  // 更新监听状态，故障不会使页面误显示正在记录。
  function renderTypes() {
    if (!state.savingTypes) {
      for (const input of byId("clipboard-types").querySelectorAll("input")) input.checked = state.types.includes(input.value);
    }
    byId("clipboard-types").disabled = state.savingTypes || !state.available;
    byId("clipboard-status").textContent = !state.available ? "记录不可用" : state.types.length ? "正在监听 · " + state.types.length + " 种类型" : "已暂停记录";
  }
  // 查询完整文本匹配的摘要，错误仅显示在剪贴板页。
  async function refresh() {
    const revision = ++state.revision;
    try {
      const data = await window.desktopBridge.call("getClipboardHistory", byId("clipboard-search").value);
      if (revision !== state.revision) return;
      state.items = data.items;
      state.types = data.types;
      state.total = data.total;
      state.available = true;
      byId("clipboard-error").textContent = data.notice || "";
      byId("clipboard-error").hidden = !data.notice;
    } catch (error) {
      if (revision !== state.revision) return;
      state.available = false;
      byId("clipboard-error").textContent = error.message;
      byId("clipboard-error").hidden = false;
    }
    renderTypes();
    render();
  }
  // 合并紧邻的信号或输入，降低图片历史刷新开销。
  function scheduleRefresh() {
    clearTimeout(state.timer);
    state.timer = setTimeout(refresh, 100);
  }
  // 设置成功才更新勾选缓存，失败恢复原值。
  async function saveTypes() {
    const types = [...byId("clipboard-types").querySelectorAll("input:checked")].map(input => input.value);
    state.savingTypes = true;
    renderTypes();
    try { await window.desktopBridge.call("setClipboardTypes", types); }
    catch (error) { state.options.reportError(error); }
    finally { state.savingTypes = false; await refresh(); }
  }
  // 准备独立详情或删除对话框，避免干扰便签编辑表单。
  function openDialog(title) {
    state.dialogRevision += 1;
    state.confirm = null;
    byId("clipboard-dialog-title").textContent = title;
    byId("clipboard-dialog-body").replaceChildren();
    byId("clipboard-dialog-error").hidden = true;
    byId("clipboard-dialog-confirm").hidden = true;
    byId("clipboard-dialog-cancel").textContent = "关闭";
    byId("clipboard-dialog").showModal();
    return state.dialogRevision;
  }
  // 读取完整记录，文件列表提供逐项复制路径和打开目录。
  async function details(id) {
    const revision = openDialog("剪贴板详情");
    const body = byId("clipboard-dialog-body");
    body.textContent = "正在读取…";
    try {
      const item = await window.desktopBridge.call("getClipboardItem", id);
      if (revision !== state.dialogRevision) return;
      body.replaceChildren(node("p", "muted", names[item.type] + " · " + date(item.copiedAt)));
      if (item.type === "image") {
        const image = node("img", "clipboard-full-image");
        image.src = item.image;
        image.alt = item.text;
        body.append(image);
        body.append(node("p", "clipboard-path", item.text));
      } else if (item.files) {
        item.files.forEach(function showFile(file, index) {
          const row = node("div", "clipboard-file-detail");
          row.append(node("strong", "", file.name || "未命名"), node("p", "clipboard-path", file.path));
          row.append(action("复制路径", async function copyPath() {
            await window.desktopBridge.call("copyText", file.path);
            state.options.toast("已复制路径");
          }));
          if (file.local) row.append(action("打开所在目录", () => window.desktopBridge.call("openClipboardDirectory", id, index)));
          body.append(row);
        });
      } else body.append(node("pre", "clipboard-full-text", item.text));
    } catch (error) { if (revision === state.dialogRevision) body.textContent = error.message; }
  }
  // 删除确认固定本次选中的标识，监听新增记录不会扩大删除范围。
  function confirmDelete(ids, clear = false) {
    if (!clear && !ids.length) return;
    openDialog(clear ? "清空剪贴板历史" : "删除剪贴板记录");
    byId("clipboard-dialog-body").textContent = clear
      ? "确定清空全部历史（含置顶记录）吗？此操作无法撤销，引用的原文件不会被删除。"
      : "确定删除这 " + ids.length + " 条记录吗？此操作无法撤销，引用的原文件不会被删除。";
    byId("clipboard-dialog-confirm").hidden = false;
    byId("clipboard-dialog-cancel").textContent = "取消";
    state.confirm = async function deleteConfirmedRecords() {
      if (clear) await window.desktopBridge.call("clearClipboardHistory");
      else await window.desktopBridge.call("deleteClipboardItems", ids);
      await refresh();
    };
  }
  // 关闭对话框时使未完成详情读取失效。
  function closeDialog() {
    if (state.dialogBusy) return;
    state.dialogRevision += 1;
    byId("clipboard-dialog").close();
  }
  // 提交删除，失败保留对话框并显示原因。
  async function submitDelete() {
    if (state.dialogBusy || !state.confirm) return;
    state.dialogBusy = true;
    byId("clipboard-dialog-confirm").disabled = true;
    try { await state.confirm(); state.dialogBusy = false; closeDialog(); }
    catch (error) { byId("clipboard-dialog-error").textContent = error.message; byId("clipboard-dialog-error").hidden = false; }
    finally { state.dialogBusy = false; byId("clipboard-dialog-confirm").disabled = false; }
  }
  // 绑定固定控件和原生通知，初始化失败不影响其他功能。
  async function initialize(options) {
    state.options = options;
    if (!state.initialized) {
      state.initialized = true;
      byId("clipboard-types").addEventListener("change", saveTypes);
      byId("clipboard-search").addEventListener("input", function searchChanged() { state.page = 0; state.revision += 1; scheduleRefresh(); });
      byId("clipboard-filter").addEventListener("change", function filterChanged() { state.page = 0; render(); });
      byId("clipboard-refresh").addEventListener("click", refresh);
      byId("clipboard-select-all").addEventListener("change", function selectResults(event) { state.selected = new Set(event.target.checked ? filtered().map(item => item.id) : []); render(); });
      byId("clipboard-delete").addEventListener("click", function deleteSelection() { confirmDelete([...state.selected]); });
      byId("clipboard-clear").addEventListener("click", function clearHistory() { confirmDelete([], true); });
      byId("clipboard-previous").addEventListener("click", function previousPage() { state.page -= 1; render(); });
      byId("clipboard-next").addEventListener("click", function nextPage() { state.page += 1; render(); });
      byId("clipboard-dialog-close").addEventListener("click", closeDialog);
      byId("clipboard-dialog-cancel").addEventListener("click", closeDialog);
      byId("clipboard-dialog-confirm").addEventListener("click", submitDelete);
      byId("clipboard-dialog").addEventListener("cancel", function escapeDialog(event) { event.preventDefault(); closeDialog(); });
      await window.desktopBridge.on("clipboardChanged", scheduleRefresh);
    }
    await refresh();
  }
  // 对主页面仅暴露初始化入口。
  window.desktopClipboard = Object.freeze({ initialize });
})();
