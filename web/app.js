/* 本地 HTML 界面：数据和系统能力通过 Qt 桥接调用，页面只管理交互与呈现。 */
(function installDesktopApplication() {
  "use strict";

  // 页面状态；列表缓存来自 Qt，勾选与筛选状态只保存在本次会话。
  const state = {
    page: "shortcuts", // 当前导航页面。
    shortcuts: { categories: [], items: [] }, // 快捷方式后端快照。
    notes: { categories: [], items: [] }, // 便签目录后端快照。
    shortcutCategory: "", // 空字符串表示全部分类。
    noteCategory: "", // 便签列表正在筛选的分类。
    shortcutSelected: new Set(), // 已勾选的快捷方式 ID。
    noteSelected: new Set(), // 已勾选的便签 ID。
    note: null, // 正在编辑的便签记录，空 ID 表示尚未保存。
    noteDirty: false, // 是否存在未保存的便签修改。
    noteRevision: 0, // 编辑修订号，避免异步保存覆盖后续输入。
    noteSaving: false, // 保存过程禁止重复提交。
    imageImporting: 0, // 图片异步读取期间阻止切换、退出或提前保存。
    noteLoadVersion: 0, // 忽略已过期的便签加载结果。
    shortcutLoadVersion: 0, // 忽略已过期的快捷方式列表结果。
    notesLoadVersion: 0, // 忽略已过期的便签目录结果。
    settings: { fontSize: 16, hotkeyModifier: 0, hotkeyKey: 119, downloadDirectory: "", autoStart: false }, // 最近保存的界面、热键、默认下载路径和开机自启状态。
    hotkeyDraft: { modifier: 0, key: 119 }, // 正在输入的热键。
    settingsDirty: false, // 设置表单是否已修改。
    settingsSaving: false, // 设置提交期间不以信号覆盖表单。
    savedRange: null, // 插入图片和应用格式前保留的编辑器选区。
    drag: null, // 本页面内部拖动的条目及类型。
    connected: false, // 是否已完成后端初始加载。
    signalTimers: new Map(), // 合并短时间内的同类后端变更通知。
    busyTimer: null, // 延迟显示全局等待提示，避免快速进度查询造成闪烁。
    dialog: null // 当前对话框提交函数及忙碌状态。
  };

  // 按 ID 取得页面固定节点。
  function byId(id) { return document.getElementById(id); }

  // 创建元素，动态用户数据统一通过 textContent 设置。
  function element(tag, className, text) {
    const result = document.createElement(tag);
    if (className) result.className = className;
    if (text !== undefined) result.textContent = String(text);
    return result;
  }

  // 创建可访问按钮，并统一接住异步事件错误。
  function button(text, className, handler, label) {
    const result = element("button", className, text);
    result.type = "button";
    if (label) { result.setAttribute("aria-label", label); result.title = label; }
    result.addEventListener("click", function onDynamicButtonClicked(event) {
      event.stopPropagation();
      Promise.resolve().then(function invokeButtonHandler() { return handler(event, result); }).catch(reportError);
    });
    return result;
  }

  // 显示短暂反馈；错误停留更久，所有消息保持纯文本。
  function toast(message, type = "success") {
    const node = element("div", "toast " + type, message);
    byId("toast-region").append(node);
    window.setTimeout(function dismissToast() { node.remove(); }, type === "error" ? 8500 : 4000);
  }

  // 将异步异常显示为可理解的界面提示。
  function reportError(error) {
    toast(error && error.message ? error.message : "操作失败，请重试。", "error");
  }

  // 执行一次按钮操作并阻止同一按钮重复触发。
  async function perform(task, successText, source) {
    if (source) source.disabled = true;
    try {
      const result = await task();
      if (successText) toast(successText);
      return result;
    } finally {
      if (source && source.isConnected) source.disabled = false;
    }
  }

  // 规范列表返回值，缺失结构视为错误，避免展示虚假的空数据。
  function validCollection(data) {
    if (!data || !Array.isArray(data.categories) || !Array.isArray(data.items)) throw new Error("列表数据不完整，请重新打开程序。");
    return data;
  }

  // 读取一种条目的缓存。
  function collection(kind) { return kind === "shortcut" ? state.shortcuts : state.notes; }

  // 返回默认分类 ID，兼容后端采用其他稳定默认 ID 的情况。
  function defaultCategory(kind) {
    const categories = collection(kind).categories;
    const preferred = categories.find(function findDefaultCategory(item) { return item.id === "default"; });
    return preferred ? preferred.id : (categories[0] ? categories[0].id : "default");
  }

  // 取得分类显示名称。
  function categoryName(kind, id) {
    const category = collection(kind).categories.find(function findCategory(item) { return item.id === id; });
    return category ? category.name : "默认分类";
  }

  // 显示旧数据迁移等后端警告，不将警告内容解析为 HTML。
  function renderWarnings(id, warnings) {
    const target = byId(id);
    target.replaceChildren();
    for (const warning of Array.isArray(warnings) ? warnings : []) {
      target.append(element("div", "notice warning", typeof warning === "string" ? warning : (warning.message || "部分旧数据需要检查。")));
    }
  }

  // 筛选快捷方式；搜索只影响呈现，不改变后端数据。
  function visibleShortcuts() {
    const query = byId("shortcut-search").value.trim().toLocaleLowerCase();
    return state.shortcuts.items.filter(function shortcutMatches(item) {
      return (!state.shortcutCategory || item.categoryId === state.shortcutCategory)
        && (!query || String(item.title).toLocaleLowerCase().includes(query) || String(item.target).toLocaleLowerCase().includes(query));
    });
  }

  // 按分类和标题筛选便签。
  function visibleNotes() {
    const query = byId("note-search").value.trim().toLocaleLowerCase();
    return state.notes.items.filter(function noteMatches(item) {
      return (!state.noteCategory || item.categoryId === state.noteCategory) && (!query || String(item.title).toLocaleLowerCase().includes(query));
    });
  }

  // 刷新快捷方式缓存，使用递增版本丢弃过期返回值。
  async function refreshShortcuts() {
    const version = ++state.shortcutLoadVersion;
    const data = validCollection(await window.desktopBridge.call("getShortcuts"));
    if (version !== state.shortcutLoadVersion) return;
    state.shortcuts = data;
    const ids = new Set(data.items.map(function shortcutId(item) { return item.id; }));
    for (const id of state.shortcutSelected) if (!ids.has(id)) state.shortcutSelected.delete(id);
    if (!data.categories.some(function currentCategoryExists(category) { return category.id === state.shortcutCategory; })) state.shortcutCategory = "";
    renderWarnings("shortcut-warnings", data.warnings);
    renderShortcuts();
  }

  // 刷新便签索引但保留编辑器未保存的内容。
  async function refreshNotes() {
    const version = ++state.notesLoadVersion;
    const data = validCollection(await window.desktopBridge.call("getNotes"));
    if (version !== state.notesLoadVersion) return;
    state.notes = data;
    const ids = new Set(data.items.map(function noteId(item) { return item.id; }));
    for (const id of state.noteSelected) if (!ids.has(id)) state.noteSelected.delete(id);
    if (!data.categories.some(function currentCategoryExists(category) { return category.id === state.noteCategory; })) state.noteCategory = "";
    renderWarnings("note-warnings", data.warnings);
    renderNotes();
    if (state.note) fillCategorySelect(byId("note-category"), "note", byId("note-category").value || state.note.categoryId);
    if (state.note && state.note.id && !ids.has(state.note.id) && !state.noteDirty && !state.noteSaving && !state.imageImporting) clearNoteEditor();
  }

  // 创建分类选择下拉框的选项。
  function fillCategorySelect(select, kind, selectedId) {
    select.replaceChildren();
    for (const category of collection(kind).categories) {
      const option = element("option", "", category.name);
      option.value = category.id;
      select.append(option);
    }
    select.value = selectedId;
    if (select.selectedIndex < 0) select.value = defaultCategory(kind);
  }

  // 绘制分类标签，支持编辑、删除及拖动到分类末尾。
  function renderCategories(kind) {
    const target = byId(kind + "-categories");
    const selected = state[kind + "Category"];
    target.replaceChildren();
    const categories = [{ id: "", name: "全部" }, ...collection(kind).categories];
    for (const category of categories) {
      const chip = element("div", "category-chip" + (category.id === selected ? " active" : ""));
      chip.dataset.categoryId = category.id;
      const select = button(category.name, "category-select", function selectCategory() {
        state[kind + "Category"] = category.id;
        state[kind + "Selected"].clear();
        if (kind === "shortcut") renderShortcuts(); else renderNotes();
      });
      select.setAttribute("aria-pressed", String(category.id === selected));
      chip.append(select);
      if (category.id && category.id !== defaultCategory(kind)) {
        chip.append(button("✎", "icon-button", function renameCategory() { editCategory(kind, category); }, "重命名分类“" + category.name + "”"));
        chip.append(button("×", "icon-button", function removeCategory() { deleteCategory(kind, category); }, "删除分类“" + category.name + "”"));
      }
      if (category.id) installDropTarget(chip, kind, category.id, "");
      target.append(chip);
    }
  }

  // 创建可复用的空状态内容。
  function emptyState(title, description, symbol) {
    const box = element("div", "empty-state");
    box.append(element("div", "empty-icon", symbol), element("h2", "", title), element("p", "", description));
    return box;
  }

  // 更新全选、半选和已选数量，选择仅限当前可见列表。
  function updateSelection(kind, visible) {
    const selected = state[kind + "Selected"];
    const count = visible.filter(function isSelected(item) { return selected.has(item.id); }).length;
    const checkbox = byId(kind + "-select-all");
    checkbox.checked = visible.length > 0 && count === visible.length;
    checkbox.indeterminate = count > 0 && count < visible.length;
    checkbox.disabled = visible.length === 0;
    byId(kind + "-selection-info").textContent = count ? "已选 " + count + " 项" : visible.length + (kind === "note" ? " 篇" : " 个快捷方式");
    byId("delete-selected-" + (kind === "note" ? "notes" : "shortcuts")).disabled = count === 0;
  }

  // 绘制快捷方式卡片及操作入口。
  function renderShortcuts() {
    byId("shortcut-count").textContent = String(state.shortcuts.items.length);
    renderCategories("shortcut");
    const target = byId("shortcut-list");
    const visible = visibleShortcuts();
    target.replaceChildren();
    updateSelection("shortcut", visible);
    if (!visible.length) {
      target.append(emptyState(state.shortcuts.items.length ? "没有匹配的快捷方式" : "把常用入口放在这里", state.shortcuts.items.length ? "试试其他关键词，或切换分类。" : "添加文件、目录或网站，随时快速打开。", "↗"));
      return;
    }
    for (const item of visible) {
      const card = element("article", "shortcut-card" + (state.shortcutSelected.has(item.id) ? " selected" : ""));
      card.dataset.id = item.id;
      const heading = element("div", "card-heading");
      const checkbox = element("input", "card-checkbox");
      checkbox.type = "checkbox";
      checkbox.checked = state.shortcutSelected.has(item.id);
      checkbox.setAttribute("aria-label", "选择“" + item.title + "”");
      checkbox.addEventListener("change", function selectShortcut() {
        if (checkbox.checked) state.shortcutSelected.add(item.id); else state.shortcutSelected.delete(item.id);
        card.classList.toggle("selected", checkbox.checked);
        updateSelection("shortcut", visibleShortcuts());
      });
      const open = button(item.title, "card-title", async function openShortcut(event, source) {
        await perform(function invokeOpenShortcut() { return window.desktopBridge.call("openShortcut", item.id); }, "已打开快捷方式", source);
      }, "打开“" + item.title + "”");
      heading.append(element("span", "shortcut-type-icon type-" + item.type, ["▱", "↗", "▤"][item.type] || "↗"), open, checkbox);
      const path = element("div", "card-target", item.target);
      path.title = item.target;
      const footer = element("div", "card-footer");
      footer.append(element("span", "card-category", categoryName("shortcut", item.categoryId)));
      footer.append(button("复制", "button quiet", async function copyShortcut(event, source) {
        await perform(function invokeCopy() { return window.desktopBridge.call("copyText", item.target); }, "已复制地址", source);
      }, "复制地址"));
      footer.append(button("编辑", "button quiet", function editShortcutClick() { editShortcut(item); }));
      footer.append(button("移动", "button quiet", function moveShortcutClick() { showMoveDialog("shortcut", item); }));
      footer.append(button("删除", "button danger quiet", function deleteShortcutClick() { deleteItems("shortcut", [item.id]); }));
      card.append(heading, path, footer);
      installDraggable(card, "shortcut", item);
      installDropTarget(card, "shortcut", item.categoryId, item.id);
      target.append(card);
    }
  }

  // 将更新时间显示为简短本地日期。
  function readableDate(value) {
    if (!value) return "";
    const date = new Date(value);
    if (Number.isNaN(date.getTime())) return "";
    return date.toLocaleDateString("zh-CN", { month: "2-digit", day: "2-digit" });
  }

  // 绘制便签目录，点击标题加载完整内容。
  function renderNotes() {
    byId("note-count").textContent = String(state.notes.items.length);
    renderCategories("note");
    const target = byId("note-list");
    const visible = visibleNotes();
    target.replaceChildren();
    updateSelection("note", visible);
    if (!visible.length) {
      target.append(emptyState(state.notes.items.length ? "没有匹配的便签" : "还没有便签", state.notes.items.length ? "试试其他标题或分类。" : "新建便签，记录第一条想法。", "▤"));
      return;
    }
    for (const item of visible) {
      const row = element("article", "note-row" + (state.note && state.note.id === item.id ? " active" : "") + (state.noteSelected.has(item.id) ? " selected" : ""));
      row.dataset.id = item.id;
      const checkbox = element("input");
      checkbox.type = "checkbox";
      checkbox.checked = state.noteSelected.has(item.id);
      checkbox.setAttribute("aria-label", "选择便签“" + item.title + "”");
      checkbox.addEventListener("change", function selectNote() {
        if (checkbox.checked) state.noteSelected.add(item.id); else state.noteSelected.delete(item.id);
        row.classList.toggle("selected", checkbox.checked);
        updateSelection("note", visibleNotes());
      });
      const content = element("div", "note-row-content");
      content.append(button(item.title, "note-row-title", function openNoteClick() { return openNote(item.id); }));
      const meta = element("div", "note-row-meta");
      meta.append(element("span", "note-row-category", categoryName("note", item.categoryId)), element("span", "", readableDate(item.updatedAt)));
      content.append(meta);
      content.addEventListener("click", function openNoteMetadata(event) {
        if (event.target.closest("button")) return;
        openNote(item.id).catch(reportError);
      });
      row.append(checkbox, content, button("↕", "icon-button", function moveNoteClick() { showMoveDialog("note", item); }, "移动便签“" + item.title + "”"));
      installDraggable(row, "note", item);
      installDropTarget(row, "note", item.categoryId, item.id);
      target.append(row);
    }
  }

  // 创建带标签的表单字段，返回输入节点供调用方读取。
  function formField(body, label, id, tag = "input") {
    const wrapper = element("label", "form-field");
    wrapper.htmlFor = id;
    wrapper.append(element("span", "", label));
    const input = element(tag);
    input.id = id;
    input.name = id;
    wrapper.append(input);
    body.append(wrapper);
    return input;
  }

  // 打开原生可访问模态对话框，提交成功后关闭、失败时原地显示错误。
  function showDialog(title, body, submit, submitText = "保存", danger = false) {
    if (byId("app-dialog").open) return;
    state.dialog = { submit, busy: false }; // 保存提交回调和重复提交保护。
    byId("dialog-title").textContent = title;
    byId("dialog-body").replaceChildren(body);
    byId("dialog-error").hidden = true;
    byId("dialog-submit").textContent = submitText;
    byId("dialog-submit").className = "button primary" + (danger ? " danger" : "");
    byId("app-dialog").setAttribute("aria-labelledby", "dialog-title");
    byId("app-dialog").showModal();
    const firstField = body.querySelector("input,select,textarea");
    if (firstField) firstField.focus(); else byId("dialog-cancel").focus();
  }

  // 关闭空闲对话框，后端提交过程中不允许中途取消。
  function closeDialog() {
    if (state.dialog && state.dialog.busy) return;
    byId("app-dialog").close();
    state.dialog = null;
  }

  // 提交当前模态表单并准确显示后端错误。
  async function submitDialog(event) {
    event.preventDefault();
    if (!state.dialog || state.dialog.busy || !byId("dialog-form").reportValidity()) return;
    const current = state.dialog;
    current.busy = true;
    byId("dialog-error").hidden = true;
    for (const id of ["dialog-submit", "dialog-cancel", "dialog-close"]) byId(id).disabled = true;
    try {
      await current.submit();
      current.busy = false;
      closeDialog();
    } catch (error) {
      byId("dialog-error").textContent = error.message || "操作失败，请重试。";
      byId("dialog-error").hidden = false;
    } finally {
      current.busy = false;
      for (const id of ["dialog-submit", "dialog-cancel", "dialog-close"]) byId(id).disabled = false;
    }
  }

  // 新建或编辑快捷方式，文件和目录交给 Qt 原生对话框选择。
  function editShortcut(item) {
    const body = element("div");
    const title = formField(body, "名称", "shortcut-title");
    title.required = true; title.maxLength = 200; title.value = item ? item.title : "";
    const type = formField(body, "类型", "shortcut-type", "select");
    ["目录", "网址", "文件"].forEach(function appendShortcutType(name, index) {
      const option = element("option", "", name); option.value = String(index); type.append(option);
    });
    type.value = String(item ? item.type : 0);
    const target = formField(body, "地址", "shortcut-target");
    target.required = true; target.value = item ? item.target : ""; target.placeholder = "文件路径、目录路径或 https://网址";
    const targetWrapper = element("div", "input-with-button");
    target.parentNode.append(targetWrapper);
    targetWrapper.append(target);
    const choose = button("选择…", "button secondary", async function chooseTarget(event, source) {
      await perform(async function invokeChooseTarget() {
        const result = await window.desktopBridge.call("chooseTarget", Number(type.value));
        if (!result || result.cancelled) return;
        target.value = result.target || "";
        if (!title.value.trim()) title.value = result.title || "";
      }, "", source);
    });
    choose.hidden = Number(type.value) === 1;
    type.addEventListener("change", function shortcutTypeChanged() { choose.hidden = Number(type.value) === 1; });
    targetWrapper.append(choose);
    const category = formField(body, "分类", "shortcut-category", "select");
    fillCategorySelect(category, "shortcut", item ? item.categoryId : (state.shortcutCategory || defaultCategory("shortcut")));
    showDialog(item ? "编辑快捷方式" : "添加快捷方式", body, async function saveShortcutForm() {
      if (!title.value.trim() || !target.value.trim()) throw new Error("请填写名称和地址。");
      await window.desktopBridge.call("saveShortcut", {
        id: item ? item.id : "", // 空 ID 请求后端创建新记录。
        categoryId: category.value, // 归属分类的稳定 ID。
        title: title.value.trim(), // 显示名称。
        target: target.value.trim(), // 本地路径或网页 URL。
        type: Number(type.value) // 0 目录、1 网址、2 文件。
      });
      await refreshShortcuts();
      toast(item ? "快捷方式已更新" : "快捷方式已添加");
    });
  }

  // 新建或重命名分类；默认分类不提供编辑入口。
  function editCategory(kind, category) {
    const body = element("div");
    const name = formField(body, "分类名称", "category-name");
    name.required = true; name.maxLength = 100; name.value = category ? category.name : "";
    showDialog(category ? "重命名分类" : "新建分类", body, async function saveCategoryForm() {
      if (!name.value.trim()) throw new Error("请输入分类名称。");
      await window.desktopBridge.call(kind === "shortcut" ? "saveShortcutCategory" : "saveNoteCategory", category ? category.id : "", name.value.trim());
      if (kind === "shortcut") await refreshShortcuts(); else await refreshNotes();
      toast(category ? "分类名称已更新" : "分类已创建");
    });
  }

  // 删除分类时明确说明条目移到默认分类，便签内容不会被删除。
  function deleteCategory(kind, category) {
    if (category.id === defaultCategory(kind)) return;
    if (kind === "note" && (state.noteSaving || state.imageImporting)) { toast("便签正在保存或导入图片，请稍后再删除分类。", "warning"); return; }
    if (kind === "note" && state.noteDirty && byId("note-category").value === category.id) {
      toast("请先保存当前便签，再删除它所在的分类。", "warning"); return;
    }
    const body = element("div");
    body.append(element("p", "dialog-description", "删除分类“" + category.name + "”？\n分类内的" + (kind === "note" ? "便签" : "快捷方式") + "将移入默认分类，内容会保留。"));
    showDialog("删除分类", body, async function confirmCategoryDeletion() {
      await window.desktopBridge.call(kind === "shortcut" ? "deleteShortcutCategory" : "deleteNoteCategory", category.id);
      if (kind === "shortcut") await refreshShortcuts(); else {
        if (state.note && state.note.categoryId === category.id) state.note.categoryId = defaultCategory("note");
        await refreshNotes();
      }
      toast("分类已删除，内容已移入默认分类");
    }, "删除分类", true);
  }

  // 确认单项或批量删除，正在编辑的便签在成功后才清空。
  function deleteItems(kind, ids) {
    if (!ids.length) return;
    if (kind === "note" && (state.noteSaving || state.imageImporting)) { toast("便签正在保存或导入图片，请稍后再删除。", "warning"); return; }
    const includesCurrent = kind === "note" && state.note && ids.includes(state.note.id);
    const body = element("div");
    body.append(element("p", "dialog-description", "确定删除 " + ids.length + (kind === "note" ? " 篇便签" : " 个快捷方式") + "？" + (includesCurrent && state.noteDirty ? "\n当前便签尚未保存的内容也会丢弃。" : "") + (kind === "note" ? "\n此操作无法撤销。" : "\n原始文件和目录不会被删除。")));
    showDialog("删除确认", body, async function confirmItemDeletion() {
      await window.desktopBridge.call(kind === "note" ? "deleteNotes" : "deleteShortcuts", ids);
      for (const id of ids) state[kind + "Selected"].delete(id);
      if (includesCurrent) clearNoteEditor();
      if (kind === "note") await refreshNotes(); else await refreshShortcuts();
      toast("已删除 " + ids.length + " 项");
    }, "确认删除", true);
  }

  // 提供鼠标拖动以外的可访问移动与排序入口。
  function showMoveDialog(kind, item) {
    if (kind === "note" && state.note && state.note.id === item.id && state.noteDirty) {
      toast("请先保存当前便签，再移动它。", "warning"); return;
    }
    const body = element("div");
    body.append(element("p", "dialog-description", "移动“" + item.title + "”"));
    const category = formField(body, "目标分类", "move-category", "select");
    fillCategorySelect(category, kind, item.categoryId);
    const before = formField(body, "排列位置", "move-before", "select");
    // 根据所选分类构建插入位置。
    function updatePositions() {
      before.replaceChildren();
      const last = element("option", "", "放在最后"); last.value = ""; before.append(last);
      for (const candidate of collection(kind).items) {
        if (candidate.id === item.id || candidate.categoryId !== category.value) continue;
        const option = element("option", "", "放在“" + candidate.title + "”之前"); option.value = candidate.id; before.append(option);
      }
    }
    category.addEventListener("change", updatePositions);
    updatePositions();
    showDialog("移动与排序", body, async function saveMovement() { await moveItem(kind, item.id, category.value, before.value); }, "移动");
  }

  // 将移动请求发给 Qt，并同步当前便签的分类信息。
  async function moveItem(kind, id, categoryId, beforeId) {
    if (id === beforeId) return;
    if (kind === "note" && state.note && state.note.id === id && (state.noteDirty || state.noteSaving || state.imageImporting)) {
      throw new Error("请等待图片导入完成并保存当前便签，再移动它。");
    }
    await window.desktopBridge.call(kind === "shortcut" ? "moveShortcut" : "moveNote", id, categoryId, beforeId || "");
    if (kind === "shortcut") await refreshShortcuts(); else {
      if (state.note && state.note.id === id) {
        state.note.categoryId = categoryId;
        byId("note-category").value = categoryId;
      }
      await refreshNotes();
    }
    toast("位置已更新");
  }

  // 注册仅在当前页面内部使用的条目拖动。
  function installDraggable(node, kind, item) {
    node.draggable = true;
    node.title = "可拖动排序，也可使用移动按钮";
    node.addEventListener("dragstart", function itemDragStarted(event) {
      if (event.target.closest("input,select")) { event.preventDefault(); return; }
      state.drag = { kind, id: item.id }; // 拖动的数据只记录类型和稳定 ID。
      event.dataTransfer.effectAllowed = "move";
      event.dataTransfer.setData("text/plain", item.id);
      node.classList.add("dragging");
    });
    node.addEventListener("dragend", function itemDragEnded() {
      state.drag = null;
      node.classList.remove("dragging");
      document.querySelectorAll(".drag-over").forEach(function clearDropMark(target) { target.classList.remove("drag-over"); });
    });
  }

  // 只接受本页面同类型条目，拒绝外部文件或网页拖入列表。
  function installDropTarget(node, kind, categoryId, beforeId) {
    node.addEventListener("dragover", function itemDraggedOver(event) {
      if (!state.drag || state.drag.kind !== kind || state.drag.id === beforeId) return;
      event.preventDefault(); event.stopPropagation();
      event.dataTransfer.dropEffect = "move";
      node.classList.add("drag-over");
    });
    node.addEventListener("dragleave", function itemDraggedAway(event) {
      if (!node.contains(event.relatedTarget)) node.classList.remove("drag-over");
    });
    node.addEventListener("drop", function itemDropped(event) {
      node.classList.remove("drag-over");
      if (!state.drag || state.drag.kind !== kind) return;
      event.preventDefault(); event.stopPropagation();
      const dragged = state.drag; state.drag = null;
      moveItem(kind, dragged.id, categoryId, beforeId).catch(reportError);
    });
  }

  // 确认切换前是否可以放弃便签草稿；保存期间保持原页面。
  function canLeaveNote() {
    if (state.noteSaving) { toast("便签正在保存，请稍候。", "warning"); return false; }
    if (state.imageImporting) { toast("图片正在导入，请完成后再切换或关闭。", "warning"); return false; }
    return !state.noteDirty || window.confirm("当前便签有未保存的修改。确定放弃这些修改吗？");
  }

  // 离开设置或退出前保留修改确认，取消时不改变下载目录等草稿字段。
  function canLeaveSettings() {
    if (state.settingsSaving) { toast("设置正在保存，请稍候。", "warning"); return false; }
    return !state.settingsDirty || window.confirm("设置中有未保存的修改。确定放弃这些修改吗？");
  }

  // 清空编辑器，同时使尚在等待的加载请求失效。
  function clearNoteEditor() {
    ++state.noteLoadVersion;
    state.note = null; state.noteDirty = false; state.savedRange = null;
    byId("note-title").value = ""; byId("note-editor").replaceChildren();
    byId("note-editor-wrap").hidden = true; byId("editor-empty").hidden = false;
    renderNotes();
  }

  // 切换侧栏页面，未保存便签必须先确认。
  function navigate(page) {
    if (page === state.page || !["shortcuts", "notes", "emoji", "downloads", "clipboard", "wallpaper", "gridmap", "settings"].includes(page)) return;
    if (state.page === "notes") {
      if (!canLeaveNote()) return;
      if (state.noteDirty) clearNoteEditor();
    }
    if (state.page === "settings") {
      if (!canLeaveSettings()) return;
      if (state.settingsDirty) applySettings(state.settings);
    }
    if (state.page === "gridmap" && !window.desktopGridmap.canLeave()) return;
    state.page = page;
    for (const name of ["shortcuts", "notes", "emoji", "downloads", "clipboard", "wallpaper", "gridmap", "settings"]) byId("page-" + name).hidden = name !== page;
    document.querySelectorAll(".nav-button").forEach(function updateNavigation(node) {
      const active = node.dataset.page === page;
      node.classList.toggle("active", active);
      if (active) node.setAttribute("aria-current", "page"); else node.removeAttribute("aria-current");
    });
    // 新页面从顶部开始，兼容文档滚动与独立主内容滚动容器。
    byId("main-content").scrollTop = 0;
    window.scrollTo({ top: 0, left: 0, behavior: "instant" });
  }

  // 允许保留的富文本标签；危险元素连同内容一起移除。
  const allowedTags = new Set(["p", "div", "span", "br", "b", "strong", "i", "em", "u", "s", "strike", "ul", "ol", "li", "h1", "h2", "h3", "h4", "h5", "h6", "blockquote", "pre", "code", "a", "font", "table", "thead", "tbody", "tfoot", "tr", "td", "th", "hr", "img", "sup", "sub"]);
  const blockedTags = new Set(["script", "style", "iframe", "object", "embed", "link", "meta", "base", "svg", "math", "form", "input", "button", "select", "textarea", "video", "audio", "source", "template", "noscript"]);
  // 仅允许基础排版样式，所有值单独检查，拒绝资源 URL 和表达式。
  const styleRules = {
    "color": /^(#[0-9a-f]{3,8}|[a-z]+|rgba?\([\d.,%\s]+\)|hsla?\([\d.,%\s]+\))$/i,
    "background-color": /^(#[0-9a-f]{3,8}|[a-z]+|rgba?\([\d.,%\s]+\)|hsla?\([\d.,%\s]+\))$/i,
    "font-weight": /^(normal|bold|bolder|lighter|[1-9]00)$/i,
    "font-style": /^(normal|italic|oblique)$/i,
    "text-decoration": /^(none|underline|line-through|overline)(\s+(underline|line-through|overline))*$/i,
    "text-align": /^(left|right|center|justify|start|end)$/i,
    "white-space": /^(normal|pre|pre-wrap|pre-line|break-spaces)$/i,
    "font-family": /^[\w\s,'"\-\u3000-\u9fff]{1,160}$/,
    "font-size": /^(\d{1,2}(\.\d+)?(px|pt)|[12](\.\d+)?em|small|medium|large|x-large)$/i,
    "vertical-align": /^(baseline|sub|super|middle|top|bottom)$/i
  };

  // 检查图片必须是内嵌栅格图像，禁止 SVG、文件路径和外部网络资源。
  function safeImageSource(value) {
    if (typeof value !== "string") return false;
    if (value.length > 16 * 1024 * 1024) throw new Error("便签中有一张图片数据超过 16 MB，操作已取消，原有内容未被截断。请缩小图片后重试。");
    const valid = /^data:image\/(png|jpeg|gif|webp);base64,[a-z0-9+/\s]+=*$/i.test(value);
    if (/^data:image\//i.test(value) && !valid) throw new Error("便签包含无效或不支持的内嵌图片，操作已取消。请使用 PNG、JPEG、GIF 或 WebP 图片。");
    return valid;
  }

  // 将不可信的便签 HTML 解析到离线模板，再逐节点复制到白名单片段。
  function sanitizeHtml(html) {
    const template = document.createElement("template");
    template.innerHTML = String(html || ""); // 模板内容不执行；输出只使用下方重新创建的白名单节点。
    const fragment = document.createDocumentFragment();
    let remaining = 50000; // 限制异常巨大结构，避免恶意深层内容阻塞界面。
    // 递归清理元素，属性默认全部丢弃，只恢复明确允许的格式。
    function copySafeNode(source, destination, depth) {
      if (--remaining < 0) throw new Error("便签内容超过 50000 个节点，操作已取消，原有内容保持完整。请分成多篇便签后重试。");
      if (depth > 100) throw new Error("便签内容嵌套超过 100 层，操作已取消，原有内容保持完整。请简化文本格式后重试。");
      if (source.nodeType === Node.TEXT_NODE) { destination.append(document.createTextNode(source.textContent)); return; }
      if (source.nodeType !== Node.ELEMENT_NODE) return;
      const tag = source.tagName.toLowerCase();
      if (blockedTags.has(tag)) return;
      if (!allowedTags.has(tag)) {
        for (const child of source.childNodes) copySafeNode(child, destination, depth + 1);
        return;
      }
      if (tag === "img" && !safeImageSource(source.getAttribute("src"))) return;
      const target = document.createElement(tag === "font" ? "span" : tag);
      // Qt 导出常用全局 pre-wrap 样式；转为内联以保留连续空格和代码缩进。
      if (tag === "p" || tag === "li" || tag === "pre") target.style.whiteSpace = "pre-wrap";
      for (const property of Object.keys(styleRules)) {
        const value = source.style.getPropertyValue(property).trim();
        if (value && styleRules[property].test(value)) target.style.setProperty(property, value);
      }
      if (tag === "font") {
        const color = source.getAttribute("color") || "";
        const face = source.getAttribute("face") || "";
        if (styleRules.color.test(color)) target.style.color = color;
        if (styleRules["font-family"].test(face)) target.style.fontFamily = face;
        const size = Number(source.getAttribute("size"));
        if (size >= 1 && size <= 7) target.style.fontSize = [0, 10, 13, 16, 18, 24, 32, 48][size] + "px";
      }
      const align = source.getAttribute("align") || "";
      if (/^(left|right|center|justify)$/i.test(align)) target.style.textAlign = align;
      if (tag === "td" || tag === "th") {
        for (const attribute of ["colspan", "rowspan"]) {
          const value = Number(source.getAttribute(attribute));
          if (Number.isInteger(value) && value > 0 && value <= 100) target.setAttribute(attribute, String(value));
        }
      }
      if (tag === "ol") {
        const start = Number(source.getAttribute("start"));
        if (Number.isInteger(start) && start > 0 && start <= 10000) target.setAttribute("start", String(start));
      }
      if (tag === "img") {
        target.src = source.getAttribute("src").replace(/\s/g, "");
        target.alt = (source.getAttribute("alt") || "便签图片").slice(0, 200);
        const width = Number(source.getAttribute("width"));
        if (width > 0 && width <= 4096) target.width = Math.round(width);
      }
      for (const child of source.childNodes) copySafeNode(child, target, depth + 1);
      destination.append(target);
    }
    for (const child of template.content.childNodes) copySafeNode(child, fragment, 0);
    return fragment;
  }

  // 对编辑器内容再次清理后序列化，Qt 永远不会接收未过滤的粘贴 HTML。
  function serializedNoteHtml() {
    const container = document.createElement("div");
    container.append(sanitizeHtml(byId("note-editor").innerHTML));
    return container.innerHTML;
  }

  // 将便签记录填充到编辑器，不直接插入来自后端的 HTML 字符串。
  function showNote(note, isNew) {
    // 先完整构建安全片段，校验失败时保留原便签和编辑器内容。
    const safeContent = sanitizeHtml(note.html || "");
    state.note = { id: note.id || "", title: note.title || "", categoryId: note.categoryId || defaultCategory("note") };
    state.noteDirty = Boolean(isNew); state.noteRevision += 1; state.savedRange = null;
    byId("note-title").value = note.title || "";
    fillCategorySelect(byId("note-category"), "note", state.note.categoryId);
    byId("note-editor").replaceChildren(safeContent);
    byId("editor-empty").hidden = true; byId("note-editor-wrap").hidden = false;
    byId("delete-current-note").disabled = !state.note.id;
    updateNoteState(); renderNotes();
  }

  // 加载便签之前确认草稿，使用版本号避免快速切换时覆盖新编辑器。
  async function openNote(id) {
    if (state.note && state.note.id === id) return;
    if (!canLeaveNote()) return;
    const version = ++state.noteLoadVersion;
    const revision = state.noteRevision;
    const data = await window.desktopBridge.call("getNote", id);
    if (version !== state.noteLoadVersion) return;
    if (revision !== state.noteRevision) throw new Error("加载期间当前便签有新的修改，已为你保留。请保存后再切换。");
    if (!data || data.id !== id) throw new Error("便签内容加载失败。");
    showNote(data, false);
  }

  // 新建一个明确未保存的本地草稿。
  function createNote() {
    if (!canLeaveNote()) return;
    ++state.noteLoadVersion;
    showNote({ id: "", title: "", categoryId: state.noteCategory || defaultCategory("note"), html: "" }, true);
    byId("note-title").focus();
  }

  // 更新字数与保存状态提示。
  function updateNoteState() {
    const indicator = byId("note-save-state");
    indicator.textContent = state.imageImporting ? "正在导入图片…" : (state.noteSaving ? "正在保存…" : (state.noteDirty ? "未保存的修改" : "已保存"));
    indicator.classList.toggle("dirty", state.noteDirty);
    byId("save-note").disabled = state.noteSaving || Boolean(state.imageImporting) || !state.note;
    byId("delete-current-note").disabled = !state.note || !state.note.id || state.noteSaving || Boolean(state.imageImporting);
    byId("insert-note-image").disabled = state.noteSaving || Boolean(state.imageImporting) || !state.note;
    byId("note-character-count").textContent = byId("note-editor").textContent.replace(/\s/g, "").length + " 字";
  }

  // 标记一次编辑，在异步保存期间也保留最新输入的脏状态。
  function markNoteDirty() {
    if (!state.note) return;
    state.noteDirty = true; state.noteRevision += 1;
    updateNoteState();
  }

  // 保存便签快照；输入期间完成的旧保存不会错误清除新修改。
  async function saveCurrentNote() {
    if (!state.note || state.noteSaving) return;
    if (state.imageImporting) throw new Error("图片正在导入，请完成后再保存便签。");
    const title = byId("note-title").value.trim();
    if (!title) { byId("note-title").focus(); throw new Error("请先填写便签标题。"); }
    const revision = state.noteRevision;
    const current = state.note;
    const draft = { id: current.id, title, categoryId: byId("note-category").value, html: serializedNoteHtml() };
    state.noteSaving = true; updateNoteState();
    try {
      const saved = await window.desktopBridge.call("saveNote", draft);
      if (!saved || !saved.id) throw new Error("保存结果缺少便签编号，请重试。");
      current.id = saved.id; current.title = title; current.categoryId = draft.categoryId;
      if (state.noteRevision === revision) state.noteDirty = false;
      byId("delete-current-note").disabled = false;
      await refreshNotes();
      toast(state.noteDirty ? "已保存，后续修改仍待保存" : "便签已保存");
    } finally {
      state.noteSaving = false; updateNoteState();
    }
  }

  // 记住编辑器中的光标和选区，供工具栏与图片按钮使用。
  function rememberSelection() {
    const selection = window.getSelection();
    if (!selection || !selection.rangeCount) return;
    const range = selection.getRangeAt(0);
    if (byId("note-editor").contains(range.commonAncestorContainer)) state.savedRange = range.cloneRange();
  }

  // 恢复编辑器有效选区，失效时将光标放到末尾。
  function restoreSelection() {
    const editor = byId("note-editor");
    editor.focus();
    const selection = window.getSelection();
    let range = state.savedRange;
    if (!range || !editor.contains(range.commonAncestorContainer)) {
      range = document.createRange(); range.selectNodeContents(editor); range.collapse(false);
    }
    selection.removeAllRanges(); selection.addRange(range);
  }

  // 在光标处插入已清理内容，并保留浏览器撤销栈。
  function insertSafeFragment(fragment) {
    restoreSelection();
    const container = document.createElement("div"); container.append(fragment);
    document.execCommand("insertHTML", false, container.innerHTML);
    rememberSelection(); markNoteDirty();
  }

  // 将本地选择或粘贴的图片读成内嵌数据，拒绝非栅格格式和过大图片。
  async function insertImageFiles(files) {
    if (!state.note) return;
    if (state.noteSaving) throw new Error("便签正在保存，请完成后再导入图片。");
    if (state.imageImporting) throw new Error("已有图片正在导入，请完成后再添加图片。");
    if (!files.length) return;
    // 导入前校验整个选择，避免只插入一部分而遗漏不支持的文件。
    for (const file of files) {
      if (!/^image\/(png|jpeg|gif|webp)$/i.test(file.type)) throw new Error("只支持 PNG、JPEG、GIF 和 WebP 图片，所选图片尚未插入。");
      if (file.size > 10 * 1024 * 1024) throw new Error("单张图片不能超过 10 MB，所选图片尚未插入。请缩小图片后重试。");
    }
    const currentNote = state.note;
    const loadVersion = ++state.noteLoadVersion; // 使导入开始前尚未完成的便签切换失效。
    state.imageImporting += 1;
    updateNoteState();
    try {
      const fragment = document.createDocumentFragment();
      for (const file of files) {
        const data = await new Promise(function readImage(resolve, reject) {
          const reader = new FileReader();
          reader.onload = function imageRead() { resolve(reader.result); };
          reader.onerror = function imageReadFailed() { reject(new Error("图片读取失败，所选图片尚未插入。")); };
          reader.onabort = function imageReadAborted() { reject(new Error("图片读取已取消，所选图片尚未插入。")); };
          reader.readAsDataURL(file);
        });
        if (currentNote !== state.note || loadVersion !== state.noteLoadVersion) throw new Error("当前便签已切换，图片导入已取消，图片没有插入其他便签。");
        if (!safeImageSource(data)) throw new Error("图片内容无法识别，所选图片尚未插入。");
        const image = document.createElement("img"); image.src = data; image.alt = file.name || "粘贴的图片";
        fragment.append(image, document.createElement("br"));
      }
      // 全部图片读取成功后一次插入，让错误不会留下半批导入结果。
      insertSafeFragment(fragment);
    } finally {
      state.imageImporting = Math.max(0, state.imageImporting - 1);
      updateNoteState();
    }
  }

  // 拦截粘贴；图片直接内嵌，富文本先白名单过滤，纯文本保留换行。
  function pasteIntoNote(event) {
    event.preventDefault();
    const clipboard = event.clipboardData;
    if (!clipboard) return;
    rememberSelection();
    const images = Array.from(clipboard.files || []).filter(function isClipboardImage(file) { return file.type.startsWith("image/"); });
    if (images.length) { insertImageFiles(images).catch(reportError); return; }
    const html = clipboard.getData("text/html");
    if (html) { insertSafeFragment(sanitizeHtml(html)); return; }
    restoreSelection();
    document.execCommand("insertText", false, clipboard.getData("text/plain"));
    rememberSelection(); markNoteDirty();
  }

  // 应用基础富文本命令，命令名称来自静态工具栏。
  function applyEditorCommand(command, value) {
    if (!state.note) return;
    restoreSelection();
    document.execCommand(command, false, value || null);
    rememberSelection(); markNoteDirty();
  }

  // 把 Windows 虚拟键码转为用户可读名称。
  function keyName(key) {
    if (key >= 112 && key <= 135) return "F" + (key - 111);
    if (key >= 65 && key <= 90) return String.fromCharCode(key);
    if (key >= 48 && key <= 57) return String.fromCharCode(key);
    if (key >= 96 && key <= 105) return "Num" + (key - 96);
    const names = { 8: "Backspace", 9: "Tab", 13: "Enter", 19: "Pause", 20: "CapsLock", 27: "Esc", 32: "Space", 33: "PageUp", 34: "PageDown", 35: "End", 36: "Home", 37: "Left", 38: "Up", 39: "Right", 40: "Down", 44: "PrintScreen", 45: "Insert", 46: "Delete", 106: "Num*", 107: "Num+", 109: "Num-", 110: "Num.", 111: "Num/", 144: "NumLock", 145: "ScrollLock", 186: ";", 187: "=", 188: ",", 189: "-", 190: ".", 191: "/", 192: "`", 219: "[", 220: "\\", 221: "]", 222: "'" };
    return names[key] || "0x" + Number(key).toString(16).toUpperCase();
  }

  // 格式化 Alt=1、Ctrl=2、Shift=4、Win=8 的 Windows 热键组合。
  function hotkeyName(modifier, key) {
    return ((modifier & 2) ? "Ctrl+" : "") + ((modifier & 1) ? "Alt+" : "") + ((modifier & 4) ? "Shift+" : "") + ((modifier & 8) ? "Win+" : "") + keyName(key);
  }

  // 通过 KeyboardEvent.code 将物理按键映射到 Windows 虚拟键码。
  function virtualKey(event) {
    if (/^Key[A-Z]$/.test(event.code)) return event.code.charCodeAt(3);
    if (/^Digit[0-9]$/.test(event.code)) return event.code.charCodeAt(5);
    if (/^Numpad[0-9]$/.test(event.code)) return 96 + Number(event.code.slice(6));
    if (/^F([1-9]|1[0-9]|2[0-4])$/.test(event.code)) return 111 + Number(event.code.slice(1));
    const keys = { Backspace: 8, Enter: 13, NumpadEnter: 13, Pause: 19, CapsLock: 20, Escape: 27, Space: 32, PageUp: 33, PageDown: 34, End: 35, Home: 36, ArrowLeft: 37, ArrowUp: 38, ArrowRight: 39, ArrowDown: 40, PrintScreen: 44, Insert: 45, Delete: 46, NumpadMultiply: 106, NumpadAdd: 107, NumpadSubtract: 109, NumpadDecimal: 110, NumpadDivide: 111, NumLock: 144, ScrollLock: 145, Semicolon: 186, Equal: 187, Comma: 188, Minus: 189, Period: 190, Slash: 191, Backquote: 192, BracketLeft: 219, Backslash: 220, BracketRight: 221, Quote: 222 };
    return keys[event.code] || 0;
  }

  // 捕获热键但保留 Tab 键的键盘导航能力。
  function captureHotkey(event) {
    if (event.key === "Tab") return;
    event.preventDefault(); event.stopPropagation();
    const key = virtualKey(event);
    if (!key || event.repeat) return;
    const modifier = (event.altKey ? 1 : 0) | (event.ctrlKey ? 2 : 0) | (event.shiftKey ? 4 : 0) | (event.metaKey ? 8 : 0);
    state.hotkeyDraft = { modifier, key };
    byId("hotkey-input").value = hotkeyName(modifier, key);
    markSettingsDirty();
  }

  // 字号仅预览界面样式，保存成功后再更新设置缓存。
  function previewFont() {
    const fontSize = Number(byId("font-size").value);
    if (Number.isInteger(fontSize) && fontSize >= 8 && fontSize <= 32) document.documentElement.style.setProperty("--list-font-size", fontSize + "px");
  }

  // 标记设置表单有未提交修改。
  function markSettingsDirty() {
    state.settingsDirty = true;
    byId("settings-save-state").textContent = "尚未保存";
    byId("settings-save-state").classList.add("dirty");
  }

  // 使用后端真实设置填充表单和全局字号。
  function applySettings(settings) {
    if (!settings || !Number.isFinite(Number(settings.fontSize))) throw new Error("系统设置数据不完整。");
    state.settings = settings;
    state.settingsDirty = false;
    state.hotkeyDraft = { modifier: Number(settings.hotkeyModifier), key: Number(settings.hotkeyKey) };
    byId("font-size").value = String(settings.fontSize);
    byId("auto-start").checked = settings.autoStart === true;
    byId("hotkey-input").value = hotkeyName(state.hotkeyDraft.modifier, state.hotkeyDraft.key);
    byId("download-default-directory").value = String(settings.downloadDirectory || "");
    window.desktopDownloads.setDefaultDirectory(settings.downloadDirectory);
    byId("settings-save-state").textContent = "";
    byId("settings-save-state").classList.remove("dirty");
    previewFont();
  }

  // 读取应用版本、本地数据目录和热键注册警告。
  async function refreshAppInfo() {
    const info = await window.desktopBridge.call("getAppInfo");
    byId("app-version").textContent = info && info.version ? "v" + info.version : "";
    byId("data-path").textContent = info && info.dataPath ? info.dataPath : "无法读取数据目录";
    const warning = info && info.hotkeyWarning ? String(info.hotkeyWarning) : "";
    byId("hotkey-warning").textContent = warning;
    byId("hotkey-warning").hidden = !warning;
  }

  // 保存字体、热键、下载目录和开机自启，并刷新原生注册结果。
  async function saveSettings(event) {
    event.preventDefault();
    if (state.settingsSaving || !byId("settings-form").reportValidity()) return;
    const fontSize = Number(byId("font-size").value);
    if (!Number.isInteger(fontSize) || fontSize < 8 || fontSize > 32) throw new Error("字体大小需要是 8–32 之间的整数。");
    state.settingsSaving = true;
    byId("settings-form").inert = true;
    try {
      const settings = await window.desktopBridge.call("saveSettings", {
        fontSize, // 列表与便签的字号。
        hotkeyModifier: state.hotkeyDraft.modifier, // Windows 热键修饰键组合。
        hotkeyKey: state.hotkeyDraft.key, // Windows 主键虚拟码。
        autoStart: byId("auto-start").checked, // 当前用户登录 Windows 时是否自动启动。
        downloadDirectory: byId("download-default-directory").value.trim() // 用户提交的默认下载目录。
      });
      applySettings(settings);
      byId("settings-save-state").textContent = "已保存";
      await refreshAppInfo();
      toast("设置已保存");
    } finally { state.settingsSaving = false; byId("settings-form").inert = false; }
  }

  // 恢复默认设置前给出具体说明。
  function resetSettings() {
    if (state.settingsSaving) return;
    const body = element("div");
    body.append(element("p", "dialog-description", "恢复为 16 px 字体、F8 唤起快捷键和系统默认下载目录，并关闭开机自启？\n已有快捷方式、便签和下载文件不会改变。"));
    showDialog("恢复默认设置", body, async function confirmResetSettings() {
      state.settingsSaving = true;
      try {
        const settings = await window.desktopBridge.call("resetSettings");
        applySettings(settings || await window.desktopBridge.call("getSettings"));
        await refreshAppInfo();
        toast("已恢复默认设置");
      } finally { state.settingsSaving = false; }
    }, "恢复默认");
  }

  // 合并后端相邻信号，避免列表操作时重复重绘。
  function scheduleRefresh(name, callback) {
    window.clearTimeout(state.signalTimers.get(name));
    state.signalTimers.set(name, window.setTimeout(function executeScheduledRefresh() {
      state.signalTimers.delete(name);
      Promise.resolve().then(callback).catch(reportError);
    }, 60));
  }

  // 连接后端并完成初始数据读取；失败时显示桌面启动说明。
  async function connectApplication() {
    byId("retry-connection").disabled = true;
    try {
      await window.desktopBridge.connect();
      await Promise.all([refreshShortcuts(), refreshNotes(), window.desktopBridge.call("getSettings").then(applySettings), refreshAppInfo(), window.desktopDownloads.initialize({ toast, reportError }), window.desktopClipboard.initialize({ toast, reportError }), window.desktopEmoji.initialize({ toast, reportError }), window.desktopWallpaper.initialize({ toast, reportError }), window.desktopGridmap.initialize({ toast, reportError })]);
      if (!state.connected) {
        await window.desktopBridge.on("shortcutsChanged", function shortcutsChanged() { scheduleRefresh("shortcuts", refreshShortcuts); });
        await window.desktopBridge.on("notesChanged", function notesChanged() { scheduleRefresh("notes", refreshNotes); });
        await window.desktopBridge.on("settingsChanged", function settingsChanged() {
          if (!state.settingsDirty && !state.settingsSaving) scheduleRefresh("settings", async function reloadSettings() {
            const settings = await window.desktopBridge.call("getSettings");
            if (!state.settingsDirty && !state.settingsSaving) applySettings(settings);
            await refreshAppInfo();
          });
        });
      }
      state.connected = true;
      byId("workspace").inert = false;
      byId("connection-error").hidden = true;
      byId("connection-dot").classList.add("connected");
      byId("connection-status").textContent = "本地已连接";
      document.documentElement.dataset.ready = "true";
      document.dispatchEvent(new CustomEvent("desktop-ready"));
    } catch (error) {
      byId("connection-error").hidden = false;
      byId("connection-status").textContent = "未连接桌面程序";
      if (window.qt) byId("connection-error").querySelector("p").textContent = error.message + " 可尝试重新连接或重启桌面工具。";
    } finally { byId("retry-connection").disabled = false; }
  }

  // 绑定固定元素的事件，统一报告 Promise 异常。
  function listen(id, eventName, handler) {
    byId(id).addEventListener(eventName, function invokeStaticHandler(event) {
      try { Promise.resolve(handler(event)).catch(reportError); } catch (error) { reportError(error); }
    });
  }

  // 页面生命周期和固定控件只绑定一次。
  function installEvents() {
    document.addEventListener("desktop-busy", function busyChanged(event) {
      if (!event.detail) {
        window.clearTimeout(state.busyTimer); state.busyTimer = null;
        byId("busy-indicator").hidden = true;
      } else if (state.busyTimer === null && byId("busy-indicator").hidden) {
        state.busyTimer = window.setTimeout(function showDelayedBusyState() {
          state.busyTimer = null;
          byId("busy-indicator").hidden = false;
        }, 150);
      }
    });
    document.querySelectorAll(".nav-button").forEach(function installNavigation(node) { node.addEventListener("click", function navigationClicked() { navigate(node.dataset.page); }); });
    listen("retry-connection", "click", connectApplication);
    listen("add-shortcut", "click", function newShortcutClicked() { editShortcut(null); });
    listen("add-shortcut-category", "click", function newShortcutCategoryClicked() { editCategory("shortcut", null); });
    listen("add-note-category", "click", function newNoteCategoryClicked() { editCategory("note", null); });
    listen("shortcut-search", "input", function shortcutSearchChanged() { state.shortcutSelected.clear(); renderShortcuts(); });
    listen("note-search", "input", function noteSearchChanged() { state.noteSelected.clear(); renderNotes(); });
    listen("shortcut-select-all", "change", function selectAllShortcuts(event) {
      for (const item of visibleShortcuts()) { if (event.target.checked) state.shortcutSelected.add(item.id); else state.shortcutSelected.delete(item.id); }
      renderShortcuts();
    });
    listen("note-select-all", "change", function selectAllNotes(event) {
      for (const item of visibleNotes()) { if (event.target.checked) state.noteSelected.add(item.id); else state.noteSelected.delete(item.id); }
      renderNotes();
    });
    listen("delete-selected-shortcuts", "click", function deleteSelectedShortcuts() { deleteItems("shortcut", Array.from(state.shortcutSelected)); });
    listen("delete-selected-notes", "click", function deleteSelectedNotes() { deleteItems("note", Array.from(state.noteSelected)); });
    listen("add-note", "click", createNote); listen("empty-add-note", "click", createNote);
    listen("note-title", "input", markNoteDirty); listen("note-category", "change", markNoteDirty); listen("note-editor", "input", markNoteDirty);
    listen("save-note", "click", saveCurrentNote);
    listen("delete-current-note", "click", function deleteCurrentNote() { if (state.note && state.note.id) deleteItems("note", [state.note.id]); });
    listen("note-editor", "paste", pasteIntoNote);
    listen("note-editor", "click", function stopEditorNavigation(event) { if (event.target.closest("a")) event.preventDefault(); });
    listen("note-editor", "drop", function preventUnsafeDrop(event) {
      event.preventDefault(); rememberSelection();
      if (event.dataTransfer.files.length) return insertImageFiles(Array.from(event.dataTransfer.files));
      if (state.drag) return;
      const html = event.dataTransfer.getData("text/html");
      if (html) insertSafeFragment(sanitizeHtml(html));
      else { restoreSelection(); document.execCommand("insertText", false, event.dataTransfer.getData("text/plain")); markNoteDirty(); }
    });
    listen("note-editor", "dragover", function acceptEditorImageDrop(event) { if (!state.drag) event.preventDefault(); });
    document.addEventListener("selectionchange", rememberSelection);
    listen("editor-toolbar", "mousedown", function preserveEditorSelection(event) { if (event.target.closest("button")) event.preventDefault(); });
    listen("editor-toolbar", "click", function editorCommandClicked(event) {
      const source = event.target.closest("button[data-command]");
      if (source) applyEditorCommand(source.dataset.command, source.dataset.value);
    });
    listen("insert-note-image", "click", function chooseNoteImage() { rememberSelection(); byId("note-image-input").click(); });
    listen("note-image-input", "change", async function noteImagesSelected(event) { const files = Array.from(event.target.files); event.target.value = ""; await insertImageFiles(files); });
    listen("hotkey-input", "keydown", captureHotkey);
    listen("font-size", "input", function fontSizeChanged() { previewFont(); markSettingsDirty(); });
    listen("download-default-directory", "input", markSettingsDirty);
    listen("auto-start", "change", markSettingsDirty);
    listen("settings-form", "submit", saveSettings);
    listen("reset-settings", "click", resetSettings);
    listen("open-data-directory", "click", async function openDataDirectory(event) { await perform(function invokeOpenDataDirectory() { return window.desktopBridge.call("openDataDirectory"); }, "已打开数据目录", event.currentTarget); });
    listen("open-github-repository", "click", async function openGitHubRepository(event) { await perform(function invokeOpenGitHubRepository() { return window.desktopBridge.call("openGithubRepository"); }, "已在浏览器中打开仓库", event.currentTarget); });
    listen("dialog-form", "submit", submitDialog);
    listen("dialog-cancel", "click", closeDialog); listen("dialog-close", "click", closeDialog);
    listen("app-dialog", "cancel", function dialogEscapePressed(event) { event.preventDefault(); closeDialog(); });
    document.addEventListener("keydown", function applicationShortcutPressed(event) {
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "s" && state.page === "notes" && !byId("app-dialog").open) { event.preventDefault(); saveCurrentNote().catch(reportError); }
    });
    // 阻止浏览器级文件拖放导航，文件只由便签编辑器或 Qt 处理。
    document.addEventListener("dragover", function preventDocumentDrag(event) { if (!state.drag) event.preventDefault(); });
    document.addEventListener("drop", function preventDocumentDrop(event) { event.preventDefault(); });
  }

  // Qt 关闭窗口前调用此函数；明确处理保存中的操作和未保存的便签。
  window.desktopToolCanClose = function desktopToolCanClose() {
    if (state.dialog && state.dialog.busy) { toast("正在提交操作，请稍候再关闭。", "warning"); return false; }
    if (state.settingsSaving) { toast("设置正在保存，请稍候再关闭。", "warning"); return false; }
    if (!canLeaveNote() || !canLeaveSettings()) return false;
    if (!window.desktopGridmap.canLeave()) return false;
    return window.desktopDownloads.canClose();
  };

  installEvents();
  connectApplication();
})();
