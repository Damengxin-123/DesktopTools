extends Node
## Godot 与 Windows C++ 工作进程之间的非阻塞请求客户端。

signal connection_changed(online: bool, detail: String)
signal backend_event(topic: String, data: Dictionary)

# 仅绑定回环地址的临时监听端口。
var listener := TCPServer.new()
# 当前候选连接或已验证的服务连接。
var peer: StreamPeerTCP
# 启动时生成的随机会话令牌。
var token := ""
# 部分到达的 JSON 帧。
var buffer := PackedByteArray()
# 还未写入套接字的请求及队首偏移。
var outgoing: Array[PackedByteArray] = []
var write_offset := 0
# 正在等候返回的请求和过期时间。
var pending := {}
# 单次会话内递增的请求标识。
var sequence := 0
# 后台进程是否已经通过握手。
var online := false
# 工作进程连接的截止时间。
var deadline := 0
# 本次会话使用的独立用户数据目录。
var data_directory := ""

# 启动后台进程，支持显式指定运行时与数据目录。
func start() -> void:
	if online or deadline > 0:
		return
	var arguments := OS.get_cmdline_user_args()
	var runtime := ProjectSettings.globalize_path("res://native/runtime/DesktopToolNative.exe")
	if not OS.has_feature("editor"):
		runtime = OS.get_executable_path().get_base_dir().path_join("native/DesktopToolNative.exe")
	data_directory = ProjectSettings.globalize_path("user://data")
	for argument in arguments:
		if argument.begins_with("--backend="):
			runtime = argument.trim_prefix("--backend=")
		if argument.begins_with("--data-dir="):
			data_directory = argument.trim_prefix("--data-dir=")
	if not FileAccess.file_exists(runtime):
		connection_changed.emit(false, "找不到 C++ 运行时，请运行启动脚本构建后端。")
		return
	if listener.listen(0, "127.0.0.1") != OK:
		connection_changed.emit(false, "无法启动本机服务连接。")
		return
	token = Crypto.new().generate_random_bytes(24).hex_encode()
	var process_args := PackedStringArray([str(listener.get_local_port()), token, data_directory])
	if "--isolated" in arguments:
		process_args.append("--isolated")
	if OS.create_process(runtime, process_args) < 0:
		_disconnect("无法启动 C++ 后台进程。")
		return
	deadline = Time.get_ticks_msec() + 15000

# 发送请求并用回调交付成功数据或中文错误。
func request(method: String, arguments: Array, callback: Callable) -> void:
	if not online:
		callback.call({"ok": false, "error": "本机服务未连接。"})
		return
	sequence += 1
	var packet := JSON.stringify({"id": sequence, "method": method, "args": arguments}) + "\n"
	pending[sequence] = [callback, Time.get_ticks_msec() + 60000]
	outgoing.append(packet.to_utf8_buffer())

# 每帧收取有限数据，后台进行索引和编码时界面仍可响应。
func _process(_delta: float) -> void:
	var now := Time.get_ticks_msec()
	if deadline > 0 and now >= deadline:
		_disconnect("本机服务启动超时，请检查 Windows 运行库。")
		return
	for id in pending.keys():
		if pending[id][1] <= now:
			var callback: Callable = pending[id][0]
			pending.erase(id)
			callback.call({"ok": false, "error": "后台操作超时。"})
	if peer == null and listener.is_listening() and listener.is_connection_available():
		peer = listener.take_connection()
	if peer == null:
		return
	peer.poll()
	if online and not outgoing.is_empty():
		var packet := outgoing[0]
		var sent := peer.put_partial_data(packet.slice(write_offset, mini(write_offset + 262144, packet.size())))
		if sent[0] != OK:
			_disconnect("本机服务连接已断开。")
			return
		write_offset += int(sent[1])
		if write_offset == packet.size():
			outgoing.pop_front()
			write_offset = 0
	var available := peer.get_available_bytes()
	if available > 0:
		var packet := peer.get_partial_data(mini(available, 2 * 1024 * 1024))
		if packet[0] == OK:
			_receive(packet[1])
	if peer != null and peer.get_status() != StreamPeerTCP.STATUS_CONNECTED:
		_disconnect("本机服务连接已关闭。")

# 保留跨帧数据，收到换行符后才解码 UTF-8 和 JSON。
func _receive(chunk: PackedByteArray) -> void:
	var start := 0
	var stop := chunk.find(10)
	while stop >= 0:
		buffer.append_array(chunk.slice(start, stop))
		var message: Variant = JSON.parse_string(buffer.get_string_from_utf8())
		buffer.clear()
		if message is Dictionary:
			_handle(message)
			if peer == null:
				return
		start = stop + 1
		stop = chunk.find(10, start)
	buffer.append_array(chunk.slice(start))
	if buffer.size() > 160 * 1024 * 1024:
		_disconnect("后台响应超出允许大小。")

# 校验握手并分发业务结果和进度通知。
func _handle(message: Dictionary) -> void:
	if not online:
		if message.get("token") != token:
			peer.disconnect_from_host()
			peer = null
			return
		if not str(message.get("error", "")).is_empty():
			_disconnect(message.error)
			return
		listener.stop()
		deadline = 0
		online = true
		connection_changed.emit(true, "C++ 服务已连接")
		return
	if message.has("event"):
		backend_event.emit(str(message.event), message.get("data", {}))
		return
	var id := int(message.get("id", 0))
	if pending.has(id):
		var callback: Callable = pending[id][0]
		pending.erase(id)
		callback.call(message.get("result", {"ok": false, "error": "后台响应无效。"}))

# 清理连接，并让等待中的界面操作收到可处理的失败结果。
func _disconnect(detail: String) -> void:
	if peer != null:
		peer.disconnect_from_host()
	peer = null
	listener.stop()
	deadline = 0
	online = false
	buffer.clear()
	outgoing.clear()
	write_offset = 0
	var previous := pending
	pending = {}
	connection_changed.emit(false, detail)
	for value in previous.values():
		value[0].call({"ok": false, "error": detail})
