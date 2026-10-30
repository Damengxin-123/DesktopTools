/* 自绘 24×24 圆角线性图标，统一线宽并随文字颜色变化，不依赖系统字体。 */
(function installIcons() {
  "use strict";
  // 图标路径目录；路径均为程序内置常量，不接收用户提供的 SVG。
  const paths = {
    shortcuts: 'M9 5H5v14h14v-4M12 4h8v8M20 4 10 14',
    notes: 'M6 3h9l4 4v14H6zM14 3v5h5M9 12h7M9 16h5',
    bookmarks: 'M6 4h12v17l-6-4-6 4z',
    emoji: 'M9 9h.01M15 9h.01M8 14q4 5 8 0M21 12a9 9 0 1 1-18 0 9 9 0 0 1 18 0',
    downloads: 'M12 3v12m-5-5 5 5 5-5M4 16v5h16v-5',
    clipboard: 'M9 5H5v16h14V5h-4M9 3h6v4H9zM8 12h8M8 16h6',
    wallpaper: 'M3 4h18v16H3zM3 17l6-6 4 4 3-3 5 5M16 8h.01',
    gridmap: 'M3 3h18v18H3zM3 9h18M3 15h18M9 3v18M15 3v18',
    paint: 'M4 20l4-1L20 7l-4-4L4 15zM13 6l5 5M3 21h18', // 画板入口的画笔图标。
    'level-layout': 'M4 4h4v4H4zM16 3h4v4h-4zM15 16h5v5h-5zM6 8l11 9M8 6l8-1', // 关卡矢量节点图标。
    terrain: 'm2 20 8-15 5 9 3-5 4 11zM7 11l3 2 3-2',
    qr: 'M3 3h6v6H3zM15 3h6v6h-6zM3 15h6v6H3zM15 15h2v2h4v4h-6v-2M21 13v1M12 3v2M3 12h2M12 10v3h4M12 19v2',
    screenshot: 'M3 8V3h5M16 3h5v5M21 16v5h-5M8 21H3v-5M7 7h10v10H7z',
    recording: 'M3 6h12v12H3zM15 10l6-3v10l-6-3M7 12h.01',
    browser: 'M3 6h7l2 3h9v11H3zM3 6V4h7l2 2h7v3',
    filesearch: 'M10 20H4V3h10l4 4v3M13 3v5h5M19 16a4 4 0 1 1-8 0 4 4 0 0 1 8 0m-1 3 4 3',
    settings: 'M4 6h16M4 12h16M4 18h16M8 3v6M16 9v6M10 15v6',
    sidebar: 'M3 4h18v16H3zM9 4v16m7-12-4 4 4 4',
    search: 'M17 10a7 7 0 1 1-14 0 7 7 0 0 1 14 0m-2 5 6 6',
    add: 'M12 5v14M5 12h14',
    close: 'm6 6 12 12M18 6 6 18',
    edit: 'm4 16-1 5 5-1L20 8l-4-4zM13 7l4 4',
    delete: 'M3 6h18M9 6V3h6v3M6 6l1 15h10l1-15M10 10v7M14 10v7',
    up: 'm6 14 6-6 6 6',
    down: 'm6 10 6 6 6-6',
    move: 'M12 3v18m-4-4 4 4 4-4M8 7l4-4 4 4',
    app: 'M4 4h6v6H4zM14 4h6v6h-6zM4 14h6v6H4zM14 14h6v6h-6z'
  };
  // 构造无障碍装饰图标，名称由外部按钮的中文标签承担。
  function create(name) {
    const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    svg.setAttribute("viewBox", "0 0 24 24");
    svg.setAttribute("class", "ui-icon");
    svg.setAttribute("aria-hidden", "true");
    const path = document.createElementNS(svg.namespaceURI, "path");
    path.setAttribute("d", paths[name] || paths.notes);
    svg.append(path);
    return svg;
  }
  document.querySelectorAll(".nav-button").forEach(button => {
    button.querySelector("span:first-child").replaceChildren(create(button.dataset.page));
    const name = Array.from(button.childNodes).find(node => node.nodeType === Node.TEXT_NODE && node.textContent.trim());
    if (name) {
      const label = document.createElement("span");
      label.className = "nav-text"; window.desktopI18n.text(label, window.desktopI18n.sourceOf(name));
      name.replaceWith(label); window.desktopI18n.attr(button, "title", window.desktopI18n.live(() => label.textContent.trim())); window.desktopI18n.attr(button, "aria-label", window.desktopI18n.live(() => label.textContent.trim()));
    }
  });
  document.querySelectorAll("[data-icon]").forEach(node => node.replaceChildren(create(node.dataset.icon)));
  document.querySelectorAll(".search-field > span:first-child").forEach(node => node.replaceChildren(create("search")));
  document.querySelectorAll(".empty-icon").forEach(node => {
    const page = node.closest(".page");
    node.replaceChildren(create(page ? page.id.replace("page-", "") : "bookmarks"));
  });
  const brand = document.querySelector(".brand-mark");
  if (brand) brand.replaceChildren(create("app"));
  // 替换固定按钮中的旧字符图标，正文和用户内容保持原样。
  const symbols = { "＋": "add", "+": "add", "×": "close", "✎": "edit", "↕": "move", "↓": "downloads", "↗": "shortcuts", "⌕": "search", "▧": "wallpaper" };
  document.querySelectorAll("button:not(.nav-button), button > span[aria-hidden=true]").forEach(button => {
    for (const node of Array.from(button.childNodes)) {
      if (node.nodeType !== Node.TEXT_NODE) continue;
      const match = node.textContent.match(/^(\s*)([＋+×✎↕↓↗⌕▧])(\s|$)/);
      if (!match || !symbols[match[2]]) continue;
      const fragment = document.createDocumentFragment();
      fragment.append(create(symbols[match[2]]), window.desktopI18n.textNode(window.desktopI18n.live(() => String(window.desktopI18n.sourceOf(node)).slice(match[1].length + match[2].length))));
      node.replaceWith(fragment);
    }
  });
  window.desktopIcons = Object.freeze({ create });
})();
