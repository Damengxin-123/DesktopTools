extends "res://ui/capture.gd"
## 下载、剪贴板、资源浏览器、文件检索和设置页面。

# 剪贴板当前筛选条件、监听配置区。
var clipboard_filter: LineEdit
var clipboard_kind: OptionButton
var clipboard_types: VBoxContainer
# 文件检索输入、进度和分页状态。
var search_input: LineEdit
var search_in_path: CheckBox
var search_status: Label
var search_more: Button
var search_request := -1
var search_serial := 0
var search_next := 0
# 最近一次提交给索引服务的检索文本。
var last_search := ""
# 下载活动摘要。
var downloading: Label
# 记录当前设置分页，切页或恢复默认后仍回到此分类。
var settings_tab := 0
# 设置分页从界面偏好到本机信息排列。
const SETTINGS_TABS := ["外观", "系统", "下载", "关于"]
# 下载状态的中文显示名称。
const DOWNLOAD_STATUS := {
	"downloading": "下载中", "paused": "已暂停", "completed": "已完成", "failed": "失败",
	"cancelled": "已取消", "queued": "等待中", "resolving": "正在获取元数据",
	"awaiting_selection": "等待选择文件", "retrying": "准备重试"
}
# 剪贴板类型的服务标识和名称。
const TYPES := {"text": "文本", "image": "图像", "files": "文件 / 文件夹", "links": "链接", "other": "其他"}
# 资源浏览器支持的视频后缀。
const VIDEOS := "*.mp4, *.mkv, *.avi, *.webm, *.mov, *.m4v, *.wmv, *.mpg, *.mpeg; 视频"

# 按入口生成对应的控件。
func _open_page() -> void:
	match page:
		"shortcuts", "notes", "emoji": open_catalog()
		"downloads": _open_downloads()
		"clipboard": _open_clipboard()
		"wallpaper": open_wallpaper()
		"qr": open_qr()
		"screenshot", "recording": open_capture()
		"browser": _open_browser()
		"filesearch": _open_search()
		"settings": _open_settings()

# 响应后台通知，更新当前页面的结果区。
func _refresh() -> void:
	match page:
		"shortcuts", "notes", "emoji": _catalog_refresh()
		"downloads": _download_refresh()
		"clipboard": _clipboard_refresh()
		"wallpaper": refresh_wallpaper()
		"qr": refresh_qr()
		"screenshot", "recording": refresh_capture()

# 保持下载链接与目录输入在上方，任务在下方。
func _open_downloads() -> void:
	downloading = label(page_actions, "等待任务状态…", 13, &"AccentLabel")
	var form := panel(body)
	label(form, "下载链接", 16)
	var url_row := row(form)
	var url := line(url_row, "粘贴 HTTP、HTTPS 或 magnet:? 磁力链接")
	label(form, "保存到", 14, &"MutedLabel")
	var path_row := row(form)
	var target := line(path_row, "留空使用默认下载目录")
	var create := func() -> void:
		var value := url.text
		invoke("createDownload", [{"url": value, "directory": target.text}], func(_data: Variant) -> void:
			if is_instance_valid(url) and url.text == value:
				url.clear()
			message("下载任务已创建。"))
	button(url_row, "↓ 创建下载", create, true)
	url.text_submitted.connect(func(_value: String) -> void: create.call())
	button(path_row, "选择目录…", func() -> void:
		pick_file("选择下载目录", "", func(path: String) -> void: target.text = path, true))
	button(path_row, "使用默认", target.clear)
	label(form, "磁力链接将先获取文件清单，确认选中的文件后开始传输。", 13, &"MutedLabel")
	label(body, "下载任务", 19)
	results = column(body, 8)
	_download_refresh()

# 绘制真实下载进度、速度和各状态对应的操作。
func _download_refresh() -> void:
	query("getDownloads", [], func(data: Dictionary) -> void:
		downloading.text = "●  %d 个任务进行中" % int(data.get("activeCount", 0))
		_show_history(data, "还没有下载任务", func(task: Dictionary) -> void:
			var id: String = task.id
			var status: String = task.status
			var card := panel(results)
			var header := row(card)
			clipped(header, first_text(task, ["fileName", "url"]), 16, &"Label")
			label(header, DOWNLOAD_STATUS.get(status, status), 13, &"AccentLabel")
			var received := int(task.get("bytesReceived", 0))
			var total := int(task.get("totalBytes", 0))
			if total > 0:
				var progress := ProgressBar.new()
				progress.custom_minimum_size.y = 6
				progress.show_percentage = false
				progress.value = 100.0 * float(received) / total
				card.add_child(progress)
			var controls := row(card)
			clipped(controls, "%s  ·  %s/s" % [String.humanize_size(received),
				String.humanize_size(int(task.get("speed", 0)))])
			if status in ["downloading", "resolving", "retrying", "queued"]:
				button(controls, "暂停", func() -> void: invoke("pauseDownload", [id]))
			if status in ["paused", "failed"]:
				button(controls, "继续 / 重试", func() -> void: invoke("resumeDownload", [id]))
			if status == "awaiting_selection":
				button(controls, "选择文件", _download_files.bind(id), true)
			if status not in ["completed", "cancelled"]:
				button(controls, "取消", func() -> void:
					confirm("取消下载", "取消此下载任务？", "cancelDownload", [id]))
			button(controls, "打开目录", func() -> void: invoke("openDownloadDirectory", [id]))
			button(controls, "移除", func() -> void: confirm("移除下载记录", "移除此下载任务。",
				"removeDownload", [id]))
			var warning := first_text(task, ["error", "warning"])
			if not warning.is_empty():
				clipped(card, warning, 13, &"ErrorLabel")))

# 磁力任务只在用户确认文件清单后开始传输。
func _download_files(id: String) -> void:
	query("getDownloadFiles", [id], func(data: Dictionary) -> void:
		var selected := {}
		var fields := dialog("选择要下载的文件", Vector2i(690, 460), func() -> void:
			if selected.is_empty():
				message("至少选择一个文件。", true)
			else:
				invoke("confirmDownloadFiles", [id, selected.keys()]))
		var tools := row(fields)
		var list := ScrollContainer.new()
		list.custom_minimum_size.y = 315
		list.size_flags_vertical = Control.SIZE_EXPAND_FILL
		fields.add_child(list)
		var entries := column(list)
		var toggles: Array[CheckBox] = []
		for file in data.get("files", []):
			var index := int(file.index)
			var active := bool(file.get("selected", false))
			var toggle := check(entries, "%s   (%s)" % [file.path, String.humanize_size(int(file.size))], active)
			if active:
				selected[index] = true
			toggle.toggled.connect(func(value: bool) -> void:
				if value:
					selected[index] = true
				else:
					selected.erase(index))
			toggles.append(toggle)
		button(tools, "全选", func() -> void:
			for toggle in toggles:
				toggle.button_pressed = true)
		button(tools, "全不选", func() -> void:
			for toggle in toggles:
				toggle.button_pressed = false))

# 创建监听类型、历史筛选和操作区。
func _open_clipboard() -> void:
	clipboard_types = panel(body)
	var tools := row(panel(body))
	clipboard_filter = line(tools, "搜索完整文本、文件名或路径…")
	clipboard_kind = options(tools, ["全部类型"] + TYPES.values())
	clipboard_filter.text_changed.connect(func(_value: String) -> void: _clipboard_refresh())
	clipboard_kind.item_selected.connect(func(_value: int) -> void: _clipboard_refresh())
	button(tools, "刷新", _clipboard_refresh)
	button(tools, "清空历史", func() -> void:
		confirm("清空剪贴板历史", "会移除所有历史记录，包括置顶内容。", "clearClipboardHistory"))
	results = column(body)
	_clipboard_refresh()

# 加载真实剪贴板历史，保留类型筛选和置顶状态。
func _clipboard_refresh() -> void:
	var filter := clipboard_filter.text
	query("getClipboardHistory", [filter], func(data: Dictionary) -> void:
		if filter != clipboard_filter.text:
			return
		clear(clipboard_types)
		label(clipboard_types, "自动保存的内容类型", 17)
		var types := row(clipboard_types, 13)
		var active_types: Array = data.get("types", [])
		for type in TYPES:
			var input := check(types, TYPES[type], type in active_types)
			input.toggled.connect(func(active: bool) -> void:
				if active:
					active_types.append(type)
				else:
					active_types.erase(type)
				invoke("setClipboardTypes", [active_types]))
		if not str(data.get("notice", "")).is_empty():
			clipped(clipboard_types, data.notice, 13, &"ErrorLabel")
		var index := clipboard_kind.selected
		var shown: Array = []
		for item in data.get("items", []):
			if index == 0 or item.type == TYPES.keys()[index - 1]:
				shown.append(item)
		clear(results)
		_set_count("clipboard", int(data.get("total", shown.size())))
		if shown.is_empty():
			empty(results, "没有剪贴板历史", "勾选需要记录的类型，或尝试调整当前筛选。")
			return
		var selected := {}
		var selection := row(results)
		var all := check(selection, "全选", false)
		var remove := button(selection, "删除已选", func() -> void:
			confirm("删除已选", "会移除选中的历史记录。", "deleteClipboardItems", [selected.keys()]))
		remove.disabled = true
		var toggles: Array[CheckBox] = []
		for item in shown:
			var id: String = item.id
			var title: String = TYPES.get(item.type, item.type)
			var tools := record(results, title + "  " + str(item.get("copiedAt", "")).replace("T", " ").left(19),
				str(item.get("preview", "")), item.get("thumbnail", ""))
			var toggle := check(tools, "", false)
			toggle.tooltip_text = "选中此记录"
			toggles.append(toggle)
			toggle.toggled.connect(func(value: bool) -> void:
				if value:
					selected[id] = true
				else:
					selected.erase(id)
				remove.disabled = selected.is_empty())
			button(tools, "详情", _clipboard_detail.bind(id))
			button(tools, "复制", func() -> void: invoke("copyClipboardItem", [id]), true)
			button(tools, "取消置顶" if item.pinned else "置顶", func() -> void:
				invoke("pinClipboardItem", [id, not item.pinned]))
		all.toggled.connect(func(value: bool) -> void:
			for toggle in toggles:
				toggle.button_pressed = value))

# 显示完整文本或图像、文件路径。
func _clipboard_detail(id: String) -> void:
	query("getClipboardItem", [id], func(item: Dictionary) -> void:
		var fields := dialog("剪贴板详情", Vector2i(680, 440))
		var image := first_text(item, ["image", "thumbnail"])
		if not image.is_empty():
			thumbnail(fields, image, 500, 245)
		var detail := TextEdit.new()
		detail.editable = false
		detail.wrap_mode = TextEdit.LINE_WRAPPING_BOUNDARY
		detail.custom_minimum_size.y = 110 if not image.is_empty() else 300
		detail.text = str(item.get("text", ""))
		fields.add_child(detail)
		if item.type == "files":
			var paths: Array = item.get("files", [])
			if not paths.is_empty():
				var selected := options(fields, paths.map(func(file: Variant) -> String:
					return str(file.get("path", file)) if file is Dictionary else str(file)))
				button(fields, "定位所选文件", func() -> void: invoke("openClipboardDirectory", [id, selected.selected])))

# 保持资源浏览器的图片和视频两张卡片。
func _open_browser() -> void:
	var layout := row(body, 12)
	for mode in ["image", "video"]:
		var is_image: bool = mode == "image"
		var card := panel(layout)
		card.get_parent().size_flags_horizontal = Control.SIZE_EXPAND_FILL
		label(card, "图像浏览器" if is_image else "视频播放器", 20)
		label(card, "等比查看、缩放与旋转" if is_image else "播放本地视频及同目录文件", 13, &"MutedLabel")
		button(card, "打开图片" if is_image else "打开视频", func() -> void:
			pick_file("选择媒体文件", IMAGES if is_image else VIDEOS, func(path: String) -> void:
				invoke("openImageBrowser" if is_image else "openVideoPlayer", [path])), true)
		button(card, "设置默认打开方式…", _media_defaults.bind(mode))
	button(body, "打开文件夹…", func() -> void:
		pick_file("选择资源文件夹", "", _media_folder, true))
	label(body, "文件夹内容", 19)
	results = column(body)
	empty(results, "还没有选择文件夹", "打开文件夹，查看其中的图片和视频。")

# 读取媒体列表，按系统关联或专用工具打开。
func _media_folder(path: String) -> void:
	query("listMediaDirectory", [path], func(data: Dictionary) -> void:
		_show_history(data, "目录内没有受支持的资源", func(item: Dictionary) -> void:
			var target: String = item.path
			var tools := record(results, first_text(item, ["name", "fileName"], target.get_file()), target)
			button(tools, "打开", func() -> void: invoke("openFilePath", [target]), true)
			button(tools, "定位", func() -> void: invoke("openFileLocation", [target]))))

# 提交所选扩展名，由 Windows 显示默认应用设置。
func _media_defaults(mode: String) -> void:
	query("getMediaAssociation", [], func(data: Dictionary) -> void:
		var extensions: Array = data.get(mode + "Extensions", [])
		var selected := {}
		var fields := dialog("选择文件类型", Vector2i(610, 310), func() -> void:
			invoke("configureMediaDefaults", [selected.keys()]))
		var choices := HFlowContainer.new()
		fields.add_child(choices)
		for extension in extensions:
			var toggle := check(choices, "." + extension, false)
			toggle.toggled.connect(func(active: bool) -> void:
				if active:
					selected[extension] = true
				else:
					selected.erase(extension)))

# 放置检索输入、索引状态和分页结果。
func _open_search() -> void:
	search_request = -1
	search_next = 0
	button(page_actions, "重建索引", func() -> void:
		confirm("重建索引", "将重新扫描本机文件，扫描期间可以继续操作工作台。", "rebuildFileSearchIndex"))
	button(page_actions, "启用高速索引", func() -> void:
		confirm("启用高速索引", "Windows 会要求确认安装索引服务。", "enableFileSearchService"), true)
	var tools := row(panel(body))
	search_input = line(tools, "搜索文件名，例如 年度总结*.docx、ext:pdf")
	search_in_path = check(tools, "匹配完整路径", false)
	button(tools, "检索", _search_files.bind(false), true)
	search_input.text_submitted.connect(func(_value: String) -> void: _search_files(false))
	search_input.text_changed.connect(_schedule_search)
	search_in_path.toggled.connect(func(_active: bool) -> void: _search_files(false))
	search_status = label(body, "正在读取索引状态…", 13, &"MutedLabel")
	results = column(body)
	empty(results, "输入关键词开始检索", "支持通配符、ext: 扩展名和 folder: / file: 类型筛选。")
	search_more = button(body, "加载更多", _search_files.bind(true))
	search_more.hide()
	_index_status()

# 等候输入稳定后再检索，避免每个按键都重新扫描。
func _schedule_search(value: String) -> void:
	var version := generation
	var serial := search_serial
	await get_tree().create_timer(0.24).timeout
	if version != generation or serial != search_serial or search_input.text != value:
		return
	if value.strip_edges().is_empty():
		search_request = -1
		search_more.hide()
		clear(results)
		empty(results, "输入关键词开始检索", "支持通配符、ext: 扩展名和 folder: / file: 类型筛选。")
	else:
		_search_files(false)

# 向 C++ 工作线程提交一次检索。
func _search_files(append: bool) -> void:
	var value := search_input.text.strip_edges()
	if value.is_empty():
		return
	if append and value != last_search:
		append = false
	last_search = value
	search_serial += 1
	var serial := search_serial
	search_more.hide()
	search_request = -1
	if not append:
		clear(results)
		empty(results, "正在检索…", value)
	query("searchFiles", [value, search_next if append else 0, 60, search_in_path.button_pressed],
		func(data: Dictionary) -> void:
			if serial == search_serial:
				search_request = int(data.requestId))

# 丢弃过期检索结果并渲染路径操作。
func _file_results(data: Dictionary) -> void:
	if page != "filesearch" or int(data.get("requestId", -2)) != search_request:
		return
	var offset := int(data.get("offset", 0))
	if offset == 0:
		clear(results)
	var items: Array = data.get("rows", [])
	if offset == 0 and items.is_empty():
		empty(results, "未找到匹配文件", "调整关键词后可以再试。")
	for item in items:
		var path: String = item.path
		var tools := record(results, first_text(item, ["name"], path.get_file()), path)
		button(tools, "打开", func() -> void: invoke("openFilePath", [path]), true)
		button(tools, "定位", func() -> void: invoke("openFileLocation", [path]))
		button(tools, "复制路径", func() -> void: DisplayServer.clipboard_set(path))
	search_next = offset + items.size()
	search_more.visible = not items.is_empty() and search_next < int(data.get("total", 0))
	message("找到 %d 条结果 · %.2f 秒" % [int(data.get("total", 0)), float(data.get("elapsedMs", 0)) / 1000.0])

# 更新真实索引规模和阶段。
func _index_status() -> void:
	if not service.online:
		return
	invoke("getFileSearchStatus", [], func(data: Dictionary) -> void:
		var count := int(data.get("total", 0))
		_set_count("filesearch", count)
		if page == "filesearch":
			var phase := first_text(data, ["phase", "status"])
			var stage: String = {"ready": "索引就绪", "building": "正在构建索引",
				"loading": "加载索引中", "scanning": "正在扫描"}.get(phase, "本机索引")
			search_status.text = "%s  ·  已收录 %d 条" % [stage, count])

# 创建设置分页，隐藏其他分类时保留各自的表单输入。
func _open_settings() -> void:
	var tabs := row(panel(body, &"InsetCard"))
	var group := ButtonGroup.new()
	var sections: Array[VBoxContainer] = []
	for index in range(SETTINGS_TABS.size()):
		var section := column(body, 13)
		section.visible = index == settings_tab
		sections.append(section)
		var tab := button(tabs, SETTINGS_TABS[index], _show_settings_tab.bind(sections, index))
		tab.theme_type_variation = &"SettingsTab"
		tab.toggle_mode = true
		tab.button_group = group
		tab.button_pressed = section.visible
		tab.custom_minimum_size.x = 100
	_settings_appearance(sections[0])
	_settings_about(sections[3])
	if not service.online:
		for index in [1, 2]:
			empty(sections[index], "等待本机服务", "连接 C++ 服务后即可修改此分类的设置。")
		return
	query("getSettings", [], func(data: Dictionary) -> void:
		_settings_font(sections[0], data)
		_settings_system(sections[1], data)
		_settings_downloads(sections[2], data))
	var actions := row(body)
	spacer(actions)
	button(actions, "恢复默认设置…", _confirm_settings_reset)

# 仅显示所选设置分页，不重新请求数据。
func _show_settings_tab(sections: Array[VBoxContainer], index: int) -> void:
	settings_tab = index
	for position in range(sections.size()):
		sections[position].visible = position == index
	scroll.scroll_vertical = 0

# 主题选项实时生效，保存后下次启动继续使用。
func _settings_appearance(section: VBoxContainer) -> void:
	var card := panel(section)
	label(card, "主题", 19)
	label(card, "选择适合当前环境的工作台配色。", 14, &"MutedLabel")
	var choices := row(card)
	var group := ButtonGroup.new()
	for id in [Style.DARK, Style.GOLD]:
		var title := "暗色" if id == Style.DARK else "金色亮色"
		var choice := button(choices, title, _choose_appearance.bind(id, group))
		choice.set_meta("appearance", id)
		choice.theme_type_variation = &"SettingsTab"
		choice.toggle_mode = true
		choice.button_group = group
		choice.button_pressed = id == appearance
		choice.custom_minimum_size = Vector2(125, 42)

# 保存主题，并在写入失败时恢复选择按钮的选中状态。
func _choose_appearance(id: String, group: ButtonGroup) -> void:
	if set_appearance(id):
		message("已切换为暗色主题。" if id == Style.DARK else "已切换为金色亮色主题。")
	else:
		for choice in group.get_buttons():
			if choice.get_meta("appearance") == appearance:
				choice.button_pressed = true

# 单独保存界面字号，避免提交其他分页的修改。
func _settings_font(section: VBoxContainer, data: Dictionary) -> void:
	var card := panel(section)
	label(card, "界面字号", 19)
	var controls := row(card)
	var sizes := range(8, 33)
	var font := options(controls, sizes, sizes.find(int(data.get("fontSize", 15))))
	button(controls, "保存字号", func() -> void:
		var selected: int = sizes[font.selected]
		invoke("saveSettings", [{"fontSize": selected}], func(_result: Variant) -> void:
			theme.default_font_size = selected
			message("界面字号已保存。")), true)

# 系统分页集中显示全局热键和 Windows 登录启动。
func _settings_system(section: VBoxContainer, data: Dictionary) -> void:
	var card := panel(section)
	label(card, "唤醒热键", 19)
	button(row(card), "设置唤醒热键", _hotkey_dialog.bind(data, false))
	var startup_card := panel(section)
	label(startup_card, "开机自启", 19)
	var startup := check(startup_card, "登录 Windows 时启动工作台", false)
	startup.disabled = true
	query("startup", [], func(state: Dictionary) -> void:
		startup.set_pressed_no_signal(bool(state.get("enabled", false)))
		startup.disabled = false)
	startup.toggled.connect(func(value: bool) -> void:
		startup.disabled = true
		service.request("startup", [value, _startup_command()], func(result: Dictionary) -> void:
			if is_instance_valid(startup):
				startup.disabled = false
				if not result.get("ok", false):
					startup.set_pressed_no_signal(not value)
			if result.get("ok", false):
				message("开机自启设置已保存。")
			else:
				message(str(result.get("error", "")), true)))

# 下载分页只修改默认下载目录。
func _settings_downloads(section: VBoxContainer, data: Dictionary) -> void:
	var card := panel(section)
	label(card, "默认下载目录", 19)
	label(card, "创建任务时可指定其他目录。", 14, &"MutedLabel")
	var field := row(card)
	var path := line(field, "默认下载目录", data.get("downloadDirectory", ""))
	button(field, "选择目录…", func() -> void:
		pick_file("选择默认下载目录", "", func(value: String) -> void: path.text = value, true))
	button(row(card), "保存目录", func() -> void:
		invoke("saveSettings", [{"downloadDirectory": path.text}], func(_result: Variant) -> void:
			message("默认下载目录已保存。")), true)

# 显示应用说明、当前数据路径及快捷入口。
func _settings_about(section: VBoxContainer) -> void:
	var card := panel(section)
	label(card, "关于 DesktopTool", 19)
	label(card, "Godot 原生工作台  /  Windows C++ 服务", 14, &"MutedLabel")
	var information := clipped(card, "等待本机服务提供数据目录。", 13)
	var links := row(card)
	button(links, "打开数据目录", func() -> void: invoke("openDataDirectory")).disabled = not service.online
	button(links, "项目仓库", func() -> void: invoke("openGithubRepository")).disabled = not service.online
	if service.online:
		query("getAppInfo", [], func(info: Dictionary) -> void:
			information.text = str(info.get("dataPath", "")))

# 通过确认对话框恢复后台和工作台的默认设置。
func _confirm_settings_reset() -> void:
	var fields := dialog("恢复默认设置", Vector2i(480, 150), func() -> void:
		invoke("resetSettings", [], func(data: Dictionary) -> void:
			theme.default_font_size = int(data.get("fontSize", 15))
			set_appearance(Style.DARK)
			invoke("startup", [false, ""], func(_state: Variant) -> void:
				if page == "settings":
					clear(body)
					_open_settings())))
	label(fields, "恢复默认主题、字号、热键和下载目录，并关闭开机自启。")

# 生成当前工程或导出程序的 Windows 自启命令。
func _startup_command() -> String:
	var command := '"%s"' % OS.get_executable_path()
	if OS.has_feature("editor"):
		command += ' --path "%s"' % ProjectSettings.globalize_path("res://").trim_suffix("/")
	if not service.data_directory.is_empty():
		command += ' -- "--data-dir=%s"' % service.data_directory
	return command
