// 在独立临时数据目录上执行真实网页与 Qt 通信，不使用模拟后端。
(async function exerciseDesktopApplication() {
  "use strict";
  // 检查条件并保留清晰的失败步骤。
  function check(condition, message) { if (!condition) throw new Error(message); }
  // 等待浏览器事件循环和 Qt 异步消息。
  function pause() { return new Promise(resolve => setTimeout(resolve, 40)); }
  // 等待异步界面达到指定状态。
  async function waitFor(predicate, message) {
    for (let attempt = 0; attempt < 200; ++attempt) {
      if (await predicate()) return;
      await pause();
    }
    throw new Error(message);
  }
  // 查找必须存在的测试控件。
  function element(selector) {
    const node = document.querySelector(selector);
    check(node, "控件不存在：" + selector);
    return node;
  }
  // 模拟输入事件，确保页面脏标记及字段状态与正常操作一致。
  function input(selector, value) {
    const node = element(selector);
    node.value = value;
    node.dispatchEvent(new Event("input", { bubbles: true }));
    node.dispatchEvent(new Event("change", { bubbles: true }));
  }
  try {
    await waitFor(() => document.documentElement.dataset.ready === "true", "应用初始化失败");
    const call = window.desktopBridge.call;
    check((await call("getShortcuts")).items.length === 0, "测试目录应为空");
    check((await call("getNotes")).items.length === 0, "便签测试目录应为空");

    // 通过可见表单创建带逗号的快捷方式，验证新持久化格式。
    element("#add-shortcut").click();
    await waitFor(() => document.querySelector("#shortcut-title"), "新增快捷方式表单未打开");
    input("#shortcut-title", "Qt 文档, 常用");
    input("#shortcut-type", "1");
    input("#shortcut-target", "https://doc.qt.io/qt-6/");
    input("#shortcut-category", "default");
    element("#dialog-submit").click();
    await waitFor(async () => (await call("getShortcuts")).items.length === 1, "快捷方式表单没有保存到 Qt");
    await waitFor(() => !element("#app-dialog").open, "保存后对话框没有关闭");
    await waitFor(() => element("#shortcut-list").textContent.includes("Qt 文档, 常用"), "Qt 变更信号未刷新列表");
    const first = (await call("getShortcuts")).items[0];
    const category = await call("saveShortcutCategory", "", "开发资料");
    const second = await call("saveShortcut", { title: "项目主页", target: "https://www.qt.io/", type: 1, categoryId: category.id });
    await call("moveShortcut", first.id, category.id, second.id);
    const moved = (await call("getShortcuts")).items.filter(item => item.categoryId === category.id);
    check(moved[0].id === first.id, "跨分类移动与排序失败");
    let duplicateRejected = false;
    try { await call("saveShortcut", { title: first.title, target: "https://example.com/", type: 1, categoryId: category.id }); }
    catch (error) { duplicateRejected = Boolean(error.message); }
    check(duplicateRejected, "后端错误未通过 Promise 返回");

    // 验证搜索交互。
    input("#shortcut-search", "没有这样的快捷方式");
    await waitFor(() => !element("#shortcut-list").textContent.includes("Qt 文档, 常用"), "搜索未过滤列表");
    input("#shortcut-search", "");

    // 通过网页富文本编辑器创建并保存便签。
    element("[data-page='notes']").click();
    element("#add-note").click();
    await waitFor(() => !element("#note-editor-wrap").hidden, "便签编辑器没有打开");
    input("#note-title", "迁移验证便签");
    const editor = element("#note-editor");
    editor.innerHTML = "<h2>你好，HTML + Qt</h2><p><strong>这是一段中文内容。</strong></p><p style='white-space:pre-wrap'>a    b</p><ul><li>分类与排序</li><li>本地保存</li></ul>";
    editor.dispatchEvent(new InputEvent("input", { bubbles: true, inputType: "insertText" }));
    // 超深粘贴不能截断已有内容，也不能把截断片段插入编辑器。
    const beforePaste = editor.innerHTML;
    const clipboard = new DataTransfer();
    clipboard.setData("text/html", "<div>".repeat(110) + "过深片段" + "</div>".repeat(110));
    editor.dispatchEvent(new ClipboardEvent("paste", { clipboardData: clipboard, bubbles: true, cancelable: true }));
    await waitFor(() => element("#toast-region").textContent.includes("100 层"), "超深粘贴未报告错误");
    check(editor.innerHTML === beforePaste, "净化失败改变了原编辑内容");
    const originalConfirm = window.confirm;
    window.confirm = () => false;
    check(window.desktopToolCanClose() === false, "未保存便签未阻止退出");
    element("#save-note").click();
    await waitFor(async () => (await call("getNotes")).items.length === 1, "便签表单没有保存到 Qt");
    await waitFor(() => window.desktopToolCanClose() === true, "保存完成后仍被标记为未保存");
    window.confirm = originalConfirm;
    const note = (await call("getNotes")).items[0];
    const loaded = await call("getNote", note.id);
    check(loaded.html.includes("这是一段中文内容"), "便签内容读取不完整");
    // 强制经过 Qt 导出 HTML 再回到网页，验证连续空格的保留。
    const switchNote = await call("saveNote", { title: "切换占位", categoryId: "default", html: "<p>切换测试</p>" });
    await waitFor(() => Array.from(document.querySelectorAll(".note-row-title")).some(node => node.textContent === "切换占位"), "便签刷新未出现切换记录");
    Array.from(document.querySelectorAll(".note-row-title")).find(node => node.textContent === "切换占位").click();
    await waitFor(() => element("#note-title").value === "切换占位", "便签切换失败");
    Array.from(document.querySelectorAll(".note-row-title")).find(node => node.textContent === "迁移验证便签").click();
    await waitFor(() => element("#note-title").value === "迁移验证便签", "旧便签重新加载失败");
    check(Array.from(editor.querySelectorAll("p")).some(node => node.textContent.includes("a    b") && getComputedStyle(node).whiteSpace === "pre-wrap"), "Qt 导出段落的连续空格未保留");
    element("#save-note").click();
    await waitFor(() => !element("#save-note").disabled, "便签再次保存未完成");
    check((await call("getNote", note.id)).html.includes("a    b"), "便签往返后空格丢失");

    // 验证图片往返与危险 HTML 的后端过滤。
    const canvas = document.createElement("canvas");
    canvas.width = 4; canvas.height = 4;
    canvas.getContext("2d").fillRect(0, 0, 4, 4);
    const rich = await call("saveNote", { id: note.id, title: note.title, categoryId: "default", html:
      "<p>图片与过滤验证</p><img src='" + canvas.toDataURL("image/png") + "'><script>window.__unsafeNoteExecuted=true;<\/script>" });
    check(rich.id === note.id && rich.html.includes("data:image/png;base64,"), "图片或稳定 ID 往返失败");
    check(!rich.html.includes("<script") && !rich.html.includes("file:///"), "便签危险内容未清理");
    check(!window.__unsafeNoteExecuted, "便签中脚本被执行");
    let imageRejected = false;
    try { await call("saveNote", { id: note.id, title: note.title, categoryId: "default", html: "<img src='file:///C:/Windows/win.ini'>" }); }
    catch (error) { imageRejected = Boolean(error.message); }
    check(imageRejected, "拒绝的图片应返回错误而不是伪装保存成功");
    check((await call("getNote", note.id)).html.includes("图片与过滤验证"), "失败保存破坏了原便签内容");

    // 设置由真实表单提交；测试已关闭 OS 热键和自启注册，不影响用户系统。
    element("[data-page='settings']").click();
    await waitFor(() => !element("#page-settings").hidden, "设置页面切换失败");
    input("#font-size", "19");
    check(!element("#auto-start").checked, "开机自启默认应关闭");
    element("#auto-start").click();
    check(element("#settings-save-state").textContent === "尚未保存", "自启勾选未标记未保存状态");
    element("#settings-form").requestSubmit();
    await waitFor(async () => (await call("getSettings")).fontSize === 19, "设置表单未保存到 Qt");
    await waitFor(() => !element("#settings-form").inert, "设置表单保存未结束");
    check((await call("getSettings")).autoStart === true, "开机自启勾选未保存到 Qt");
    check(element("#auto-start").checked, "保存后自启勾选状态丢失");
    element("#auto-start").click();
    element("#settings-form").requestSubmit();
    await waitFor(async () => (await call("getSettings")).autoStart === false, "取消开机自启未保存");
    await waitFor(() => !element("#settings-form").inert, "取消自启保存未结束");
    await call("saveSettings", { autoStart: true });
    await call("resetSettings");
    check((await call("getSettings")).fontSize === 16, "恢复默认设置失败");
    check((await call("getSettings")).autoStart === false, "恢复默认未关闭开机自启");
    await waitFor(() => !element("#auto-start").checked, "恢复默认后未同步自启复选框");

    // 关于卡片：头像与昵称随应用打包展示，跳转按钮调用原生接口打开固定仓库（测试不点击）。
    await waitFor(() => element("#github-avatar").naturalWidth > 0, "GitHub 头像未加载");
    check(element(".about-name").textContent === "天涯快乐人", "关于昵称显示错误");
    check(element(".about-repo-url").textContent === "https://github.com/Damengxin-123/DesktopTools", "仓库地址显示错误");
    check(element("#open-github-repository").textContent.includes("跳转"), "仓库跳转按钮缺失");

    // 批量删除和分类删除均由后端执行，不触发任何外部程序。
    await call("deleteShortcuts", [first.id, second.id]);
    await call("deleteShortcutCategory", category.id);
    await call("deleteNotes", [note.id, switchNote.id]);
    check((await call("getShortcuts")).items.length === 0, "快捷方式批删失败");
    check((await call("getNotes")).items.length === 0, "便签删除失败");


    // 网格图：新建、双击打开、涂色、撤销、缩放、拖动、线条设置、保存与删除。
    element("[data-page='gridmap']").click();
    await waitFor(() => !element("#page-gridmap").hidden, "网格图页面切换失败");
    check(element("#gridmap-editor").hidden, "未打开项目时编辑器应隐藏");
    element("#add-gridmap").click();
    await waitFor(() => element("#gridmap-dialog").open, "新建网格图对话框未打开");
    input("#gridmap-name-input", "机房示意");
    element("#gridmap-dialog-confirm").click();
    await waitFor(() => !element("#gridmap-editor").hidden, "网格图编辑器没有打开");
    await waitFor(() => element("#gridmap-count").textContent === "1", "网格图列表未刷新");
    await waitFor(() => element("#gridmap-zoom").textContent === "缩放 100%", "画布视图未复位");

    const gridCanvas = element("#gridmap-canvas");
    const gridRect = () => gridCanvas.getBoundingClientRect();
    // 复位视图后原点在画布中心，每格 24px；dx/dy 为格子坐标。
    const gridCell = (dx, dy) => {
      const rect = gridRect();
      return { clientX: rect.left + rect.width / 2 + (dx + 0.5) * 24, clientY: rect.top + rect.height / 2 + (dy + 0.5) * 24 };
    };
    const gridPoint = (x, y) => {
      const rect = gridRect();
      return { clientX: rect.left + x, clientY: rect.top + y };
    };
    const gridPointer = (type, point, button) => gridCanvas.dispatchEvent(new PointerEvent(type,
      { bubbles: true, cancelable: true, button, clientX: point.clientX, clientY: point.clientY, pointerId: 7, isPrimary: true }));
    const gridStroke = (points, button = 0) => {
      gridPointer("pointerdown", points[0], button);
      for (let index = 1; index < points.length; ++index) gridPointer("pointermove", points[index], button);
      gridPointer("pointerup", points[points.length - 1], button);
    };
    const gridUndo = () => document.dispatchEvent(new KeyboardEvent("keydown",
      { key: "z", ctrlKey: true, bubbles: true, cancelable: true }));

    // 单击涂色与 Ctrl+Z 撤销。
    gridStroke([gridCell(1, 0)]);
    await waitFor(() => element("#gridmap-cells").textContent === "1 格", "涂色没有生效");
    check(element("#gridmap-save-state").textContent === "未保存的修改", "涂色未标记未保存状态");
    gridUndo();
    await waitFor(() => element("#gridmap-cells").textContent === "0 格", "Ctrl+Z 撤销涂色失败");

    // 跨两格拖动涂色，修改网格线设置后一起保存。
    gridStroke([gridCell(1, 0), gridCell(2, 0)]);
    await waitFor(() => element("#gridmap-cells").textContent === "2 格", "拖动涂色没有覆盖经过的格子");
    input("#gridmap-line-color", "#123456");
    input("#gridmap-line-width", "3");
    element("#gridmap-save").click();
    const gridId = (await call("getGridMaps")).items[0].id;
    await waitFor(async () => {
      const data = await call("getGridMap", gridId);
      return data.cells.length === 2 && data.lineColor === "#123456" && data.lineWidth === 3;
    }, "网格图保存不完整");
    const savedCells = (await call("getGridMap", gridId)).cells;
    check(savedCells.some(cell => cell[0] === 1 && cell[1] === 0), "原点右侧格子坐标不正确");
    check(savedCells.some(cell => cell[0] === 2 && cell[1] === 0), "拖动终点格子坐标不正确");

    // 右键拖动把原点向左移两格，中心点击落到 (2, 0)，随后撤销。
    const dragStart = gridCell(0, 0);
    gridStroke([{ clientX: dragStart.clientX, clientY: dragStart.clientY },
      gridPoint(-48, 0), gridPoint(-48, 0)], 2);
    gridStroke([gridCell(0, 0)]);
    await waitFor(() => element("#gridmap-cells").textContent === "3 格", "拖动画布后涂色位置错误");
    gridUndo();
    await waitFor(() => element("#gridmap-cells").textContent === "2 格", "画布拖动后的撤销失败");

    // 复位视图让坐标确定，再验证空格框选与橡皮擦。
    element("#gridmap-reset-view").click();
    await pause();

    // 按住空格拖动框选 2x2 区域一次填充 4 格，Ctrl+Z 一步全部撤销。
    document.dispatchEvent(new KeyboardEvent("keydown", { key: " ", bubbles: true, cancelable: true }));
    gridStroke([gridCell(0, 1), gridCell(1, 2)]);
    document.dispatchEvent(new KeyboardEvent("keyup", { key: " ", bubbles: true }));
    await waitFor(() => element("#gridmap-cells").textContent === "6 格", "空格框选没有填充区域");
    gridUndo();
    await waitFor(() => element("#gridmap-cells").textContent === "2 格", "框选填充未能一次撤销");

    // 橡皮擦：选中状态可见，开启后清除格子，取消后恢复涂色。
    element("#gridmap-eraser").click();
    check(element("#gridmap-eraser").getAttribute("aria-pressed") === "true", "橡皮擦未标记选中状态");
    gridStroke([gridCell(1, 0)]);
    await waitFor(() => element("#gridmap-cells").textContent === "1 格", "橡皮擦没有清除格子");
    element("#gridmap-eraser").click();
    check(element("#gridmap-eraser").getAttribute("aria-pressed") === "false", "橡皮擦未取消选中状态");
    gridStroke([gridCell(1, 0)]);
    await waitFor(() => element("#gridmap-cells").textContent === "2 格", "取消橡皮擦后未能恢复涂色");

    // 当前颜色固定显示在常用颜色区最左侧，并随选色更新。
    check(element("#gridmap-current").style.backgroundColor === "rgb(224, 47, 47)", "当前颜色未显示在最左侧");
    element('.gridmap-swatch[title="#2f6fe0"]').click();
    check(element("#gridmap-color").value === "#2f6fe0", "选择常用颜色失败");
    check(element("#gridmap-current").style.backgroundColor === "rgb(47, 111, 224)", "当前颜色未随选色更新");

    // Alt+点击吸取格子颜色；空白格子不改变当前颜色。
    const pickPoint = gridCell(1, 0);
    gridCanvas.dispatchEvent(new PointerEvent("pointerdown",
      { bubbles: true, cancelable: true, button: 0, altKey: true, clientX: pickPoint.clientX, clientY: pickPoint.clientY, pointerId: 11 }));
    gridCanvas.dispatchEvent(new PointerEvent("pointerup",
      { bubbles: true, button: 0, clientX: pickPoint.clientX, clientY: pickPoint.clientY, pointerId: 11 }));
    await waitFor(() => element("#gridmap-color").value === "#e02f2f", "Alt 吸色未更新当前颜色");
    check(element("#gridmap-cells").textContent === "2 格", "吸色不应增减格子");
    const emptyPoint = gridCell(5, 5);
    gridCanvas.dispatchEvent(new PointerEvent("pointerdown",
      { bubbles: true, cancelable: true, button: 0, altKey: true, clientX: emptyPoint.clientX, clientY: emptyPoint.clientY, pointerId: 11 }));
    gridCanvas.dispatchEvent(new PointerEvent("pointerup",
      { bubbles: true, button: 0, clientX: emptyPoint.clientX, clientY: emptyPoint.clientY, pointerId: 11 }));
    check(element("#gridmap-color").value === "#e02f2f", "吸取空白格子不应改变当前颜色");

    // 画笔半径：1 时一次涂出 3×3 圆形区域，Ctrl+Z 一步撤销；恢复 0 为单格。
    input("#gridmap-brush-radius", "1");
    gridStroke([gridCell(-3, 3)]);
    await waitFor(() => element("#gridmap-cells").textContent === "11 格", "画笔半径没有涂出圆形区域");
    gridUndo();
    await waitFor(() => element("#gridmap-cells").textContent === "2 格", "半径笔刷未能一次撤销");
    input("#gridmap-brush-radius", "0");

    // 滚轮以光标为锚缩放。
    const wheelPoint = gridPoint(10, 10);
    gridCanvas.dispatchEvent(new WheelEvent("wheel",
      { bubbles: true, cancelable: true, deltaY: -120, clientX: wheelPoint.clientX, clientY: wheelPoint.clientY }));
    await waitFor(() => element("#gridmap-zoom").textContent === "缩放 115%", "滚轮缩放未生效");

    // 有未保存修改时切换页面必须确认；确认放弃后才放行。
    gridStroke([gridCell(-1, 0)]);
    await waitFor(() => element("#gridmap-cells").textContent === "3 格", "再次涂色失败");
    const originalGridConfirm = window.confirm;
    window.confirm = () => false;
    element("[data-page='settings']").click();
    check(!element("#page-gridmap").hidden, "未确认放弃修改时不应离开网格图页");
    window.confirm = () => true;
    element("[data-page='settings']").click();
    await waitFor(() => !element("#page-settings").hidden, "确认后未能离开网格图页");
    window.confirm = originalGridConfirm;
    element("[data-page='gridmap']").click();
    await waitFor(() => !element("#page-gridmap").hidden, "返回网格图页失败");

    // 重命名当前项目并从列表刷新。
    document.querySelectorAll(".gridmap-row .icon-button")[0].click();
    await waitFor(() => element("#gridmap-dialog").open, "重命名对话框未打开");
    input("#gridmap-name-input", "机房布置图");
    element("#gridmap-dialog-confirm").click();
    await waitFor(() => element(".gridmap-row-title").textContent === "机房布置图", "重命名未刷新列表");

    // 删除当前项目：编辑器关闭，后端记录清空。
    document.querySelectorAll(".gridmap-row .icon-button")[1].click();
    await waitFor(() => element("#gridmap-dialog").open, "删除对话框未打开");
    element("#gridmap-dialog-confirm").click();
    await waitFor(() => element("#gridmap-count").textContent === "0", "网格图未删除");
    await waitFor(() => element("#gridmap-editor").hidden, "删除当前项目后编辑器未关闭");
    check((await call("getGridMaps")).items.length === 0, "网格图记录未删除");

    // 桥接新建后双击列表项进入编辑。
    await call("createGridMap", "双击打开");
    await waitFor(() => element("#gridmap-count").textContent === "1", "桥接新建未刷新列表");
    element(".gridmap-row").dispatchEvent(new MouseEvent("dblclick", { bubbles: true }));
    await waitFor(() => !element("#gridmap-editor").hidden, "双击未能打开编辑器");
    check(element("#gridmap-title").textContent === "双击打开", "双击打开的项目不正确");
    document.querySelectorAll(".gridmap-row .icon-button")[1].click();
    await waitFor(() => element("#gridmap-dialog").open, "清理删除对话框未打开");
    element("#gridmap-dialog-confirm").click();
    await waitFor(() => element("#gridmap-count").textContent === "0", "清理网格图未删除");

    // 为离屏渲染截图提供临时示例数据，不写入用户数据目录。
    const work = await call("saveShortcutCategory", "", "工作常用");
    await call("saveShortcut", { title: "Qt 开发文档", target: "https://doc.qt.io/qt-6/", type: 1, categoryId: work.id });
    await call("saveShortcut", { title: "项目资料", target: "C:/Projects", type: 0, categoryId: work.id });
    await call("saveShortcut", { title: "灵感与参考", target: "https://www.qt.io/", type: 1, categoryId: "default" });
    await call("saveNote", { title: "今天的想法", categoryId: "default", html: "<h2>让日常井然有序</h2><p>用熟悉的 Qt 处理本地数据，用 HTML 打造界面。</p>" });
    const sampleGrid = await call("createGridMap", "机房布置示意");
    const sampleCells = [];
    for (let x = -3; x <= 3; ++x) { sampleCells.push([x, -2, "#e02f2f"]); sampleCells.push([x, 2, "#e02f2f"]); }
    for (let y = -1; y <= 1; ++y) { sampleCells.push([-3, y, "#e02f2f"]); sampleCells.push([3, y, "#e02f2f"]); }
    sampleCells.push([-1, 0, "#f08c1c"], [0, 0, "#f08c1c"], [1, 0, "#f08c1c"]);
    await call("saveGridMap", sampleGrid.id, { lineColor: "#94a3b8", lineWidth: 1, cells: sampleCells });
    element("[data-page='shortcuts']").click();
    window.__smokeResult = { ok: true, summary: "网页 CRUD、信号刷新、分类排序、错误回传、便签图片、退出保护、设置表单与网格图画板全部通过。" };
  } catch (error) {
    window.__smokeResult = { ok: false, error: error.stack || error.message || String(error) };
  }
})();
