/* 界面本地化：只绑定程序创建的文案，不遍历或翻译运行时用户内容。 */
(function installInterfaceLanguage() {
  "use strict";
  const catalog = window.desktopEnglish || {}; // 随程序打包的英文文案。
  let language = "zh-CN"; // 当前界面语言；由持久化设置覆盖。
  const entries = new WeakMap(); // 节点绑定不阻止已删除节点回收。
  const references = new Set(); // 弱引用集合，仅在切换时访问可见节点。
  const liveTag = Symbol("interface-message"); // 区分界面表达式与普通用户字符串。

  // 只由源码中的明确界面文案调用；不用于用户标题、文件名或输入内容。
  function t(source) { return language === "en" && Object.hasOwn(catalog, source) ? catalog[source] : source; }
  // 仅替换程序模板的编号参数，用户参数只插入一次，不递归翻译。
  function format(source, ...args) { return t(source).replace(/\{(\d+)\}/g, (_, index) => String(args[Number(index)] ?? "")); }
  // 后端只在错误通道使用此转换；参数和路径作为不透明内容保留。
  const errorPatterns = Object.entries(catalog).filter(([key]) => /%\d+/.test(key)).map(([key, english]) => {
    const slots = [];
    const escaped = key.split(/(%\d+)/).map(part => {
      if (/^%\d+$/.test(part)) { slots.push(part); return "([\\s\\S]*?)"; }
      return part.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
    }).join("");
    return { expression: new RegExp("^" + escaped + "$"), slots, english };
  });
  // 已登记的完整错误模板优先；失败信息中的用户值不会再次经过字典。
  function errorText(source) {
    source = String(source ?? "");
    if (language !== "en") return source;
    if (Object.hasOwn(catalog, source)) return catalog[source];
    for (const pattern of errorPatterns) {
      const match = source.match(pattern.expression);
      if (match) return pattern.english.replace(/%\d+/g, slot => match[pattern.slots.indexOf(slot) + 1] ?? slot);
    }
    for (const [key, english] of Object.entries(catalog)) {
      if (key.length >= 6 && /：$/.test(key) && source.startsWith(key)) return english + source.slice(key.length);
    }
    return source;
  }
  // 保留错误原文，现有提示也能随语言切换；不修改后端结果或用户数据。
  function failure(source) {
    const result = new Error();
    Object.defineProperty(result, "message", { get() { return errorText(source); } });
    return result;
  }
  // 保留界面表达式，切换语言时重新求值；用户参数仍原样插入。
  function live(render) { return { [liveTag]: render, toString() { return String(render() ?? ""); } }; }
  // 解开绑定表达式，不对得到的用户字符串做字典匹配。
  function value(message) {
    while (message && message[liveTag]) message = message[liveTag]();
    return String(message ?? "");
  }
  // 记录属性绑定，后续写入同一属性时覆盖旧绑定。
  function bind(node, key, message) {
    if (!node) return;
    let bindings = entries.get(node);
    if (!bindings) { bindings = new Map(); entries.set(node, bindings); references.add(new WeakRef(node)); }
    bindings.set(key, message);
    update(node, key, message);
  }
  // 仅当值变化时更新 DOM，避免破坏输入状态或造成多余重排。
  function update(node, key, message) {
    const result = value(message);
    if (key === "text") { if (node.textContent !== result) node.textContent = result; }
    else if (node.getAttribute(key) !== result) node.setAttribute(key, result);
  }
  // 写入界面文本；普通字符串保持原样，只有显式表达式会产生翻译。
  function text(node, message) { bind(node, "text", message); }
  // 写入可访问名称、提示或占位文字，绝不改写表单 value。
  function attr(node, name, message) { bind(node, name, message); }
  // 创建可即时切换的文本节点，供图标与文字混排使用。
  function textNode(message) { const node = document.createTextNode(""); text(node, message); return node; }
  // 图标整理时转移原有文本绑定，避免丢失静态翻译来源。
  function sourceOf(node) { return entries.get(node)?.get("text") ?? node.textContent; }
  // 仅在脚本首次加载、业务数据进入页面之前登记 HTML 固定文案。
  function captureStatic() {
    const walk = document.createTreeWalker(document.documentElement, NodeFilter.SHOW_TEXT);
    const nodes = [];
    while (walk.nextNode()) nodes.push(walk.currentNode);
    for (const node of nodes) {
      if (node.parentElement?.closest("script,style,[contenteditable],textarea,[data-user-content]")) continue;
      const raw = node.textContent;
      const key = raw.trim();
      if (Object.hasOwn(catalog, key)) text(node, live(() => raw.replace(key, t(key))));
    }
    document.querySelectorAll("[title],[placeholder],[aria-label],[alt]").forEach(node => {
      for (const name of ["title", "placeholder", "aria-label", "alt"]) {
        const key = node.getAttribute(name);
        if (key && Object.hasOwn(catalog, key)) attr(node, name, live(() => t(key)));
      }
    });
  }
  // 原位更新已登记的文案，不重载页面、不重绘用户列表或编辑器。
  function setLanguage(next) {
    next = next === "en" ? "en" : "zh-CN";
    if (language === next) return;
    language = next;
    document.documentElement.lang = next;
    for (const reference of references) {
      const node = reference.deref();
      if (!node) { references.delete(reference); continue; }
      if (!node.isConnected) continue;
      for (const [key, message] of entries.get(node) || []) update(node, key, message);
    }
    document.dispatchEvent(new CustomEvent("desktop-language-changed", { detail: next }));
  }
  window.desktopI18n = Object.freeze({ t, format, errorText, failure, live, text, attr, textNode, sourceOf, setLanguage, get language() { return language; } });
  captureStatic();
  // 低频清理失效节点，不遍历 DOM 或业务数据。
  window.setInterval(function pruneCollectedBindings() {
    if (references.size < 1024) return;
    for (const reference of references) if (!reference.deref()) references.delete(reference);
  }, 30000);
})();
