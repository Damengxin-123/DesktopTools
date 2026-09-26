/* 使用真实 QWebChannel 与本地做种 fixture 验证磁力任务的文件选择流程。 */
(async function verifyMagnetDownloadPage() {
  "use strict";
  const fixture = window.__magnetFixture; // C++ 提供磁力链接、临时目录和文件清单。
  const call = window.desktopBridge.call; // 始终调用真实 Qt 后端，不替换生产桥接。
  const originalConfirm = window.confirm; // 测试结束后恢复原始确认行为。
  let auxiliaryTask = null; // 可选的第二磁力任务，用于验证弹窗排队。

  // 将失败步骤返回给 C++，避免仅凭页面文字判断下载成功。
  function check(condition, message) { if (!condition) throw new Error(message); }
  // 获取稳定的页面控件，缺失时给出明确错误。
  function element(selector) {
    const node = document.querySelector(selector);
    check(node, "缺少磁力下载控件：" + selector);
    return node;
  }
  // 等待 Qt 元数据解析或前端刷新，允许本地 BitTorrent 握手所需时间。
  async function waitFor(predicate, message) {
    const deadline = Date.now() + (fixture.timeoutMs || 45000);
    while (Date.now() < deadline) {
      if (await predicate()) return;
      await delay(40);
    }
    throw new Error(message);
  }
  // 短暂等待事件循环和合并刷新，不控制计算机屏幕。
  function delay(milliseconds) { return new Promise(function scheduleTestDelay(resolve) { window.setTimeout(resolve, milliseconds); }); }
  // 为原生测试留出确认前的文件检查与离屏截图时间，最长等待十五秒。
  async function waitForNativeInspection() {
    if (!fixture.holdForInspection) return;
    const deadline = Date.now() + 15000;
    while (!window.__magnetAllowConfirm && Date.now() < deadline) await delay(40);
    check(window.__magnetAllowConfirm === true, "原生测试未在十五秒内完成确认前检查");
  }
  // 模拟输入并触发页面既有事件。
  function input(selector, value) {
    const node = element(selector);
    node.value = value;
    node.dispatchEvent(new Event("input", { bubbles: true }));
  }
  // 读取原生任务状态，确保前端显示与后端一致。
  async function task(id) { return (await call("getDownloads")).items.find(function matchingTask(item) { return item.id === id; }); }
  // 返回当前任务卡片，任务 ID 由 Qt 生成。
  function card(id) { return element('[data-download-id="' + id + '"]'); }
  // 检查真实解析卡片的文本与等待时间，兼容尚未提供发现诊断字段的后端。
  function checkResolvingPresentation(current) {
    const view = document.querySelector('[data-download-id="' + current.id + '"]'); // 卡片刷新可能晚于本次原生快照。
    if (current.status !== "resolving" || !view || view.dataset.status !== "resolving") return;
    const discovery = view.querySelector(".download-bytes"); // 发现提示必须保持单纯文本，不能创建动态 HTML。
    const elapsed = view.querySelector(".download-speed"); // 元数据阶段使用原速度位置展示实际等待时间。
    check(discovery.childElementCount === 0 && discovery.textContent.trim().length > 0, "解析节点提示缺失或被解释为 HTML");
    check(elapsed.childElementCount === 0 && /^(?:—|已等待 \d+ 秒|已等待 \d+ 分 \d+ 秒)$/.test(elapsed.textContent), "解析等待时间不是清晰的纯文本");
    if (!current.discoveryMessage) check(discovery.textContent === "正在寻找可用节点并获取文件列表…", "缺少发现诊断时没有保留原有解析提示");
    if (current.metadataElapsedSeconds === undefined) check(elapsed.textContent === "—", "缺少后端计时数据时界面虚构了等待时长");
  }
  // 检查文件弹窗是否确实属于指定任务并已加载所有文件。
  function filesReady(id, expectedCount) {
    const dialog = element("#download-files-dialog");
    return dialog.open && dialog.dataset.downloadId === id && !element("#download-files-list").inert
      && document.querySelectorAll("#download-files-list input[data-file-index]").length === expectedCount;
  }
  // 通过原有创建表单输入磁力链接，验证协议自动识别。
  async function createFromForm() {
    const previous = new Set((await call("getDownloads")).items.map(function previousTaskId(item) { return item.id; }));
    input("#download-url", fixture.magnet);
    input("#download-directory", fixture.directory);
    element("#add-download").click();
    await waitFor(async function taskCreated() { return (await call("getDownloads")).items.some(function newTask(item) { return !previous.has(item.id); }); }, "磁力链接没有通过原有表单创建任务");
    await waitFor(function creationFinished() { return !element("#add-download").disabled; }, "创建磁力任务后表单没有恢复");
    return (await call("getDownloads")).items.find(function createdTask(item) { return !previous.has(item.id); });
  }

  try {
    check(fixture && typeof fixture.magnet === "string" && Array.isArray(fixture.files) && fixture.files.length >= 2, "磁力测试 fixture 至少需要两个文件");
    window.confirm = function acceptTestConfirmation() { return true; };
    element('[data-page="downloads"]').click();
    const created = await createFromForm();
    check(created.kind === "magnet", "创建表单没有将磁力链接识别为磁力任务");
    await waitFor(async function metadataResolved() {
      const current = await task(created.id); // 在真实解析等待期间检查呈现，不替换桥接或伪造后端数据。
      checkResolvingPresentation(current);
      return current.status === "awaiting_selection";
    }, "磁力任务未获取文件列表");
    await waitFor(function selectionPresented() { return filesReady(created.id, fixture.files.length); }, "获取元数据后没有自动显示文件选择框");
    const waiting = await task(created.id);
    check(!waiting.selectionConfirmed && waiting.bytesReceived === 0, "确认文件前已经开始内容下载");
    check(card(created.id).dataset.kind === "magnet" && card(created.id).dataset.status === "awaiting_selection", "卡片没有呈现磁力待选状态");
    for (const file of fixture.files) {
      const checkbox = element('#download-files-list input[data-file-index="' + file.index + '"]');
      check(checkbox.closest("label").querySelector(".download-file-path").textContent === file.path, "文件路径未使用纯文本完整显示");
    }
    check(!element("#download-files-list").querySelector("script,img,iframe,svg"), "文件路径被解析成了 HTML 元素");

    element("#download-files-all").click();
    check(Array.from(document.querySelectorAll("#download-files-list input")).every(function everyFileChecked(node) { return node.checked; }), "全选没有勾选所有文件");
    element("#download-files-none").click();
    check(element("#download-files-confirm").disabled, "全不选后仍允许开始下载");
    element("#download-files-form").requestSubmit();
    check(!element("#download-files-error").hidden && element("#download-files-error").textContent.includes("至少"), "空选择提交没有明确提示");
    check((await task(created.id)).status === "awaiting_selection", "空选择错误地启动了下载");
    const selectedFile = fixture.files.find(function nonemptyFile(file) { return file.size > 0; }) || fixture.files[0]; // 只下载一个非空文件，便于原生测试检查未选文件。
    let checkbox = element('#download-files-list input[data-file-index="' + selectedFile.index + '"]');
    checkbox.click(); checkbox.focus();
    check(element("#download-files-summary").textContent.includes("已选 1 / " + fixture.files.length), "选中数量统计不正确");
    await delay(350);
    check(document.activeElement === checkbox && checkbox.isConnected, "进度刷新重建了文件选择控件或抢走焦点");

    element("#download-files-dialog").dispatchEvent(new Event("cancel", { cancelable: true }));
    check(!element("#download-files-dialog").open, "ESC 没有关闭文件选择框");
    await delay(500);
    check(!element("#download-files-dialog").open && (await task(created.id)).status === "awaiting_selection", "关闭弹窗后任务丢失或自动反复弹窗");

    if (fixture.magnetSecond) {
      const otherDialog = element("#app-dialog"); // 模拟其他功能已打开 HTML 模态，不修改其业务状态。
      otherDialog.showModal();
      auxiliaryTask = await call("createDownload", { url: fixture.magnetSecond, directory: fixture.directory }); // 第二链接应来自不同 info-hash 的本地 fixture。
      await waitFor(async function secondMetadataResolved() { return (await task(auxiliaryTask.id)).status === "awaiting_selection"; }, "第二磁力任务未完成解析");
      await delay(400);
      check(otherDialog.open && !element("#download-files-dialog").open, "磁力文件列表抢占了其他模态");
      otherDialog.close();
      await waitFor(function queuedSelectionPresented() { return element("#download-files-dialog").open && element("#download-files-dialog").dataset.downloadId === auxiliaryTask.id; }, "其他模态关闭后没有继续处理待选队列");
      element("#download-files-close").click();
      await call("cancelDownload", auxiliaryTask.id);
      auxiliaryTask = null;
    }

    await waitFor(function selectionButtonAvailable() { const button = card(created.id).querySelector('[data-action="select"]'); return button && !button.hidden && !button.disabled; }, "关闭后卡片没有保留选择文件按钮");
    card(created.id).querySelector('[data-action="select"]').click();
    await waitFor(function filesReopened() { return filesReady(created.id, fixture.files.length); }, "卡片无法重新打开文件列表");
    checkbox = element('#download-files-list input[data-file-index="' + selectedFile.index + '"]');
    check(checkbox.checked && document.querySelectorAll("#download-files-list input:checked").length === 1, "重新打开时丢失了未确认的勾选草稿");
    window.__magnetSelectionReady = { id: created.id, files: fixture.files }; // C++ 由此核对尚未下载内容并截取文件选择弹窗。
    await waitForNativeInspection();
    element("#download-files-confirm").click();
    await waitFor(async function selectedDownloadStarted() { const current = await task(created.id); return current.selectionConfirmed && current.selectedCount === 1; }, "文件确认没有传递所选索引给 Qt");
    await waitFor(function dialogClosedAfterConfirm() { return !element("#download-files-dialog").open; }, "确认成功后文件列表没有关闭");
    await waitFor(async function selectedDownloadCompleted() { return (await task(created.id)).status === "completed"; }, "选择的磁力文件未完成下载");
    const completed = await task(created.id);
    check(completed.totalBytes === selectedFile.size && completed.bytesReceived === selectedFile.size, "完成进度没有按所选文件统计");
    const confirmedFiles = await call("getDownloadFiles", created.id);
    check(confirmedFiles.files.filter(function selectedNativeFile(file) { return file.selected; }).length === 1, "Qt 没有保存单文件选择");
    check(confirmedFiles.files.find(function requestedNativeFile(file) { return file.index === selectedFile.index; }).selected, "Qt 下载了错误的文件索引");
    await waitFor(function completedCardPresented() { return card(created.id).dataset.status === "completed"; }, "完成状态没有同步到磁力卡片");
    window.__magnetSmokeResult = {
      ok: true, // C++ 读取此标识判断脚本成功。
      id: created.id, // 供原生测试直接查找此次任务。
      completed, // 原生任务终态，供 C++ 检查实际目录和大小。
      selectedIndices: [selectedFile.index], // 应当完整存在的已选文件。
      selectedFiles: [selectedFile], // 已选文件的索引、路径和预期大小。
      unselectedIndices: fixture.files.filter(function unselectedFixtureFile(file) { return file.index !== selectedFile.index; }).map(function unselectedIndex(file) { return file.index; }) // 供原生测试确认未选内容没有完整下载。
    };
  } catch (error) {
    window.__magnetSmokeResult = { ok: false, error: error.stack || error.message || String(error) }; // 保留失败上下文以便 C++ 输出。
  } finally {
    window.confirm = originalConfirm;
    if (auxiliaryTask) await call("cancelDownload", auxiliaryTask.id).catch(function ignoreTestCleanupError() {});
  }
})();
