extends "res://ui/catalog.gd"
## 截图、录屏、二维码和壁纸页面。

# 历史页中独立于列表刷新的功能设置容器。
var history_controls: VBoxContainer
# 截图开关同步控制的启动按钮。
var capture_action: Button
# 当前选中的壁纸显示器。
var screen_id := ""
# 二维码选图和识别结果区域。
var qr_preview: VBoxContainer
var qr_source := ""
var qr_payload := ""
var qr_result: TextEdit

# 组装截图和录屏的操作、设置与历史区域。
func open_capture() -> void:
	var is_video := page == "recording"
	var topic := page
	capture_action = button(page_actions, "●  开始录屏" if is_video else "＋  开始截图", func() -> void:
		invoke("startRecording" if is_video else "startScreenshot"), true)
	history_controls = panel(body)
	query("getSettings", [], _capture_options)
	var header := row(body)
	label(header, "录屏记录" if is_video else "截图记录", 19)
	spacer(header)
	button(header, "清空记录", func() -> void:
		confirm("清空记录", "这会删除全部历史及对应的磁盘文件。",
			"clearRecordingHistory" if topic == "recording" else "clearScreenshotHistory"))
	results = column(body, 7)
	refresh_capture()

# 显示截图热键或录屏画质、声音和鼠标指针设置。
func _capture_options(data: Dictionary) -> void:
	clear(history_controls)
	if page == "screenshot":
		var tools := row(history_controls)
		var active := check(tools, "启用截图", data.get("screenshotEnabled", true))
		capture_action.disabled = not active.button_pressed
		active.toggled.connect(func(value: bool) -> void:
			invoke("setScreenshotEnabled", [value], func(_data: Variant) -> void:
				if page == "screenshot":
					capture_action.disabled = not value))
		spacer(tools)
		button(tools, "设置截图热键", _hotkey_dialog.bind(data, true))
		label(history_controls, "拖拽框选，使用工具栏添加标注，按 Enter 完成。", 13, &"MutedLabel")
		return
	var video := row(history_controls)
	label(video, "录制分辨率", 14)
	var heights := [0, 720, 1080, 1440, 2160]
	var resolution := options(video, ["原始分辨率", "720p", "1080p", "1440p", "2160p"],
		heights.find(int(data.get("recordingResolution", 1080))))
	var cursor := check(video, "录制鼠标指针", data.get("recordingCursorEnabled", true))
	var audio := row(history_controls)
	var active := check(audio, "录制音频", data.get("recordingAudioEnabled", false))
	var microphone := check(audio, "麦克风", data.get("recordingAudioMicrophone", true))
	var system := check(audio, "系统声音", data.get("recordingAudioSystem", true))
	button(video, "保存录制选项", func() -> void:
		invoke("saveSettings", [{"recordingResolution": heights[resolution.selected],
			"recordingCursorEnabled": cursor.button_pressed, "recordingAudioEnabled": active.button_pressed,
			"recordingAudioMicrophone": microphone.button_pressed, "recordingAudioSystem": system.button_pressed}],
			func(_data: Variant) -> void: message("录制选项已保存。")))

# 从历史服务读取截图或录屏缩略图及操作。
func refresh_capture() -> void:
	var video := page == "recording"
	var suffix := "Recording" if video else "Screenshot"
	query("get" + suffix + "History", [], func(data: Dictionary) -> void:
		_show_history(data, "还没有" + ("录屏" if video else "截图"), func(item: Dictionary) -> void:
			var id: String = item.id
			var time := first_text(item, ["recordedAt", "capturedAt"]).replace("T", " ").left(19)
			var details := "%d × %d  ·  %s" % [int(item.get("width", 0)), int(item.get("height", 0)),
				String.humanize_size(int(item.get("size", item.get("bytes", 0))))]
			if video:
				details += "  ·  %.2f 秒" % (float(item.get("durationMs", 0)) / 1000.0)
			var tools := record(results, time, details, item.get("thumbnail", ""))
			button(tools, "播放" if video else "查看", func() -> void:
				if video:
					invoke("openRecordingPlayer", [id])
				else:
					query("getScreenshot", [id], func(image: Dictionary) -> void:
						var content := dialog("截图预览", Vector2i(780, 550))
						thumbnail(content, image.get("image", ""), 720, 460)), true)
			button(tools, "复制", func() -> void: invoke("copy" + suffix, [id]))
			button(tools, "定位", func() -> void: invoke("open" + suffix + "Directory", [id]))
			button(tools, "删除", func() -> void: confirm("删除记录", "记录和对应文件将被删除。",
				"delete" + suffix + "s", [[id]]))))

# 处理以列表返回的历史记录并更新侧栏数量。
func _show_history(data: Dictionary, title: String, draw: Callable) -> void:
	clear(results)
	var items: Array = data.get("items", [])
	_set_count(page, int(data.get("total", items.size())))
	if items.is_empty():
		empty(results, title, "完成操作后，记录会显示在这里。")
	for item in items:
		draw.call(item)

# 接受字母、数字和 F1 至 F12，保存 Windows 全局热键。
func _hotkey_dialog(data: Dictionary, screenshot: bool) -> void:
	var controls := {}
	var prefix := "screenshotHotkey" if screenshot else "hotkey"
	var fields := dialog("截图热键" if screenshot else "工作台唤醒热键", Vector2i(465, 200), func() -> void:
		var key := str(controls.key.text).strip_edges().to_upper()
		var vk := 0
		if key.length() == 1 and (("A" <= key and key <= "Z") or ("0" <= key and key <= "9")):
			vk = key.unicode_at(0)
		elif key.begins_with("F") and key.substr(1).is_valid_int():
			var number := key.substr(1).to_int()
			if number in range(1, 13):
				vk = 111 + number
		if vk == 0:
			message("请输入 A–Z、0–9 或 F1–F12。", true)
			return
		var flags := 0
		for index in range(4):
			if controls.flags[index].button_pressed:
				flags |= 1 << index
		if screenshot:
			invoke("saveScreenshotHotkey", [flags, vk], func(_data: Variant) -> void: message("截图热键已保存。"))
		else:
			invoke("saveSettings", [{"hotkeyModifier": flags, "hotkeyKey": vk}],
				func(_data: Variant) -> void: message("唤醒热键已保存。")))
	var key_code := int(data.get(prefix + "Key", 119))
	var initial := "F%d" % (key_code - 111) if key_code in range(112, 124) else String.chr(key_code)
	var modifiers := int(data.get(prefix + "Modifier", 0))
	var keys := row(fields)
	controls.flags = []
	for index in range(4):
		controls.flags.append(check(keys, ["Alt", "Ctrl", "Shift", "Win"][index], bool(modifiers & (1 << index))))
	controls.key = line(fields, "A–Z、0–9 或 F1–F12", initial)

# 创建二维码的历史栏和原生预览栏。
func open_qr() -> void:
	qr_source = ""
	qr_payload = ""
	button(page_actions, "粘贴图片", func() -> void:
		query("pasteQrImage", [], func(data: Dictionary) -> void:
			qr_payload = str(data.get("image", ""))
			qr_source = first_text(data, ["source"], "剪贴板")
			_show_qr(qr_payload, "")))
	button(page_actions, "＋ 打开图片", func() -> void: pick_file("选择二维码图片", IMAGES, func(path: String) -> void:
		query("loadQrImage", [path], func(data: Dictionary) -> void:
			qr_payload = data.image
			qr_source = data.source
			_show_qr(qr_payload, ""))), true)
	var layout := row(body, 12)
	var history := panel(layout, &"Record")
	history.get_parent().custom_minimum_size.x = 325
	var heading := row(history)
	label(heading, "识别历史", 17)
	spacer(heading)
	button(heading, "清空", func() -> void:
		confirm("清空二维码历史", "将删除所有识别记录。", "clearQrHistory"))
	results = column(history)
	var card := panel(layout)
	card.get_parent().size_flags_horizontal = Control.SIZE_EXPAND_FILL
	qr_preview = column(card)
	empty(qr_preview, "请选择一张二维码图片", "可打开本地图片，也可从剪贴板粘贴。")
	var actions := row(card)
	button(actions, "识别", func() -> void:
		if qr_payload.is_empty():
			message("请先打开或粘贴图片。", true)
			return
		query("decodeQr", [qr_payload, qr_source], func(data: Dictionary) -> void:
			_show_qr(data.get("image", qr_payload), str(data.get("text", "")))), true)
	button(actions, "复制结果", func() -> void:
		if not qr_result.text.is_empty():
			DisplayServer.clipboard_set(qr_result.text))
	qr_result = TextEdit.new()
	qr_result.editable = false
	qr_result.placeholder_text = "识别结果"
	qr_result.custom_minimum_size.y = 90
	qr_result.wrap_mode = TextEdit.LINE_WRAPPING_BOUNDARY
	card.add_child(qr_result)
	refresh_qr()

# 显示当前二维码和识别文本。
func _show_qr(image: String, value: String) -> void:
	clear(qr_preview)
	thumbnail(qr_preview, image, 240, 220)
	qr_result.text = value

# 读取识别历史并支持回看及删除。
func refresh_qr() -> void:
	query("getQrHistory", [], func(data: Dictionary) -> void:
		_show_history(data, "还没有记录", func(item: Dictionary) -> void:
			var id: String = item.id
			var actions := record(results, str(item.get("preview", "二维码")), "", item.get("thumbnail", ""))
			button(actions, "查看", func() -> void:
				query("readQr", [id], func(entry: Dictionary) -> void:
					qr_payload = str(entry.get("image", ""))
					qr_source = str(entry.get("source", ""))
					_show_qr(qr_payload, str(entry.get("text", "")))))
			button(actions, "删除", func() -> void:
				confirm("删除识别记录", "将移除此识别记录。", "removeQrHistory", [[id]]))))

# 组装壁纸选择、显示器控制和历史区。
func open_wallpaper() -> void:
	screen_id = ""
	history_controls = panel(body)
	var tools := row(body)
	label(tools, "壁纸历史", 19)
	spacer(tools)
	button(tools, "从剪贴板添加", func() -> void:
		var id := screen_id
		invoke("pasteWallpaperResource", [], func(data: Dictionary) -> void:
			_add_wallpaper(str(data.get("target", "")), id)))
	button(tools, "＋ 选择资源", func() -> void:
		var id := screen_id
		pick_file("选择壁纸", "*.png, *.jpg, *.jpeg, *.gif, *.webp, *.bmp, *.mp4, *.webm, *.mkv, *.mov; 图片和视频",
			func(path: String) -> void: _add_wallpaper(path, id)), true)
	results = column(body)
	refresh_wallpaper()

# 添加文件，并在选中的显示器上应用。
func _add_wallpaper(path: String, id: String) -> void:
	var value := {"path": path}
	if not id.is_empty():
		value.screenId = id
	invoke("addWallpaper", [value])

# 更新壁纸开关、显示器设置和历史项目。
func refresh_wallpaper() -> void:
	query("getWallpaper", [], _draw_wallpaper)

# 保留显示器选择，并同步当前壁纸和显示模式。
func _draw_wallpaper(data: Dictionary) -> void:
	clear(history_controls)
	clear(page_actions)
	var enabled := check(page_actions, "开启动态壁纸", data.get("enabled", false))
	enabled.toggled.connect(func(value: bool) -> void: invoke("setWallpaperEnabled", [value]))
	label(history_controls, "选择显示器", 18)
	var screens: Array = data.get("screens", [])
	var ids: Array = []
	var names: Array = []
	for screen in screens:
		ids.append(screen.id)
		names.append(first_text(screen, ["name", "id"]))
	if not ids.is_empty():
		if screen_id not in ids:
			screen_id = ids[0]
		var layout := row(history_controls)
		var selected := options(layout, names, ids.find(screen_id))
		selected.item_selected.connect(func(index: int) -> void:
			screen_id = ids[index]
			_draw_wallpaper(data))
		var modes := ["fill", "fit", "stretch", "tile", "center"]
		var info: Dictionary = screens[ids.find(screen_id)]
		var modes_ui := options(layout, ["填充", "适应", "拉伸", "平铺", "居中"], modes.find(info.displayMode))
		modes_ui.item_selected.connect(func(index: int) -> void:
			invoke("setWallpaperDisplayMode", [screen_id, modes[index]]))
		button(layout, "此屏恢复系统壁纸", func() -> void: invoke("clearScreenWallpaper", [screen_id]))
		if not str(info.get("playbackError", "")).is_empty():
			clipped(history_controls, info.playbackError, 13, &"ErrorLabel")
	else:
		label(history_controls, "本机服务尚未报告可用显示器。", 13, &"MutedLabel")
	_show_history(data, "还没有壁纸", func(item: Dictionary) -> void:
		var id: String = item.id
		var actions := record(results, item.name, item.path, item.get("thumbnail", ""))
		button(actions, "应用", func() -> void:
			if screen_id.is_empty():
				invoke("useWallpaper", [id])
			else:
				invoke("useWallpaperOnScreen", [id, screen_id]), true)
		button(actions, "定位", func() -> void: invoke("openWallpaperDirectory", [id]))
		button(actions, "删除", func() -> void:
			confirm("删除壁纸记录", "将从壁纸历史移除此项。", "removeWallpapers", [[id]])))
