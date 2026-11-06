extends "res://ui/controls.gd"
## 快捷方式、文档和表情的分类与列表。

# 当前分类、搜索栏和分类选择控件。
var catalog_category := ""
var catalog_categories: Array = []
var catalog_filter: LineEdit
var catalog_tabs: HFlowContainer
# 文档列表旁显示编辑入口和 HTML 排版说明。
var document_panel: VBoxContainer
# 图片选择器支持的格式。
const IMAGES := "*.png, *.jpg, *.jpeg, *.gif, *.webp, *.bmp, *.tif, *.ico; 图片"

# 放置分类栏、搜索、列表和文档分栏。
func open_catalog() -> void:
	catalog_category = ""
	catalog_categories = []
	var toolbar := row(panel(body))
	catalog_filter = line(toolbar, "搜索名称、地址或关键词…")
	catalog_filter.text_changed.connect(func(_value: String) -> void: _catalog_refresh())
	button(toolbar, "＋ 新建分类", _category_dialog)
	catalog_tabs = HFlowContainer.new()
	catalog_tabs.add_theme_constant_override("h_separation", 7)
	catalog_tabs.add_theme_constant_override("v_separation", 6)
	body.add_child(catalog_tabs)
	if page == "shortcuts":
		button(page_actions, "＋ 添加快捷方式", _shortcut_editor.bind({}), true)
	elif page == "emoji":
		button(page_actions, "从剪贴板添加", func() -> void:
			invoke("pasteEmojiImage", [], func(data: Dictionary) -> void: _emoji_editor(data, false)))
		button(page_actions, "＋ 添加表情", func() -> void:
			pick_file("选择表情", IMAGES, func(path: String) -> void: _emoji_editor({"path": path}, false)), true)
	else:
		button(page_actions, "＋ 新建文档", _open_document.bind(""), true)
		button(page_actions, "导入文档", func() -> void:
			invoke("importDocument", [catalog_category if not catalog_category.is_empty() else "default", appearance]))
		var layout := row(body, 13)
		var list := panel(layout, &"InsetCard")
		list.get_parent().custom_minimum_size.x = 248
		results = list
		document_panel = panel(layout)
		document_panel.get_parent().size_flags_horizontal = Control.SIZE_EXPAND_FILL
		label(document_panel, "文档编辑", 20)
		var hint := label(document_panel, "从左侧打开文档，或新建一篇。支持段落导航、字体样式预设、表格和图片；可导入或导出文档包。", 14, &"MutedLabel")
		hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		label(document_panel, "保存格式  ·  .DaDocH（正文与资源打包）", 12, &"AccentLabel")
		button(row(document_panel), "新建文档", _open_document.bind(""), true)
	if page != "notes":
		results = column(body, 8)
	_catalog_refresh()

# 读取分类和结果，过滤过期的输入请求。
func _catalog_refresh() -> void:
	var filter := catalog_filter.text
	var method := {"shortcuts": "getShortcuts", "notes": "getNotes", "emoji": "getEmojis"}
	query(method[page], [filter] if page == "emoji" else [], func(data: Dictionary) -> void:
		if catalog_filter.text != filter:
			return
		catalog_categories = data.get("categories", [])
		var items: Array = data.get("items", [])
		_set_count(page, int(data.get("total", items.size())))
		_draw_categories()
		_draw_catalog(items, filter))

# 分类控件支持筛选、重命名和移除。
func _draw_categories() -> void:
	clear(catalog_tabs)
	button(catalog_tabs, "全部", _select_category.bind(""), catalog_category.is_empty())
	for category in catalog_categories:
		button(catalog_tabs, category.name, _select_category.bind(category.id), catalog_category == category.id)
	if catalog_category not in ["", "default"]:
		button(catalog_tabs, "编辑分类", _category_dialog.bind(catalog_category))

# 更新当前分类与结果。
func _select_category(id: String) -> void:
	catalog_category = id
	_catalog_refresh()

# 选择当前目录对应的服务分类接口。
func _catalog_suffix() -> String:
	return {"shortcuts": "Shortcut", "notes": "Note", "emoji": "Emoji"}[page]

# 打开分类新建或编辑窗口。
func _category_dialog(id: String = "") -> void:
	var fields := {}
	var suffix := _catalog_suffix()
	var contents := dialog("编辑分类" if not id.is_empty() else "新建分类", Vector2i(420, 155),
		func() -> void: invoke("save" + suffix + "Category", [id, fields.input.text]))
	var existing := ""
	for category in catalog_categories:
		if category.id == id:
			existing = category.name
	fields.input = line(contents, "分类名称", existing)
	if not id.is_empty():
		button(contents, "删除分类", func() -> void:
			var editor := contents.get_window()
			editor.hide()
			editor.queue_free()
			catalog_category = ""
			confirm("删除分类", "该分类中的条目将移回默认分类。", "delete" + suffix + "Category", [id]))

# 绘制当前分类下的条目，图库使用自适应网格。
func _draw_catalog(items: Array, filter: String) -> void:
	clear(results)
	var shown: Array = []
	for item in items:
		if not catalog_category.is_empty() and item.get("categoryId") != catalog_category:
			continue
		var name := first_text(item, ["title", "name"])
		if page != "emoji" and not filter.strip_edges().is_empty():
			if not (name + " " + str(item.get("target", ""))).containsn(filter.strip_edges()):
				continue
		shown.append(item)
	if shown.is_empty():
		empty(results, "没有符合条件的条目", "试试切换分类，或使用上方按钮添加。")
		return
	var mode := page
	var delete_method: String = {"shortcuts": "deleteShortcuts", "notes": "deleteNotes", "emoji": "deleteEmojis"}[mode]
	var selection := {}
	var tool := row(results)
	var all := check(tool, "全选", false)
	var remove := button(tool, "删除已选", func() -> void:
		confirm("删除已选记录", "将移除 %d 条记录。" % selection.size(), delete_method, [selection.keys()]))
	remove.disabled = true
	var list: Node = results
	if mode != "notes":
		var grid := HFlowContainer.new()
		grid.add_theme_constant_override("h_separation", 11)
		grid.add_theme_constant_override("v_separation", 11)
		results.add_child(grid)
		list = grid
	var toggles: Array[CheckBox] = []
	for item in shown:
		var id: String = item.id
		var title := first_text(item, ["title", "name"], "未命名")
		var card := panel(list, &"Tile")
		if mode != "notes":
			card.get_parent().custom_minimum_size.x = 245
			card.get_parent().size_flags_horizontal = Control.SIZE_EXPAND_FILL
		var heading := row(card)
		var selected := check(heading, "", false)
		selected.tooltip_text = "选择" + title
		toggles.append(selected)
		selected.toggled.connect(func(active: bool) -> void:
			if active:
				selection[id] = true
			else:
				selection.erase(id)
			remove.disabled = selection.is_empty())
		clipped(heading, title, 15, &"Label")
		var actions := row(card, 5)
		match mode:
			"shortcuts":
				clipped(card, item.target)
				button(actions, "打开", func() -> void: invoke("openShortcut", [id]), true)
				button(actions, "编辑", _shortcut_editor.bind(item))
				button(actions, "复制", func() -> void: DisplayServer.clipboard_set(item.target))
			"notes":
				clipped(card, str(item.get("updatedAt", "")).replace("T", " ").left(19), 12)
				button(actions, "打开文档", _open_document.bind(id), true)
				button(actions, "排序", _move_catalog.bind(item, shown))
			"emoji":
				thumbnail(card, item.get("thumbnail", ""), 105, 83)
				clipped(card, str(item.get("path", "")))
				button(actions, "复制", func() -> void: invoke("copyEmojiFile", [id]), true)
				button(actions, "定位", func() -> void: invoke("openEmojiDirectory", [id]))
				button(actions, "编辑", _emoji_editor.bind(item, true))
		if mode == "shortcuts":
			button(actions, "排序", _move_catalog.bind(item, shown))
	all.toggled.connect(func(active: bool) -> void:
		for toggle in toggles:
			toggle.button_pressed = active)

# 返回分类下拉列表，并选中当前归属。
func _category_options(parent: Node, id: String) -> OptionButton:
	var names: Array = []
	var current := 0
	for entry in catalog_categories:
		if entry.id == id:
			current = names.size()
		names.append(entry.name)
	if names.is_empty():
		names.append("默认分类")
	return options(parent, names, current)

# 获取分类下拉选中的持久化标识。
func _category_id(index: int) -> String:
	if index >= 0 and index < catalog_categories.size():
		return catalog_categories[index].id
	return "default"

# 保存快捷方式的类型、名称、路径和分类。
func _shortcut_editor(item: Dictionary) -> void:
	var fields := {}
	var contents := dialog("快捷方式", Vector2i(560, 300), func() -> void:
		invoke("saveShortcut", [{"id": item.get("id", ""), "title": fields.title.text,
			"target": fields.target.text, "type": fields.type.selected, "categoryId": _category_id(fields.category.selected)}]))
	fields.title = line(contents, "名称", item.get("title", ""))
	fields.type = options(contents, ["目录", "网址", "文件"], int(item.get("type", 0)))
	var path_row := row(contents)
	fields.target = line(path_row, "文件、目录或网址", item.get("target", ""))
	button(path_row, "浏览…", func() -> void:
		if fields.type.selected == 1:
			fields.target.grab_focus()
			return
		pick_file("选择目标", "", func(path: String) -> void: fields.target.text = path, fields.type.selected == 0))
	fields.category = _category_options(contents, item.get("categoryId", catalog_category))

# 按目标分类和插入位置重新排序。
func _move_catalog(item: Dictionary, shown: Array) -> void:
	var fields := {}
	var suffix := _catalog_suffix()
	var entries := [""]
	var titles := ["放到分类末尾"]
	for candidate in shown:
		if candidate.id != item.id and candidate.categoryId == item.categoryId:
			entries.append(candidate.id)
			titles.append("放在「%s」之前" % candidate.title)
	var contents := dialog("移动与排序", Vector2i(460, 220), func() -> void:
		var category := _category_id(fields.category.selected)
		invoke("move" + suffix, [item.id, category, entries[fields.order.selected] if category == item.categoryId else ""]))
	fields.category = _category_options(contents, item.categoryId)
	fields.order = options(contents, titles)

# 保存表情关键字和所属分类。
func _emoji_editor(item: Dictionary, editing: bool) -> void:
	var fields := {}
	var path := first_text(item, ["path", "target"])
	var contents := dialog("编辑表情" if editing else "添加表情", Vector2i(520, 245), func() -> void:
		var words: Array = []
		for word in fields.words.text.replace("，", ",").split(",", false):
			if not word.strip_edges().is_empty():
				words.append(word.strip_edges())
		invoke("saveEmoji" if editing else "addEmoji", [{"id": item.get("id", ""), "path": path,
			"keywords": words, "categoryId": _category_id(fields.category.selected)}]))
	clipped(contents, path)
	fields.words = line(contents, "用逗号分隔检索关键词", ", ".join(item.get("keywords", [])))
	fields.category = _category_options(contents, item.get("categoryId", catalog_category))

# 打开所见即所得的 HTML 编辑器，新文档使用当前分类。
func _open_document(id: String) -> void:
	var category := catalog_category if not catalog_category.is_empty() else "default"
	invoke("openDocument", [id, category, appearance])
