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
##   --force-cpu         JsuiView.force_cpu: CPU raster + texture upload even
##                       when Godot runs on Vulkan / Metal
##   --expect-renderer R fail the run unless JsuiView rendered with R
##                       ("vulkan", "metal", "webgpu" or "cpu")
##   --max-gpu-diff D    with --capture on a GPU renderer: also paint the same
##                       frame with the CPU raster, save it as <png>-cpu.png and
##                       fail if the mean channel difference exceeds D
##   --max-viewport-diff D
##                       with --capture and a display (not --headless): read
##                       back what Godot drew for the view (the viewport
##                       texture, with the view frozen over an opaque backdrop)
##                       and fail if it differs from get_frame_image() over
##                       that backdrop by a mean channel difference above D.
##                       get_frame_image() reads JsuiView's own texture, so
##                       only this sees a canvas item that shows something
##                       else (e.g. Godot's magenta / black placeholder)
##   --max-render-waits N
##                       with --self-test on a GPU renderer: drive hover
##                       springs and four surface views of the same runtime
##                       for a while (all render every frame) and fail if the
##                       main thread blocked on Godot's render thread more
##                       than N times meanwhile (get_render_stats()
##                       .blocking_waits), a view handed no frame over
##                       without waiting (gpu_jobs) or the runtime did not
##                       queue exactly one render-thread job per frame
##                       (render_thread_jobs). With --max-viewport-diff the
##                       viewport check also runs right after, mid-animation
##   --bench <n>         measure n frames of hover springs and n frames of
##                       list scrolling (JsuiView.get_render_stats, frame
##                       times), print them and quit
##   --no-world          leave the 3D scene out (benchmarks: the UI only)
##   --fake-webgpu-interop REV
##                       register a stand-in WebGPUInterop singleton (as the
##                       godot-aw3 fork's WebGPU driver does) that reports Dawn
##                       revision REV and null handles: tests JsuiView's
##                       backend choice on stock Godot (another revision: one
##                       warning, CPU renderer; the same one: no device, CPU)
##
## Scene: world/world.tscn renders live 3D behind the view; `godot.scene`
## (shared state) tells the UI to leave its backdrop out so the scene shows.
##
## Game data: `godot.stats` and `player` are shared state (set_state, also
## before the UI starts; the showcase's profile chip reads `player`),
## `godot.ping` a call from the UI (call_requested -> resolve_call, which pays
## out credits with patch_state), `ui.flash` a call into the UI (call_ui).

@onready var ui = $JsuiView

const PING_REWARD := 120
const VIEWPORT_BACKDROP := Color8(40, 150, 110)  # --max-viewport-diff
const HAND_OFF_SURFACES := 4  # --max-render-waits: surface views of ui's runtime

var _capture := ""
var _frames := 120
var _frame := 0
var _stats_timer := 0.0
var _self_test := false
var _window_size := Vector2i.ZERO
var _expect_renderer := ""
var _max_gpu_diff := -1.0
var _max_viewport_diff := -1.0
var _max_render_waits := -1
var _bench := 0
var _pongs := 0
var _ui_answers := 0
var _buffer_seen := []  # what the UI's useHostBufferListener saw (set_buffer)
var _unhandled_clicks := 0
var _player := {"name": "Valkyrie_07", "level": 42, "role": "Vanguard", "credits": 12480}

# Self-test bookkeeping: what the UI reported (showcase:event) and where its
# elements are (jsui-record:anchors / demo:anchors, logical px).
var _events: Array = []
var _anchors := {}
var _failures := 0
var _testing := false
var _fake_interop: Object = null
var _badge_events: Array = []  # "mount:<label>", "render:<label>", "press:<label>", "unmount:<label>"


## The WebGPUInterop contract (integrations/godot/README.md) with null handles.
class FakeWebGPUInterop extends Object:
	var revision := ""

	func get_proc_address() -> int:
		return 0

	func get_instance() -> int:
		return 0

	func get_device() -> int:
		return 0

	func get_queue() -> int:
		return 0

	func get_dawn_revision() -> String:
		return revision


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
			"--force-cpu":
				view.force_cpu = true
			"--expect-renderer":
				_expect_renderer = v
				i += 1
			"--max-gpu-diff":
				_max_gpu_diff = float(v)
				i += 1
			"--max-viewport-diff":
				_max_viewport_diff = float(v)
				i += 1
			"--max-render-waits":
				_max_render_waits = int(v)
				i += 1
			"--bench":
				_bench = int(v)
				i += 1
			"--no-world":
				var world := get_node_or_null("World")
				if world:
					remove_child(world)
					world.free()
			"--fake-webgpu-interop":
				if not Engine.has_singleton("WebGPUInterop"):
					_fake_interop = FakeWebGPUInterop.new()
					_fake_interop.revision = v
					Engine.register_singleton("WebGPUInterop", _fake_interop)
				i += 1
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
	view.set_state("godot.scene", true)


func _exit_tree() -> void:
	if _fake_interop:
		Engine.unregister_singleton("WebGPUInterop")
		_fake_interop.free()
		_fake_interop = null


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
	if _bench > 0:
		_run_bench()
	elif _capture != "" and _self_test:
		_run_self_test()


func _on_ui_message(name: String, data, json: String) -> void:
	match name:
		"showcase:event":
			_events.append(data)
			if _testing:
				print("[ui event] ", json)
		"jsui-record:anchors", "demo:anchors":
			_anchors.merge(data, true)
		"badge:mount", "badge:render", "badge:press", "badge:unmount":
			_badge_events.append(name.trim_prefix("badge:") + ":" + str(data.label))
		"buffer:seen":
			_buffer_seen.append(data)
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
	if _capture != "" and not _self_test and _bench == 0:
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


## A quick tap: press and release in the same frame. (A frame apart, a slow
## frame, e.g. 2x on a software Vulkan renderer, can outlast JSUI's 400 ms
## D-pad repeat delay and move the focus twice.)
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

	# Frame data: a packed array reaches the UI as a typed array (no JSON).
	ui.set_buffer("godot.selftest", PackedFloat32Array([1.5, 2.5, 3.5]))
	var got_buffer := await _wait_for(func(): return _buffer_seen.size() > 0 and _buffer_seen[-1] == [1.5, 2.5, 3.5])
	_check("set_buffer", got_buffer, "useHostBufferListener saw %s" % [_buffer_seen])
	_check("set_buffer unchanged", ui.set_buffer("godot.selftest", PackedFloat32Array([1.5, 2.5, 3.5])) == 0)
	ui.set_buffer("godot.selftest", PackedVector2Array([Vector2(4, 5)]))
	_check("set_buffer vectors", await _wait_for(func(): return _buffer_seen[-1] == [4.0, 5.0]), "saw %s" % [_buffer_seen])

	await _test_surface()  # with the render hand-off check (--max-render-waits)
	await _test_runtime_freed_first()

	_testing = false
	await _wait(0.8)  # let the toast and springs settle for the capture
	_finish()


## A second JsuiView on the same runtime (runtime_view): the bundle's
## 'Badge' component in a surface of its own, sized by its content. Ticked by
## the main view; has its own props, input and texture.
func _test_surface() -> void:
	var badge := JsuiView.new()
	badge.component = "Badge"
	badge.initial_props = {"label": "Surface"}
	badge.auto_size = true
	badge.position = Vector2(16, 16)
	add_child(badge)
	badge.runtime_view = badge.get_path_to(ui)
	var mounted := await _wait_for(func(): return _badge_events.has("mount:Surface"))
	_check("surface mount", mounted and badge.is_running() and badge.get_root_tag() > 1,
		"root tag %d" % badge.get_root_tag())
	var ratio: float = badge.get_effective_pixel_ratio()
	var want := Vector2i(roundi(96 * ratio), roundi(32 * ratio))
	var sized := await _wait_for(func():
		var im: Image = badge.get_frame_image()
		return im != null and im.get_size() == want)
	_check("surface auto size", sized and badge.custom_minimum_size == Vector2(96, 32),
		"min size %s, frame %s, want %s" % [badge.custom_minimum_size, badge.get_frame_image().get_size() if badge.get_frame_image() else null, want])
	var img: Image = badge.get_frame_image()
	var c := img.get_pixel(int(want.x * 0.15), want.y / 2) if img else Color()
	_check("surface pixels", img != null and absi(c.r8 - 71) <= 3 and absi(c.g8 - 140) <= 3 and absi(c.b8 - 191) <= 3,
		"badge fill %s" % [c])
	badge.initial_props = {"label": "Props"}
	_check("surface props", await _wait_for(func(): return _badge_events.has("render:Props")) \
		and not _badge_events.has("mount:Props"), "re-rendered with new props, state kept")
	await _click(badge.position + Vector2(48, 16))
	_check("surface input", await _wait_for(func(): return _badge_events.has("press:Props")), "onPress in the surface")
	if _max_render_waits >= 0:
		await _check_render_hand_off(badge)
	badge.queue_free()
	_check("surface unmount", await _wait_for(func(): return _badge_events.has("unmount:Surface")), "freeing the view unmounts it")
	ui.grab_focus()


## --max-render-waits: with a separate render thread the runtime view queues
## the frame's GPU renders of all its views there as one job and does not
## wait for it; Godot's main loop syncs with that thread after process, so the
## next frame finds the job done. The surface test's badge and three more
## surface views of ui's runtime are there too and get a new label every
## frame, so every frame one job paints all five views (one Skia context for
## the runtime), and the runtime view collects each view's result (stats,
## frame_rendered) the next frame. Only process_frame awaits here: they resume
## before the views' _process, the props changes run then, and the mouse
## events reach the view at the start of the next frame (accumulated input),
## so nothing in this loop calls into the runtime mid-frame.
func _check_render_hand_off(badge: JsuiView) -> void:
	if ui.get_renderer() == "cpu":
		print("render hand-off: cpu renderer, nothing to check")
		return
	var extra := []
	for i in HAND_OFF_SURFACES - 1:
		var v := JsuiView.new()
		v.component = "Badge"
		v.initial_props = {"label": "S%d" % (i + 1)}
		v.auto_size = true
		v.position = Vector2(128 + 112 * i, 16)
		add_child(v)
		v.runtime_view = v.get_path_to(ui)
		extra.append(v)
	var surfaces: Array = [badge] + extra
	var up := await _wait_for(func(): return extra.all(func(v): return v.get_render_stats().renders > 0))
	var views: Array = [ui] + surfaces
	var signals := []  # frame_rendered per view
	var counters := []
	for i in views.size():
		signals.append(0)
		counters.append(func(_r): signals[i] += 1)
	var targets := ["menu-1", "menu-2", "menu-3", "menu-4"]
	var frames := 30  # lavapipe draws the 3D scene at a few fps: keep it short
	await get_tree().process_frame  # from here on: before the views' _process
	var s0: Array = views.map(func(v): return v.get_render_stats())
	for i in views.size():
		views[i].frame_rendered.connect(counters[i])
	for f in frames:
		# Across the menu buttons: hover-in / out springs, a repaint every frame.
		_mouse_move(_center(targets[(f / 6) % targets.size()]) + Vector2(float((f % 6) * 8 - 20), 0))
		for i in surfaces.size():
			surfaces[i].initial_props = {"label": "%d.%d" % [i, f]}
		await get_tree().process_frame
	var s1: Array = views.map(func(v): return v.get_render_stats())
	for i in views.size():
		views[i].frame_rendered.disconnect(counters[i])
	var ok := up
	var waits_total := 0
	var parts := []
	for i in views.size():
		var jobs: int = s1[i].gpu_jobs - s0[i].gpu_jobs
		var renders: int = s1[i].renders - s0[i].renders
		var waits: int = s1[i].blocking_waits - s0[i].blocking_waits
		waits_total += waits
		# Every collected frame with damage counts once and emits frame_rendered
		# once; a surface with a new label every frame hands over every frame.
		ok = ok and jobs > 0 and renders > 0 and signals[i] == renders and (i == 0 or jobs == frames)
		parts.append("%s %d frames handed over, %d with damage (%d frame_rendered), %d blocking waits (%.3f ms), render thread %.3f ms/frame (jsui_render_gpu_target %.3f ms)" % [
			"view" if i == 0 else "surface %d" % i, jobs, renders, signals[i], waits,
			(s1[i].main_wait_usec_total - s0[i].main_wait_usec_total) / 1000.0,
			(s1[i].gpu_job_usec_total - s0[i].gpu_job_usec_total) / 1000.0 / max(1, jobs),
			(s1[i].jsui_render_usec_total - s0[i].jsui_render_usec_total) / 1000.0 / max(1, jobs)])
	# With a separate render thread: one job per frame for all the runtime's
	# views. Without one, the frames are painted right away (no jobs).
	var separate: bool = not RenderingServer.is_on_render_thread()
	var thread_jobs: int = s1[0].render_thread_jobs - s0[0].render_thread_jobs
	ok = ok and thread_jobs == (frames if separate else 0)
	_check("render hand-off", ok and waits_total <= _max_render_waits,
		"%d frames, one runtime, %d views: %d render-thread jobs (%s); %s; limit %d waits" % [
			frames, views.size(), thread_jobs, "separate render thread" if separate else "rendering on the main thread",
			"; ".join(parts), _max_render_waits])
	if _max_viewport_diff >= 0.0 and _capture != "":
		# Mid-animation: the last frames were all handed over without a wait.
		var shown_ok: bool = await _compare_with_viewport(_capture.get_basename() + "-animating.png", surfaces)
		if not shown_ok:
			_failures += 1
	# A call into the runtime later in the frame (from a timer: timers run
	# after the views' _process) waits for the runtime's whole frame job,
	# whichever of its views it is made on: once it returns, the badge's frame
	# (in that job) has been painted. Once on the runtime view (a state call),
	# once on the badge (its surface handle).
	var fence_ok := true
	var fence_parts := []
	for side in ["view", "surface"]:
		await get_tree().process_frame
		var queued: int = badge.get_render_stats().gpu_jobs
		badge.initial_props = {"label": "Fence " + side}
		await get_tree().create_timer(0.0).timeout
		var caller: JsuiView = ui if side == "view" else badge
		var w0: int = caller.get_render_stats().blocking_waits
		if side == "view":
			ui.get_state_json("season")
		else:
			badge.get_root_tag()
		var waited: bool = caller.get_render_stats().blocking_waits > w0
		var bs: Dictionary = badge.get_render_stats()
		fence_ok = fence_ok and bs.gpu_jobs == queued + 1 and not bs.gpu_frame_in_flight
		fence_parts.append("a call on the %s: %d frame(s) handed over, then %s (%s)" % [side, bs.gpu_jobs - queued,
			"still in flight" if bs.gpu_frame_in_flight else "painted",
			"the call waited for the render thread" if waited else "it had run already"])
	_check("surface fence", fence_ok, "the badge's frame after %s" % "; ".join(fence_parts))
	# Freed while their last frames are still queued (queue_free frees them at
	# the end of the frame, after ui's _process queued them).
	await get_tree().process_frame
	for v in extra:
		v.initial_props = {"label": "Bye"}
	await get_tree().create_timer(0.0).timeout
	var in_flight: int = extra.filter(func(v): return v.get_render_stats().gpu_frame_in_flight).size()
	for v in extra:
		v.queue_free()
	var gone := await _wait_for(func():
		for i in extra.size():
			if not _badge_events.has("unmount:S%d" % (i + 1)):
				return false
		return true)
	_check("surfaces freed mid-frame", gone, "%d of %d freed with their frame on the render thread" % [in_flight, extra.size()])


## A runtime view freed before its surface views, right after the frame job
## with all their frames was queued (with a separate render thread it is
## still on that thread), and a surface view taken out of the tree and put
## back the frame before: the runtime view waits for the job before the
## runtime, its textures and the runtime's Skia context go; its surface views
## lose their surfaces, keep working without them and can be freed after it.
func _test_runtime_freed_first() -> void:
	var host := JsuiView.new()
	host.bundle_url = ui.bundle_url
	host.component = "Badge"
	host.initial_props = {"label": "Host"}
	host.auto_size = true
	host.position = Vector2(16, 64)
	add_child(host)
	var guests := []
	for i in 2:
		var g := JsuiView.new()
		g.component = "Badge"
		g.initial_props = {"label": "Guest %d" % i}
		g.auto_size = true
		g.position = Vector2(128 + 112 * i, 64)
		add_child(g)
		g.runtime_view = g.get_path_to(host)
		guests.append(g)
	var views: Array = [host] + guests
	var up := await _wait_for(func(): return views.all(func(v): return v.get_render_stats().renders > 0), 10.0)
	# Out of the tree and back, its frame queued with the others.
	await get_tree().process_frame
	for v in views:
		v.initial_props = {"label": "A"}
	await get_tree().create_timer(0.0).timeout
	var in_flight := 0
	if guests[0].get_render_stats().gpu_frame_in_flight:
		in_flight += 1
	remove_child(guests[0])
	var detached: bool = not guests[0].is_running()
	add_child(guests[0])
	var reattached: bool = guests[0].is_running()
	# The runtime view freed first.
	await get_tree().process_frame
	for v in views:
		v.initial_props = {"label": "B"}
	await get_tree().create_timer(0.0).timeout
	if guests[1].get_render_stats().gpu_frame_in_flight:
		in_flight += 1
	host.free()
	var orphaned: bool = guests.all(func(g): return not g.is_running())
	await get_tree().process_frame
	await get_tree().process_frame
	for g in guests:
		g.free()
	_check("runtime freed first", up and detached and reattached and orphaned,
		"surface left and re-entered the tree, then its runtime view was freed (%d of 2 with a frame on the render thread)" % in_flight)


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
	print("capture: JsuiView frame %dx%d (pixel ratio %.2f, renderer %s) -> %s" % [img.get_width(), img.get_height(), ui.get_effective_pixel_ratio(), ui.get_renderer(), path])
	var ok: bool = ui.get_last_error() == ""
	if _expect_renderer != "" and ui.get_renderer() != _expect_renderer:
		printerr("renderer: %s, expected %s" % [ui.get_renderer(), _expect_renderer])
		ok = false
	if _max_gpu_diff >= 0.0 and ui.get_renderer() != "cpu":
		ok = _compare_with_cpu(path) and ok
	if _max_viewport_diff >= 0.0:
		var shown_ok: bool = await _compare_with_viewport(path)
		ok = shown_ok and ok
	# With a real renderer (not --headless) also grab the composited window,
	# which shows the texture as Godot drew it.
	if DisplayServer.get_name() != "headless":
		await RenderingServer.frame_post_draw
		var shot := get_viewport().get_texture().get_image()
		if shot:
			var vpath := path.get_basename() + "-viewport.png"
			shot.save_png(vpath)
			print("capture: viewport %dx%d -> %s" % [shot.get_width(), shot.get_height(), vpath])
	if _self_test:
		print("self-test: %d failed (pongs=%d ui_answers=%d unhandled_clicks=%d)" % [_failures, _pongs, _ui_answers, _unhandled_clicks])
		if _failures > 0:
			printerr("self-test failed")
			ok = false
	get_tree().quit(0 if ok else 1)


## Paints the current frame with the CPU raster too and compares it with the
## GPU frame (both read now, so they show the same state).
func _compare_with_cpu(path: String) -> bool:
	var cpu: Image = ui.get_cpu_frame_image()
	var gpu: Image = ui.get_frame_image()
	if cpu == null or gpu == null or cpu.get_size() != gpu.get_size():
		printerr("gpu-vs-cpu: no frames to compare")
		return false
	cpu.save_png(path.get_basename() + "-cpu.png")
	gpu.save_png(path.get_basename() + "-gpu.png")
	var m: Dictionary = gpu.compute_image_metrics(cpu, false)
	# Share of channels that differ by more than 8 / 255.
	var w := gpu.get_width()
	var h := gpu.get_height()
	var a := gpu.get_data()
	var b := cpu.get_data()
	var over := 0
	for k in range(0, a.size(), 7):  # a sample of the channels is plenty
		if absi(a[k] - b[k]) > 8:
			over += 1
	var over_pct := 100.0 * over / (a.size() / 7.0)
	var ok: bool = m.mean <= _max_gpu_diff
	print("gpu-vs-cpu: %s %dx%d mean %.3f, rms %.3f, max %d, channels off by > 8: %.3f%% (limit mean %.3f) %s" % [
		ui.get_renderer(), w, h, m.mean, m.root_mean_squared, int(m.max), over_pct, _max_gpu_diff, "ok" if ok else "FAIL"])
	if not ok:
		printerr("gpu-vs-cpu: the GPU frame differs from the CPU raster")
	return ok


## --max-viewport-diff: what Godot drew for the view against the frame
## JsuiView rendered. get_frame_image() reads the view's own texture, so it is
## right even when the canvas item draws another one (Godot's placeholder, a
## magenta / black checkerboard: Texture2DRD.get_rid() before the render
## thread created its RenderingServer texture). The view is frozen (no ticks,
## no repaints) over an opaque backdrop of a known colour, so the screen must
## show the frame blended over that colour, also where the UI is translucent
## and the 3D scene would show through. The frame is scaled to the view's
## rect in window pixels if they differ. `overlays`: surface views drawn over
## the view (frozen with it: the runtime view ticks them); their frames are
## blended in at their rects, and each rect must match on its own too (a
## small one barely moves the mean of the whole view).
func _compare_with_viewport(path: String, overlays: Array = []) -> bool:
	if DisplayServer.get_name() == "headless":
		printerr("viewport-vs-frame: needs a display (not --headless) FAIL")
		return false
	ui.set_process(false)
	var backdrop := ColorRect.new()
	backdrop.color = VIEWPORT_BACKDROP
	backdrop.mouse_filter = Control.MOUSE_FILTER_IGNORE
	backdrop.show_behind_parent = true  # under the view's own canvas item
	ui.add_child(backdrop)
	backdrop.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	# With a separate render thread the frame_post_draw that arrives first can
	# be for a frame drawn before the backdrop was there.
	for n in 3:
		await RenderingServer.frame_post_draw
	var shot: Image = get_viewport().get_texture().get_image()
	var frame: Image = ui.get_frame_image()
	var overlay_frames: Array = overlays.map(func(v): return v.get_frame_image())
	ui.remove_child(backdrop)
	backdrop.free()
	ui.set_process(true)
	if shot == null or frame == null or overlay_frames.has(null):
		printerr("viewport-vs-frame: no viewport image or frame FAIL")
		return false
	var full := _window_rect(ui)
	var rect := full.intersection(Rect2i(Vector2i.ZERO, shot.get_size()))
	if not rect.has_area():
		printerr("viewport-vs-frame: the view (%s) is not in the viewport (%s) FAIL" % [full, shot.get_size()])
		return false
	shot.convert(Image.FORMAT_RGBA8)
	frame.convert(Image.FORMAT_RGBA8)
	var frame_size := frame.get_size()
	if frame_size != full.size:
		frame.resize(full.size.x, full.size.y, Image.INTERPOLATE_BILINEAR)
	var shown := shot.get_region(rect)
	var expected := Image.create_empty(rect.size.x, rect.size.y, false, Image.FORMAT_RGBA8)
	expected.fill(VIEWPORT_BACKDROP)
	expected.blend_rect(frame, Rect2i(rect.position - full.position, rect.size), Vector2i.ZERO)
	var overlay_rects := []  # in `shown` / `expected` pixels
	for i in overlays.size():
		var orect: Rect2i = _window_rect(overlays[i])
		var oframe: Image = overlay_frames[i]
		oframe.convert(Image.FORMAT_RGBA8)
		if oframe.get_size() != orect.size:
			oframe.resize(orect.size.x, orect.size.y, Image.INTERPOLATE_BILINEAR)
		expected.blend_rect(oframe, Rect2i(Vector2i.ZERO, orect.size), orect.position - rect.position)
		overlay_rects.append(Rect2i(orect.position - rect.position, orect.size).intersection(Rect2i(Vector2i.ZERO, rect.size)))
	shown.convert(Image.FORMAT_RGB8)
	expected.convert(Image.FORMAT_RGB8)
	shown.save_png(path.get_basename() + "-shown.png")
	expected.save_png(path.get_basename() + "-expected.png")
	var m: Dictionary = shown.compute_image_metrics(expected, false)
	var mean: float = m.mean * 4.0 / 3.0  # metrics average R, G, B and A; A is 255 in both
	var ok: bool = mean <= _max_viewport_diff
	var details := ""
	for i in overlays.size():
		var orect: Rect2i = overlay_rects[i]
		var om: Dictionary = shown.get_region(orect).compute_image_metrics(expected.get_region(orect), false) \
			if orect.has_area() else {"mean": INF, "max": 255}
		var omean: float = om.mean * 4.0 / 3.0
		ok = ok and omean <= _max_viewport_diff
		details += ", surface %s %s at %s: mean %.3f, max %d" % [
			overlays[i].component, overlays[i].get_renderer(), orect, omean, int(om.max)]
	print("viewport-vs-frame: %s frame %dx%d, view at %s in a %dx%d viewport: mean %.3f, max %d%s (limit mean %.3f) %s" % [
		ui.get_renderer(), frame_size.x, frame_size.y, rect, shot.get_width(), shot.get_height(), mean, int(m.max),
		details, _max_viewport_diff, "ok" if ok else "FAIL"])
	if not ok:
		printerr("viewport-vs-frame: Godot does not show the frame JsuiView rendered (see %s-shown.png / -expected.png)" % path.get_basename())
	return ok


## A view's rect in window (viewport texture) pixels.
func _window_rect(v: Control) -> Rect2i:
	var r: Rect2 = get_viewport().get_final_transform() * v.get_global_transform_with_canvas() * Rect2(Vector2.ZERO, v.size)
	return Rect2i(Vector2i(r.position.round()), Vector2i(r.size.round()))


## --bench: hover springs (small damage) then list scrolling (larger damage),
## `_bench` frames each, with the vsync and frame cap off.
func _run_bench() -> void:
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	Engine.max_fps = 0
	var need := ["menu-1", "menu-2", "menu-3", "menu-4", "armory"]
	var ready := await _wait_for(func():
		for a in need:
			if not _anchors.has(a):
				return false
		return ui.get_pending_loads() == 0, 8.0)
	if not ready:
		printerr("bench: the UI did not report its anchors")
		get_tree().quit(1)
		return
	await _wait(1.5)
	var results := []
	for phase in ["hover", "scroll"]:
		var s0: Dictionary = ui.get_render_stats()
		var t0 := Time.get_ticks_usec()
		var list := _center("armory")
		for f in _bench:
			if phase == "hover":
				# Sweep across the menu buttons: hover-in / out springs.
				var target := _center(need[(f / 6) % 4])
				_mouse_move(target + Vector2(float((f % 6) * 8 - 20), 0))
			else:
				_mouse_move(list)
				_mouse_button(list, MOUSE_BUTTON_WHEEL_DOWN if (f / 20) % 2 == 0 else MOUSE_BUTTON_WHEEL_UP, true)
				_mouse_button(list, MOUSE_BUTTON_WHEEL_DOWN if (f / 20) % 2 == 0 else MOUSE_BUTTON_WHEEL_UP, false)
			await get_tree().process_frame
		await RenderingServer.frame_post_draw
		var dt := Time.get_ticks_usec() - t0
		var s1: Dictionary = ui.get_render_stats()
		var renders: int = s1.renders - s0.renders
		var r := {
			"phase": phase,
			"renderer": ui.get_renderer(),
			"frames": _bench,
			"frame_ms": dt / 1000.0 / _bench,
			"renders": renders,
			"render_ms": (s1.render_usec_total - s0.render_usec_total) / 1000.0 / max(1, renders),
			"dirty_kpx": (s1.dirty_pixels_total - s0.dirty_pixels_total) / 1000.0 / max(1, renders),
		}
		results.append(r)
		print("bench: %s %s: %d frames, %.2f ms/frame (wall), %d renders, %.3f ms/render (JsuiView.render, CPU side), %.1f kpx damage/render" % [
			r.renderer, phase, _bench, r.frame_ms, renders, r.render_ms, r.dirty_kpx])
		_mouse_move(Vector2(-100, -100))
		await _wait(1.0)
	print("bench-json: ", JSON.stringify(results))
	get_tree().quit(0)


func _unhandled_input(event: InputEvent) -> void:
	# Input the UI did not consume reaches the game.
	if event is InputEventMouseButton and event.pressed:
		_unhandled_clicks += 1
		print("[game] unhandled click at ", event.position)
	elif event is InputEventKey and event.pressed and event.keycode == KEY_F5:
		ui.reload()
