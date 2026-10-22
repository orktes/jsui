extends Control
## JSUI GDExtension demo.
##
## User arguments (after `--` on the Godot command line):
##   --dev <url>         load from a dev server with live reload, e.g.
##                       http://127.0.0.1:5173/bundle.js (`npx jsui dev ...`)
##   --bundle <url>      load another bundle (res://, user://, path or http://)
##   --capture <png>     render, save a screenshot and quit (CI / headless)
##   --frames <n>        frames to run before --capture (default 120)
##   --window <WxH>      resize the window on start (headless ignores
##                       --resolution), e.g. 2560x1440 for a 2x content scale
##   --self-test         with --capture: drive the UI through Godot's own input
##                       pipeline (Input.parse_input_event, like real devices):
##                       hover, click, wheel, trackpad pan, keyboard into a
##                       TextInput, gamepad focus, the Ping button and an
##                       unhandled click, plus a call_ui. Prints PASS / FAIL
##                       per check and fails the run on any FAIL
##
## Game data: `godot.stats` and `player` are shared state (set_state, also
## before the UI starts; the showcase's profile chip reads `player`),
## `godot.ping` a call from the UI (call_requested -> resolve_call, which pays
## out credits with patch_state), `ui.flash` a call into the UI (call_ui).

@onready var ui = $JsuiView

const PING_REWARD := 120

var _capture := ""
var _frames := 120
var _frame := 0
var _stats_timer := 0.0
var _self_test := false
var _window_size := Vector2i.ZERO
var _pongs := 0
var _ui_answers := 0
var _unhandled_clicks := 0
var _player := {"name": "Valkyrie_07", "level": 42, "role": "Vanguard", "credits": 12480}

# Self-test bookkeeping: what the UI reported (showcase:event) and where its
# elements are (jsui-record:anchors / demo:anchors, logical px).
var _events: Array = []
var _anchors := {}
var _failures := 0
var _testing := false


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
			"--window":
				var wh := v.split("x")
				if wh.size() == 2:
					_window_size = Vector2i(int(wh[0]), int(wh[1]))
				i += 1
		i += 1
	# Shared state can be set before the view starts: the UI renders it on
	# its first frame.
	view.set_state("godot.stats", _stats())
	view.set_state("player", _player)


func _ready() -> void:
	# The headless display server ignores --resolution and reports a 64x64
	# window, which the canvas_items stretch turns into a 1280x1280 view at
	# 0.05 scale. --window WxH sets the size so headless runs (CI) get the
	# real layout and scale, e.g. 2560x1440 for a Retina-like 2x.
	if _window_size != Vector2i.ZERO:
		get_window().size = _window_size
	ui.message_received.connect(_on_ui_message)
	ui.call_requested.connect(_on_ui_call)
	ui.js_error.connect(func(msg): printerr("[jsui error] ", msg))
	ui.grab_focus()  # keyboard / gamepad go to the UI
	print("JsuiView running=%s bundle=%s dev=%s" % [ui.is_running(), ui.bundle_url, ui.dev_mode])
	if _capture != "" and _self_test:
		_run_self_test()


func _on_ui_message(name: String, data, json: String) -> void:
	match name:
		"showcase:event":
			_events.append(data)
			if _testing:
				print("[ui event] ", json)
		"jsui-record:anchors", "demo:anchors":
			_anchors.merge(data, true)
		_:
			print("[ui -> godot] ", name, " ", json)


func _on_ui_call(id: int, name: String, args) -> void:
	print("[ui call] #%d %s %s" % [id, name, JSON.stringify(args)])
	match name:
		"godot.ping":
			_pongs += 1
			_player.credits += PING_REWARD
			ui.patch_state("player", {"credits": _player.credits})  # the profile chip updates
			# Could also answer on a later frame: keep the id, resolve then.
			ui.resolve_call(id, {"count": args.count, "reward": PING_REWARD, "time": Time.get_ticks_msec() / 1000.0})
		_:
			ui.reject_call(id, "unknown call " + name)


func _stats() -> Dictionary:
	var v := Engine.get_version_info()
	return {
		"fps": Engine.get_frames_per_second(),
		"frame": Engine.get_process_frames(),
		"version": "%d.%d.%d" % [v.major, v.minor, v.patch],
	}


func _send_stats() -> void:
	ui.set_state("godot.stats", _stats())  # no-op when nothing changed


func _process(delta: float) -> void:
	_stats_timer -= delta
	if _stats_timer <= 0.0:
		_stats_timer = 1.0
		_send_stats()
	if _capture != "" and not _self_test:
		_frame += 1
		if _frame >= _frames and ui.get_pending_loads() == 0:
			_save_capture()


func _on_flash_answer(ok: bool, result) -> void:
	print("[godot -> ui call] ok=%s result=%s" % [ok, JSON.stringify(result)])
	if ok and result.shown:
		_ui_answers += 1


# ------------------------------------------------------------------ self-test

func _check(name: String, ok: bool, detail := "") -> void:
	if ok:
		print("PASS ", name, ("  " + detail) if detail != "" else "")
	else:
		_failures += 1
		printerr("FAIL ", name, ("  " + detail) if detail != "" else "")


func _wait(seconds: float) -> void:
	await get_tree().create_timer(seconds).timeout


func _wait_for(cond: Callable, timeout := 3.0) -> bool:
	var end := Time.get_ticks_msec() + int(timeout * 1000.0)
	while not cond.call():
		if Time.get_ticks_msec() > end:
			return false
		await get_tree().process_frame
	return true


## Finds the first reported event of `type` (after index `from`) whose fields
## match `fields`.
func _find_event(type: String, fields := {}, from := 0):
	for i in range(from, _events.size()):
		var e = _events[i]
		if typeof(e) != TYPE_DICTIONARY or e.get("type") != type:
			continue
		var ok := true
		for k in fields:
			if e.get(k) != fields[k]:
				ok = false
				break
		if ok:
			return e
	return null


func _center(anchor: String) -> Vector2:
	var r: Dictionary = _anchors[anchor]
	# Round to quarter pixels so the mapped window position is exact in floats.
	return Vector2(snappedf(r.x + r.width / 2.0, 0.25), snappedf(r.y + r.height / 2.0, 0.25))


## JsuiView-local (= JSUI logical) px -> window px, the space Input events
## use. Includes the stretch transform (content scale), so with a 2560x1440
## window and the 1280x720 base size every logical px is 2 window px.
## (Not get_screen_transform(): with embedded subwindows, the default, it
## leaves the stretch transform out.)
func _to_window(p: Vector2) -> Vector2:
	return get_viewport().get_final_transform() * ui.get_global_transform_with_canvas() * p


func _mouse_move(p: Vector2) -> void:
	var e := InputEventMouseMotion.new()
	e.position = _to_window(p)
	e.global_position = e.position
	Input.parse_input_event(e)


func _mouse_button(p: Vector2, button: MouseButton, pressed: bool, factor := 1.0) -> void:
	var e := InputEventMouseButton.new()
	e.button_index = button
	e.position = _to_window(p)
	e.global_position = e.position
	e.pressed = pressed
	e.factor = factor
	if pressed and button == MOUSE_BUTTON_LEFT:
		e.button_mask = MOUSE_BUTTON_MASK_LEFT
	Input.parse_input_event(e)


func _click(p: Vector2) -> void:
	_mouse_move(p)
	await get_tree().process_frame
	_mouse_button(p, MOUSE_BUTTON_LEFT, true)
	await get_tree().process_frame
	_mouse_button(p, MOUSE_BUTTON_LEFT, false)
	await get_tree().process_frame


func _key(keycode: Key, unicode := 0, shift := false) -> void:
	for pressed in [true, false]:
		var e := InputEventKey.new()
		e.keycode = keycode
		e.physical_keycode = keycode
		e.unicode = unicode if pressed else 0
		e.shift_pressed = shift
		e.pressed = pressed
		Input.parse_input_event(e)
		await get_tree().process_frame


func _pad(button: JoyButton) -> void:
	for pressed in [true, false]:
		var e := InputEventJoypadButton.new()
		e.device = 0
		e.button_index = button
		e.pressed = pressed
		e.pressure = 1.0 if pressed else 0.0
		Input.parse_input_event(e)
		await get_tree().process_frame


func _run_self_test() -> void:
	var need := ["menu-1", "menu-2", "menu-3", "armory", "recruit", "ping"]
	var anchors_ready := func():
		for a in need:
			if not _anchors.has(a):
				return false
		return ui.get_pending_loads() == 0
	var have_anchors := await _wait_for(anchors_ready, 8.0)
	_check("anchors", have_anchors, "missing: %s" % [need.filter(func(a): return not _anchors.has(a))])
	if not have_anchors:
		_finish()
		return
	await _wait(1.5)  # the staggered entrance settles (anchors move with it)
	var win := get_window()
	print("self-test: window %s, view %s, view -> window scale %s, pixel ratio %.2f, screen scale %.2f" % [
		win.size, ui.size, (get_viewport().get_final_transform() * ui.get_global_transform_with_canvas()).get_scale(),
		ui.get_effective_pixel_ratio(), DisplayServer.screen_get_scale()])
	_testing = true

	# The texture covers the view in device pixels.
	var img: Image = ui.get_frame_image()
	var ratio: float = ui.get_effective_pixel_ratio()
	var want := Vector2i((ui.size * ratio).round())
	_check("frame size", img != null and img.get_size() == want,
		"frame %s, want view %s x ratio %.2f = %s" % [img.get_size() if img else null, ui.size, ratio, want])

	# Hover: motion over a menu button fires its onHoverIn.
	var mark := _events.size()
	_mouse_move(_center("menu-1") + Vector2(-60, 0))
	await get_tree().process_frame
	_mouse_move(_center("menu-1"))
	var hovered := await _wait_for(func(): return _find_event("hover", {"anchor": "menu-1"}, mark) != null)
	_check("hover", hovered, "onHoverIn on 'New Journey'")

	# Click: press + release on another menu button; pressIn reports where the
	# runtime saw the pointer, which must be exactly the logical point.
	mark = _events.size()
	var target := _center("menu-2")
	await _click(target)
	var pressed := await _wait_for(func(): return _find_event("press", {"anchor": "menu-2"}, mark) != null)
	_check("click", pressed, "onPress on 'Multiplayer'")
	var pin = _find_event("pressIn", {"anchor": "menu-2"}, mark)
	var seen := Vector2(pin.x, pin.y) if pin else Vector2(-1, -1)
	_check("mapping", pin != null and seen.distance_to(target) < 0.01,
		"injected logical %s (window %s), UI saw %s" % [target, _to_window(target), seen])

	# Gamepad: the click focused 'Multiplayer'; D-pad down moves focus to
	# 'Armory' (JSUI spatial navigation), A presses it.
	mark = _events.size()
	await _pad(JOY_BUTTON_DPAD_DOWN)
	var moved := await _wait_for(func(): return _find_event("focus", {"anchor": "menu-3", "visible": true}, mark) != null)
	_check("gamepad focus", moved, "D-pad down: 'Multiplayer' -> 'Armory'")
	await _pad(JOY_BUTTON_A)
	var activated := await _wait_for(func(): return _find_event("press", {"anchor": "menu-3"}, mark) != null)
	_check("gamepad press", activated, "A on 'Armory'")

	# Wheel: three notches down over the armory list scroll it (momentum
	# list), a trackpad pan gesture (macOS) scrolls further.
	mark = _events.size()
	var list := _center("armory")
	_mouse_move(list)
	await get_tree().process_frame
	for n in 3:
		_mouse_button(list, MOUSE_BUTTON_WHEEL_DOWN, true)
		_mouse_button(list, MOUSE_BUTTON_WHEEL_DOWN, false)
		await get_tree().process_frame
	var scrolled := await _wait_for(func(): return _last_scroll_y() > 0.0)
	var wheel_y := _last_scroll_y()
	_check("wheel", scrolled, "armory list at y=%s" % wheel_y)
	await _wait(0.6)
	wheel_y = _last_scroll_y()
	var pan := InputEventPanGesture.new()
	pan.position = _to_window(list)
	pan.delta = Vector2(0, 2)
	Input.parse_input_event(pan)
	var panned := await _wait_for(func(): return _last_scroll_y() > wheel_y + 1.0)
	_check("pan gesture", panned, "armory list %s -> %s" % [wheel_y, _last_scroll_y()])

	# Keyboard: clicking the TextInput focuses it, typed keys arrive as text,
	# Enter submits (the squad gains a chip).
	mark = _events.size()
	await _click(_center("recruit"))
	var focused := await _wait_for(func(): return _find_event("focus", {"anchor": "recruit"}, mark) != null)
	_check("text focus", focused, "click focuses the TextInput")
	await _key(KEY_N, "N".unicode_at(0), true)
	await _key(KEY_O, "o".unicode_at(0))
	await _key(KEY_V, "v".unicode_at(0))
	await _key(KEY_A, "a".unicode_at(0))
	var typed := await _wait_for(func(): return _find_event("text", {"value": "Nova"}, mark) != null)
	_check("typing", typed, "TextInput value 'Nova'")
	await _key(KEY_ENTER)
	var submitted := await _wait_for(func(): return _find_event("recruit", {"name": "Nova"}, mark) != null)
	_check("enter", submitted, "onSubmitEditing recruits 'Nova'")

	# Bridge: click Ping (Host.call -> resolve_call), call into the UI, and a
	# click on empty space falls through to _unhandled_input.
	var pongs := _pongs
	await _click(_center("ping"))
	_check("ping", await _wait_for(func(): return _pongs > pongs), "Host.call('godot.ping') answered")
	ui.call_ui("ui.flash", {"text": "hello from Godot"}, _on_flash_answer)
	_check("call_ui", await _wait_for(func(): return _ui_answers > 0), "ui.flash handler answered")
	var clicks := _unhandled_clicks
	await _click(Vector2(200, 620))  # open space below the season card
	_check("unhandled click", _unhandled_clicks > clicks, "reaches the game")

	_testing = false
	await _wait(0.8)  # let the toast and springs settle for the capture
	_finish()


func _last_scroll_y() -> float:
	for i in range(_events.size() - 1, -1, -1):
		var e = _events[i]
		if typeof(e) == TYPE_DICTIONARY and e.get("type") == "scroll":
			return e.y
	return 0.0


func _finish() -> void:
	if _capture == "":
		return
	await _wait_for(func(): return ui.get_pending_loads() == 0, 5.0)
	_save_capture()


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
		print("self-test: %d failed (pongs=%d ui_answers=%d unhandled_clicks=%d)" % [_failures, _pongs, _ui_answers, _unhandled_clicks])
		if _failures > 0:
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
