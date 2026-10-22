extends Node3D
## The live 3D scene behind the demo's JsuiView: a slowly turning core cube
## with a ring of glowing shards, and a field of cubes drifting up through
## the whole frame. The UI is drawn over it with premultiplied alpha, so the
## scene shows in the gaps and through the showcase's translucent panels.
## The core sits upper right of centre, where the showcase leaves the most
## open space (above the featured carousel and the armory).

const SHARDS := 12
const RING_RADIUS := 2.6
const FIELD := 40
const FIELD_HALF := Vector3(11.0, 6.5, 0.0)
const FIELD_NEAR := -7.0
const FIELD_FAR := -22.0

@onready var _core: Node3D = $Core
@onready var _ring: Node3D = $Core/Ring
@onready var _warm: OmniLight3D = $Warm

var _time := 0.0
var _shards: Array[MeshInstance3D] = []
var _field: Array[MeshInstance3D] = []
var _spin: PackedVector3Array = []
var _rise: PackedFloat32Array = []


func _ready() -> void:
	var gold := _glow_material(Color(0.95, 0.72, 0.32), Color(1.0, 0.62, 0.22), 1.6)
	var blue := _glow_material(Color(0.4, 0.6, 1.0), Color(0.3, 0.5, 1.0), 1.6)
	var shard_mesh := BoxMesh.new()
	shard_mesh.size = Vector3.ONE * 0.3
	for i in SHARDS:
		var shard := _cube(shard_mesh, gold if i % 3 else blue)
		_ring.add_child(shard)
		_shards.append(shard)

	# Seeded, so captures look the same on every run.
	var rng := RandomNumberGenerator.new()
	rng.seed = 7
	var dim := StandardMaterial3D.new()
	dim.albedo_color = Color(0.22, 0.26, 0.4)
	dim.metallic = 0.7
	dim.roughness = 0.35
	var faint := _glow_material(Color(0.5, 0.65, 1.0), Color(0.35, 0.5, 1.0), 0.7)
	var ember := _glow_material(Color(1.0, 0.7, 0.35), Color(1.0, 0.55, 0.2), 0.9)
	for i in FIELD:
		var mesh := BoxMesh.new()
		mesh.size = Vector3.ONE * rng.randf_range(0.12, 0.45)
		var mat: Material = dim
		if i % 5 == 0:
			mat = ember
		elif i % 3 == 0:
			mat = faint
		var c := _cube(mesh, mat)
		c.position = Vector3(rng.randf_range(-1, 1) * FIELD_HALF.x, rng.randf_range(-1, 1) * FIELD_HALF.y, rng.randf_range(FIELD_FAR, FIELD_NEAR))
		c.rotation = Vector3(rng.randf() * TAU, rng.randf() * TAU, 0.0)
		add_child(c)
		_field.append(c)
		_spin.append(Vector3(rng.randf_range(-0.8, 0.8), rng.randf_range(-0.8, 0.8), 0.0))
		_rise.append(rng.randf_range(0.15, 0.45))


func _glow_material(albedo: Color, emission: Color, energy: float) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	m.albedo_color = albedo
	m.metallic = 0.8
	m.roughness = 0.3
	m.emission_enabled = true
	m.emission = emission
	m.emission_energy_multiplier = energy
	return m


func _cube(mesh: Mesh, mat: Material) -> MeshInstance3D:
	var c := MeshInstance3D.new()
	c.mesh = mesh
	c.material_override = mat
	c.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	return c


func _process(delta: float) -> void:
	_time += delta
	var t := _time
	_core.position.y = 2.7 + sin(t * 0.9) * 0.12
	var cube: Node3D = $Core/Cube
	cube.rotation = Vector3(t * 0.35, t * 0.5, 0.0)
	_ring.rotation.y = -t * 0.45
	for i in _shards.size():
		var a := TAU * i / _shards.size()
		var s := _shards[i]
		s.position = Vector3(cos(a) * RING_RADIUS, sin(t * 1.3 + a * 2.0) * 0.3, sin(a) * RING_RADIUS)
		s.rotation = Vector3(t * 1.7 + a, t * 1.1 + a, 0.0)
	for i in _field.size():
		var c := _field[i]
		c.rotation += _spin[i] * delta
		c.position.y += _rise[i] * delta
		if c.position.y > FIELD_HALF.y:
			c.position.y -= FIELD_HALF.y * 2.0
	_warm.light_energy = 4.0 + sin(t * 2.1) * 0.5
