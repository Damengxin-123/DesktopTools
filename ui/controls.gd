extends Control
## 工作台页面共用的 Godot 原生控件与交互。

const Style = preload("res://ui/style.gd")
const Client = preload("res://native/client.gd")

# 外观偏好保存在 Godot 用户目录，与 Windows 后台设置分开。
const APPEARANCE_FILE := "user://appearance.cfg"

# 主题变化时通知主工作台更新背景和图标。
signal appearance_changed

# 当前工作台的主题标识。
var appearance := Style.DARK

# 独立 C++ 服务客户端。
var service := Client.new()
# 当前页面的键和切换代次。
var page := ""
var generation := 0
# 底部服务连接状态和操作消息。
var connection_label: Label
var status_label: Label
# 当前页面的主栏、操作区和动态列表。
var body: VBoxContainer
var page_actions: HBoxContainer
var results: VBoxContainer
# 工作区滚动容器。
var scroll: ScrollContainer

# 启动时载入主题，损坏或未知的偏好使用暗色。
func load_appearance() -> void:
	var config := ConfigFile.new()
	if config.load(APPEARANCE_FILE) == OK and config.get_value("ui", "theme", Style.DARK) == Style.GOLD:
		appearance = Style.GOLD
	theme = Style.create_theme(appearance)

# 持久化后更新整个工作台，不重建页面或清空输入。
func set_appearance(id: String) -> bool:
	if id == appearance:
		return true
	var config := ConfigFile.new()
	config.set_value("ui", "theme", id)
	var error := config.save(APPEARANCE_FILE)
	if error != OK:
		message("主题保存失败（%s）。" % error_string(error), true)
		return false
	appearance = id
	theme = Style.create_theme(appearance, theme.default_font_size)
	appearance_changed.emit()
	return true

# 创建纵向或横向控件组。
func column(parent: Node, gap: int = 10) -> VBoxContainer:
	var node := VBoxContainer.new()
	node.add_theme_constant_override("separation", gap)
	parent.add_child(node)
	return node

# 创建一行控件并统一对齐。
func row(parent: Node, gap: int = 8) -> HBoxContainer:
	var node := HBoxContainer.new()
	node.add_theme_constant_override("separation", gap)
	parent.add_child(node)
	return node

# 添加文字，支持裁剪和色彩层级。
func label(parent: Node, value: String, font_size: int = 15, variant: StringName = &"Label") -> Label:
	var node := Label.new()
	node.text = value
	node.add_theme_font_size_override("font_size", font_size)
	node.theme_type_variation = variant
	parent.add_child(node)
	return node

# 创建带统一边框和内边距的卡片内容。
func panel(parent: Node, variant: StringName = &"Card") -> VBoxContainer:
	var shell := PanelContainer.new()
	shell.theme_type_variation = variant
	parent.add_child(shell)
	return column(shell)

# 创建留白或弹性间隔。
func spacer(parent: Node, width: int = 0, height: int = 0, expand: bool = true) -> Control:
	var node := Control.new()
	node.custom_minimum_size = Vector2(width, height)
	if expand:
		node.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		node.size_flags_vertical = Control.SIZE_EXPAND_FILL
	parent.add_child(node)
	return node

# 添加原生按钮，强调操作使用当前主题的主色。
func button(parent: Node, value: String, pressed: Callable, primary: bool = false) -> Button:
	var node := Button.new()
	node.text = value
	node.custom_minimum_size.y = 34
	if primary:
		node.theme_type_variation = &"PrimaryButton"
	node.pressed.connect(pressed)
	parent.add_child(node)
	return node

# 创建原生单行输入框。
func line(parent: Node, placeholder: String, value: String = "") -> LineEdit:
	var node := LineEdit.new()
	node.placeholder_text = placeholder
	node.text = value
	node.custom_minimum_size.y = 39
	node.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	parent.add_child(node)
	return node

# 创建原生勾选项。
func check(parent: Node, value: String, active: bool) -> CheckBox:
	var node := CheckBox.new()
	node.text = value
	node.button_pressed = active
	parent.add_child(node)
	return node

# 创建原生下拉选择框。
func options(parent: Node, choices: Array, selected: int = 0) -> OptionButton:
	var node := OptionButton.new()
	for value in choices:
		node.add_item(str(value))
	node.select(clampi(selected, 0, choices.size() - 1))
	parent.add_child(node)
	return node

# 添加可缩略的单行文本。
func clipped(parent: Node, value: String, font_size: int = 14, variant: StringName = &"MutedLabel") -> Label:
	var node := label(parent, value.replace("\n", " "), font_size, variant)
	node.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
	node.tooltip_text = value
	node.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	return node

# 移除当前容器中的控件。
func clear(parent: Node) -> void:
	if not is_instance_valid(parent):
		return
	for child in parent.get_children():
		parent.remove_child(child)
		child.queue_free()

# 将失败或成功提示写入工作台状态栏。
func message(value: String, failed: bool = false) -> void:
	status_label.text = value
	status_label.theme_type_variation = &"ErrorLabel" if failed else &"MutedLabel"

# 调用服务并统一显示后端错误。
func invoke(method: String, args: Array = [], done: Callable = Callable()) -> void:
	service.request(method, args, func(result: Dictionary) -> void:
		if not result.get("ok", false):
			message(str(result.get("error", "操作失败。")), true)
		elif done.is_valid():
			done.call(result.get("data")))

# 调用当前页面服务，切页后忽略过期的页面响应。
func query(method: String, args: Array, done: Callable) -> void:
	var version := generation
	invoke(method, args, func(data: Variant) -> void:
		if version == generation:
			done.call(data))

# 创建带确认按钮的原生弹窗。
func dialog(title: String, size_hint: Vector2i, submit: Callable = Callable()) -> VBoxContainer:
	var popup := ConfirmationDialog.new()
	popup.title = title
	_style_dialog(popup)
	popup.ok_button_text = "确定" if submit.is_valid() else "关闭"
	popup.get_cancel_button().visible = submit.is_valid()
	popup.unresizable = false
	add_child(popup)
	var margin := MarginContainer.new()
	for side in ["left", "right", "top", "bottom"]:
		margin.add_theme_constant_override("margin_" + side, 12)
	popup.add_child(margin)
	var fields := column(margin, 9)
	popup.confirmed.connect(func() -> void:
		if submit.is_valid():
			submit.call()
		popup.queue_free())
	popup.canceled.connect(popup.queue_free)
	popup.close_requested.connect(popup.queue_free)
	popup.popup_centered(size_hint)
	return fields

# 提示用户确认后再调用删除、清空或重建操作。
func confirm(title: String, detail: String, method: String, args: Array = []) -> void:
	var fields := dialog(title, Vector2i(470, 150), func() -> void: invoke(method, args))
	var note := label(fields, detail)
	note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART

# 配置应用内弹窗的通用主题与主次按钮。
func _style_dialog(popup: ConfirmationDialog) -> void:
	popup.theme = theme
	popup.transient_to_focused = true
	popup.get_ok_button().theme_type_variation = &"PrimaryButton"
	popup.get_cancel_button().text = "取消"

# 通过 Godot FileDialog 选择本地文件或文件夹。
func pick_file(title: String, filter: String, callback: Callable, directory: bool = false) -> void:
	var picker := FileDialog.new()
	picker.use_native_dialog = false
	picker.mode_overrides_title = false
	picker.title = title
	picker.access = FileDialog.ACCESS_FILESYSTEM
	picker.file_mode = FileDialog.FILE_MODE_OPEN_DIR if directory else FileDialog.FILE_MODE_OPEN_FILE
	_style_dialog(picker)
	picker.ok_button_text = "选择目录" if directory else "打开"
	if not filter.is_empty():
		picker.add_filter(filter)
	add_child(picker)
	var chosen := func(path: String) -> void:
		picker.hide()
		picker.queue_free()
		callback.call(path)
	picker.file_selected.connect(chosen)
	picker.dir_selected.connect(chosen)
	picker.canceled.connect(picker.queue_free)
	picker.close_requested.connect(picker.queue_free)
	picker.popup_centered_ratio(0.72)

# 读取服务返回的 base64 图片以绘制缩略图。
func texture_from_url(value: String) -> ImageTexture:
	var separator := value.find(",")
	if not value.begins_with("data:image/") or separator < 0:
		return null
	var bytes := Marshalls.base64_to_raw(value.substr(separator + 1))
	var image := Image.new()
	var mime := value.substr(0, separator)
	var error := ERR_UNAVAILABLE
	if "png" in mime:
		error = image.load_png_from_buffer(bytes)
	elif "jpeg" in mime:
		error = image.load_jpg_from_buffer(bytes)
	elif "webp" in mime:
		error = image.load_webp_from_buffer(bytes)
	return ImageTexture.create_from_image(image) if error == OK else null

# 添加等比例图片预览并保持列表布局稳定。
func thumbnail(parent: Node, url: String, width: int = 52, height: int = 48) -> void:
	var image := texture_from_url(url)
	if image == null:
		return
	var view := TextureRect.new()
	view.texture = image
	view.custom_minimum_size = Vector2(width, height)
	view.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	view.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	parent.add_child(view)

# 为空列表保留与正式卡片一致的版面。
func empty(parent: Node, title: String, detail: String) -> void:
	var card := panel(parent, &"EmptyCard")
	label(card, title, 19)
	var hint := label(card, detail, 14, &"MutedLabel")
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART

# 创建带标题和摘要的记录行，并返回操作区。
func record(parent: Node, title: String, detail: String, image: String = "") -> HBoxContainer:
	var card := panel(parent, &"Record")
	var layout := row(card, 12)
	thumbnail(layout, image)
	var copy := column(layout, 3)
	copy.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	clipped(copy, title, 15, &"Label")
	if not detail.is_empty():
		clipped(copy, detail, 13)
	return row(layout, 5)

# 由工作台实现的侧栏数量更新。
func _set_count(_key: String, _total: int) -> void:
	pass

# 从列表中取得条目名称，兼容服务提供的不同字段。
func first_text(item: Dictionary, keys: Array, fallback: String = "") -> String:
	for key in keys:
		var value: Variant = item.get(key)
		if value != null and not str(value).is_empty():
			return str(value)
	return fallback
