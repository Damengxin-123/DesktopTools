/* 网格图页：以画布中心为原点的栅格示意图编辑器，数据由原生服务持久化。 */
(function installGridMapPage() {
  "use strict";
  // 基准格子尺寸与缩放范围；缩放只改变显示比例，坐标始终是格子数。
  const BASE_SIZE = 24;
  const MIN_SIZE = 6;
  const MAX_SIZE = 96;
  // 与后端一致的坐标范围和撤销步数上限。
  const MAX_COORD = 1000000;
  const MAX_UNDO = 200;
  // 单边最多绘制的格子数量，防止极端缩放时一次画出几十万条线。
  const DRAW_LIMIT = 400;
  // 常用颜色与默认网格线颜色。
  const COMMON_COLORS = ["#111827", "#e02f2f", "#f08c1c", "#f7d51d", "#37a356",
    "#1fa99b", "#2f6fe0", "#7a4fd8", "#e2619e", "#8b5a2b", "#9aa3b2", "#ffffff"];
  const DEFAULT_LINE_COLOR = "#94a3b8";
  // 导出 PNG 的最大边长，超出时按比例缩小格子像素，避免占用过多内存。
  const EXPORT_MAX_DIMENSION = 4096;

  const state = {
    projects: [], // 后端返回的项目摘要。
    current: null, // 正在编辑的项目 {id,title,updatedAt}。
    cells: new Map(), // "x,y" → 颜色，只保存被涂色的格子。
    color: COMMON_COLORS[1], // 当前涂色。
    eraser: false, // 橡皮擦模式，涂色改为清除。
    radius: 0, // 画笔半径：0 为单格，N 为以点击格子为圆心 N 格半径的圆形笔触。
    lineColor: DEFAULT_LINE_COLOR, // 网格线颜色，随项目保存。
    lineWidth: 1, // 网格线粗细，随项目保存。
    zoom: 1, // 缩放系数，格子尺寸 = BASE_SIZE * zoom。
    panX: 0, // 原点 (0,0) 在画布中的横向像素位置。
    panY: 0, // 原点 (0,0) 在画布中的纵向像素位置。
    hover: null, // 悬停格子坐标 [x,y]。
    stroke: null, // 正在进行的左键涂色笔画。
    pan: null, // 正在进行的右键拖动。
    box: null, // 空格框选进行中的矩形 {start,current}。
    spaceDown: false, // 空格键当前按住，左键拖动进入框选。
    undoStack: [], // 每次笔画一组格子变更。
    dirty: false, // 是否有未保存的修改。
    saving: false,
    loading: false,
    revision: 0, // 列表刷新序号，丢弃过期响应。
    loadRevision: 0, // 项目读取序号，丢弃过期响应。
    timer: null, // 后端通知合并定时器。
    initialized: false,
    available: false,
    options: null, // 主页面提供的提示函数。
    confirm: null, // 当前对话框提交操作。
    dialogBusy: false
  };

  function byId(id) { return document.getElementById(id); }
  // 以纯文本创建节点，项目名称等内容绝不作为 HTML 执行。
  function node(tag, className, text) {
    const result = document.createElement(tag);
    if (className) result.className = className;
    if (text !== undefined) result.textContent = String(text);
    return result;
  }
  function cellKey(x, y) { return x + "," + y; }
  function cellSize() { return BASE_SIZE * state.zoom; }
  // 把 #RRGGBB 颜色转为带透明度的 rgba，用于框选预览填充。
  function withAlpha(color, alpha) {
    return "rgba(" + parseInt(color.slice(1, 3), 16) + "," + parseInt(color.slice(3, 5), 16) + ","
      + parseInt(color.slice(5, 7), 16) + "," + alpha + ")";
  }
  // 把更新时间显示为简短本地日期。
  function readableDate(value) {
    if (!value) return "";
    const date = new Date(value);
    return Number.isNaN(date.getTime()) ? "" : date.toLocaleDateString("zh-CN", { month: "2-digit", day: "2-digit" });
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
      finally { if (source && source.isConnected) source.disabled = false; updateState(); }
    };
  }

  // ---------- 画布渲染 ----------

  // 把画布按容器尺寸与设备像素比重建，并绘制格子、网格线和坐标轴。
  function render() {
    const canvas = byId("gridmap-canvas");
    const wrap = canvas.parentNode;
    const width = wrap.clientWidth;
    const height = wrap.clientHeight;
    if (!width || !height) return;
    const ratio = window.devicePixelRatio || 1;
    const pixelWidth = Math.round(width * ratio);
    const pixelHeight = Math.round(height * ratio);
    if (canvas.width !== pixelWidth || canvas.height !== pixelHeight) {
      canvas.width = pixelWidth;
      canvas.height = pixelHeight;
    }
    const ctx = canvas.getContext("2d");
    ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
    ctx.fillStyle = "#ffffff";
    ctx.fillRect(0, 0, width, height);
    const size = cellSize();
    const x0 = Math.floor(-state.panX / size);
    const x1 = Math.ceil((width - state.panX) / size);
    const y0 = Math.floor(-state.panY / size);
    const y1 = Math.ceil((height - state.panY) / size);
    const drawX0 = Math.max(x0, -DRAW_LIMIT);
    const drawX1 = Math.min(x1, DRAW_LIMIT);
    const drawY0 = Math.max(y0, -DRAW_LIMIT);
    const drawY1 = Math.min(y1, DRAW_LIMIT);
    for (let gx = drawX0; gx <= drawX1; ++gx) {
      for (let gy = drawY0; gy <= drawY1; ++gy) {
        const color = state.cells.get(cellKey(gx, gy));
        if (!color) continue;
        ctx.fillStyle = color;
        ctx.fillRect(state.panX + gx * size, state.panY + gy * size, size, size);
      }
    }
    // 网格线画在涂色之上，形成清晰的栅格；缩得太小时只保留坐标轴。
    if (size >= 7) {
      ctx.strokeStyle = state.lineColor;
      ctx.lineWidth = state.lineWidth;
      ctx.beginPath();
      for (let gx = drawX0; gx <= drawX1 + 1; ++gx) {
        const px = state.panX + gx * size;
        ctx.moveTo(px, Math.max(0, state.panY + drawY0 * size));
        ctx.lineTo(px, Math.min(height, state.panY + (drawY1 + 1) * size));
      }
      for (let gy = drawY0; gy <= drawY1 + 1; ++gy) {
        const py = state.panY + gy * size;
        ctx.moveTo(Math.max(0, state.panX + drawX0 * size), py);
        ctx.lineTo(Math.min(width, state.panX + (drawX1 + 1) * size), py);
      }
      ctx.stroke();
    }
    // 从中心向四个方向的坐标轴，比普通网格线更粗。
    ctx.strokeStyle = state.lineColor;
    ctx.lineWidth = Math.max(2, state.lineWidth * 2);
    ctx.beginPath();
    ctx.moveTo(0, state.panY);
    ctx.lineTo(width, state.panY);
    ctx.moveTo(state.panX, 0);
    ctx.lineTo(state.panX, height);
    ctx.stroke();
    // 空格框选预览：半透明填充待涂区域，橡皮擦状态下用红色提示清除。
    if (state.box) {
      const bx0 = Math.min(state.box.start[0], state.box.current[0]);
      const bx1 = Math.max(state.box.start[0], state.box.current[0]);
      const by0 = Math.min(state.box.start[1], state.box.current[1]);
      const by1 = Math.max(state.box.start[1], state.box.current[1]);
      ctx.fillStyle = state.eraser ? "rgba(201, 83, 91, 0.3)" : withAlpha(state.color, "0.4");
      ctx.fillRect(state.panX + bx0 * size, state.panY + by0 * size,
        (bx1 - bx0 + 1) * size, (by1 - by0 + 1) * size);
      ctx.strokeStyle = state.eraser ? "#c9535b" : "#315ee7";
      ctx.lineWidth = 2;
      ctx.strokeRect(state.panX + bx0 * size + 1, state.panY + by0 * size + 1,
        (bx1 - bx0 + 1) * size - 2, (by1 - by0 + 1) * size - 2);
    }
    // 悬停提示：单格描边，或半径大于 0 时的圆形笔刷范围。
    if (state.hover && !state.box) {
      ctx.strokeStyle = state.eraser ? "#c9535b" : "#315ee7";
      ctx.lineWidth = 2;
      if (state.radius > 0) {
        ctx.beginPath();
        ctx.arc(state.panX + (state.hover[0] + 0.5) * size, state.panY + (state.hover[1] + 0.5) * size,
          (state.radius + 0.5) * size, 0, Math.PI * 2);
        ctx.stroke();
      } else {
        ctx.strokeRect(state.panX + state.hover[0] * size + 1, state.panY + state.hover[1] * size + 1,
          size - 2, size - 2);
      }
    }
  }

  function resetView() {
    const wrap = byId("gridmap-canvas").parentNode;
    state.panX = wrap.clientWidth / 2;
    state.panY = wrap.clientHeight / 2;
    state.zoom = 1;
    render();
    updateZoom();
  }

  // 以指定画布点为锚缩放，保持光标下的格子位置不动。
  function zoomAt(anchorX, anchorY, factor) {
    const before = cellSize();
    const after = Math.min(MAX_SIZE, Math.max(MIN_SIZE, before * factor));
    if (after === before) return;
    const gridX = (anchorX - state.panX) / before;
    const gridY = (anchorY - state.panY) / before;
    state.zoom = after / BASE_SIZE;
    state.panX = anchorX - gridX * cellSize();
    state.panY = anchorY - gridY * cellSize();
    render();
    updateZoom();
  }

  function updateZoom() {
    byId("gridmap-zoom").textContent = "缩放 " + Math.round(state.zoom * 100) + "%";
  }
  function updateCellCount() {
    byId("gridmap-cells").textContent = state.cells.size + " 格";
  }
  function updateHoverCell(cell) {
    state.hover = cell;
    byId("gridmap-pos").textContent = cell ? "坐标 " + cell[0] + ", " + cell[1] : "坐标 —";
  }

  // ---------- 涂色与撤销 ----------

  // 把一次涂色应用到数据模型；颜色没有变化时不产生记录。
  function applyCell(x, y, changes) {
    if (x < -MAX_COORD || x > MAX_COORD || y < -MAX_COORD || y > MAX_COORD) return;
    const key = cellKey(x, y);
    const before = state.cells.get(key) || null;
    const after = state.eraser ? null : state.color;
    if (before === after) return;
    if (after === null) state.cells.delete(key); else state.cells.set(key, after);
    changes.push({ x, y, from: before, to: after });
  }

  // 以 (cx,cy) 为圆心落下当前画笔：格子中心落在半径加半格的圆内即被涂色，
  // 因此半径 0 为单格、1 为 3×3、2 为去角的 5×5，整体呈圆形。
  function stampBrush(cx, cy, changes) {
    if (state.radius <= 0) {
      applyCell(cx, cy, changes);
      return;
    }
    const reach = state.radius + 0.5;
    const squared = reach * reach;
    for (let x = cx - state.radius; x <= cx + state.radius; ++x) {
      for (let y = cy - state.radius; y <= cy + state.radius; ++y) {
        const dx = x - cx;
        const dy = y - cy;
        if (dx * dx + dy * dy <= squared) applyCell(x, y, changes);
      }
    }
  }

  // Bresenham 直线插值，快速拖动时笔刷沿途落下，涂满经过的每个格子。
  function paintSpan(from, to, changes) {
    let x0 = from[0];
    let y0 = from[1];
    const x1 = to[0];
    const y1 = to[1];
    const dx = Math.abs(x1 - x0);
    const dy = -Math.abs(y1 - y0);
    const stepX = x0 < x1 ? 1 : -1;
    const stepY = y0 < y1 ? 1 : -1;
    let error = dx + dy;
    for (;;) {
      stampBrush(x0, y0, changes);
      if (x0 === x1 && y0 === y1) break;
      const doubled = 2 * error;
      if (doubled >= dy) { error += dy; x0 += stepX; }
      if (doubled <= dx) { error += dx; y0 += stepY; }
    }
  }

  function pushUndo(changes) {
    state.undoStack.push(changes);
    if (state.undoStack.length > MAX_UNDO) state.undoStack.shift();
  }

  // 把框选矩形内的所有格子整体涂色或清除，一次撤销即可全部回退。
  function applyBox(box) {
    const x0 = Math.min(box.start[0], box.current[0]);
    const x1 = Math.max(box.start[0], box.current[0]);
    const y0 = Math.min(box.start[1], box.current[1]);
    const y1 = Math.max(box.start[1], box.current[1]);
    const changes = [];
    for (let x = x0; x <= x1; ++x) {
      for (let y = y0; y <= y1; ++y) {
        applyCell(x, y, changes);
      }
    }
    if (changes.length) {
      pushUndo(changes);
      markDirty();
    }
    render();
  }

  // 按住空格时切换画布光标，提示已进入框选状态。
  function updateCursor() {
    byId("gridmap-canvas").style.cursor = state.spaceDown ? "cell" : "crosshair";
  }

  function undo() {
    const changes = state.undoStack.pop();
    if (!changes) return;
    for (let index = changes.length - 1; index >= 0; --index) {
      const change = changes[index];
      const key = cellKey(change.x, change.y);
      if (change.from === null) state.cells.delete(key); else state.cells.set(key, change.from);
    }
    markDirty();
    render();
    updateCellCount();
  }

  function markDirty() {
    if (!state.current) return;
    state.dirty = true;
    updateState();
  }

  // 根据当前状态刷新工具栏按钮与保存状态提示。
  function updateState() {
    const editable = Boolean(state.current);
    byId("gridmap-save").disabled = !editable || state.saving;
    byId("gridmap-undo").disabled = !editable || state.undoStack.length === 0;
    byId("gridmap-export").disabled = !editable || state.saving;
    byId("gridmap-copy-png").disabled = !editable || state.saving;
    const indicator = byId("gridmap-save-state");
    indicator.textContent = state.loading ? "正在读取…" : (state.saving ? "正在保存…" : (state.dirty ? "未保存的修改" : "已保存"));
    indicator.classList.toggle("dirty", state.dirty && !state.saving);
    updateCellCount();
  }

  // ---------- 项目列表 ----------

  function renderList() {
    byId("gridmap-count").textContent = String(state.projects.length);
    const target = byId("gridmap-list");
    target.replaceChildren();
    if (!state.projects.length) {
      const empty = node("div", "empty-state");
      empty.append(node("div", "empty-icon", "▦"),
        node("h2", "", state.available ? "还没有网格图" : "网格图不可用"),
        node("p", "", state.available ? "点击右上角“新建网格图”开始。" : "请检查数据目录后重试。"));
      target.append(empty);
      return;
    }
    for (const item of state.projects) {
      const row = node("article", "gridmap-row" + (state.current && state.current.id === item.id ? " active" : ""));
      row.dataset.id = item.id;
      const content = node("div", "gridmap-row-content");
      const title = node("button", "gridmap-row-title", item.title);
      title.type = "button";
      title.title = item.title + "（双击编辑）";
      title.setAttribute("aria-label", "编辑网格图“" + item.title + "”");
      content.append(title, node("div", "gridmap-row-meta", readableDate(item.updatedAt)));
      const rename = node("button", "icon-button", "✎");
      rename.type = "button";
      rename.setAttribute("aria-label", "重命名网格图“" + item.title + "”");
      rename.addEventListener("click", function renameClicked() { renameProject(item); });
      const remove = node("button", "icon-button", "×");
      remove.type = "button";
      remove.setAttribute("aria-label", "删除网格图“" + item.title + "”");
      remove.addEventListener("click", function removeClicked() { confirmDelete([item.id]); });
      row.append(content, rename, remove);
      // 双击列表项进入编辑。
      row.addEventListener("dblclick", function openOnDoubleClick(event) {
        if (event.target.closest("button")) return;
        openProject(item.id).catch(report);
      });
      target.append(row);
    }
  }

  // 刷新项目列表；正在编辑且仍存在的项目保持打开状态。
  async function refresh() {
    const revision = ++state.revision;
    try {
      const data = await window.desktopBridge.call("getGridMaps");
      if (revision !== state.revision) return;
      state.projects = data.items;
      state.available = true;
      byId("gridmap-error").hidden = true;
      if (state.current && !state.dirty && !state.saving
        && !state.projects.some(function projectStillExists(item) { return item.id === state.current.id; })) {
        closeEditor();
      }
    } catch (error) {
      if (revision !== state.revision) return;
      state.available = false;
      byId("gridmap-error").textContent = error.message;
      byId("gridmap-error").hidden = false;
    }
    renderList();
  }

  function scheduleRefresh() {
    clearTimeout(state.timer);
    state.timer = setTimeout(function runScheduledRefresh() { refresh().catch(report); }, 100);
  }

  function showEditor(visible) {
    byId("gridmap-editor").hidden = !visible;
    byId("gridmap-empty").hidden = visible;
    renderList();
  }

  function closeEditor() {
    state.current = null;
    state.cells = new Map();
    state.undoStack = [];
    state.dirty = false;
    state.stroke = null;
    state.pan = null;
    state.hover = null;
    byId("gridmap-title").textContent = "";
    showEditor(false);
    updateState();
  }

  // 读取项目数据并进入编辑；有未保存修改时先确认。
  async function openProject(id) {
    if (!state.current || state.current.id !== id) {
      if (!canLeave()) return;
      const revision = ++state.loadRevision;
      state.loading = true;
      updateState();
      try {
        const data = await window.desktopBridge.call("getGridMap", id);
        if (revision !== state.loadRevision) return;
        state.current = { id: data.id, title: data.title, updatedAt: data.updatedAt };
        state.cells = new Map();
        for (const triple of data.cells) {
          state.cells.set(cellKey(Number(triple[0]), Number(triple[1])), String(triple[2]).toLowerCase());
        }
        state.lineColor = data.lineColor || DEFAULT_LINE_COLOR;
        state.lineWidth = Number(data.lineWidth) || 1;
        byId("gridmap-line-color").value = state.lineColor;
        byId("gridmap-line-width").value = String(state.lineWidth);
        state.undoStack = [];
        state.dirty = false;
        byId("gridmap-title").textContent = data.title;
        showEditor(true);
        resetView();
      } finally {
        state.loading = false;
        updateState();
      }
    } else if (byId("gridmap-editor").hidden) {
      showEditor(true);
      resetView();
    }
  }

  // 切换页面或项目前的保护；返回 false 表示必须留在当前状态。
  function canLeave() {
    if (state.saving) { state.options.toast("网格图正在保存，请稍候。", "warning"); return false; }
    if (state.loading) { state.options.toast("网格图正在读取，请稍候。", "warning"); return false; }
    return !state.dirty || window.confirm("当前网格图有未保存的修改。确定放弃这些修改吗？");
  }

  // ---------- 保存与导出 ----------

  async function saveProject() {
    if (!state.current || state.saving || state.loading) return;
    const triples = [];
    for (const [key, color] of state.cells) {
      const comma = key.indexOf(",");
      triples.push([Number(key.slice(0, comma)), Number(key.slice(comma + 1)), color]);
    }
    state.saving = true;
    updateState();
    try {
      const saved = await window.desktopBridge.call("saveGridMap", state.current.id, {
        lineColor: state.lineColor,
        lineWidth: state.lineWidth,
        cells: triples
      });
      state.current.updatedAt = saved.updatedAt;
      state.dirty = false;
      await refresh();
      updateState();
      state.options.toast("网格图已保存");
    } finally {
      state.saving = false;
      updateState();
    }
  }

  // 把当前网格渲染为离屏画布：内容包围盒加一圈余量，含网格线与坐标轴。
  function buildExportDataUrl() {
    let minX = -2, maxX = 2, minY = -2, maxY = 2, hasCells = false;
    for (const key of state.cells.keys()) {
      const comma = key.indexOf(",");
      const x = Number(key.slice(0, comma));
      const y = Number(key.slice(comma + 1));
      if (!hasCells) { minX = maxX = x; minY = maxY = y; hasCells = true; continue; }
      if (x < minX) minX = x;
      if (x > maxX) maxX = x;
      if (y < minY) minY = y;
      if (y > maxY) maxY = y;
    }
    if (hasCells) { minX -= 1; maxX += 1; minY -= 1; maxY += 1; }
    const columns = maxX - minX + 1;
    const rows = maxY - minY + 1;
    let cellPx = 24;
    if (Math.max(columns, rows) * cellPx > EXPORT_MAX_DIMENSION) {
      cellPx = Math.max(2, Math.floor(EXPORT_MAX_DIMENSION / Math.max(columns, rows)));
    }
    const canvas = document.createElement("canvas");
    canvas.width = columns * cellPx;
    canvas.height = rows * cellPx;
    const ctx = canvas.getContext("2d");
    ctx.fillStyle = "#ffffff";
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    for (const [key, color] of state.cells) {
      const comma = key.indexOf(",");
      const x = Number(key.slice(0, comma)) - minX;
      const y = Number(key.slice(comma + 1)) - minY;
      ctx.fillStyle = color;
      ctx.fillRect(x * cellPx, y * cellPx, cellPx, cellPx);
    }
    ctx.strokeStyle = state.lineColor;
    ctx.lineWidth = state.lineWidth;
    ctx.beginPath();
    for (let column = 0; column <= columns; ++column) {
      ctx.moveTo(column * cellPx, 0);
      ctx.lineTo(column * cellPx, canvas.height);
    }
    for (let row = 0; row <= rows; ++row) {
      ctx.moveTo(0, row * cellPx);
      ctx.lineTo(canvas.width, row * cellPx);
    }
    ctx.stroke();
    // 坐标轴落在导出范围内时按更粗的线宽绘制。
    ctx.lineWidth = Math.max(2, state.lineWidth * 2);
    ctx.beginPath();
    if (minX <= 0 && maxX + 1 >= 0) {
      const axisX = -minX * cellPx;
      ctx.moveTo(axisX, 0);
      ctx.lineTo(axisX, canvas.height);
    }
    if (minY <= 0 && maxY + 1 >= 0) {
      const axisY = -minY * cellPx;
      ctx.moveTo(0, axisY);
      ctx.lineTo(canvas.width, axisY);
    }
    ctx.stroke();
    return canvas.toDataURL("image/png");
  }

  async function exportPng() {
    if (!state.current) return;
    const result = await window.desktopBridge.call("exportGridMapPng", state.current.title, buildExportDataUrl());
    if (result && !result.cancelled && result.target) state.options.toast("已导出到 " + result.target);
  }

  async function copyPng() {
    if (!state.current) return;
    await window.desktopBridge.call("copyGridMapPng", buildExportDataUrl());
    state.options.toast("PNG 已复制到剪贴板");
  }

  // ---------- 对话框 ----------

  function openDialog(title, submitText, danger) {
    byId("gridmap-dialog-title").textContent = title;
    byId("gridmap-dialog-body").replaceChildren();
    byId("gridmap-dialog-error").hidden = true;
    byId("gridmap-dialog-confirm").textContent = submitText;
    byId("gridmap-dialog-confirm").className = "button primary" + (danger ? " danger" : "");
    byId("gridmap-dialog").showModal();
    const firstField = byId("gridmap-dialog-body").querySelector("input,select");
    if (firstField) firstField.focus();
  }

  function closeDialog() {
    if (state.dialogBusy) return;
    byId("gridmap-dialog").close();
  }

  async function submitDialog(event) {
    event.preventDefault();
    if (state.dialogBusy || !state.confirm) return;
    state.dialogBusy = true;
    byId("gridmap-dialog-confirm").disabled = true;
    try {
      await state.confirm();
      state.dialogBusy = false;
      closeDialog();
    } catch (error) {
      byId("gridmap-dialog-error").textContent = error.message || "操作失败，请重试。";
      byId("gridmap-dialog-error").hidden = false;
    } finally {
      state.dialogBusy = false;
      byId("gridmap-dialog-confirm").disabled = false;
    }
  }

  function createProject() {
    openDialog("新建网格图", "创建");
    const name = formField(byId("gridmap-dialog-body"), "名称", "gridmap-name-input");
    name.required = true;
    name.maxLength = 200;
    name.placeholder = "例如：一楼平面布置";
    state.confirm = async function submitCreate() {
      if (!name.value.trim()) throw new Error("请输入网格图名称。");
      const created = await window.desktopBridge.call("createGridMap", name.value.trim());
      await refresh();
      await openProject(created.id);
      state.options.toast("网格图已创建");
    };
  }

  function renameProject(item) {
    openDialog("重命名网格图", "保存");
    const name = formField(byId("gridmap-dialog-body"), "名称", "gridmap-name-input");
    name.required = true;
    name.maxLength = 200;
    name.value = item.title;
    state.confirm = async function submitRename() {
      if (!name.value.trim()) throw new Error("请输入网格图名称。");
      const saved = await window.desktopBridge.call("renameGridMap", item.id, name.value.trim());
      if (state.current && state.current.id === item.id) {
        state.current.title = saved.title;
        byId("gridmap-title").textContent = saved.title;
      }
      await refresh();
      state.options.toast("名称已更新");
    };
  }

  function confirmDelete(ids) {
    if (!ids.length) return;
    openDialog("删除网格图", "确认删除", true);
    byId("gridmap-dialog-body").append(node("p", "dialog-description",
      "确定删除这 " + ids.length + " 个网格图吗？\n其栅格数据将一并删除，此操作无法撤销。"));
    state.confirm = async function submitDelete() {
      await window.desktopBridge.call("deleteGridMap", ids);
      if (state.current && ids.includes(state.current.id)) closeEditor();
      await refresh();
      state.options.toast("已删除 " + ids.length + " 个网格图");
    };
  }

  // ---------- 颜色与画布交互 ----------

  function setColor(color) {
    state.color = String(color).toLowerCase();
    if (state.eraser) {
      state.eraser = false;
      byId("gridmap-eraser").setAttribute("aria-pressed", "false");
    }
    byId("gridmap-color").value = state.color;
    updateSwatches();
  }

  function updateSwatches() {
    const container = byId("gridmap-swatches");
    container.replaceChildren();
    // 最左侧固定显示当前使用的颜色；点击可从橡皮擦恢复为该颜色涂色。
    const current = node("button", "gridmap-current");
    current.type = "button";
    current.id = "gridmap-current";
    current.style.backgroundColor = state.color;
    current.title = "当前颜色 " + state.color;
    current.setAttribute("aria-label", "当前颜色 " + state.color + "，点击恢复涂色");
    current.addEventListener("click", function currentClicked() { setColor(state.color); });
    container.append(current);
    for (const color of COMMON_COLORS) {
      const swatch = node("button", "gridmap-swatch");
      swatch.type = "button";
      swatch.style.backgroundColor = color;
      swatch.setAttribute("aria-pressed", String(color.toLowerCase() === state.color));
      swatch.setAttribute("aria-label", "使用颜色 " + color);
      swatch.title = color;
      swatch.addEventListener("click", function swatchClicked() { setColor(color); });
      container.append(swatch);
    }
  }

  function cellAt(event) {
    const rect = byId("gridmap-canvas").getBoundingClientRect();
    const size = cellSize();
    return [Math.floor((event.clientX - rect.left - state.panX) / size),
      Math.floor((event.clientY - rect.top - state.panY) / size)];
  }

  function sameCell(a, b) {
    return a && b && a[0] === b[0] && a[1] === b[1];
  }

  // 指针捕获在合成事件或极端时序下可能失败，忽略但不影响绘制状态。
  function capturePointer(canvas, event) {
    try { canvas.setPointerCapture(event.pointerId); } catch (error) { /* 无活动指针时忽略 */ }
  }
  function releasePointer(canvas, event) {
    try { canvas.releasePointerCapture(event.pointerId); } catch (error) { /* 捕获已释放时忽略 */ }
  }

  function installCanvasEvents() {
    const canvas = byId("gridmap-canvas");
    canvas.addEventListener("pointerdown", function pointerPressed(event) {
      if (!state.current) return;
      if (event.button === 0) {
        const cell = cellAt(event);
        // 按住空格时左键拖动改为框选，松开鼠标时整片应用。
        if (state.spaceDown && !state.pan) {
          state.box = { start: cell, current: cell };
          capturePointer(canvas, event);
          render();
          return;
        }
        // Alt+点击吸取格子颜色；空白格子保持当前选择不变。
        if (event.altKey) {
          const picked = state.cells.get(cellKey(cell[0], cell[1]));
          if (picked) setColor(picked);
          return;
        }
        if (state.pan) return;
        const changes = [];
        stampBrush(cell[0], cell[1], changes);
        state.stroke = { changes, last: cell };
        capturePointer(canvas, event);
        render();
      } else if (event.button === 2) {
        if (state.box || state.stroke) return;
        state.pan = { x: event.clientX, y: event.clientY };
        capturePointer(canvas, event);
      }
    });
    canvas.addEventListener("pointermove", function pointerMoved(event) {
      const cell = cellAt(event);
      if (!sameCell(cell, state.hover)) {
        updateHoverCell(cell);
        if (state.current) render();
      }
      if (state.pan) {
        state.panX += event.clientX - state.pan.x;
        state.panY += event.clientY - state.pan.y;
        state.pan = { x: event.clientX, y: event.clientY };
        render();
        return;
      }
      if (state.box) {
        const boxCell = cellAt(event);
        if (!sameCell(boxCell, state.box.current)) {
          state.box.current = boxCell;
          render();
        }
        return;
      }
      if (state.stroke) {
        if (!sameCell(cell, state.stroke.last)) {
          paintSpan(state.stroke.last, cell, state.stroke.changes);
          state.stroke.last = cell;
          render();
        }
      }
    });
    const finishPointer = function pointerReleased(event) {
      // 先结束状态再应用，避免应用过程中的重绘残留框选高亮。
      const box = state.box;
      const stroke = state.stroke;
      const pan = state.pan;
      state.box = null;
      state.stroke = null;
      state.pan = null;
      if (box) {
        releasePointer(canvas, event);
        applyBox(box);
      }
      if (stroke) {
        releasePointer(canvas, event);
        if (stroke.changes.length) {
          pushUndo(stroke.changes);
          markDirty();
        }
      }
      if (pan) releasePointer(canvas, event);
      updateState();
    };
    canvas.addEventListener("pointerup", finishPointer);
    canvas.addEventListener("pointercancel", finishPointer);
    canvas.addEventListener("pointerleave", function pointerLeft() {
      updateHoverCell(null);
      if (state.current && !state.stroke && !state.pan && !state.box) render();
    });
    canvas.addEventListener("contextmenu", function contextMenuRequested(event) {
      event.preventDefault();
    });
    canvas.addEventListener("wheel", function wheelScrolled(event) {
      event.preventDefault();
      const rect = canvas.getBoundingClientRect();
      zoomAt(event.clientX - rect.left, event.clientY - rect.top, event.deltaY < 0 ? 1.15 : 1 / 1.15);
    }, { passive: false });
  }

  // ---------- 初始化 ----------

  async function initialize(options) {
    state.options = options;
    if (!state.initialized) {
      state.initialized = true;
      updateSwatches();
      installCanvasEvents();
      byId("add-gridmap").addEventListener("click", action(function addClicked() { createProject(); }));
      byId("gridmap-color").addEventListener("input", function customColorChanged(event) { setColor(event.target.value); });
      byId("gridmap-eraser").addEventListener("click", function eraserToggled() {
        state.eraser = !state.eraser;
        byId("gridmap-eraser").setAttribute("aria-pressed", String(state.eraser));
      });
      byId("gridmap-brush-radius").addEventListener("change", function brushRadiusChanged(event) {
        const radius = Number(event.target.value);
        if (Number.isInteger(radius) && radius >= 0 && radius <= 10) state.radius = radius;
      });
      byId("gridmap-line-color").addEventListener("input", function lineColorChanged(event) {
        state.lineColor = String(event.target.value).toLowerCase();
        if (state.current) { markDirty(); render(); }
      });
      byId("gridmap-line-width").addEventListener("change", function lineWidthChanged(event) {
        const width = Number(event.target.value);
        if (width >= 1 && width <= 6) state.lineWidth = width;
        if (state.current) { markDirty(); render(); }
      });
      byId("gridmap-undo").addEventListener("click", function undoClicked() { undo(); });
      byId("gridmap-reset-view").addEventListener("click", function resetViewClicked() { resetView(); });
      byId("gridmap-save").addEventListener("click", action(function saveClicked() { return saveProject(); }));
      byId("gridmap-export").addEventListener("click", action(exportPng));
      byId("gridmap-copy-png").addEventListener("click", action(copyPng));
      byId("gridmap-dialog-close").addEventListener("click", closeDialog);
      byId("gridmap-dialog-cancel").addEventListener("click", closeDialog);
      byId("gridmap-dialog-form").addEventListener("submit", submitDialog);
      byId("gridmap-dialog").addEventListener("cancel", function dialogCancelled(event) {
        event.preventDefault();
        closeDialog();
      });
      if (typeof ResizeObserver === "function") {
        new ResizeObserver(function canvasResized() { if (!byId("gridmap-editor").hidden) render(); })
          .observe(byId("gridmap-canvas").parentNode);
      }
      // 页面级快捷键：Ctrl+Z 撤销涂色，Ctrl+S 保存；按住空格进入框选状态。
      const editingFormField = function editingFormField(target) {
        const tag = target && target.tagName;
        return tag === "INPUT" || tag === "SELECT" || tag === "TEXTAREA" || tag === "BUTTON"
          || Boolean(target && target.isContentEditable);
      };
      document.addEventListener("keydown", function gridmapShortcutPressed(event) {
        if (byId("page-gridmap").hidden) return;
        if (byId("gridmap-dialog").open || byId("app-dialog").open) return;
        if (event.key === " ") {
          // 表单和按钮上的空格保持原生行为，画布区域用空格开启框选。
          if (editingFormField(event.target)) return;
          event.preventDefault();
          state.spaceDown = true;
          updateCursor();
          return;
        }
        if (!(event.ctrlKey || event.metaKey)) return;
        const key = event.key.toLowerCase();
        if (key === "z" && !event.shiftKey && state.current && !state.saving) {
          event.preventDefault();
          undo();
        } else if (key === "s" && state.current && !state.saving) {
          event.preventDefault();
          saveProject().catch(report);
        }
      });
      // 松开空格或窗口失焦时结束框选就绪；已经拖出的框在松开鼠标时照常应用。
      document.addEventListener("keyup", function gridmapSpaceReleased(event) {
        if (event.key === " " && state.spaceDown) {
          state.spaceDown = false;
          updateCursor();
        }
      });
      window.addEventListener("blur", function gridmapFocusLost() {
        if (state.spaceDown) {
          state.spaceDown = false;
          updateCursor();
        }
      });
      await window.desktopBridge.on("gridMapsChanged", scheduleRefresh);
    }
    await refresh();
  }

  // 对主页面仅暴露初始化与离开保护。
  window.desktopGridmap = Object.freeze({
    initialize,
    canLeave: function gridmapCanLeave() { return state.current ? canLeave() : true; }
  });
})();
