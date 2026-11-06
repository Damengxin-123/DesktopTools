extends "res://ui/pages.gd"
## 左侧导航、工作区与底部状态条。

# 保持截图中十二个入口的原顺序。
const NAV := [
	["shortcuts", "快捷方式"],
	["notes", "文档"],
	["emoji", "表情"],
	["downloads", "下载"],
	["clipboard", "剪贴板"],
	["wallpaper", "壁纸"],
	["qr", "二维码"],
	["screenshot", "截图"],
	["recording", "录屏"],
	["browser", "资源浏览器"],
	["filesearch", "文件检索"],
	["settings", "设置"]
]
# 按钮、标签和徽章按入口索引。
var navigation := {}
# 可折叠的导航面板及其品牌区域。
var sidebar: PanelContainer
var brand: VBoxContainer
var nav_caption: Label
var collapsed := false
# 需要随主题同步更新的背景与品牌图标。
var window_background: ColorRect
var brand_icon: TextureRect

# 组装原生工作台并连接 C++ 进程。
func _ready() -> void:
	load_appearance()
	window_background = ColorRect.new()
	window_background.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(window_background)
	window_background.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	var frame := HBoxContainer.new()
	frame.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	frame.add_theme_constant_override("separation", 0)
	add_child(frame)
	_make_sidebar(frame)
	_make_workspace(frame)
	appearance_changed.connect(_recolor_shell)
	_recolor_shell()
	add_child(service)
	service.connection_changed.connect(_connection)
	service.backend_event.connect(_event)
	_switch("shortcuts")
	service.start()

# 构建保留原有顺序的侧栏导航。
func _make_sidebar(frame: HBoxContainer) -> void:
	sidebar = PanelContainer.new()
	sidebar.custom_minimum_size.x = 230
	sidebar.theme_type_variation = &"Sidebar"
	frame.add_child(sidebar)
	var inner := column(sidebar, 12)
	var identity := row(inner, 10)
	var mark := PanelContainer.new()
	mark.custom_minimum_size = Vector2(40, 40)
	mark.theme_type_variation = &"BrandMark"
	identity.add_child(mark)
	brand_icon = _icon(mark, "brand", 23)
	brand = column(identity, 0)
	label(brand, "DESKTOP TOOL", 15)
	label(brand, "本地桌面工作台", 12, &"MutedLabel")
	var nav_scroll := ScrollContainer.new()
	nav_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	nav_scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	inner.add_child(nav_scroll)
	var nav := column(nav_scroll, 5)
	nav.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	spacer(nav, 0, 7, false)
	nav_caption = label(nav, "  工具模块  /  MODULES", 11, &"MutedLabel")
	spacer(nav, 0, 5, false)
	for item in NAV:
		var key: String = item[0]
		var entry := button(nav, "", _switch.bind(key))
		entry.custom_minimum_size.y = 45
		entry.tooltip_text = item[1]
		var inset := MarginContainer.new()
		inset.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
		inset.add_theme_constant_override("margin_left", 13)
		inset.add_theme_constant_override("margin_right", 12)
		entry.add_child(inset)
		var layout := row(inset, 11)
		var glyph := _icon(layout, key, 20)
		glyph.size_flags_vertical = Control.SIZE_SHRINK_CENTER
		var name := label(layout, item[1], 15)
		name.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		var badge := label(layout, "", 11, &"CountLabel")
		badge.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
		badge.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
		badge.visible = false
		_ignore_pointer(inset)
		navigation[key] = [entry, glyph, name, badge]
	var footer := panel(inner, &"SidebarFooter")
	label(footer, "◆  WINDOWS 工作区", 12, &"AccentLabel")
	button(footer, "收起侧栏", func() -> void:
		collapsed = not collapsed
		sidebar.custom_minimum_size.x = 66 if collapsed else 230
		brand.visible = not collapsed
		nav_caption.visible = not collapsed
		footer.visible = not collapsed
		for entry in navigation.values():
			entry[2].visible = not collapsed
			entry[3].visible = not collapsed and not entry[3].text.is_empty())
	var expand := button(inner, "☰", func() -> void:
		collapsed = false
		sidebar.custom_minimum_size.x = 230
		brand.show()
		nav_caption.show()
		footer.show()
		for entry in navigation.values():
			entry[2].show()
			entry[3].visible = not entry[3].text.is_empty())
	expand.visible = false
	footer.visibility_changed.connect(func() -> void: expand.visible = collapsed)

# 让导航子控件的点击统一由外层按钮处理。
func _ignore_pointer(node: Control) -> void:
	node.mouse_filter = Control.MOUSE_FILTER_IGNORE
	for child in node.get_children():
		_ignore_pointer(child)

# 创建滚动工作区、按需显示的页面操作区及底部状态条。
func _make_workspace(frame: HBoxContainer) -> void:
	var workspace := column(frame, 0)
	workspace.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll = ScrollContainer.new()
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	workspace.add_child(scroll)
	var inset := MarginContainer.new()
	inset.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	for side in ["left", "right", "top", "bottom"]:
		inset.add_theme_constant_override("margin_" + side, 24)
	scroll.add_child(inset)
	var surface := column(inset, 19)
	page_actions = row(surface)
	page_actions.visible = false
	page_actions.child_order_changed.connect(func() -> void:
		page_actions.visible = page_actions.get_child_count() > 0)
	body = column(surface, 15)
	var bottom := row(panel(workspace, &"StatusBar"))
	label(bottom, "◆  本地工作区", 12, &"AccentLabel")
	status_label = clipped(bottom, "正在初始化…", 12)
	connection_label = label(bottom, "●  正在连接本机服务", 12, &"AccentLabel")
	button(bottom, "重新连接", service.start).name = "Reconnect"

# 添加一个 Godot 矢量图标控件。
func _icon(parent: Node, key: String, pixels: int) -> TextureRect:
	var node := TextureRect.new()
	node.texture = Style.icon(key)
	node.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	node.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	node.custom_minimum_size = Vector2(pixels, pixels)
	parent.add_child(node)
	return node

# 切换页面并更新导航、操作区和滚动状态。
func _switch(key: String) -> void:
	generation += 1
	page = key
	clear(body)
	clear(page_actions)
	scroll.scroll_vertical = 0
	message("准备就绪" if service.online else "正在等待本机服务")
	_update_navigation_styles(Style.palette(appearance))
	if service.online or key == "settings":
		_open_page()
	else:
		empty(body, "等待本机服务", "启动 Windows C++ 进程后，这里将显示实际数据。")

# 将当前主题应用到原生主题控件之外的绘制内容。
func _recolor_shell() -> void:
	var colors := Style.palette(appearance)
	window_background.color = colors.background
	brand_icon.modulate = colors.accent
	_update_navigation_styles(colors)
	if service.online:
		invoke("setDocumentAppearance", [appearance])

# 保留侧栏选中状态，并随主题更新文字与图标。
func _update_navigation_styles(colors: Dictionary) -> void:
	for key in navigation:
		var entry: Array = navigation[key]
		var active: bool = key == page
		entry[0].theme_type_variation = &"ActiveNavigationButton" if active else &"NavigationButton"
		entry[1].modulate = colors.accent if active else colors.muted
		entry[2].theme_type_variation = &"Label" if active else &"MutedLabel"

# 更新连接提示和首页，并从后端读取真实数量。
func _connection(online: bool, detail: String) -> void:
	connection_label.text = "●  C++ 服务在线" if online else "●  本机服务离线"
	connection_label.theme_type_variation = &"OnlineLabel" if online else &"ErrorLabel"
	connection_label.get_parent().get_node("Reconnect").visible = not online
	_switch(page)
	message(detail, not online)
	if online:
		invoke("getSettings", [], func(data: Dictionary) -> void:
			theme.default_font_size = int(data.get("fontSize", 15)))
		invoke("getCounts", [], func(data: Dictionary) -> void:
			for key in data:
				_set_count(key, int(data[key])))

# 接收下载进度、历史变更和文件检索结果。
func _event(topic: String, data: Dictionary) -> void:
	if topic == "activate":
		get_window().mode = Window.MODE_WINDOWED if get_window().mode == Window.MODE_MINIMIZED else get_window().mode
		get_window().grab_focus()
	elif topic == "fileSearchResults":
		_file_results(data)
	elif topic == "fileSearchStatus":
		_index_status()
	elif topic == page:
		_refresh()
	elif navigation.has(topic):
		invoke("getCounts", [topic], func(counts: Dictionary) -> void:
			if counts.has(topic):
				_set_count(topic, int(counts[topic])))

# 用真实条目数刷新侧栏徽章。
func _set_count(key: String, total: int) -> void:
	if not navigation.has(key):
		return
	var badge: Label = navigation[key][3]
	badge.text = "%d万" % (total / 10000) if total >= 100000 else str(total)
	badge.tooltip_text = str(total)
	badge.visible = not collapsed
