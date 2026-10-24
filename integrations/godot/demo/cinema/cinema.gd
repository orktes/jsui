extends Control
## Host textures demo: pictures Godot makes, shown inside a JSUI UI.
##
##   godot --path integrations/godot/demo res://cinema/cinema.tscn
##   (add `--rendering-method forward_plus` for the GPU path on Vulkan / Metal;
##   the project defaults to Compatibility, where JSUI rasterizes on the CPU
##   and host textures are read back with get_image())
##
## - An in-game TV: a VideoStreamPlayer decodes the Big Buck Bunny trailer
##   (Ogg Theora; run cinema/fetch-video.sh first, without it the TV shows
##   "NO SIGNAL") and its texture is handed to the UI with
##   set_texture("tv", video.get_video_texture(), true). The play / pause /
##   restart / mute buttons and the progress bar are JSUI (ui/cinema.jsx).
## - A character preview: a SubViewport renders a spinning 3D figure every
##   frame, shown in a round JSUI frame with colour swatches.
##
## JSUI plays and renders nothing itself: `live = true` tells the view the
## texture's contents change every frame (a video, a viewport), so it copies
## them each frame instead of waiting for Texture2D.changed.
##
## User arguments (after `--`): --capture <png> [--frames <n>] saves a
## screenshot after n frames (default 180) and quits.

const VIDEO_FILES := [
	"res://cinema/big_buck_bunny_trailer.ogv",
	"user://big_buck_bunny_trailer.ogv",
]
const PREVIEW_SIZE := Vector2i(512, 512)

@onready var ui: JsuiView = $JsuiView

var video: VideoStreamPlayer
var preview: SubViewport
var figure: Node3D
var body_material := StandardMaterial3D.new()
var spinning := true
var color := "#f2c35b"
var capture_path := ""
var capture_frames := 180


func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	for i in args.size():
		if args[i] == "--capture" and i + 1 < args.size():
			capture_path = args[i + 1]
		elif args[i] == "--frames" and i + 1 < args.size():
			capture_frames = int(args[i + 1])

	_build_preview()
	_build_video()
	# Both are live: their contents change every frame.
	ui.set_texture("preview", preview.get_texture(), true)
	if video.stream:
		ui.set_texture("tv", video.get_video_texture(), true)
	ui.message_received.connect(_on_ui_message)
	_publish()


func _process(delta: float) -> void:
	if spinning and figure:
		figure.rotate_y(delta * 1.2)
	if video.stream:
		var length := video.get_stream_length()
		var t := video.stream_position / length if length > 0.0 else 0.0
		ui.set_buffer("cinema.progress", PackedFloat32Array([clampf(t, 0.0, 1.0)]))
	if capture_path != "":
		capture_frames -= 1
		if capture_frames <= 0:
			var path := capture_path
			capture_path = ""
			get_viewport().get_texture().get_image().save_png(path)
			print("saved ", path)
			get_tree().quit()


func _on_ui_message(name: String, data, _json: String) -> void:
	match name:
		"cinema:toggle":
			if not video.stream:
				return
			if not video.is_playing():
				video.play()
			else:
				video.paused = not video.paused
		"cinema:restart":
			if video.stream:
				video.stop()
				video.play()
				video.paused = false
		"cinema:mute":
			video.volume_db = 0.0 if video.volume_db < -60.0 else -80.0
		"preview:color":
			if data is Dictionary and data.has("color"):
				color = str(data["color"])
				body_material.albedo_color = Color.html(color)
		"preview:spin":
			spinning = not spinning
	_publish()


func _publish() -> void:
	ui.set_state("cinema", {
		"hasVideo": video.stream != null,
		"playing": video.stream != null and video.is_playing() and not video.paused,
		"muted": video.volume_db < -60.0,
		"spinning": spinning,
		"color": color,
	})


func _build_video() -> void:
	video = VideoStreamPlayer.new()
	video.visible = false  # only its texture is shown, in the UI
	video.loop = true
	video.finished.connect(_publish)
	for path in VIDEO_FILES:
		if FileAccess.file_exists(path):
			var stream := VideoStreamTheora.new()
			stream.file = path
			video.stream = stream
			break
	if not video.stream:
		print("cinema: no video (run cinema/fetch-video.sh); the TV shows NO SIGNAL")
	add_child(video)
	if video.stream:
		video.play()


## A small stylized figure on a turntable, lit, in a world of its own.
func _build_preview() -> void:
	preview = SubViewport.new()
	preview.size = PREVIEW_SIZE
	preview.own_world_3d = true
	preview.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	preview.msaa_3d = Viewport.MSAA_4X
	add_child(preview)

	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.06, 0.07, 0.12)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.35, 0.38, 0.5)
	var world_env := WorldEnvironment.new()
	world_env.environment = env
	preview.add_child(world_env)

	var camera := Camera3D.new()
	camera.position = Vector3(0, 1.1, 3.4)
	camera.fov = 40
	preview.add_child(camera)
	camera.look_at(Vector3(0, 0.85, 0))

	var key := DirectionalLight3D.new()
	key.rotation_degrees = Vector3(-40, 35, 0)
	key.light_energy = 1.4
	preview.add_child(key)
	var rim := OmniLight3D.new()
	rim.position = Vector3(-1.5, 2.0, -1.5)
	rim.light_color = Color(0.4, 0.8, 1.0)
	rim.light_energy = 2.0
	rim.omni_range = 6.0
	preview.add_child(rim)

	figure = Node3D.new()
	preview.add_child(figure)
	body_material.albedo_color = Color.html(color)
	body_material.roughness = 0.35
	var dark := StandardMaterial3D.new()
	dark.albedo_color = Color(0.12, 0.13, 0.2)
	dark.metallic = 0.6
	dark.roughness = 0.3

	var body_mesh := CapsuleMesh.new()
	body_mesh.radius = 0.32
	body_mesh.height = 1.0
	_part(body_mesh, body_material, Vector3(0, 0.7, 0))
	var head_mesh := SphereMesh.new()
	head_mesh.radius = 0.24
	head_mesh.height = 0.48
	_part(head_mesh, body_material, Vector3(0, 1.45, 0))
	var visor := BoxMesh.new()
	visor.size = Vector3(0.3, 0.1, 0.1)
	_part(visor, dark, Vector3(0, 1.47, 0.2))
	var ring := TorusMesh.new()
	ring.inner_radius = 0.42
	ring.outer_radius = 0.5
	_part(ring, dark, Vector3(0, 0.75, 0)).rotation_degrees = Vector3(12, 0, 0)
	var base := CylinderMesh.new()
	base.top_radius = 0.7
	base.bottom_radius = 0.75
	base.height = 0.12
	_part(base, dark, Vector3(0, 0.06, 0))


func _part(mesh: Mesh, material: Material, at: Vector3) -> MeshInstance3D:
	var m := MeshInstance3D.new()
	m.mesh = mesh
	m.material_override = material
	m.position = at
	figure.add_child(m)
	return m
