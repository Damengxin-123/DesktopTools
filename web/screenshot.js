/* 截图页：热键设置、触发原生截图流程与历史记录管理。 */
(function installScreenshotPage() {
  "use strict";
  const state = {
    items: [], // 后端历史摘要。
    revision: 0, // 列表刷新序号，丢弃过期响应。
    timer: null, // 后端通知合并定时器。
    hotkeyDraft: null, // 正在输入的截图热键 {modifier,key}。
    hotkeySaving: false,
    currentId: "", // 预览弹窗显示的记录。
    initialized: false,
    options: null // 主页面提供的提示函数。
  };

  function byId(id) { return document.getElementById(id); }
  // 以纯文本创建节点，路径与时间不作为 HTML 执行。
  function node(tag, className, text) {
    const result = document.createElement(tag);
    if (className) result.className = className;
    if (text !== undefined) result.textContent = String(text);
    return result;
  }
  function report(error) {
    state.options.reportError(error);
  }
  // 事件里的 Promise 异常统一报告，并保持按钮可再次点击。
  function action(handler) {
    return async function invoked(event) {
      const source = event.currentTarget;
      if (source) source.disabled = true;
      try { await handler(event); }
      catch (error) { report(error); }
      finally { if (source && source.isConnected) source.disabled = false; }
    };
  }
  // 把 Windows 虚拟键码转为用户可读名称（与设置页保持一致）。
  function keyName(key) {
    if (key >= 112 && key <= 135) return "F" + (key - 111);
    if ((key >= 65 && key <= 90) || (key >= 48 && key <= 57)) return String.fromCharCode(key);
    if (key >= 96 && key <= 105) return "Num" + (key - 96);
    const names = { 8: "Backspace", 9: "Tab", 13: "Enter", 19: "Pause", 20: "CapsLock", 27: "Esc", 32: "Space", 33: "PageUp", 34: "PageDown", 35: "End", 36: "Home", 37: "Left", 38: "Up", 39: "Right", 40: "Down", 44: "PrintScreen", 45: "Insert", 46: "Delete", 106: "Num*", 107: "Num+", 109: "Num-", 110: "Num.", 111: "Num/", 144: "NumLock", 145: "ScrollLock", 186: ";", 187: "=", 188: ",", 189: "-", 190: ".", 191: "/", 192: "`", 219: "[", 220: "\\", 221: "]", 222: "'" };
    return names[key] || "0x" + Number(key).toString(16).toUpperCase();
  }
  function hotkeyName(modifier, key) {
    return ((modifier & 2) ? "Ctrl+" : "") + ((modifier & 1) ? "Alt+" : "") + ((modifier & 4) ? "Shift+" : "")
      + ((modifier & 8) ? "Win+" : "") + keyName(key);
  }
  // 时间显示为本地完整日期时间。
  function readableTime(value) {
    const date = new Date(value);
    if (Number.isNaN(date.getTime())) return "";
    return date.toLocaleString("zh-CN", { year: "numeric", month: "2-digit", day: "2-digit",
      hour: "2-digit", minute: "2-digit", second: "2-digit", hour12: false });
  }
  // 文件大小显示为 KB / MB。
  function readableSize(bytes) {
    if (!Number.isFinite(bytes) || bytes < 0) return "";
    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(0) + " KB";
    return (bytes / (1024 * 1024)).toFixed(1) + " MB";
  }

  function updateState() {
    byId("screenshot-count").textContent = String(state.items.length);
    byId("screenshot-summary").textContent = state.items.length + " 张";
    byId("screenshot-clear").disabled = !state.items.length;
    byId("screenshot-hotkey-save").disabled = !state.hotkeyDraft || state.hotkeySaving;
  }

  function renderList() {
    updateState();
    const target = byId("screenshot-list");
    target.replaceChildren();
    if (!state.items.length) {
      byId("screenshot-empty").hidden = false;
      return;
    }
    byId("screenshot-empty").hidden = true;
    for (const item of state.items) {
      const row = node("article", "screenshot-row");
      const thumb = node("img", "screenshot-thumb");
      thumb.src = item.thumbnail;
      thumb.alt = "截图缩略图";
      thumb.loading = "lazy";
      thumb.title = "点击查看原图";
      thumb.addEventListener("click", function thumbClicked() { openPreview(item.id).catch(report); });
      const content = node("div", "screenshot-row-content");
      content.append(node("p", "screenshot-row-title", "截图于 " + readableTime(item.capturedAt)));
      const meta = node("div", "screenshot-row-meta");
      meta.append(node("span", "", item.width + " × " + item.height + " px"),
        node("span", "", readableSize(item.bytes)));
      content.append(meta);
      const actions = node("div", "screenshot-row-actions");
      const copy = node("button", "button secondary", "复制");
      copy.type = "button";
      copy.addEventListener("click", action(async function copyClicked(event) {
        await window.desktopBridge.call("copyScreenshot", item.id);
        state.options.toast("截图已复制到剪贴板");
      }));
      const open = node("button", "button secondary", "打开位置");
      open.type = "button";
      open.addEventListener("click", action(async function openClicked() {
        await window.desktopBridge.call("openScreenshotDirectory", item.id);
      }));
      const remove = node("button", "button danger quiet", "删除");
      remove.type = "button";
      remove.addEventListener("click", action(async function removeClicked() {
        await window.desktopBridge.call("deleteScreenshots", [item.id]);
        state.options.toast("已删除截图记录");
      }));
      actions.append(copy, open, remove);
      row.append(thumb, content, actions);
      target.append(row);
    }
  }

  async function openPreview(id) {
    const record = await window.desktopBridge.call("getScreenshot", id);
    state.currentId = id;
    byId("screenshot-dialog-image").src = record.image;
    byId("screenshot-dialog-meta").textContent =
      "截图时间 " + readableTime(record.capturedAt) + " · " + record.width + " × " + record.height + " px";
    byId("screenshot-dialog").showModal();
  }

  function closePreview() {
    if (byId("screenshot-dialog").open) byId("screenshot-dialog").close();
    state.currentId = "";
  }

  async function refresh() {
    const revision = ++state.revision;
    try {
      const data = await window.desktopBridge.call("getScreenshotHistory");
      if (revision !== state.revision) return;
      state.items = data.items;
      byId("screenshot-error").hidden = true;
    } catch (error) {
      if (revision !== state.revision) return;
      byId("screenshot-error").textContent = error.message;
      byId("screenshot-error").hidden = false;
    }
    renderList();
  }

  function scheduleRefresh() {
    clearTimeout(state.timer);
    state.timer = setTimeout(function runScheduledRefresh() { refresh().catch(report); }, 100);
  }

  // 读取热键设置与启动警告，刷新输入框显示。
  async function refreshHotkey() {
    const settings = await window.desktopBridge.call("getSettings");
    state.hotkeyDraft = null;
    byId("screenshot-hotkey-input").value =
      hotkeyName(Number(settings.screenshotHotkeyModifier), Number(settings.screenshotHotkeyKey));
    const info = await window.desktopBridge.call("getAppInfo");
    const warning = info && info.screenshotHotkeyWarning ? String(info.screenshotHotkeyWarning) : "";
    byId("screenshot-hotkey-warning").textContent = warning;
    byId("screenshot-hotkey-warning").hidden = !warning;
    updateState();
  }

  // 捕获热键但保留 Tab 键的键盘导航能力。
  function captureHotkey(event) {
    if (event.key === "Tab") return;
    event.preventDefault();
    event.stopPropagation();
    if (event.repeat) return;
    const key = (event.code.charCodeAt ? virtualKeyFromCode(event.code) : 0);
    if (!key) return;
    const modifier = (event.altKey ? 1 : 0) | (event.ctrlKey ? 2 : 0)
      | (event.shiftKey ? 4 : 0) | (event.metaKey ? 8 : 0);
    state.hotkeyDraft = { modifier, key };
    byId("screenshot-hotkey-input").value = hotkeyName(modifier, key);
    byId("screenshot-hotkey-state").textContent = "尚未保存";
    updateState();
  }

  // 通过 KeyboardEvent.code 将物理按键映射到 Windows 虚拟键码。
  function virtualKeyFromCode(code) {
    if (/^Key[A-Z]$/.test(code)) return code.charCodeAt(3);
    if (/^Digit[0-9]$/.test(code)) return code.charCodeAt(5);
    if (/^Numpad[0-9]$/.test(code)) return 96 + Number(code.slice(6));
    if (/^F([1-9]|1[0-9]|2[0-4])$/.test(code)) return 111 + Number(code.slice(1));
    const keys = { Backspace: 8, Enter: 13, NumpadEnter: 13, Pause: 19, CapsLock: 20, Escape: 27, Space: 32, PageUp: 33, PageDown: 34, End: 35, Home: 36, ArrowLeft: 37, ArrowUp: 38, ArrowRight: 39, ArrowDown: 40, PrintScreen: 44, Insert: 45, Delete: 46, NumpadMultiply: 106, NumpadAdd: 107, NumpadSubtract: 109, NumpadDecimal: 110, NumpadDivide: 111, NumLock: 144, ScrollLock: 145, Semicolon: 186, Equal: 187, Comma: 188, Minus: 189, Period: 190, Slash: 191, Backquote: 192, BracketLeft: 219, Backslash: 220, BracketRight: 221, Quote: 222 };
    return keys[code] || 0;
  }

  async function initialize(options) {
    state.options = options;
    if (!state.initialized) {
      state.initialized = true;
      byId("screenshot-start").addEventListener("click", action(async function startClicked() {
        await window.desktopBridge.call("startScreenshot");
      }));
      byId("screenshot-hotkey-input").addEventListener("keydown", captureHotkey);
      byId("screenshot-hotkey-save").addEventListener("click", action(async function saveHotkeyClicked() {
        if (!state.hotkeyDraft || state.hotkeySaving) return;
        state.hotkeySaving = true;
        updateState();
        try {
          await window.desktopBridge.call("saveScreenshotHotkey",
            state.hotkeyDraft.modifier, state.hotkeyDraft.key);
          state.hotkeyDraft = null;
          byId("screenshot-hotkey-state").textContent = "已保存";
          state.options.toast("截图热键已保存");
          await refreshHotkey();
        } finally {
          state.hotkeySaving = false;
          updateState();
        }
      }));
      byId("screenshot-clear").addEventListener("click", function clearClicked() {
        if (!state.items.length) return;
        if (!window.confirm("确定清空全部截图记录吗？\n对应的截图文件将从磁盘删除，此操作无法撤销。")) return;
        window.desktopBridge.call("clearScreenshotHistory")
          .then(function cleared() { state.options.toast("已清空截图记录"); })
          .catch(report);
      });
      byId("screenshot-dialog-close").addEventListener("click", closePreview);
      byId("screenshot-dialog-cancel").addEventListener("click", closePreview);
      byId("screenshot-dialog-copy").addEventListener("click", action(async function dialogCopyClicked() {
        if (!state.currentId) return;
        await window.desktopBridge.call("copyScreenshot", state.currentId);
        state.options.toast("截图已复制到剪贴板");
      }));
      byId("screenshot-dialog").addEventListener("cancel", function dialogCancelled(event) {
        event.preventDefault();
        closePreview();
      });
      await window.desktopBridge.on("screenshotsChanged", scheduleRefresh);
      await window.desktopBridge.on("settingsChanged", function settingsChanged() {
        refreshHotkey().catch(report);
      });
    }
    await Promise.all([refresh(), refreshHotkey()]);
  }

  // 对主页面仅暴露初始化入口。
  window.desktopScreenshot = Object.freeze({ initialize });
})();
