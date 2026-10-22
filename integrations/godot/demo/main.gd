extends Control
## JSUI GDExtension demo.
##
## User arguments (after `--` on the Godot command line):
##   --dev <url>         load from a dev server with live reload, e.g.
##                       http://127.0.0.1:5173/bundle.js (`npx jsui dev ...`)
##   --bundle <url>      load another bundle (res://, user://, path or http://)
##   --capture <png>     render, save a screenshot and quit (CI / headless)
##   --frames <n>        frames to run before --capture (default 120)
##   --self-test         with --capture: press Enter on the focused "Ping Godot"
##                       button and click an empty spot through Godot's input
##                       pipeline; fail unless the ping/pong round trip happened

@onready var ui = $JsuiView

var _capture := ""
var _frames := 120
var _frame := 0
var _stats_timer := 0.0
var _self_test := false
var _pongs := 0
var _unhandled_clicks := 0


func _enter_tree() -> void:
	# Runs before JsuiView's _ready, so the view starts with these settings.
	var view = get_node("JsuiView")
	var args := OS.get_cmdline_user_args()
	var i := 0
	while i < args.size():
		var a: String = args[i]
		var v: String = args[i + 1] if i + 1 < args.size() else ""
		match a:
			"--dev":
				view.bundle_url = v
				view.dev_mode = true
				i += 1
			"--bundle":
				view.bundle_url = v
				i += 1
			"--capture":
				_capture = v
				i += 1
			"--frames":
				_frames = int(v)
				i += 1
			"--self-test":
				_self_test = true
		i += 1


func _ready() -> void:
	ui.message_received.connect(_on_ui_message)
	ui.js_error.connect(func(msg): printerr("[jsui error] ", msg))
	ui.grab_focus()  # keyboard / gamepad go to the UI
	print("JsuiView running=%s bundle=%s dev=%s" % [ui.is_running(), ui.bundle_url, ui.dev_mode])


func _on_ui_message(name: String, data, json: String) -> void:
	print("[ui -> godot] ", name, " ", json)
	match name:
		"ui:ready":
			_send_stats()
		"ui:ping":
			_pongs += 1
			ui.post_message("godot:pong", {"count": data.count, "time": Time.get_ticks_msec() / 1000.0})


func _send_stats() -> void:
	ui.post_message("godot:stats", {
		"fps": Engine.get_frames_per_second(),
		"frame": Engine.get_process_frames(),
		"version": "%d.%d.%d" % [Engine.get_version_info().major, Engine.get_version_info().minor, Engine.get_version_info().patch],
	})


func _process(delta: float) -> void:
	_stats_timer -= delta
	if _stats_timer <= 0.0:
		_stats_timer = 1.0
		_send_stats()
	if _capture != "":
		_frame += 1
		if _self_test and _frame == _frames / 2:
			_inject_input()
		if _frame >= _frames and ui.get_pending_loads() == 0:
			_save_capture()


func _inject_input() -> void:
	# Through Input -> Viewport -> JsuiView._gui_input, like real devices.
	for pressed in [true, false]:
		var key := InputEventKey.new()
		key.keycode = KEY_ENTER
		key.physical_keycode = KEY_ENTER
		key.pressed = pressed
		Input.parse_input_event(key)
	for pressed in [true, false]:
		var click := InputEventMouseButton.new()
		click.button_index = MOUSE_BUTTON_LEFT
		click.position = Vector2(700, 640)  # open sky: nothing handles it
		click.global_position = click.position
		click.pressed = pressed
		Input.parse_input_event(click)


func _save_capture() -> void:
	var path := _capture
	_capture = ""
	var img: Image = ui.get_frame_image()
	if img == null:
		printerr("capture: no frame rendered (error: %s)" % ui.get_last_error())
		get_tree().quit(1)
		return
	img.save_png(path)
	print("capture: JsuiView frame %dx%d (pixel ratio %.2f) -> %s" % [img.get_width(), img.get_height(), ui.get_effective_pixel_ratio(), path])
	# With a real renderer (not --headless) also grab the composited window,
	# which shows the texture as Godot drew it.
	if DisplayServer.get_name() != "headless":
		await RenderingServer.frame_post_draw
		var shot := get_viewport().get_texture().get_image()
		if shot:
			var vpath := path.get_basename() + "-viewport.png"
			shot.save_png(vpath)
			print("capture: viewport %dx%d -> %s" % [shot.get_width(), shot.get_height(), vpath])
	var ok: bool = ui.get_last_error() == ""
	if _self_test:
		print("self-test: pongs=%d unhandled_clicks=%d" % [_pongs, _unhandled_clicks])
		if _pongs < 1 or _unhandled_clicks < 1:
			printerr("self-test failed")
			ok = false
	get_tree().quit(0 if ok else 1)


func _unhandled_input(event: InputEvent) -> void:
	# Input the UI did not consume reaches the game.
	if event is InputEventMouseButton and event.pressed:
		_unhandled_clicks += 1
		print("[game] unhandled click at ", event.position)
	elif event is InputEventKey and event.pressed and event.keycode == KEY_F5:
		ui.reload()
