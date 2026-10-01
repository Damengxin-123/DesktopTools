/* 在真实 WebChannel 上验证表情页：卡片、关键字搜索、分类筛选、编辑与复制。 */
(async function exerciseEmojiPage() {
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
  // 输入搜索词并派发输入事件。
  function search(value) { element("#emoji-search").value = value; element("#emoji-search").dispatchEvent(new Event("input")); }
  // 在指定文字的卡片上点击按钮。
  async function cardAction(name, label) {
    await waitFor(() => [...document.querySelectorAll(".emoji-card")].some(card => card.textContent.includes(name)), "找不到卡片：" + name);
    const card = [...document.querySelectorAll(".emoji-card")].find(item => item.textContent.includes(name));
    const button = [...card.querySelectorAll("button")].find(item => item.textContent === label);
    if (!button) throw new Error("按钮不存在：" + label);
    button.click();
  }
  try {
    const call = window.desktopBridge.call;
    const fixture = window.__emojiFixture;
    if (!fixture || !fixture.firstId || !fixture.secondId) throw new Error("缺少表情测试数据");
    element("[data-page='emoji']").click();
    await waitFor(() => document.querySelectorAll(".emoji-card").length === 2, "表情卡片未显示");
    await waitFor(() => element(".emoji-thumbnail").naturalWidth > 0, "缩略图未加载");

    // 关键字检索同时匹配关键字和文件名。
    search("开心");
    await waitFor(() => document.querySelectorAll(".emoji-card").length === 1 && element(".emoji-card").textContent.includes("笑脸"), "关键字搜索未生效");
    search("蓝星");
    await waitFor(() => document.querySelectorAll(".emoji-card").length === 1 && element(".emoji-card").textContent.includes("蓝星"), "按文件名搜索失败");
    search("");
    await waitFor(() => document.querySelectorAll(".emoji-card").length === 2, "清除搜索未恢复列表");

    // 新建分类并通过编辑对话框移动表情、更新关键字。
    element("#add-emoji-category").click();
    await waitFor(() => element("#emoji-dialog").open, "新建分类对话框未打开");
    element("#emoji-category-name").value = "常用";
    element("#emoji-dialog-confirm").click();
    await waitFor(async () => (await call("getEmojis", "")).categories.length === 2, "新建分类未保存");
    await waitFor(() => !element("#emoji-dialog").open, "分类保存后对话框未关闭");
    const targetCategory = (await call("getEmojis", "")).categories.find(item => item.name === "常用");
    if (!targetCategory) throw new Error("找不到新建的常用分类");
    await cardAction("蓝星", "编辑");
    await waitFor(() => element("#emoji-dialog").open, "编辑对话框未打开");
    element("#emoji-keywords-input").value = "星星 蓝色";
    element("#emoji-category-input").value = targetCategory.id;
    element("#emoji-dialog-confirm").click();
    await waitFor(async function editSaved() {
      const items = (await call("getEmojis", "")).items;
      const target = items.find(item => item.id === fixture.secondId);
      return target && target.keywords.join(" ") === "星星 蓝色" && target.categoryId === targetCategory.id;
    }, "表情编辑未保存");
    await waitFor(() => !element("#emoji-dialog").open, "编辑保存后对话框未关闭");

    // 分类筛选只显示该分类的表情。
    const chip = [...document.querySelectorAll("#emoji-categories .category-select")].find(item => item.textContent === "常用");
    chip.click();
    await waitFor(() => document.querySelectorAll(".emoji-card").length === 1 && element(".emoji-card").textContent.includes("蓝星"), "分类筛选失败");
    [...document.querySelectorAll("#emoji-categories .category-select")].find(item => item.textContent === "全部").click();
    await waitFor(() => document.querySelectorAll(".emoji-card").length === 2, "返回全部分类失败");

    // 复制文件给出成功提示；剪贴板内容与历史由原生测试验证。
    await cardAction("笑脸", "复制文件");
    await waitFor(() => element("#toast-region").textContent.includes("文件已复制到剪贴板"), "复制文件未提示成功");

    // 删除表情需要确认，原文件不删除。
    await cardAction("蓝星", "删除");
    await waitFor(() => element("#emoji-dialog").open && element("#emoji-dialog-body").textContent.includes("不会删除原图片文件"), "删除确认未说明保留原文件");
    element("#emoji-dialog-confirm").click();
    await waitFor(async () => (await call("getEmojis", "")).items.length === 1, "删除表情未生效");
    await waitFor(() => !element("#emoji-dialog").open, "删除后对话框未关闭");

    // 删除分类把剩余表情留在默认分类。
    const removeChip = [...document.querySelectorAll("#emoji-categories .icon-button")].find(item => item.getAttribute("aria-label") === "删除分类“常用”");
    removeChip.click();
    await waitFor(() => element("#emoji-dialog").open, "删除分类确认未打开");
    element("#emoji-dialog-confirm").click();
    await waitFor(async () => (await call("getEmojis", "")).categories.length === 1, "删除分类未生效");
    const rest = (await call("getEmojis", "")).items;
    if (rest.length !== 1 || rest[0].categoryId !== "default") throw new Error("分类删除后表情未回到默认分类");
    window.__emojiResult = { ok: true }; // 向原生测试返回成功。
  } catch (error) {
    window.__emojiResult = { ok: false, error: error.stack || error.message }; // 返回失败步骤。
  }
})();
