extends RefCounted
## 工作台调色、原生控件主题与侧栏线性图标。

# 可持久化的主题标识。
const DARK := "dark"
const GOLD := "gold"

# 颜色按用途定义，页面控件共享同一套色板。
const PALETTES := {
	DARK: {
		"ink": Color("#eaf0ff"), "muted": Color("#9aaac7"), "accent": Color("#6de8f4"),
		"tag": Color("#a597ff"), "online": Color("#57e8a9"), "error": Color("#ff8da7"),
		"background": Color("#0b0e17"), "sidebar": Color("#0d1527"), "bar": Color("#0e1729"),
		"card": Color("#121c30"), "tile": Color("#131f36"), "record": Color("#111b2e"),
		"empty": Color("#10192b"), "border": Color("#283550"), "field": Color("#0c1425"),
		"control": Color("#17233a"), "hover": Color("#243458"), "hover_edge": Color("#5368a8"),
		"selection": Color("#242952"), "selection_edge": Color("#5756a0"),
		"badge": Color("#20294a"), "mark": Color("#283060"), "mark_edge": Color("#52609b"),
		"primary": Color("#4a50d4"), "primary_hover": Color("#646af2"), "primary_ink": Color("#ffffff"),
		"primary_edge": Color("#8388ff"), "scrollbar": Color("#364666"),
		"dialog_header": Color("#202b46"), "dialog_shadow": Color("#00000070"),
	},
	GOLD: {
		"ink": Color("#352817"), "muted": Color("#6d5b45"), "accent": Color("#795015"),
		"tag": Color("#83550e"), "online": Color("#28633f"), "error": Color("#aa3450"),
		"background": Color("#f7f1e6"), "sidebar": Color("#fff9eb"), "bar": Color("#fffaf0"),
		"card": Color("#fffdf7"), "tile": Color("#fff8e9"), "record": Color("#fffaf0"),
		"empty": Color("#fcf5e7"), "border": Color("#ded0b3"), "field": Color("#fffdf8"),
		"control": Color("#f7edd9"), "hover": Color("#f1dfb6"), "hover_edge": Color("#b9924e"),
		"selection": Color("#f5e2b4"), "selection_edge": Color("#ae7c27"),
		"badge": Color("#f2e4c4"), "mark": Color("#f5e3ba"), "mark_edge": Color("#c39a50"),
		"primary": Color("#dcad52"), "primary_hover": Color("#ebbe66"), "primary_ink": Color("#342207"),
		"primary_edge": Color("#ad7a24"), "scrollbar": Color("#baa178"),
		"dialog_header": Color("#f4e5c2"), "dialog_shadow": Color("#543b1940"),
	},
}

# 文本语义与主题项的对应关系。
const LABEL_COLORS := {
	"MutedLabel": "muted", "AccentLabel": "accent", "TagLabel": "tag",
	"OnlineLabel": "online", "ErrorLabel": "error", "CountLabel": "tag",
}

# 面板变体定义背景用途、圆角和内边距。
const PANELS := {
	"Card": ["card", 11, 16, 16], "InsetCard": ["record", 11, 10, 10],
	"Tile": ["tile", 11, 12, 12], "Record": ["record", 11, 11, 11],
	"EmptyCard": ["empty", 11, 26, 26], "TopBar": ["bar", 11, 14, 14],
	"StatusBar": ["bar", 11, 10, 10], "SidebarFooter": ["card", 11, 11, 11],
	"Sidebar": ["sidebar", 0, 14, 16], "BrandMark": ["mark", 10, 8, 8],
	"HeroMark": ["mark", 14, 15, 15],
}

# 侧栏图标使用同一 24 像素线框基准。
const PATHS := {
	"shortcuts": "M14 3h7v7m0-7L10 14m10-2v8H4V4h8",
	"notes": "M5 3h9l5 5v13H5zM14 3v6h5M8 13h8m-8 4h5",
	"emoji": "M22 12a10 10 0 1 1-20 0 10 10 0 1 1 20 0ZM8 9h.01M16 9h.01M7 14q5 6 10 0",
	"downloads": "M12 3v12m-5-5 5 5 5-5M4 16v4h16v-4",
	"clipboard": "M8 5H5v16h14V5h-3M8 3h8v4H8zM8 11h8m-8 4h6",
	"wallpaper": "M3 4h18v16H3zM4 17l5-5 4 4 3-3 5 5m-5-9h.01",
	"qr": "M3 3h6v6H3zM15 3h6v6h-6zM3 15h6v6H3zM13 13h3v3h5v5h-4m-4 0v-3m8-5h.01",
	"screenshot": "M8 3H3v5m13-5h5v5M3 16v5h5m13-5v5h-5M7 7h10v10H7z",
	"recording": "M3 6h12v12H3zM15 10l6-4v12l-6-4M7 10h.01",
	"browser": "M3 6h7l2 2h9v12H3zM3 6V4h7l2 2h8v2",
	"filesearch": "M10 3H4v18h7M10 3v6h5m-5-6 5 6m4 9 3 3m-2-7a4 4 0 1 1-8 0 4 4 0 1 1 8 0Z",
	"settings": "M3 6h18M8 3v6M3 12h18m-4-3v6M3 18h18M8 15v6",
	"brand": "M12 2l9 5v10l-9 5-9-5V7zm-9 5 9 5 9-5m-9 5v10m-4-14 4-2 4 2",
	"search": "M16 16l5 5m-4-11a7 7 0 1 1-14 0 7 7 0 1 1 14 0Z"
}

# 对未知的持久化主题回退到默认暗色。
static func palette(id: String) -> Dictionary:
	return PALETTES.get(id, PALETTES[DARK])

# 生成可用于所有原生控件的主题，主题变更时保留当前字号。
static func create_theme(id: String, font_size: int = 15) -> Theme:
	var colors := palette(id)
	var result := Theme.new()
	var font := SystemFont.new()
	font.font_names = PackedStringArray(["Microsoft YaHei UI", "Microsoft YaHei", "Noto Sans CJK SC"])
	result.default_font = font
	result.default_font_size = font_size
	result.set_color("font_color", "Label", colors.ink)
	for type in LABEL_COLORS:
		result.set_type_variation(type, "Label")
		result.set_color("font_color", type, colors[LABEL_COLORS[type]])
	result.set_stylebox("normal", "CountLabel", box(colors.badge, Color.TRANSPARENT, 6, 6, 3))
	result.set_stylebox("panel", "PanelContainer", box(colors.card, colors.border, 12))
	for type in PANELS:
		var spec: Array = PANELS[type]
		result.set_type_variation(type, "PanelContainer")
		var edge: Color = colors.mark_edge if spec[0] == "mark" else colors.border
		result.set_stylebox("panel", type, box(colors[spec[0]], edge, spec[1], spec[2], spec[3]))
	_button_styles(result, colors)
	for type in ["LineEdit", "TextEdit", "SpinBox", "Tree", "ItemList"]:
		result.set_stylebox("normal", type, box(colors.field, colors.border, 8, 12, 9))
		result.set_stylebox("read_only", type, box(colors.empty, colors.border, 8, 12, 9))
		result.set_stylebox("focus", type, box(Color.TRANSPARENT, colors.selection_edge, 8))
		result.set_color("font_color", type, colors.ink)
		result.set_color("font_uneditable_color", type, colors.muted)
		result.set_color("font_readonly_color", type, colors.ink)
		result.set_color("font_placeholder_color", type, colors.muted)
		result.set_color("caret_color", type, colors.ink)
		result.set_color("font_selected_color", type, colors.ink)
		result.set_color("selection_color", type, colors.hover)
	for type in ["Tree", "ItemList"]:
		result.set_stylebox("panel", type, box(colors.field, colors.border, 8, 9, 7))
		result.set_stylebox("hovered", type, box(colors.hover, Color.TRANSPARENT, 5))
		for state in ["selected", "selected_focus"]:
			result.set_stylebox(state, type, box(colors.selection, colors.selection_edge, 5))
		result.set_color("font_hovered_color", type, colors.ink)
		result.set_color("font_selected_color", type, colors.ink)
	_dialog_styles(result, colors, font)
	result.set_color("default_color", "RichTextLabel", colors.ink)
	result.set_color("font_selected_color", "RichTextLabel", colors.ink)
	result.set_color("selection_color", "RichTextLabel", colors.hover)
	result.set_stylebox("background", "ProgressBar", box(colors.field, colors.border, 3))
	result.set_stylebox("fill", "ProgressBar", box(colors.primary_hover, colors.primary_edge, 3))
	for type in ["VScrollBar", "HScrollBar"]:
		result.set_stylebox("scroll", type, box(Color.TRANSPARENT, Color.TRANSPARENT, 3, 4, 2))
		for state in ["grabber", "grabber_highlight", "grabber_pressed"]:
			result.set_stylebox(state, type, box(colors.scrollbar, Color.TRANSPARENT, 3, 4, 2))
	return result

# 为应用内窗口、文件选择器、菜单和悬浮提示应用同一套配色。
static func _dialog_styles(result: Theme, colors: Dictionary, font: Font) -> void:
	var title_height := 40
	for focused in [true, false]:
		var fill: Color = colors.dialog_header if focused else colors.bar
		var edge: Color = colors.mark_edge if focused else colors.border
		var frame := box(fill, edge, 12, 14, 0)
		frame.expand_margin_top = title_height
		frame.expand_margin_left = 1
		frame.expand_margin_right = 1
		frame.expand_margin_bottom = 1
		frame.corner_radius_bottom_left = 5
		frame.corner_radius_bottom_right = 5
		frame.shadow_color = colors.dialog_shadow
		frame.shadow_size = 14 if focused else 8
		frame.shadow_offset = Vector2(0, 5)
		result.set_stylebox("embedded_border" if focused else "embedded_unfocused_border", "Window", frame)
	result.set_font("title_font", "Window", font)
	result.set_color("title_color", "Window", colors.ink)
	result.set_constant("title_height", "Window", title_height)
	result.set_constant("close_h_offset", "Window", 29)
	result.set_constant("close_v_offset", "Window", 28)
	result.set_icon("close", "Window", _glyph("M6 6l12 12M18 6L6 18", colors.muted, 16))
	result.set_icon("close_pressed", "Window", _glyph("M6 6l12 12M18 6L6 18", colors.error, 16))

	var content := box(colors.card, colors.border, 0, 14, 12)
	content.set_border_width_all(0)
	content.border_width_top = 1
	result.set_stylebox("panel", "AcceptDialog", content)
	result.set_constant("buttons_min_width", "AcceptDialog", 80)
	result.set_constant("buttons_min_height", "AcceptDialog", 36)
	result.set_constant("buttons_separation", "AcceptDialog", 12)
	result.set_color("folder_icon_color", "FileDialog", colors.accent)
	result.set_color("file_icon_color", "FileDialog", colors.muted)
	result.set_color("file_disabled_color", "FileDialog", colors.muted)
	for type in ["PopupMenu", "TooltipPanel"]:
		result.set_stylebox("panel", type, box(colors.card, colors.mark_edge, 8, 9, 6))
	result.set_stylebox("hover", "PopupMenu", box(colors.hover, Color.TRANSPARENT, 5))
	result.set_color("font_color", "PopupMenu", colors.ink)
	result.set_color("font_hover_color", "PopupMenu", colors.ink)
	result.set_color("font_disabled_color", "PopupMenu", colors.muted)
	result.set_color("font_color", "TooltipLabel", colors.ink)
	result.set_icon("submenu", "PopupMenu", _glyph("M9 6l6 6-6 6", colors.muted, 16))
	result.set_icon("submenu_mirrored", "PopupMenu", _glyph("M15 6l-6 6 6 6", colors.muted, 16))

# 创建普通、强调、分页和侧栏按钮的交互样式。
static func _button_styles(result: Theme, colors: Dictionary) -> void:
	for type in ["PrimaryButton", "SettingsTab", "NavigationButton", "ActiveNavigationButton"]:
		result.set_type_variation(type, "Button")
	for type in ["Button", "OptionButton", "CheckButton", "CheckBox", "PrimaryButton",
		"SettingsTab", "NavigationButton", "ActiveNavigationButton"]:
		for state in ["normal", "hover", "pressed", "hover_pressed", "disabled", "focus"]:
			var fill: Color = colors.control if state in ["normal", "disabled"] else colors.hover
			var edge: Color = colors.border if state in ["normal", "disabled"] else colors.hover_edge
			var ink: Color = colors.muted if state == "disabled" else colors.ink
			if type == "PrimaryButton" and state != "disabled":
				fill = colors.primary if state == "normal" else colors.primary_hover
				edge = colors.primary_edge
				ink = colors.primary_ink
			if type == "SettingsTab" and state in ["pressed", "hover_pressed"]:
				fill = colors.selection
				edge = colors.selection_edge
			if type == "NavigationButton" and state == "normal":
				fill = Color.TRANSPARENT
				edge = Color.TRANSPARENT
			if type == "ActiveNavigationButton" and state != "focus":
				fill = colors.selection
				edge = colors.selection_edge
			if state == "focus":
				fill = Color.TRANSPARENT
				edge = colors.accent
			var skin := box(fill, edge, 8, 13, 8)
			if type == "ActiveNavigationButton" and state != "focus":
				skin.border_width_left = 3
			result.set_stylebox(state, type, skin)
			result.set_color("font_color" if state == "normal" else "font_%s_color" % state, type, ink)
			result.set_color("icon_%s_color" % state, type, ink)
	result.set_icon("arrow", "OptionButton", _glyph("M6 9l6 6 6-6", colors.muted, 16))
	result.set_icon("unchecked", "CheckBox", _glyph("M5 5h14v14H5z", colors.muted, 19))
	result.set_icon("checked", "CheckBox", _glyph("M5 5h14v14H5zM8 12l3 3 5-6", colors.accent, 19))

# 构造面板边框和内边距。
static func box(fill: Color, edge: Color, radius: int, x: int = 0, y: int = 0) -> StyleBoxFlat:
	var skin := StyleBoxFlat.new()
	skin.bg_color = fill
	skin.border_color = edge
	skin.set_border_width_all(1 if edge.a > 0.0 else 0)
	skin.set_corner_radius_all(radius)
	skin.content_margin_left = x
	skin.content_margin_right = x
	skin.content_margin_top = y
	skin.content_margin_bottom = y
	return skin

# 将矢量轮廓转为高分屏图标纹理。
static func icon(key: String) -> ImageTexture:
	return _glyph(PATHS.get(key, PATHS.brand), Color.WHITE, 48)

# 将图标路径栅格化为 Godot 可用的纹理。
static func _glyph(path: String, tint: Color, pixels: int) -> ImageTexture:
	var image := Image.new()
	var svg := '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 24 24"><path d="%s" fill="none" stroke="#%s" stroke-width="1.65" stroke-linecap="round" stroke-linejoin="round"/></svg>'
	image.load_svg_from_string(svg % [pixels, pixels, path, tint.to_html(false)])
	return ImageTexture.create_from_image(image)
