extends Node3D
## The Godot version of the engine examples' game (integrations/raylib,
## sokol, unity, unreal), for `--hud`: a ground, a tower and units orbiting
## it, with the shared HUD (integrations/shared/ui/app.jsx) on top through the
## demo's JsuiView. The same game runs with `--no-ui` (no JsuiView, no
## runtime), so `--bench-seconds` measures what the UI costs it.
##
## Per frame (as the other examples): every unit is projected to the screen
## and written as x, y, visible, hue into the `entities` frame buffer
## (set_buffer), which moves the nameplates without running JS; fps, unit
## count, the selected unit and the pass-through counters go to the `game`
## shared state (set_state_json, only when one of them changes).
##
## Input the UI did not consume (_unhandled_input): click a unit to select it
## or empty ground to clear the selection, drag to orbit the camera, wheel to
## zoom, Space spawns a unit, Esc pauses. UI messages: game:spawn {count},
## game:clear, game:pause {paused}, game:quit.

const MAX_UNITS := 48
const STRIDE := 4  # x, y, visible, hue
const CLICK_SLOP := 4.0  # px: a press that moved less is a click
const HUES := [
	Color8(242, 195, 91), Color8(79, 209, 255), Color8(178, 107, 255), Color8(74, 222, 128),
	Color8(255, 154, 60), Color8(79, 140, 255), Color8(255, 77, 106),
]
const FPS_WINDOW := 60
const FPS_WARMUP := 10

## The demo's JsuiView, or null with --no-ui.
var view: JsuiView = null

var _camera: Camera3D
var _units: Array = []  # {angle, radius, speed, height, size, phase, hue, body}
var _entities := PackedFloat32Array()
var _materials: Array[StandardMaterial3D] = []
var _selected_material: StandardMaterial3D
var _ring_material: StandardMaterial3D
var _ring: MeshInstance3D
var _cube_mesh := BoxMesh.new()
var _rng := RandomNumberGenerator.new()
var _spawned := 0
var _time := 0.0
var _yaw := 0.8
var _pitch := 0.55
var _distance := 16.0
var _paused := false
var _selected := -1
var _dragging := false
var _press_at := Vector2.ZERO
var _last_pointer := Vector2.ZERO
var _travel := 0.0
var _clicks := 0
var _wheel := 0
var _keys := 0
var _engine := ""
var _last_state := []
# FPS over the last 60 frames after a 10-frame warm-up, shown at most every
# 250 ms (FpsCounter in integrations/shared/example_host.h).
var _frame_times := PackedFloat64Array()
var _frames_seen := 0
var _fps_shown_at := -1.0
var _fps := 0


func _ready() -> void:
	var v := Engine.get_version_info()
	_engine = "Godot %d.%d.%d" % [v.major, v.minor, v.patch]
	_rng.seed = 1  # the same units with and without the UI
	_frame_times.resize(FPS_WINDOW)
	_build_scene()
	for i in 8:
		_spawn()
	if view:
		view.message_received.connect(_on_ui_message)
		_publish_state()  # before the bundle's first render


func _material(c: Color, roughness := 0.65) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	m.albedo_color = c
	m.roughness = roughness
	return m


func _build_scene() -> void:
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color8(9, 12, 22)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.25, 0.28, 0.38)
	var world_env := WorldEnvironment.new()
	world_env.environment = env
	add_child(world_env)

	_camera = Camera3D.new()
	_camera.fov = 50
	add_child(_camera)
	_camera.make_current()

	var light := DirectionalLight3D.new()
	light.light_energy = 1.1
	light.rotation_degrees = Vector3(-50, -30, 0)
	add_child(light)

	var ground := MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Vector2(40, 40)
	ground.mesh = plane
	ground.material_override = _material(Color8(14, 19, 34), 1.0)
	add_child(ground)

	var tower := MeshInstance3D.new()
	var cyl := CylinderMesh.new()
	cyl.top_radius = 1.05
	cyl.bottom_radius = 1.05
	cyl.height = 2.2
	tower.mesh = cyl
	tower.position = Vector3(0, 1.1, 0)
	tower.material_override = _material(Color8(26, 36, 64))
	add_child(tower)

	for c in HUES:
		_materials.append(_material(c))
	# Selection: the selected cube gets a material of its own, lit towards
	# white and pulsing, and a glowing disc under it.
	_selected_material = _material(Color.WHITE, 0.4)
	_ring_material = _material(Color.WHITE, 1.0)
	_ring = MeshInstance3D.new()
	var disc := CylinderMesh.new()
	disc.top_radius = 0.5
	disc.bottom_radius = 0.5
	disc.height = 1.0
	_ring.mesh = disc
	_ring.material_override = _ring_material
	_ring.visible = false
	add_child(_ring)


func _spawn() -> void:
	if _units.size() >= MAX_UNITS:
		if _selected == 0:
			_select(-1)
		_units[0].body.queue_free()
		_units.remove_at(0)
		if _selected > 0:
			_selected -= 1
	var u := {
		"angle": _rng.randf_range(0, 6.28),
		"radius": 2.5 + _rng.randf_range(0, 7),
		"speed": (1.0 if _rng.randf() < 0.5 else -1.0) * (0.2 + _rng.randf_range(0, 0.6)),
		"height": 0.5 + _rng.randf_range(0, 1.5),
		"size": 0.6 + _rng.randf_range(0, 0.6),
		"phase": _rng.randf_range(0, 6.28),
		"hue": _spawned % HUES.size(),
	}
	_spawned += 1
	var body := MeshInstance3D.new()
	body.mesh = _cube_mesh
	body.scale = Vector3.ONE * u.size
	body.material_override = _materials[u.hue]
	add_child(body)
	u.body = body
	_units.append(u)


func _clear() -> void:
	_select(-1)
	for u in _units:
		u.body.queue_free()
	_units.clear()


func _select(index: int) -> void:
	if _selected >= 0 and _selected < _units.size():
		_units[_selected].body.material_override = _materials[_units[_selected].hue]
	_selected = index
	if _selected >= 0:
		_units[_selected].body.material_override = _selected_material
		_ring_material.albedo_color = HUES[_units[_selected].hue].lerp(Color.WHITE, 0.25)
	_ring.visible = _selected >= 0


func _unit_pos(u: Dictionary) -> Vector3:
	return Vector3(cos(u.angle) * u.radius, u.height + 0.25 * sin(_time * 2.0 + u.phase), sin(u.angle) * u.radius)


## The unit under `p` (viewport px), nearest first: the camera ray through
## that point against each cube, in the cube's own (rotated, scaled) space.
func _pick(p: Vector2) -> int:
	var origin := _camera.project_ray_origin(p)
	var dir := _camera.project_ray_normal(p)
	var box := AABB(Vector3(-0.5, -0.5, -0.5), Vector3.ONE)
	var best := -1
	var best_distance := 0.0
	for i in _units.size():
		var inv: Transform3D = _units[i].body.global_transform.affine_inverse()
		var hit = box.intersects_ray(inv * origin, inv.basis * dir)
		if hit == null:
			continue
		var d := origin.distance_to(_units[i].body.global_transform * hit)
		if best < 0 or d < best_distance:
			best = i
			best_distance = d
	return best


func _process(delta: float) -> void:
	if not _paused:
		_time += delta
		for u in _units:
			u.angle += u.speed * delta
	_pitch = clampf(_pitch, 0.15, 1.35)
	_camera.position = Vector3(cos(_yaw) * cos(_pitch), sin(_pitch), sin(_yaw) * cos(_pitch)) * _distance
	_camera.look_at(Vector3(0, 0.8, 0))
	for u in _units:
		u.body.position = _unit_pos(u)
		u.body.rotation = Vector3(0, -u.angle, 0)
	if _selected >= 0:
		var u: Dictionary = _units[_selected]
		_selected_material.albedo_color = HUES[u.hue].lerp(Color.WHITE, 0.35 + 0.15 * sin(_time * 6.0))
		var at: Vector3 = u.body.position
		var r: float = u.size * (2.1 + 0.15 * sin(_time * 4.0))
		_ring.position = Vector3(at.x, 0.02, at.z)
		_ring.scale = Vector3(r, 0.01, r)
	if view == null:
		return
	_publish_frame_data()
	_count_frame(Time.get_ticks_usec() / 1000.0)
	_publish_state()


func _publish_frame_data() -> void:
	var size := view.size
	if _entities.size() != _units.size() * STRIDE:
		_entities.resize(_units.size() * STRIDE)
	for i in _units.size():
		var u: Dictionary = _units[i]
		var p := _unit_pos(u)
		p.y += u.size * 0.5 + 0.35
		var s := _camera.unproject_position(p)
		var visible := not _camera.is_position_behind(p) and s.x > -60 and s.y > -60 and s.x < size.x + 60 and s.y < size.y + 60
		_entities[i * STRIDE] = roundf(s.x)
		_entities[i * STRIDE + 1] = roundf(s.y)
		_entities[i * STRIDE + 2] = 1.0 if visible else 0.0
		_entities[i * STRIDE + 3] = i % 7
	view.set_buffer("entities", _entities)


func _count_frame(now_ms: float) -> void:
	_frames_seen += 1
	if _frames_seen <= FPS_WARMUP:
		return
	var n := _frames_seen - FPS_WARMUP
	_frame_times[(n - 1) % FPS_WINDOW] = now_ms
	if n < 20:
		return
	var oldest: float = _frame_times[0 if n < FPS_WINDOW else n % FPS_WINDOW]
	var count := mini(n, FPS_WINDOW)
	if now_ms <= oldest or (_fps_shown_at >= 0 and now_ms - _fps_shown_at < 250):
		return
	_fps = roundi((count - 1) * 1000.0 / (now_ms - oldest))
	_fps_shown_at = now_ms


func _publish_state() -> void:
	# The JSON only when something in it changed (it is built every frame
	# otherwise: a Dictionary and JSON.stringify are most of this script's
	# per-frame cost on the web).
	var renderer: String = view.get_renderer()
	var key := [renderer, _fps, _units.size(), _paused, _selected, _clicks, _wheel, _keys]
	if key == _last_state:
		return
	_last_state = key
	view.set_state_json("game", JSON.stringify({
		"engine": _engine,
		"renderer": renderer,
		"fps": _fps,
		"entities": _units.size(),
		"paused": _paused,
		"selected": _selected,
		"passed": {"clicks": _clicks, "wheel": _wheel, "keys": _keys, "pad": 0},
	}))


func _on_ui_message(name: String, data, _json: String) -> void:
	match name:
		"game:spawn":
			for i in clampi(int(data.get("count", 1)), 1, 50):
				_spawn()
		"game:clear":
			_clear()
		"game:pause":
			_paused = bool(data.get("paused", not _paused))
		"game:quit":
			get_tree().quit(0)


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouseButton:
		var b := event as InputEventMouseButton
		if b.button_index == MOUSE_BUTTON_LEFT:
			if b.pressed:
				_dragging = true
				_press_at = b.position
				_last_pointer = b.position
				_travel = 0.0
				_clicks += 1
			elif _dragging:
				_dragging = false
				_travel = maxf(_travel, b.position.distance_to(_press_at))
				if _travel <= CLICK_SLOP:
					_select(_pick(b.position))
		elif b.pressed and (b.button_index == MOUSE_BUTTON_WHEEL_UP or b.button_index == MOUSE_BUTTON_WHEEL_DOWN):
			_distance = clampf(_distance * (0.9 if b.button_index == MOUSE_BUTTON_WHEEL_UP else 1.1), 6.0, 40.0)
			_wheel += 1
	elif event is InputEventMouseMotion and _dragging:
		var m := event as InputEventMouseMotion
		_yaw -= (m.position.x - _last_pointer.x) * 0.006
		_pitch += (m.position.y - _last_pointer.y) * 0.006
		_travel = maxf(_travel, m.position.distance_to(_press_at))
		_last_pointer = m.position
	elif event is InputEventKey and event.pressed and not event.echo:
		if event.keycode == KEY_SPACE:
			_spawn()
		elif event.keycode == KEY_ESCAPE:
			_paused = not _paused
		else:
			return
		_keys += 1
