/* 在真实 WebChannel 上验证剪贴板页，测试内容由 C++ 临时服务提供。 */
(async function exerciseClipboardPage() {
  "use strict";
  // 获取测试页面元素。
  function element(selector) { const result = document.querySelector(selector); if (!result) throw new Error("控件不存在：" + selector); return result; }
  // 等待原生信号和页面异步更新。
  async function waitFor(predicate, message) {
    for (let index = 0; index < 150; ++index) {
      if (await predicate()) return;
      await new Promise(resolve => setTimeout(resolve, 40));
    }
    throw new Error(message);
  }
  // 按按钮文字触发真实页面操作。
  function clickLabel(container, label) {
    const button = [...element(container).querySelectorAll("button")].find(item => item.textContent === label);
    if (!button) throw new Error("按钮不存在：" + label);
    button.click();
  }
  // 输入搜索词并派发输入事件。
  function search(value) { element("#clipboard-search").value = value; element("#clipboard-search").dispatchEvent(new Event("input")); }
  try {
    const call = window.desktopBridge.call;
    search("今天的灵感");
    await waitFor(() => document.querySelectorAll(".clipboard-card").length === 1 && element(".clipboard-card").textContent.includes("今天的灵感"), "全文搜索未生效");
    clickLabel(".clipboard-card", "查看");
    await waitFor(() => element("#clipboard-dialog-body").textContent.includes("<script>"), "完整文本详情未显示");
    if (window.clipboardInjected) throw new Error("剪贴板内容被执行");
    element("#clipboard-dialog-close").click();
    clickLabel(".clipboard-card", "置顶");
    await waitFor(() => document.querySelector(".clipboard-card.pinned"), "置顶后未刷新");
    search("");
    await waitFor(() => document.querySelectorAll(".clipboard-card").length === 3, "搜索清除后未恢复列表");
    if (!element(".clipboard-card").classList.contains("pinned")) throw new Error("置顶记录没有排在前面");
    element("#clipboard-filter").value = "files";
    element("#clipboard-filter").dispatchEvent(new Event("change"));
    await waitFor(() => document.querySelectorAll(".clipboard-card").length === 1 && element(".clipboard-card").textContent.includes("需求说明.pdf"), "文件类型筛选失败");
    clickLabel(".clipboard-card", "查看");
    await waitFor(() => element("#clipboard-dialog-body").textContent.includes("需求说明.pdf"), "文件路径详情未显示");
    element("#clipboard-dialog-close").click();
    element("#clipboard-select-all").click();
    element("#clipboard-delete").click();
    await waitFor(() => element("#clipboard-dialog").open, "批量删除确认未打开");
    element("#clipboard-dialog-confirm").click();
    await waitFor(async () => (await call("getClipboardHistory", "")).items.length === 2, "批量删除未生效");
    await waitFor(() => !element("#clipboard-dialog").open, "删除后弹窗未关闭");
    element("#clipboard-filter").value = "";
    element("#clipboard-filter").dispatchEvent(new Event("change"));
    await waitFor(() => document.querySelectorAll(".clipboard-card").length === 2, "删除后剩余记录错误");
    element("#clipboard-clear").click();
    element("#clipboard-dialog-cancel").click();
    if ((await call("getClipboardHistory", "")).items.length !== 2) throw new Error("取消清空仍删除了数据");
    element("#clipboard-clear").click();
    element("#clipboard-dialog-confirm").click();
    await waitFor(async () => (await call("getClipboardHistory", "")).items.length === 0, "清空未生效");
    await waitFor(() => !element("#clipboard-empty").hidden, "清空后未显示空状态");
    if ((await call("getClipboardHistory", "")).types.length !== 3) throw new Error("清空历史修改了监听设置");
    window.__clipboardResult = { ok: true }; // 向原生测试返回成功。
  } catch (error) {
    window.__clipboardResult = { ok: false, error: error.stack || error.message }; // 返回失败步骤。
  }
})();
