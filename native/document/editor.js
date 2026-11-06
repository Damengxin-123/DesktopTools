"use strict";

// 页面只持有当前编辑会话，HTML 和索引通过现有 C++ 文档服务保存。
const byId = id => document.getElementById(id);
const title = byId("title");
const category = byId("category");
const status = byId("status");
const toolbar = byId("toolbar");
const saveButton = byId("save");
const exportButton = byId("export");
const tableDialog = byId("table-dialog");
const imageInput = byId("image-file");
let editor = byId("editor");
let host;
let revision = 0;
let range = null;
let changes = 0;
let saved = 0;
let reportedDirty = false;
let existing = false;
let saving = false;

// 更新状态并将草稿变化发送给窗口，供关闭和切换文档时确认。
function report(message, error = false) {
    status.textContent = message;
    status.dataset.error = String(error);
}

// 同步保存状态和按钮可用性，避免重复发送草稿状态。
function refresh() {
    const dirty = changes !== saved;
    if (dirty !== reportedDirty) {
        reportedDirty = dirty;
        host.setDirty(revision, dirty);
    }
    saveButton.disabled = saving || revision === 0;
    exportButton.disabled = saving || dirty || !existing;
}

// 记录正文、标题或分类的修改。
function changed() {
    if (!host || !revision)
        return;
    ++changes;
    updateOutline();
    refresh();
    report("有未保存的修改");
}

// 每篇文档使用独立的编辑区域，切换时清空选区引用。
function apply(state) {
    revision = state.revision;
    range = null;
    changes = saved = 0;
    reportedDirty = saving = false;
    existing = Boolean(state.document.id);
    presets = normalizePresets(state.document.styles);
    renderPresets();
    updateLock(state.document.locked);
    title.value = state.document.title || "";
    category.replaceChildren(...state.categories.map(item => new Option(item.name, item.id)));
    category.value = state.document.categoryId || "default";
    if (category.selectedIndex < 0)
        category.selectedIndex = 0;
    const next = editor.cloneNode(false);
    if (state.document.html)
        next.innerHTML = new DOMParser().parseFromString(state.document.html, "text/html").body.innerHTML;
    next.contentEditable = revision ? "true" : "false";
    editor.replaceWith(next);
    editor = next;
    editor.addEventListener("input", changed);
    editor.addEventListener("paste", paste);
    editor.addEventListener("dragover", event => event.preventDefault());
    editor.addEventListener("drop", drop);
    editor.addEventListener("keydown", tableNavigation);
    document.querySelector(".document-scroll").scrollTop = 0;
    updateOutline();
    appearance(state.theme);
    refresh();
    report(existing ? "已打开" : "新建文档");
    if (revision)
        (existing ? editor : title).focus();
}

// 同步窗口配色，保留当前正文、选区及滚动位置。
function appearance(theme) {
    document.documentElement.dataset.theme = theme === "gold" ? "gold" : "dark";
}

// 记录正文选区，点击字号等工具时仍然格式化原选区。
function rememberSelection() {
    const selection = window.getSelection();
    if (selection.rangeCount && editor.contains(selection.getRangeAt(0).commonAncestorContainer)) {
        range = selection.getRangeAt(0).cloneRange();
        for (const button of toolbar.querySelectorAll("[data-toggle]"))
            button.setAttribute("aria-pressed", String(document.queryCommandState(button.dataset.command)));
    }
}

// 恢复工具栏动作对应的正文选区，默认从文末开始编辑。
function restoreSelection(selection = range) {
    editor.focus();
    const target = window.getSelection();
    target.removeAllRanges();
    if (selection && editor.contains(selection.commonAncestorContainer)) {
        target.addRange(selection);
    } else {
        const end = document.createRange();
        end.selectNodeContents(editor);
        end.collapse(false);
        target.addRange(end);
    }
}

// 由 Chromium 执行 HTML 编辑命令，复用浏览器撤销与重做。
function format(command, value = null, selection = range) {
    if (!revision)
        return;
    restoreSelection(selection);
    document.execCommand("styleWithCSS", false, true);
    const before = changes;
    if (document.execCommand(command, false, value) && changes === before)
        changed();
    rememberSelection();
}

// 保存当前 HTML 快照，保存期间的新修改仍保留为草稿。
function save() {
    if (!host || saving || !revision)
        return;
    const name = title.value.trim();
    if (!name) {
        report("请先填写文档标题。", true);
        title.focus();
        return;
    }
    saving = true;
    const version = revision;
    const submitted = changes;
    refresh();
    report("正在保存…");
    host.saveDocument(version, {title: name, categoryId: category.value, html: editor.innerHTML, styles: presets}, result => {
        if (version !== revision)
            return;
        saving = false;
        if (!result.ok) {
            refresh();
            report(result.error, true);
            return;
        }
        saved = submitted;
        existing = true;
        refresh();
        report(changes === saved ? "文档已保存" : "较新的修改尚未保存");
    });
}

// 插入真正的 HTML 表格，浏览器负责单元格内的文字编辑。
function insertTable(event) {
    event.preventDefault();
    const rows = byId("table-rows").valueAsNumber;
    const columns = byId("table-columns").valueAsNumber;
    const table = document.createElement("table");
    table.setAttribute("border", "1");
    table.setAttribute("cellpadding", "5");
    table.setAttribute("cellspacing", "0");
    for (let y = 0; y < rows; y++) {
        const row = table.insertRow();
        for (let x = 0; x < columns; x++)
            row.insertCell().append(document.createElement("br"));
    }
    tableDialog.close();
    format("insertHTML", table.outerHTML + "<p><br></p>");
}

// 在表格内用 Tab 和 Shift+Tab 移到相邻单元格。
function tableNavigation(event) {
    if (event.key !== "Tab")
        return;
    const anchor = window.getSelection().anchorNode;
    const element = anchor instanceof Element ? anchor : anchor?.parentElement;
    const cell = element?.closest("td,th");
    if (!cell || !editor.contains(cell))
        return;
    const cells = [...cell.closest("table").querySelectorAll("td,th")];
    const next = cells[cells.indexOf(cell) + (event.shiftKey ? -1 : 1)];
    if (next) {
        event.preventDefault();
        window.getSelection().collapse(next, 0);
    }
}

// 把本地图片作为标准 data: 图片内嵌，导出后也能独立打开。
async function insertImage(file, selection = range) {
    if (!file || !/^image\/(png|jpeg|gif|webp|bmp)$/.test(file.type) || file.size > 8 * 1024 * 1024) {
        report("请选择 8 MB 以内的 PNG、JPEG、GIF、WebP 或 BMP 图片。", true);
        return;
    }
    const version = revision;
    const reader = new FileReader();
    reader.onload = async () => {
        const image = new Image();
        image.src = reader.result;
        try {
            await image.decode();
            if (image.naturalWidth * image.naturalHeight > 20000000)
                throw new Error();
        } catch {
            if (version === revision)
                report("无法读取图片，或图片超过 2000 万像素。", true);
            return;
        }
        if (version !== revision)
            return;
        image.alt = file.name || "内嵌图片";
        image.width = Math.min(image.naturalWidth, 640);
        format("insertHTML", image.outerHTML, selection);
    };
    reader.onerror = () => {
        if (version === revision)
            report("无法读取图片文件。", true);
    };
    reader.readAsDataURL(file);
}

// 粘贴或拖入纯文本及本地图片，网页来源的格式和外部资源不会被带入。
function insertTransfer(transfer, selection) {
    const file = [...transfer.files].find(item => item.type.startsWith("image/"));
    if (file)
        insertImage(file, selection);
    else if (transfer.getData("text/plain"))
        format("insertText", transfer.getData("text/plain"), selection);
}

// 通过剪贴板事件读取用户主动粘贴的内容。
function paste(event) {
    event.preventDefault();
    insertTransfer(event.clipboardData, range);
}

// 将拖入的内容插入鼠标落点。
function drop(event) {
    event.preventDefault();
    const position = document.caretRangeFromPoint(event.clientX, event.clientY);
    insertTransfer(event.dataTransfer, position);
}

document.addEventListener("selectionchange", rememberSelection);
toolbar.addEventListener("mousedown", event => {
    rememberSelection();
    if (event.target.closest("button"))
        event.preventDefault();
});
toolbar.addEventListener("click", event => {
    const action = event.target.closest("button[data-command]");
    if (action)
        format(action.dataset.command);
});
toolbar.addEventListener("change", event => {
    const control = event.target;
    if (control.dataset.command && control.value) {
        format(control.dataset.command, control.value);
        if (control instanceof HTMLSelectElement)
            control.selectedIndex = 0;
    }
});
title.addEventListener("input", changed);
category.addEventListener("change", changed);
saveButton.addEventListener("click", save);
exportButton.addEventListener("click", () => host.exportPackage(revision, result => {
    if (!result.ok)
        report(result.error, true);
    else if (result.data?.path)
        report("已导出 .DaDocH 文档");
}));
byId("table").addEventListener("click", () => tableDialog.showModal());
byId("table-cancel").addEventListener("click", () => tableDialog.close());
tableDialog.querySelector("form").addEventListener("submit", insertTable);
byId("image").addEventListener("click", () => imageInput.click());
imageInput.addEventListener("change", () => {
    if (imageInput.files.length)
        insertImage(imageInput.files[0]);
    imageInput.value = "";
});
document.addEventListener("keydown", event => {
    if (event.ctrlKey && event.key.toLowerCase() === "s") {
        event.preventDefault();
        save();
    }
});

// 内置样式随文档保存，配置不会影响其他文档。
const defaultPresets = [
    {name: "正文", font: "微软雅黑", size: "3", bold: false, italic: false, color: ""},
    {name: "一级标题", font: "微软雅黑", size: "6", bold: true, italic: false, color: ""},
    {name: "二级标题", font: "微软雅黑", size: "5", bold: true, italic: false, color: ""},
    {name: "三级标题", font: "微软雅黑", size: "4", bold: true, italic: false, color: ""},
    {name: "重点", font: "微软雅黑", size: "3", bold: true, italic: false, color: "#9a6224"},
    {name: "引用", font: "楷体", size: "3", bold: false, italic: true, color: ""}
];
// 当前文档的预设及导航到正文节点的映射。
let presets = defaultPresets.map(item => ({...item}));
let outlineEntries = [];

// 只读取支持的预设字段，避免无效字号或样式进入编辑命令。
function normalizePresets(value) {
    if (!Array.isArray(value) || value.length !== defaultPresets.length)
        return defaultPresets.map(item => ({...item}));
    return value.map((item, index) => ({
        name: String(item?.name || defaultPresets[index].name).slice(0, 30),
        font: ["微软雅黑", "宋体", "黑体", "楷体", "Arial", "Times New Roman"].includes(item?.font) ? item.font : "微软雅黑",
        size: /^[2-7]$/.test(item?.size) ? String(item.size) : "3",
        bold: Boolean(item?.bold), italic: Boolean(item?.italic),
        color: /^#[a-f0-9]{6}$/i.test(item?.color) ? item.color : ""
    }));
}

// 读取有序列表的实际编号，包括嵌套层级、起始值和单独覆盖的编号。
function listNumber(item) {
    const list = item.parentElement;
    const children = [...list.children].filter(child => child.tagName === "LI");
    const step = list.reversed ? -1 : 1;
    let number = list.hasAttribute("start") ? list.start : (list.reversed ? children.length : 1);
    for (const child of children) {
        if (child.hasAttribute("value")) number = child.value;
        if (child === item) break;
        number += step;
    }
    const parent = list.parentElement.closest("li");
    return (parent?.parentElement.tagName === "OL" ? listNumber(parent) + "." : "") + number;
}

// 从实际编号段落生成目录；不向文档添加锚点或影响撤销记录。
function updateOutline() {
    outlineEntries = [];
    const entries = [];
    for (const node of editor.querySelectorAll("li, p, h1, h2, h3, h4, h5, h6")) {
        if (node.tagName !== "LI" && node.closest("li")) continue;
        const copy = node.cloneNode(true);
        copy.querySelectorAll("ol,ul").forEach(list => list.remove());
        const text = copy.textContent.trim().replace(/\s+/g, " ");
        const ordered = node.tagName === "LI" && node.parentElement.tagName === "OL";
        if (!ordered && !/^(?:\d+(?:[.．]\d+)*[.．、)）\s]|[（(]\d+[)）]|[一二三四五六七八九十百]+[、.．]|第[一二三四五六七八九十百\d]+[章节条])/.test(text)) continue;
        const button = document.createElement("button");
        button.type = "button";
        button.textContent = (ordered ? listNumber(node) + "  " : "") + (text || "未命名段落");
        button.title = button.textContent;
        const depth = ordered ? listNumber(node).split(".").length - 1 : 0;
        button.style.paddingInlineStart = 9 + Math.min(depth, 5) * 12 + "px";
        button.addEventListener("click", () => {
            node.scrollIntoView({block: "start", behavior: "smooth"});
            editor.focus({preventScroll: true});
            const target = document.createRange();
            target.selectNodeContents(node);
            target.collapse(true);
            restoreSelection(target);
            rememberSelection();
        });
        outlineEntries.push({node, button});
        entries.push(button);
    }
    if (!entries.length) {
        const empty = document.createElement("p");
        empty.className = "hint";
        empty.textContent = "暂无编号段落。选中段落后点击工具栏“编号”。";
        entries.push(empty);
    }
    byId("outline").replaceChildren(...entries);
    highlightOutline();
}

// 根据光标所在段落标记当前导航项。
function highlightOutline() {
    const anchor = window.getSelection().anchorNode;
    for (const {node, button} of outlineEntries)
        button.setAttribute("aria-current", String(Boolean(anchor && node.contains(anchor))));
}

// 展示预设的实际字体示例，并保持正文选区。
function renderPresets() {
    byId("styles").replaceChildren(...presets.map((preset, index) => {
        const button = document.createElement("button");
        button.type = "button";
        const sample = document.createElement("span");
        sample.textContent = preset.name;
        sample.style.fontFamily = preset.font;
        sample.style.fontWeight = preset.bold ? "bold" : "normal";
        sample.style.fontStyle = preset.italic ? "italic" : "normal";
        sample.style.fontSize = Math.min(24, 10 + Number(preset.size) * 2) + "px";
        if (preset.color) sample.style.color = preset.color;
        const detail = document.createElement("small");
        detail.textContent = preset.font + " · " + ["", "", "小", "正文", "中", "大", "特大", "标题"][preset.size];
        button.append(sample, detail);
        button.addEventListener("mousedown", event => event.preventDefault());
        button.addEventListener("click", () => applyPreset(index));
        return button;
    }));
}

// 通过浏览器格式命令应用预设，整段模式先扩展选区，保留编号与表格结构。
function applyPreset(index) {
    if (!revision) return;
    restoreSelection();
    const selection = window.getSelection();
    if (byId("style-scope").value === "paragraph") {
        const selected = selection.getRangeAt(0).cloneRange();
        const block = node => (node instanceof Element ? node : node.parentElement)?.closest("p,li,h1,h2,h3,h4,h5,h6,blockquote,td,th,div");
        const start = block(selected.startContainer);
        const end = block(selected.endContainer);
        if (start && editor.contains(start)) selected.setStart(start, 0);
        if (end && editor.contains(end)) selected.setEnd(end, end.childNodes.length);
        restoreSelection(selected);
    } else if (selection.isCollapsed) {
        report("请先选中要应用样式的文字。", true);
        return;
    }
    rememberSelection();
    const preset = presets[index];
    format("removeFormat");
    format("fontName", preset.font);
    format("fontSize", preset.size);
    if (document.queryCommandState("bold") !== preset.bold) format("bold");
    if (document.queryCommandState("italic") !== preset.italic) format("italic");
    if (preset.color) format("foreColor", preset.color);
    report("已应用“" + preset.name + "”");
}

// 将选中的预设载入配置对话框，保存后与文档一同持久化。
function loadPresetForm() {
    const preset = presets[Number(byId("preset-index").value)];
    byId("preset-name").value = preset.name;
    byId("preset-font").value = preset.font;
    byId("preset-size").value = preset.size;
    byId("preset-bold").checked = preset.bold;
    byId("preset-italic").checked = preset.italic;
    byId("preset-use-color").checked = Boolean(preset.color);
    byId("preset-color").value = preset.color || "#9a6224";
}

// 密码只由原生窗口处理，网页仅显示是否已加锁。
function updateLock(locked) {
    byId("password").textContent = locked ? "密码锁：已设置" : "密码锁：未设置";
}
byId("password").addEventListener("click", () => {
    if (changes !== saved || !existing) { report("请先保存文档，再设置密码锁。", true); return; }
    host.configurePassword(revision, result => {
        if (!result.ok) report(result.error, true);
        else if (result.data) { updateLock(result.data.locked); report("密码锁已更新"); }
    });
});
byId("edit-styles").addEventListener("click", () => {
    byId("preset-index").replaceChildren(...presets.map((preset, index) => new Option(preset.name, index)));
    loadPresetForm();
    byId("styles-dialog").showModal();
});
byId("preset-index").addEventListener("change", loadPresetForm);
byId("styles-cancel").addEventListener("click", () => byId("styles-dialog").close());
byId("styles-dialog").querySelector("form").addEventListener("submit", event => {
    event.preventDefault();
    presets[Number(byId("preset-index").value)] = {
        name: byId("preset-name").value.trim() || "自定义样式", font: byId("preset-font").value,
        size: byId("preset-size").value, bold: byId("preset-bold").checked, italic: byId("preset-italic").checked,
        color: byId("preset-use-color").checked ? byId("preset-color").value : ""
    };
    renderPresets();
    changed();
    byId("styles-dialog").close();
});
document.addEventListener("selectionchange", highlightOutline);

saveButton.disabled = exportButton.disabled = true;
new QWebChannel(qt.webChannelTransport, channel => {
    host = channel.objects.documentHost;
    host.documentChanged.connect(apply);
    host.appearanceChanged.connect(appearance);
    host.state(apply);
});
