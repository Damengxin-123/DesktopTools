/* 表情页：分类、关键字与文件引用由原生服务管理，网页只负责筛选、呈现和操作。 */
(function installEmojiPage() {
  "use strict";
  // 页面状态，监听刷新不覆盖正在提交的对话框。
  const state = {
    items: [], // 后端返回的全部表情摘要。
    categories: [], // 分类列表，含默认分类。
    total: 0, // 全部表情数量，与筛选结果数量分开。
    category: "", // 当前筛选分类，空字符串表示全部。
    page: 0, // 当前分页，从零开始。
    revision: 0, // 查询序号，忽略过期响应。
    timer: null, // 搜索与后端通知合并定时器。
    initialized: false, // 控件和信号只绑定一次。
    available: false, // 表情库读取是否正常。
    options: null, // 主页面提供的提示函数。
    confirm: null, // 当前对话框提交操作。
    dialogBusy: false, // 对话框提交期间阻止重复操作。
    dialogRevision: 0, // 防止过期响应覆盖新对话框。
    pendingPath: "" // 添加对话框中已选择的图片路径。
  };
  // 每页卡片数量，限制同时显示的缩略图数量。
  const pageSize = 48;
  // 获取固定页面控件。
  function byId(id) { return document.getElementById(id); }
  // 以纯文本创建节点，表情数据绝不作为 HTML 执行。
  function node(tag, className, text) {
    const result = document.createElement(tag);
    if (className) result.className = className;
    if (text !== undefined) result.textContent = String(text);
    return result;
  }
  // 统一处理按钮的异步错误和重复点击。
  function action(label, handler, className = "button quiet small") {
    const result = node("button", className, label);
    result.type = "button";
    result.addEventListener("click", async function clicked() {
      result.disabled = true;
      try { await handler(); } catch (error) { state.options.reportError(error); }
      finally { result.disabled = false; }
    });
    return result;
  }
  // 将添加时间显示为本机中文日期。
  function date(value) { return new Date(value).toLocaleDateString("zh-CN"); }
  // 按当前分类筛选，最近添加的记录在后端已排在前面。
  function filtered() {
    return state.items.filter(item => !state.category || item.categoryId === state.category);
  }
  // 取得分类显示名称。
  function categoryName(id) {
    const category = state.categories.find(item => item.id === id);
    return category ? category.name : "默认分类";
  }
  // 创建带标签的表单字段，返回输入节点供调用方读取。
  function formField(body, label, id) {
    const wrapper = node("label", "form-field");
    wrapper.htmlFor = id;
    wrapper.append(node("span", "", label));
    const input = node("input");
    input.id = id;
    input.name = id;
    wrapper.append(input);
    body.append(wrapper);
    return input;
  }
  // 将关键字文本拆分为去空白的关键字列表。
  function parseKeywords(value) {
    return value.split(/[\s,，、]+/).map(keyword => keyword.trim()).filter(Boolean);
  }
  // 绘制分类标签，支持编辑和删除；表情不提供拖动排序。
  function renderCategories() {
    const target = byId("emoji-categories");
    target.replaceChildren();
    const categories = [{ id: "", name: "全部" }, ...state.categories];
    for (const category of categories) {
      const chip = node("div", "category-chip" + (category.id === state.category ? " active" : ""));
      const select = node("button", "category-select", category.name);
      select.type = "button";
      select.setAttribute("aria-pressed", String(category.id === state.category));
      select.addEventListener("click", function selectCategory() {
        state.category = category.id;
        state.page = 0;
        renderCategories();
        render();
      });
      chip.append(select);
      if (category.id && category.id !== "default") {
        const rename = node("button", "icon-button", "✎");
        rename.type = "button";
        rename.setAttribute("aria-label", "重命名分类“" + category.name + "”");
        rename.addEventListener("click", function renameCategory() { editCategory(category); });
        const remove = node("button", "icon-button", "×");
        remove.type = "button";
        remove.setAttribute("aria-label", "删除分类“" + category.name + "”");
        remove.addEventListener("click", function removeCategory() { confirmCategoryDeletion(category); });
        chip.append(rename, remove);
      }
      target.append(chip);
    }
  }
  // 创建表情卡片，显示缩略图、名称、关键字和操作。
  function card(item) {
    const result = node("article", "emoji-card");
    result.dataset.id = item.id;
    const image = node("img", "emoji-thumbnail");
    image.src = item.thumbnail;
    image.alt = "表情 " + item.name;
    image.loading = "lazy";
    const name = node("p", "emoji-name", item.name);
    name.title = item.name;
    const path = node("p", "emoji-path", item.path);
    path.title = item.path;
    const keywords = node("div", "emoji-keywords");
    for (const keyword of item.keywords) keywords.append(node("span", "emoji-keyword", keyword));
    const footer = node("div", "emoji-card-meta");
    footer.append(node("span", "emoji-card-category", categoryName(item.categoryId)), node("span", "muted", date(item.addedAt)));
    const actions = node("div", "emoji-card-actions");
    actions.append(action("复制文件", async function copyEmojiFile() {
      await window.desktopBridge.call("copyEmojiFile", item.id);
      state.options.toast("文件已复制到剪贴板，不会进入剪贴板历史");
    }, "button secondary small"));
    actions.append(action("打开位置", async function openEmojiLocation() {
      await window.desktopBridge.call("openEmojiDirectory", item.id);
    }, "button secondary small"));
    actions.append(action("编辑", function editEmojiClick() { editEmoji(item); }));
    actions.append(action("删除", function deleteEmojiClick() { confirmDelete([item.id]); }, "button danger quiet small"));
    result.append(image, name, path, keywords, footer, actions);
    return result;
  }
  // 绘制当前页和分页状态。
  function render() {
    const items = filtered();
    const pages = Math.max(1, Math.ceil(items.length / pageSize));
    state.page = Math.min(state.page, pages - 1);
    byId("emoji-list").replaceChildren(...items.slice(state.page * pageSize, (state.page + 1) * pageSize).map(card));
    byId("emoji-count").textContent = state.total;
    byId("emoji-empty").hidden = items.length > 0 || !state.available;
    byId("emoji-empty").querySelector("h2").textContent = state.total ? "没有匹配的表情" : "还没有表情";
    byId("emoji-empty").querySelector("p").textContent = state.total ? "试试其他关键字或切换分类。" : "点击“添加表情”选择图片文件，并设置检索关键字。";
    byId("emoji-page-info").textContent = "第 " + (state.page + 1) + " / " + pages + " 页";
    byId("emoji-previous").disabled = state.page === 0;
    byId("emoji-next").disabled = state.page >= pages - 1;
  }
  // 查询后端摘要，错误仅显示在表情页。
  async function refresh() {
    const revision = ++state.revision;
    try {
      const data = await window.desktopBridge.call("getEmojis", byId("emoji-search").value);
      if (revision !== state.revision) return;
      state.items = data.items;
      state.categories = data.categories;
      state.total = data.total;
      state.available = true;
      byId("emoji-error").textContent = "";
      byId("emoji-error").hidden = true;
    } catch (error) {
      if (revision !== state.revision) return;
      state.available = false;
      byId("emoji-error").textContent = error.message;
      byId("emoji-error").hidden = false;
    }
    renderCategories();
    render();
  }
  // 合并紧邻的信号或输入，降低缩略图刷新开销。
  function scheduleRefresh() {
    clearTimeout(state.timer);
    state.timer = setTimeout(refresh, 100);
  }
  // 准备表情对话框，返回本次打开的序号。
  function openDialog(title, submitText, danger = false) {
    state.dialogRevision += 1;
    state.confirm = null;
    byId("emoji-dialog-title").textContent = title;
    byId("emoji-dialog-body").replaceChildren();
    byId("emoji-dialog-error").hidden = true;
    byId("emoji-dialog-confirm").textContent = submitText;
    byId("emoji-dialog-confirm").className = "button primary" + (danger ? " danger" : "");
    byId("emoji-dialog-cancel").textContent = "取消";
    byId("emoji-dialog").showModal();
    const firstField = byId("emoji-dialog-body").querySelector("input,select");
    if (firstField) firstField.focus();
    return state.dialogRevision;
  }
  // 关闭对话框时使未完成操作失效。
  function closeDialog() {
    if (state.dialogBusy) return;
    state.dialogRevision += 1;
    byId("emoji-dialog").close();
  }
  // 填充分类选择框。
  function fillCategorySelect(select, selectedId) {
    select.replaceChildren();
    for (const category of state.categories) {
      const option = node("option", "", category.name);
      option.value = category.id;
      select.append(option);
    }
    select.value = selectedId;
    if (select.selectedIndex < 0) select.value = "default";
  }
  // 构建添加或编辑共用的关键字和分类表单。
  function buildForm(body, item) {
    const path = node("p", "emoji-form-path", item.path);
    const keywords = formField(body, "检索关键字（空格或逗号分隔）", "emoji-keywords-input");
    keywords.value = item.keywords.join(" ");
    keywords.maxLength = 600;
    keywords.placeholder = "例如：开心 猫 沙雕";
    keywords.setAttribute("aria-label", "检索关键字");
    const category = formField(body, "分类", "emoji-category-input", "select");
    fillCategorySelect(category, item.categoryId || state.category || "default");
    body.append(node("p", "field-help", "关键字用于快速检索表情；修改只影响记录，不会改动原文件。"));
    return { keywords, category };
  }
  // 打开添加对话框，path 来自原生文件选择或剪贴板转换，之后设置关键字和分类。
  function openAddDialog(path, hint) {
    const revision = openDialog("添加表情", "添加");
    state.pendingPath = path;
    const body = byId("emoji-dialog-body");
    body.append(node("p", "emoji-form-path", state.pendingPath), node("p", "field-help", hint));
    const form = buildForm(body, { path: state.pendingPath, keywords: [], categoryId: state.category });
    form.keywords.focus();
    state.confirm = async function submitAddEmoji() {
      const keywords = parseKeywords(form.keywords.value);
      if (keywords.length > 20) throw new Error("关键字最多 20 个。");
      await window.desktopBridge.call("addEmoji", {
        path: state.pendingPath,
        categoryId: form.category.value,
        keywords
      });
      await refresh();
      state.options.toast("表情已添加");
    };
    if (revision !== state.dialogRevision) return;
  }
  // 添加表情：先选择图片文件，再设置关键字和分类。
  async function addEmoji() {
    const result = await window.desktopBridge.call("chooseEmojiImage");
    if (!result || result.cancelled) return;
    openAddDialog(result.target || "", "图片路径来自刚才的选择，添加后仍保存在原位置。");
  }
  // 从剪贴板添加：复制的文件直接引用，截屏等图像内容先保存到数据目录。
  async function addEmojiFromClipboard() {
    const result = await window.desktopBridge.call("pasteEmojiImage");
    openAddDialog(result.target || "", "图片路径来自当前剪贴板内容，添加后仅保存对该文件的引用。");
  }
  // 编辑表情的关键字和分类。
  function editEmoji(item) {
    const revision = openDialog("编辑表情", "保存");
    const body = byId("emoji-dialog-body");
    body.append(node("p", "emoji-form-path", item.name + "\n" + item.path));
    const form = buildForm(body, item);
    state.confirm = async function submitEditEmoji() {
      const keywords = parseKeywords(form.keywords.value);
      if (keywords.length > 20) throw new Error("关键字最多 20 个。");
      await window.desktopBridge.call("saveEmoji", { id: item.id, categoryId: form.category.value, keywords });
      await refresh();
      state.options.toast("表情已更新");
    };
    if (revision !== state.dialogRevision) return;
  }
  // 新建或重命名分类；默认分类不提供编辑入口。
  function editCategory(category) {
    const revision = openDialog(category ? "重命名分类" : "新建分类", "保存");
    const body = byId("emoji-dialog-body");
    const name = formField(body, "分类名称", "emoji-category-name");
    name.required = true;
    name.maxLength = 100;
    name.value = category ? category.name : "";
    state.confirm = async function submitCategoryForm() {
      if (!name.value.trim()) throw new Error("请输入分类名称。");
      await window.desktopBridge.call("saveEmojiCategory", category ? category.id : "", name.value.trim());
      await refresh();
      state.options.toast(category ? "分类名称已更新" : "分类已创建");
    };
    if (revision !== state.dialogRevision) return;
  }
  // 删除分类时明确说明表情移入默认分类，原文件不受影响。
  function confirmCategoryDeletion(category) {
    openDialog("删除分类", "删除分类", true);
    byId("emoji-dialog-body").append(node("p", "dialog-description",
      "删除分类“" + category.name + "”？\n分类内的表情将移入默认分类，记录和原文件都会保留。"));
    state.confirm = async function submitCategoryDeletion() {
      await window.desktopBridge.call("deleteEmojiCategory", category.id);
      if (state.category === category.id) state.category = "";
      await refresh();
      state.options.toast("分类已删除，表情已移入默认分类");
    };
  }
  // 删除确认固定本次选中的标识，原文件不会被删除。
  function confirmDelete(ids) {
    if (!ids.length) return;
    openDialog("删除表情", "确认删除", true);
    byId("emoji-dialog-body").append(node("p", "dialog-description",
      "确定删除这 " + ids.length + " 个表情吗？\n只移除表情记录，不会删除原图片文件。"));
    state.confirm = async function deleteConfirmedEmojis() {
      await window.desktopBridge.call("deleteEmojis", ids);
      await refresh();
    };
  }
  // 提交对话框操作，失败保留对话框并显示原因。
  async function submitDialog() {
    if (state.dialogBusy || !state.confirm) return;
    state.dialogBusy = true;
    byId("emoji-dialog-confirm").disabled = true;
    try { await state.confirm(); state.dialogBusy = false; closeDialog(); }
    catch (error) { byId("emoji-dialog-error").textContent = error.message; byId("emoji-dialog-error").hidden = false; }
    finally { state.dialogBusy = false; byId("emoji-dialog-confirm").disabled = false; }
  }
  // 绑定固定控件和原生通知，初始化失败不影响其他功能。
  async function initialize(options) {
    state.options = options;
    if (!state.initialized) {
      state.initialized = true;
      byId("add-emoji").addEventListener("click", function addClicked() { addEmoji().catch(state.options.reportError); });
      byId("add-emoji-clipboard").addEventListener("click", function addFromClipboardClicked() { addEmojiFromClipboard().catch(state.options.reportError); });
      byId("add-emoji-category").addEventListener("click", function newCategoryClicked() { editCategory(null); });
      byId("emoji-search").addEventListener("input", function searchChanged() { state.page = 0; state.revision += 1; scheduleRefresh(); });
      byId("emoji-previous").addEventListener("click", function previousPage() { state.page -= 1; render(); });
      byId("emoji-next").addEventListener("click", function nextPage() { state.page += 1; render(); });
      byId("emoji-dialog-close").addEventListener("click", closeDialog);
      byId("emoji-dialog-cancel").addEventListener("click", closeDialog);
      byId("emoji-dialog-form").addEventListener("submit", function dialogSubmitted(event) { event.preventDefault(); submitDialog(); });
      byId("emoji-dialog").addEventListener("cancel", function escapeDialog(event) { event.preventDefault(); closeDialog(); });
      await window.desktopBridge.on("emojisChanged", scheduleRefresh);
    }
    await refresh();
  }
  // 对主页面仅暴露初始化入口。
  window.desktopEmoji = Object.freeze({ initialize });
})();
