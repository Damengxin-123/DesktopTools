/* 通过真实表单、下载卡片和 Qt 网络后端验证完整下载流程。 */
(async function verifyDownloadPage() {
  "use strict";
  const originalConfirm = window.confirm; // 在测试结束时恢复原有确认行为。
  const fixture = window.__downloadFixture; // C++ 提供的本机服务地址和临时目录。
  const call = window.desktopBridge.call; // 真实 QWebChannel 调用入口。

  // 断言失败时将原因交给 C++ 测试输出。
  function check(condition, message) { if (!condition) throw new Error(message); }
  // 获取应当存在的页面控件。
  function element(selector) {
    const node = document.querySelector(selector);
    check(node, "缺少下载控件：" + selector);
    return node;
  }
  // 等待异步网络和界面刷新完成，超时则给出具体步骤。
  async function waitFor(predicate, message) {
    const deadline = Date.now() + 15000;
    while (Date.now() < deadline) {
      if (await predicate()) return;
      await new Promise(resolve => window.setTimeout(resolve, 40));
    }
    throw new Error(message);
  }
  // 模拟用户输入并触发原有表单脏状态处理。
  function input(selector, value) {
    const node = element(selector);
    node.value = value;
    node.dispatchEvent(new Event("input", { bubbles: true }));
    node.dispatchEvent(new Event("change", { bubbles: true }));
  }
  // 按稳定标识读取 Qt 中的最新任务，避免以页面缓存断言后台成功。
  async function task(id) { return (await call("getDownloads")).items.find(item => item.id === id); }
  // 取得对应任务卡片中的原位更新节点。
  function card(id) { return element('[data-download-id="' + id + '"]'); }
  // 通过下载页面的常驻表单输入链接与目录，支持连续创建任务。
  async function createFromForm(path, directory) {
    const previous = new Set((await call("getDownloads")).items.map(item => item.id));
    input("#download-url", fixture.baseUrl + path);
    input("#download-directory", directory);
    element("#add-download").click();
    await waitFor(async () => (await call("getDownloads")).items.some(item => !previous.has(item.id)), "表单没有创建 Qt 下载任务");
    await waitFor(() => !element("#add-download").disabled, "创建任务后没有恢复表单");
    return (await call("getDownloads")).items.find(item => !previous.has(item.id));
  }

  try {
    window.confirm = () => true;
    element('[data-page="settings"]').click();
    input("#download-default-directory", fixture.defaultDirectory);
    element("#settings-form").requestSubmit();
    await waitFor(async () => (await call("getSettings")).downloadDirectory === fixture.defaultDirectory, "默认下载路径没有通过设置表单保存");
    await waitFor(() => !element("#settings-form").inert, "设置保存状态没有结束");
    element('[data-page="downloads"]').click();
    check(!element("#page-downloads").hidden, "下载页面没有显示");

    const quick = await createFromForm("/quick.txt", fixture.invalidDirectory);
    await waitFor(async () => (await task(quick.id)).status === "completed", "下载到默认目录未完成");
    const completeQuick = await task(quick.id);
    check(completeQuick.directory === fixture.defaultDirectory, "无效自定义路径没有回退默认路径");
    check(Boolean(completeQuick.warning), "目录回退缺少用户提示");
    check(completeQuick.bytesReceived === 65536 && completeQuick.totalBytes === 65536, "已下载或总大小不准确");
    await waitFor(() => card(quick.id).dataset.status === "completed", "完成状态没有从 Qt 刷新到卡片");
    check(card(quick.id).textContent.includes(completeQuick.fileName), "卡片没有显示文件名称");

    const first = await createFromForm("/slow-first.bin", fixture.customDirectory);
    const second = await createFromForm("/slow-unknown.bin", "");
    await waitFor(async () => {
      const left = await task(first.id), right = await task(second.id);
      return left.status === "downloading" && right.status === "downloading" && left.bytesReceived > 0 && right.bytesReceived > 0;
    }, "两个下载任务没有并行运行");
    await waitFor(() => document.querySelectorAll("[data-download-id]").length === 3, "下载列表未显示三个独立卡片");
    await waitFor(() => card(second.id).textContent.includes("大小未知"), "未知总长度被界面当成确定大小");
    check(!card(second.id).querySelector('[role="progressbar"]').hasAttribute("aria-valuenow"), "未知大小任务不应显示虚假的确定进度");
    const rectangles = Array.from(document.querySelectorAll("[data-download-id]")).map(node => node.getBoundingClientRect());
    check(rectangles.every((rect, index) => index === 0 || rect.top >= rectangles[index - 1].bottom - 1), "下载卡片不是从上到下的单列排列");

    window.confirm = () => false;
    check(window.desktopToolCanClose() === false, "活动下载退出未进行确认");
    window.confirm = () => true;
    const pause = card(first.id).querySelector('[data-action="pause"]');
    check(pause, "活动下载缺少暂停按钮");
    pause.focus();
    await new Promise(resolve => window.setTimeout(resolve, 350));
    check(document.activeElement === pause && pause.isConnected, "下载进度刷新打断了按钮焦点");
    pause.click();
    await waitFor(async () => (await task(first.id)).status === "paused", "卡片暂停按钮未停止 Qt 下载");
    await waitFor(() => card(first.id).dataset.status === "paused", "暂停状态未刷新到界面");
    const pausedSize = (await task(first.id)).bytesReceived;
    await new Promise(resolve => window.setTimeout(resolve, 150));
    check((await task(first.id)).bytesReceived === pausedSize && pausedSize > 0, "暂停后仍在写入数据");
    card(second.id).querySelector('[data-action="cancel"]').click();
    await waitFor(async () => (await task(second.id)).status === "cancelled", "取消按钮未取消对应任务");
    card(first.id).querySelector('[data-action="resume"]').click();
    await waitFor(async () => (await task(first.id)).status === "completed", "卡片继续按钮未完成断点续传");
    const completeFirst = await task(first.id);
    check(completeFirst.directory === fixture.customDirectory, "自定义下载路径未生效");
    check(completeFirst.bytesReceived === 2097152, "续传后的文件大小不准确");
    await waitFor(() => card(first.id).dataset.status === "completed" && card(second.id).dataset.status === "cancelled", "下载终态未刷新到界面");
    check((await call("getDownloads")).activeCount === 0, "任务完成后活动数量未清零");
    check(window.desktopToolCanClose() === true, "任务结束后仍错误阻止退出");
    window.__downloadSmokeResult = { ok: true, completed: [completeQuick, completeFirst] };
  } catch (error) {
    window.__downloadSmokeResult = { ok: false, error: error.stack || error.message || String(error) };
  } finally {
    window.confirm = originalConfirm;
  }
})();
