/* 下载页负责交互与进度呈现；网络传输、磁盘校验和任务持久化均由 Qt 处理。 */
(function installDownloadsPage() {
  "use strict";

  // 下载页独立状态，进度刷新不修改其他页面的表单或焦点。
  const state = {
    items: [], // 后端最近一次返回的完整任务列表。
    activeCount: 0, // 后端统计的活动任务数量，用于退出提醒。
    views: new Map(), // 按任务 ID 复用卡片和操作按钮。
    pendingIds: new Set(), // 正在提交操作的任务，防止重复点击。
    creating: false, // 是否正在创建下载任务。
    choosing: 0, // 当前等待原生目录选择器返回的数量。
    defaultDirectory: "", // 已保存的默认下载路径，不使用设置草稿。
    refreshTimer: null, // 高频下载信号的合并定时器。
    refreshPromise: null, // 正在读取的后端快照。
    refreshAgain: false, // 读取期间的新通知需要再取一次快照。
    bridgeBusy: false, // 原生目录等桥接操作未结束时不自动打开文件弹窗。
    selection: null, // 当前文件选择会话，异步响应通过对象身份确认仍有效。
    selectionQueue: [], // 已获取元数据、等待自动提示选择的磁力任务。
    selectionShown: new Set(), // 每个任务自动提示一次，关闭后仅由卡片重新打开。
    selectionDrafts: new Map(), // 关闭弹窗时保留尚未确认的逐文件勾选。
    selectionTimer: null, // 延迟检查弹窗队列，避免抢占当前操作。
    modalObserver: null, // 观察页面切换和其他 HTML 弹窗关闭，以恢复队列。
    initialized: false, // 固定控件和信号仅绑定一次。
    options: null // 由主页面提供的提示和错误处理函数。
  };

  // 下载状态的中文名称。
  const statusNames = Object.freeze({
    resolving: "获取文件信息", // 磁力任务仅解析元数据，尚未下载所选内容。
    awaiting_selection: "等待选择文件", // 元数据已就绪，确认前不开始内容下载。
    downloading: "正在下载", // 包括连接与接收数据阶段。
    paused: "已暂停", // 等待用户继续。
    completed: "已完成", // 文件已成功写入。
    cancelled: "已取消", // 用户主动取消。
    failed: "下载失败" // 可以重试的失败任务。
  });

  // 每种按钮的固定标签，重绘进度时不替换按钮节点。
  const actionNames = Object.freeze({
    select: "选择文件", // 打开磁力任务的逐文件勾选弹窗。
    pause: "暂停", // 暂停活动任务。
    resume: "继续", // 继续已暂停任务。
    cancel: "取消", // 取消未完成任务。
    retry: "重试", // 失败后尝试继续或重新下载。
    open: "打开目录", // 打开已完成文件所在目录。
    remove: "移除记录" // 从列表删除结束的任务记录。
  });

  // 取得固定页面节点。
  function byId(id) { return document.getElementById(id); }

  // 创建元素，任务文件名和 URL 等动态值始终使用纯文本。
  function element(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = String(text);
    return node;
  }

  // 文本没有变化时不触碰 DOM，减轻高频进度更新。
  function setText(node, value) {
    const text = String(value);
    if (node.textContent !== text) node.textContent = text;
  }

  // 显示主应用样式的反馈。
  function notify(text, type = "success") {
    if (state.options) state.options.toast(text, type);
  }

  // 把异常转换为便于用户理解的信息。
  function message(error) { return error && error.message ? error.message : "下载操作失败，请重试。"; }

  // 通知主应用显示异步错误。
  function reportError(error) {
    if (state.options) state.options.reportError(error);
  }

  // 将字节数转换为合适的二进制单位，不把缺失数据解释为总大小。
  function formatBytes(value) {
    const bytes = Math.max(0, Number(value) || 0);
    const units = ["B", "KB", "MB", "GB", "TB"];
    let unit = 0;
    let amount = bytes;
    while (amount >= 1024 && unit < units.length - 1) { amount /= 1024; unit += 1; }
    return amount.toFixed(unit === 0 ? 0 : (amount < 10 ? 2 : 1)) + " " + units[unit];
  }

  // 计算可展示的进度；只有后端完成状态且总大小已知时显示 100%。
  function progressFor(task) {
    const received = Math.max(0, Number(task.bytesReceived) || 0);
    const total = Number(task.totalBytes);
    const known = task.totalBytes !== null && task.totalBytes !== undefined && Number.isFinite(total) && total >= 0;
    let percent = total > 0 ? Math.floor(received / total * 100) : 0;
    percent = task.status === "completed" ? 100 : Math.min(99, Math.max(0, percent));
    return {
      received, // 已写入或接收的字节数。
      total, // 服务端声明的总大小，未知为负数。
      known, // 是否可以计算可靠百分比。
      percent // 限制在安全范围内的整数进度。
    };
  }

  // 根据真实状态决定显示的操作，取消与移除均不宣称删除原始文件。
  function visibleActions(status) {
    if (status === "downloading" || status === "resolving") return ["pause", "cancel"];
    if (status === "awaiting_selection") return ["select", "cancel"];
    if (status === "paused") return ["resume", "cancel"];
    if (status === "failed") return ["retry", "cancel", "remove"];
    if (status === "completed") return ["open", "remove"];
    if (status === "cancelled") return ["remove"];
    return [];
  }

  // 创建一张卡片，此后只更新其中字段，保证键盘焦点和点击目标稳定。
  function createTaskView(task) {
    const root = element("article", "download-card");
    root.dataset.downloadId = task.id;
    const heading = element("div", "download-card-heading");
    const icon = element("div", "download-file-icon", "↓");
    icon.setAttribute("aria-hidden", "true");
    const identity = element("div", "download-identity");
    const title = element("h3", "download-title");
    const url = element("p", "download-source");
    identity.append(title, url);
    const status = element("span", "download-status");
    heading.append(icon, identity, status);
    const metrics = element("div", "download-metrics");
    const percent = element("strong", "download-percent");
    const bytes = element("span", "download-bytes");
    const speed = element("span", "download-speed");
    metrics.append(percent, bytes, speed);
    const progress = element("div", "download-progress");
    progress.setAttribute("role", "progressbar");
    progress.setAttribute("aria-valuemin", "0");
    progress.setAttribute("aria-valuemax", "100");
    const bar = element("span", "download-progress-bar");
    progress.append(bar);
    const footer = element("div", "download-card-footer");
    const destination = element("p", "download-destination");
    const actions = element("div", "download-actions");
    const buttons = new Map();
    for (const action of Object.keys(actionNames)) {
      const node = element("button", "button small " + (action === "cancel" || action === "remove" ? "quiet danger" : "secondary"), actionNames[action]);
      node.type = "button";
      node.dataset.action = action;
      node.hidden = true;
      node.addEventListener("click", function downloadActionClicked() { performAction(task.id, action).catch(reportError); });
      buttons.set(action, node);
      actions.append(node);
    }
    footer.append(destination, actions);
    const warning = element("p", "download-message download-warning");
    const error = element("p", "download-message download-error");
    const actionError = element("p", "download-message download-error");
    actionError.setAttribute("role", "alert");
    warning.hidden = true; error.hidden = true; actionError.hidden = true;
    root.append(heading, metrics, progress, footer, warning, error, actionError);
    return {
      root, // 卡片稳定根节点。
      title, url, status, icon, // 文件名称、来源、状态与装饰图标。
      percent, bytes, speed, progress, bar, // 高频更新的进度字段。
      destination, actions, buttons, // 保存路径及可访问操作区。
      warning, error, actionError, // 后端警告、任务错误和操作错误。
      restoreFocus: false // 状态切换隐藏焦点按钮后，需要迁移到可用操作。
    };
  }

  // 更新状态和数值，整个过程中不重建卡片或按钮。
  function updateTaskView(view, task) {
    const focused = document.activeElement;
    const pending = state.pendingIds.has(task.id);
    const progress = progressFor(task);
    const resolving = task.status === "resolving"; // 元数据阶段不能显示为内容下载进度。
    const awaiting = task.status === "awaiting_selection"; // 明确提示仍需用户确认文件。
    const knownStatus = Object.prototype.hasOwnProperty.call(statusNames, task.status);
    view.root.dataset.status = knownStatus ? task.status : "unknown";
    view.root.dataset.kind = task.kind === "magnet" ? "magnet" : "http";
    view.root.setAttribute("aria-busy", String(pending));
    const title = task.fileName || "正在获取文件名…";
    setText(view.title, title); view.title.title = title;
    setText(view.url, task.url || ""); view.url.title = task.url || "";
    setText(view.status, knownStatus ? statusNames[task.status] : "状态未知");
    setText(view.icon, task.status === "completed" ? "✓" : (task.kind === "magnet" ? "⊙" : "↓"));
    setText(view.percent, resolving ? "解析中" : (awaiting ? "待选择" : (progress.known ? progress.percent + "%" : "大小未知")));
    const fileCount = Math.max(0, Number(task.fileCount) || 0); // 元数据就绪后的文件总数。
    const selectedCount = Math.max(0, Number(task.selectedCount) || 0); // 后端实际确认的文件数量。
    const transferBytes = formatBytes(progress.received) + " / " + (progress.known ? formatBytes(progress.total) : "大小未知"); // 已确认任务的真实传输量。
    setText(view.bytes, resolving ? "正在寻找可用节点并获取文件列表…" : (awaiting ? fileCount + " 个文件 · 确认后开始下载" : transferBytes + (task.kind === "magnet" && task.selectionConfirmed ? " · 已选 " + selectedCount + " 个文件" : "")));
    setText(view.speed, task.status === "downloading" ? formatBytes(task.speed) + "/s" : "—");
    view.progress.setAttribute("aria-label", title + "的下载进度");
    view.progress.classList.toggle("indeterminate", resolving || (!awaiting && !progress.known));
    view.progress.classList.toggle("running", task.status === "downloading" || resolving);
    if (progress.known && !resolving && !awaiting) {
      view.progress.setAttribute("aria-valuenow", String(progress.percent));
      view.bar.style.width = progress.percent + "%";
    } else {
      view.progress.removeAttribute("aria-valuenow");
      view.bar.style.removeProperty("width");
    }
    view.progress.setAttribute("aria-valuetext", view.status.textContent + "，" + view.percent.textContent + "，" + view.bytes.textContent);
    setText(view.destination, "保存到：" + (task.directory || "默认下载目录"));
    view.destination.title = task.filePath || task.directory || "";
    setText(view.warning, task.warning || ""); view.warning.hidden = !task.warning;
    setText(view.error, task.error || ""); view.error.hidden = !task.error;
    const available = visibleActions(task.status);
    for (const [action, node] of view.buttons) {
      const visible = available.includes(action);
      if (focused === node && !visible) view.restoreFocus = true;
      node.hidden = !visible;
      node.disabled = pending;
      node.setAttribute("aria-label", actionNames[action] + "“" + title + "”");
    }
    if (view.restoreFocus && !pending) {
      const next = available.map(function findVisibleButton(action) { return view.buttons.get(action); }).find(function enabledButton(node) { return !node.disabled; });
      if (next) next.focus({ preventScroll: true });
      view.restoreFocus = false;
    }
  }

  // 按后端顺序对齐卡片，只有新增、删除或排序变化才调整根节点。
  function renderSnapshot(snapshot) {
    if (!snapshot || !Array.isArray(snapshot.items)) throw new Error("下载列表数据不完整，请重新打开程序。");
    const list = byId("download-list");
    const ids = new Set(snapshot.items.map(function taskId(task) { return task.id; }));
    let removedFocusedCard = false;
    for (const [id, view] of state.views) {
      if (ids.has(id)) continue;
      removedFocusedCard = removedFocusedCard || view.root.contains(document.activeElement);
      view.root.remove(); state.views.delete(id);
    }
    state.items = snapshot.items;
    state.activeCount = Math.max(0, Number(snapshot.activeCount) || 0);
    snapshot.items.forEach(function updateDownloadCard(task, index) {
      let view = state.views.get(task.id);
      if (!view) { view = createTaskView(task); state.views.set(task.id, view); }
      const next = list.children[index] || null;
      if (next !== view.root) list.insertBefore(view.root, next);
      updateTaskView(view, task);
    });
    setText(byId("download-count"), snapshot.items.length);
    setText(byId("download-active-count"), state.activeCount + " 个任务进行中");
    byId("download-active-count").parentNode.classList.toggle("active", state.activeCount > 0);
    const waitingCount = snapshot.items.filter(function awaitingFiles(task) { return task.status === "awaiting_selection"; }).length; // 待确认任务不算活动下载。
    setText(byId("download-summary"), snapshot.items.length ? "共 " + snapshot.items.length + " 个任务 · 进行中 " + state.activeCount + (waitingCount ? " · 待选择 " + waitingCount : "") : "暂无下载任务");
    byId("download-empty").hidden = snapshot.items.length > 0;
    const warnings = Array.isArray(snapshot.warnings) ? snapshot.warnings.filter(function readableDownloadWarning(value) { return typeof value === "string" && value.trim(); }) : []; // 独立下载服务异常保留可用任务，并显示后端提供的警告。
    setText(byId("download-list-error"), warnings.join("；"));
    byId("download-list-error").hidden = warnings.length === 0;
    if (removedFocusedCard) byId("download-url").focus({ preventScroll: true });
    updateSelectionQueue();
  }

  // 获取一次最新快照；刷新中的通知合并为后续的一次读取。
  function refresh() {
    if (state.refreshPromise) { state.refreshAgain = true; return state.refreshPromise; }
    state.refreshPromise = window.desktopBridge.call("getDownloads").then(renderSnapshot).catch(function downloadRefreshFailed(error) {
      setText(byId("download-list-error"), "无法更新下载列表：" + message(error));
      byId("download-list-error").hidden = false;
      throw error;
    }).finally(function downloadRefreshFinished() {
      state.refreshPromise = null;
      if (state.refreshAgain) { state.refreshAgain = false; scheduleRefresh(); }
    });
    return state.refreshPromise;
  }

  // 合并连续信号，每 250 毫秒最多发起一次刷新；持续下载时不会无限推迟。
  function scheduleRefresh() {
    if (state.refreshTimer !== null) return;
    state.refreshTimer = window.setTimeout(function scheduledDownloadRefresh() {
      state.refreshTimer = null;
      // 状态区显示失败原因，后台刷新失败不反复弹出通知。
      refresh().catch(function keepRefreshErrorInPage() {});
    }, 250);
  }

  // 用户操作成功后立即取最新数据，不等待下一次进度信号。
  async function refreshAfterAction() {
    window.clearTimeout(state.refreshTimer); state.refreshTimer = null;
    if (state.refreshPromise) await state.refreshPromise.catch(function ignoreOlderRefreshError() {});
    await refresh();
  }

  // 自动识别 HTTP(S) 与磁力链接，磁力格式和目录回退由 Qt 再次校验。
  async function createDownload(event) {
    event.preventDefault();
    if (state.creating || !byId("download-form").reportValidity()) return;
    const urlText = byId("download-url").value.trim();
    byId("download-create-error").hidden = true;
    try {
      let url;
      try { url = new URL(urlText); } catch (error) { throw new Error("请输入有效的 HTTP、HTTPS 或 magnet:? 磁力链接。"); }
      if (!["http:", "https:", "magnet:"].includes(url.protocol)) throw new Error("下载链接只支持 HTTP、HTTPS 和 magnet:? 磁力链接。");
      if (url.protocol === "magnet:" && !url.searchParams.getAll("xt").some(function hasMagnetTopic(value) { return /^urn:bt(?:ih|mh):/i.test(value); })) throw new Error("磁力链接缺少有效的文件标识（xt），请粘贴完整链接。");
      state.creating = true;
      byId("download-form").inert = true;
      byId("add-download").disabled = true;
      const task = await window.desktopBridge.call("createDownload", {
        url: urlText, // 下载来源，Qt 再次校验协议及响应。
        directory: byId("download-directory").value.trim() // 空值或无效路径允许 Qt 回退到默认目录。
      });
      byId("download-url").value = "";
      await refreshAfterAction();
      notify(task && task.kind === "magnet" ? "磁力任务已创建，获取文件列表后请确认所需文件。" : (task && task.warning ? "任务已创建，请查看卡片中的提示。" : "下载任务已创建"), task && task.warning ? "warning" : "success");
    } catch (error) {
      setText(byId("download-create-error"), message(error));
      byId("download-create-error").hidden = false;
    } finally {
      state.creating = false;
      byId("download-form").inert = false;
      byId("add-download").disabled = false;
      scheduleSelectionQueue();
    }
  }

  // 通过原生目录选择器填写指定输入框，设置页只改草稿不自动保存。
  async function chooseDirectory(inputId, buttonId) {
    const source = byId(buttonId);
    if (source.disabled) return;
    state.choosing += 1; source.disabled = true;
    try {
      const result = await window.desktopBridge.call("chooseDownloadDirectory");
      if (!result || result.cancelled) return;
      byId(inputId).value = result.directory || "";
      byId(inputId).dispatchEvent(new Event("input", { bubbles: true }));
    } catch (error) { reportError(error); }
    finally { state.choosing = Math.max(0, state.choosing - 1); source.disabled = false; scheduleSelectionQueue(); }
  }

  // 提交任务操作，同一任务等待期间禁用按钮，其他任务仍可操作。
  async function performAction(id, action) {
    if (state.pendingIds.has(id)) return;
    const task = state.items.find(function findTask(item) { return item.id === id; });
    if (!task || !visibleActions(task.status).includes(action)) return;
    if (action === "select") { await openFileSelection(id); return; }
    if (action === "cancel" && !window.confirm("确定取消“" + (task.fileName || "此下载") + "”吗？")) return;
    const methods = { pause: "pauseDownload", resume: "resumeDownload", cancel: "cancelDownload", retry: "resumeDownload", open: "openDownloadDirectory", remove: "removeDownload" }; // 界面操作对应的原生 API。
    const view = state.views.get(id);
    state.pendingIds.add(id);
    if (view) { view.actionError.hidden = true; updateTaskView(view, task); }
    try {
      await window.desktopBridge.call(methods[action], id);
      await refreshAfterAction();
      const messages = { pause: "任务已暂停", resume: "任务已继续", cancel: "任务已取消", retry: "正在重试下载", open: "已打开下载目录", remove: "下载记录已移除" }; // 各操作成功后的中文反馈。
      notify(messages[action]);
    } catch (error) {
      if (view && view.root.isConnected) { setText(view.actionError, message(error)); view.actionError.hidden = false; }
      throw error;
    } finally {
      state.pendingIds.delete(id);
      const latest = state.items.find(function findLatestTask(item) { return item.id === id; });
      if (latest && state.views.has(id)) updateTaskView(state.views.get(id), latest);
      scheduleSelectionQueue();
    }
  }

  // 判断是否可以显示文件弹窗，避免抢占其他页面、原生操作或 HTML 模态。
  function canPresentFileSelection() {
    return !state.selection && !state.creating && !state.choosing && !state.bridgeBusy && !state.pendingIds.size
      && !byId("page-downloads").hidden && !byId("workspace").inert && !document.querySelector("dialog[open]");
  }

  // 等当前事件和桥接回调结束后检查队列，每个任务只自动提示一次。
  function scheduleSelectionQueue() {
    if (state.selectionTimer !== null) return;
    state.selectionTimer = window.setTimeout(function presentNextFileSelection() {
      state.selectionTimer = null;
      if (!canPresentFileSelection()) return;
      while (state.selectionQueue.length) {
        const id = state.selectionQueue.shift();
        const task = state.items.find(function findQueuedTask(item) { return item.id === id; });
        if (!task || task.status !== "awaiting_selection" || state.selectionShown.has(id)) continue;
        openFileSelection(id).catch(reportError);
        break;
      }
    }, 0);
  }

  // 从最新快照维护待选队列，任务被取消或移除时关闭失效的文件列表。
  function updateSelectionQueue() {
    const ids = new Set(state.items.map(function currentTaskId(task) { return task.id; }));
    for (const id of state.selectionShown) if (!ids.has(id)) state.selectionShown.delete(id);
    for (const id of state.selectionDrafts.keys()) if (!ids.has(id)) state.selectionDrafts.delete(id);
    state.selectionQueue = state.selectionQueue.filter(function stillAwaitingSelection(id) {
      return state.items.some(function matchingAwaitingTask(task) { return task.id === id && task.status === "awaiting_selection"; });
    });
    for (const task of state.items) {
      if (task.kind === "magnet" && task.status === "awaiting_selection" && !state.selectionShown.has(task.id) && !state.selectionQueue.includes(task.id)) state.selectionQueue.push(task.id);
    }
    if (state.selection && !state.selection.submitting) {
      const current = state.items.find(function selectedTask(task) { return task.id === state.selection.id; });
      if (!current || current.status !== "awaiting_selection") closeFileSelection();
    }
    scheduleSelectionQueue();
  }

  // 同步选择数量、总字节与控件状态，至少勾选一个文件才允许开始。
  function updateFileSelectionControls() {
    const session = state.selection;
    if (!session) return;
    const blocked = session.loading || session.submitting; // 读取或提交过程中冻结文件选择。
    const selectedFiles = session.files.filter(function selectedFile(file) { return session.selected.has(file.index); });
    const selectedBytes = selectedFiles.reduce(function sumSelectedBytes(total, file) { return total + file.size; }, 0);
    setText(byId("download-files-summary"), session.loading ? "正在读取文件列表…" : "已选 " + selectedFiles.length + " / " + session.files.length + " 个文件 · " + formatBytes(selectedBytes));
    byId("download-files-loading").hidden = !session.loading;
    byId("download-files-list").inert = blocked;
    byId("download-files-all").disabled = blocked || !session.files.length;
    byId("download-files-none").disabled = blocked || !session.files.length;
    byId("download-files-confirm").disabled = blocked || !selectedFiles.length;
    byId("download-files-later").disabled = session.submitting;
    byId("download-files-close").disabled = session.submitting;
    byId("download-files-reload").hidden = !session.failed;
    byId("download-files-reload").disabled = blocked;
    setText(byId("download-files-confirm"), session.submitting ? "正在开始…" : "开始下载");
    byId("download-files-dialog").setAttribute("aria-busy", String(blocked));
  }

  // 按需读取完整文件清单；通过会话身份丢弃关闭后迟到的异步响应。
  async function loadFileSelection(session) {
    if (state.selection !== session || session.submitting) return;
    session.loading = true; session.failed = false;
    byId("download-files-error").hidden = true;
    updateFileSelectionControls();
    try {
      const result = await window.desktopBridge.call("getDownloadFiles", session.id);
      if (state.selection !== session) return;
      if (!result || result.id !== session.id || !Array.isArray(result.files) || !result.files.length) throw new Error("未能取得可选择的文件，请重新读取或稍后重试任务。");
      const indices = new Set(); // 拒绝重复或无效索引，避免选择到错误文件。
      const files = result.files.map(function normalizeDownloadFile(file) {
        const index = Number(file.index), size = Number(file.size);
        if (!Number.isSafeInteger(index) || index < 0 || indices.has(index) || !Number.isSafeInteger(size) || size < 0 || typeof file.path !== "string") throw new Error("文件列表数据无效，请重新读取。");
        indices.add(index);
        return { index, path: file.path, size, selected: file.selected !== false }; // 后端文件索引、纯文本路径、字节数及默认勾选状态。
      });
      const draft = state.selectionDrafts.get(session.id); // 保留关闭前尚未确认的勾选，不更改后端任务。
      session.files = files;
      session.selected = new Set(files.filter(function initiallySelected(file) { return draft ? draft.has(file.index) : file.selected; }).map(function selectedIndex(file) { return file.index; }));
      const fragment = document.createDocumentFragment();
      for (const file of files) {
        const row = element("label", "download-file-row");
        const checkbox = element("input");
        checkbox.type = "checkbox"; checkbox.dataset.fileIndex = String(file.index); checkbox.checked = session.selected.has(file.index);
        const path = element("span", "download-file-path", file.path); path.title = file.path;
        const size = element("span", "download-file-size", formatBytes(file.size));
        checkbox.addEventListener("change", function fileSelectionChanged() {
          if (state.selection !== session || session.loading || session.submitting) return;
          if (checkbox.checked) session.selected.add(file.index); else session.selected.delete(file.index);
          byId("download-files-error").hidden = true;
          updateFileSelectionControls();
        });
        row.append(checkbox, path, size); fragment.append(row);
      }
      byId("download-files-list").replaceChildren(fragment);
      setText(byId("download-files-name"), result.fileName || "磁力下载");
    } catch (error) {
      if (state.selection !== session) return;
      session.failed = true;
      setText(byId("download-files-error"), message(error)); byId("download-files-error").hidden = false;
    } finally {
      if (state.selection === session) { session.loading = false; updateFileSelectionControls(); }
    }
  }

  // 打开独立的可访问模态，加载期间仍允许稍后选择，不启动文件下载。
  async function openFileSelection(id) {
    const task = state.items.find(function findSelectionTask(item) { return item.id === id; });
    if (!task || task.status !== "awaiting_selection") return;
    if (!canPresentFileSelection()) { notify("请先完成当前操作，再选择下载文件。", "warning"); return; }
    const session = {
      id, // 此次选择对应的任务 ID。
      files: [], // 仅打开弹窗时读取的完整文件清单。
      selected: new Set(), // 用户当前勾选的后端文件索引。
      loading: true, // 是否等待文件清单响应。
      submitting: false, // 确认开始下载时阻止重复提交或关闭。
      failed: false, // 清单读取失败时显示重新读取操作。
      returnFocus: document.activeElement // 关闭后恢复到原有操作位置。
    };
    state.selection = session;
    state.selectionShown.add(id);
    state.selectionQueue = state.selectionQueue.filter(function removePresentedTask(queuedId) { return queuedId !== id; });
    byId("download-files-dialog").dataset.downloadId = id;
    setText(byId("download-files-name"), task.fileName || "磁力下载");
    byId("download-files-list").replaceChildren();
    byId("download-files-error").hidden = true;
    updateFileSelectionControls();
    byId("download-files-dialog").showModal();
    byId("download-files-later").focus();
    await loadFileSelection(session);
  }

  // 关闭时保留任务和勾选草稿，后续进度信号不会再次自动打开同一任务。
  function closeFileSelection(confirmed = false) {
    const session = state.selection;
    if (!session || session.submitting) return;
    if (confirmed) state.selectionDrafts.delete(session.id);
    else if (session.files.length) state.selectionDrafts.set(session.id, new Set(session.selected));
    state.selection = null;
    if (byId("download-files-dialog").open) byId("download-files-dialog").close();
    byId("download-files-dialog").removeAttribute("data-download-id");
    const view = state.views.get(session.id);
    const task = state.items.find(function currentSelectionTask(item) { return item.id === session.id; });
    const action = task && visibleActions(task.status)[0]; // 优先回到该任务当前可用的卡片操作。
    const button = view && action ? view.buttons.get(action) : session.returnFocus;
    if (button && button.isConnected && !button.disabled) button.focus({ preventScroll: true });
    scheduleSelectionQueue();
  }

  // 批量改变本次勾选，保持原复选框节点与滚动位置。
  function selectAllFiles(checked) {
    const session = state.selection;
    if (!session || session.loading || session.submitting) return;
    session.selected = new Set(checked ? session.files.map(function allFileIndices(file) { return file.index; }) : []);
    byId("download-files-list").querySelectorAll("input[data-file-index]").forEach(function applyFileCheck(checkbox) { checkbox.checked = checked; });
    byId("download-files-error").hidden = true;
    updateFileSelectionControls();
  }

  // 仅在用户确认非空文件集合后提交，后端成功前保持弹窗与错误可见。
  async function confirmFileSelection(event) {
    event.preventDefault();
    const session = state.selection;
    if (!session || session.loading || session.submitting) return;
    if (!session.selected.size) {
      setText(byId("download-files-error"), "请至少选择一个文件。"); byId("download-files-error").hidden = false;
      return;
    }
    session.submitting = true;
    byId("download-files-error").hidden = true;
    updateFileSelectionControls();
    try {
      await window.desktopBridge.call("confirmDownloadFiles", session.id, Array.from(session.selected).sort(function sortFileIndices(left, right) { return left - right; }));
      if (state.selection !== session) return;
      session.submitting = false;
      closeFileSelection(true);
      notify("已确认所选文件，开始下载。");
      await refreshAfterAction();
    } catch (error) {
      if (state.selection === session) {
        setText(byId("download-files-error"), message(error)); byId("download-files-error").hidden = false;
      } else reportError(error);
    } finally {
      if (state.selection === session) { session.submitting = false; updateFileSelectionControls(); }
      scheduleSelectionQueue();
    }
  }

  // 绑定选择弹窗及忙碌状态，队列仅在下载页可见且没有其他模态时推进。
  function installFileSelectionEvents() {
    byId("download-files-form").addEventListener("submit", confirmFileSelection);
    byId("download-files-close").addEventListener("click", function closeFilesFromHeading() { closeFileSelection(); });
    byId("download-files-later").addEventListener("click", function chooseFilesLater() { closeFileSelection(); });
    byId("download-files-all").addEventListener("click", function checkEveryFile() { selectAllFiles(true); });
    byId("download-files-none").addEventListener("click", function uncheckEveryFile() { selectAllFiles(false); });
    byId("download-files-reload").addEventListener("click", function reloadFileList() { if (state.selection) loadFileSelection(state.selection); });
    byId("download-files-dialog").addEventListener("cancel", function escapeFileSelection(event) { event.preventDefault(); closeFileSelection(); });
    byId("download-files-dialog").addEventListener("close", function fileDialogClosedExternally() { if (!byId("download-files-dialog").open) closeFileSelection(); });
    document.addEventListener("desktop-busy", function downloadBridgeBusyChanged(event) { state.bridgeBusy = Boolean(event.detail); if (!state.bridgeBusy) scheduleSelectionQueue(); });
    state.modalObserver = new MutationObserver(function modalOrPageChanged() { scheduleSelectionQueue(); });
    state.modalObserver.observe(document.body, { attributes: true, subtree: true, attributeFilter: ["open", "hidden", "inert"] }); // 只观察可见性与模态属性，不轮询页面。
  }

  // 同步已保存的默认路径，永不回写设置输入框或新任务自定义目录。
  function setDefaultDirectory(directory) {
    state.defaultDirectory = String(directory || "");
    setText(byId("download-default-hint"), "默认下载到：" + (state.defaultDirectory || "系统下载目录"));
    byId("download-default-hint").title = state.defaultDirectory;
  }

  // 退出前的同步检查，可由 Qt runJavaScript 直接取得布尔结果。
  function canClose() {
    if (state.creating || state.pendingIds.size || state.choosing || (state.selection && (state.selection.loading || state.selection.submitting))) { notify("正在处理下载操作，请完成后再退出。", "warning"); return false; }
    return state.activeCount === 0 || window.confirm("还有 " + state.activeCount + " 个下载任务正在进行。\n退出会暂停下载，任务将在下次启动后保留，可继续下载。\n确定退出程序吗？");
  }

  // 连接下载信号并绑定固定表单，重复连接只重新读取快照。
  async function initialize(options) {
    state.options = options;
    if (!state.initialized) {
      state.initialized = true;
      installFileSelectionEvents();
      byId("download-form").addEventListener("submit", createDownload);
      byId("download-url").addEventListener("input", function clearDownloadFormError() { byId("download-create-error").hidden = true; });
      byId("choose-download-directory").addEventListener("click", function chooseTaskDirectory() { chooseDirectory("download-directory", "choose-download-directory"); });
      byId("choose-default-download-directory").addEventListener("click", function chooseDefaultDirectory() { chooseDirectory("download-default-directory", "choose-default-download-directory"); });
      byId("use-default-download-directory").addEventListener("click", function clearCustomDirectory() { byId("download-directory").value = ""; byId("download-directory").focus(); });
      await window.desktopBridge.on("downloadsChanged", scheduleRefresh);
    }
    // 下载索引异常只停留在本页提示，其他功能仍应完成初始化。
    await refresh().catch(function keepInitialDownloadErrorInPage() {});
  }

  // 主页面仅需初始化、同步默认路径和调用退出保护。
  window.desktopDownloads = Object.freeze({ initialize, setDefaultDirectory, canClose });
})();
