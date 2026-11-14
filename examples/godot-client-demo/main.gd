# SPDX-License-Identifier: Apache-2.0
# Connects to a Tessera server and draws a sphere for every entity it replicates. With the server started with
# --players, each connected player has a sphere too, and WASD moves yours: the green sphere moves at once, on the
# client's own guess of where the server will put it, and the server's answer corrects the guess. The other
# spheres are shown a few ticks in the past, smoothed between the two states of the server around that time.
extends Node3D

const DEFAULT_HOST := "127.0.0.1"
const DEFAULT_PORT := 7777
# How many times a second the input is sent, and how far the keys push (the server reads -127 to 127).
const INPUT_RATE := 30.0
const AXIS_MAX := 127
# The movement rule of the server, which the client needs to move the sphere before the answer comes.
const PLAYER_SPEED := 8.0
const WORLD_SIDE := 200.0
# The server runs at 30 ticks a second. The other spheres are shown this many ticks behind the newest state, so
# that there are two states to move between: 2 ticks is about 66 ms.
const TICK_RATE := 30.0
const INTERPOLATION_DELAY_TICKS := 2
const INTERPOLATION_SNAP_DISTANCE := 2.5
const OWN_COLOR := Color(0.2, 0.85, 0.3)
const OTHER_PLAYER_COLOR := Color(0.95, 0.6, 0.15)

var client := TesseraClient.new()
var spheres := {}  # entity id -> MeshInstance3D
var status := Label.new()
var input_clock := 0.0
var own_entity := -1  # the entity of this player, once the server has told us about it


func _ready() -> void:
	# The schema has to describe the entities exactly as the server does: the same types, the same properties in
	# the same order, and the same kind and numbers for each one.
	var schema := TesseraSchema.new()
	schema.add_type(1, [{"name": "position", "kind": "position", "step": 0.01, "range": 2048.0}])
	# The players of a server started with --players: a position and the connection that owns the entity.
	schema.add_type(2, [
		{"name": "position", "kind": "position", "step": 0.01, "range": 2048.0},
		{"name": "owner", "kind": "integer", "min": 0, "max": 65535, "signed": false},
	])
	client.schema = schema

	# Before connecting is fine: the setting stays through a reconnection.
	if client.set_interpolation(INTERPOLATION_DELAY_TICKS, INTERPOLATION_SNAP_DISTANCE) != OK:
		push_warning("No interpolation: " + client.get_last_error())

	client.entity_appeared.connect(_on_entity_appeared)
	client.entity_removed.connect(_on_entity_removed)

	var host := DEFAULT_HOST
	var port := DEFAULT_PORT
	for argument in OS.get_cmdline_user_args():
		if argument.begins_with("--host="):
			host = argument.substr(7)
		elif argument.begins_with("--port="):
			port = int(argument.substr(7))

	if client.connect_to(host, port) != OK:
		push_error("Could not connect: " + client.get_last_error())

	_build_scene()


func _process(delta: float) -> void:
	# Nothing is read from the network, and no signal is sent, until poll() is called.
	client.poll()
	_send_input(delta)
	for id in spheres:
		# Your own sphere shows the prediction; the others show the interpolated value, a little in the past.
		var position = client.get_predicted_property(id, "position") if id == own_entity else client.get_interpolated_property(id, "position")
		if position != null:
			spheres[id].position = position
	var stats := client.get_prediction_stats()
	status.text = "%s, %d entities %s\nprediction: %s, error %.2f m\ninterpolation: %d ticks (%d ms)\nWASD moves the green sphere at once; the others are shown %d ms in the past, smoothed" % [
		_state_name(), spheres.size(), client.get_rejection(),
		"on" if client.is_predicting() else "off", stats["last_error"],
		INTERPOLATION_DELAY_TICKS, _delay_ms(), _delay_ms()]


# Sends the keys as two bytes, the axis along x and the axis along z, 30 times a second while connected, zeros
# included: the prediction counts one step for each input, so the client sends one for every tick of the server.
# The server reads each as a number from -127 to 127 and moves the player's entity.
func _send_input(delta: float) -> void:
	if client.get_state() != TesseraClient.State.CONNECTED:
		return
	input_clock += delta
	if input_clock < 1.0 / INPUT_RATE:
		return
	input_clock = fmod(input_clock, 1.0 / INPUT_RATE)
	var x := 0
	var z := 0
	if Input.is_physical_key_pressed(KEY_D):
		x += AXIS_MAX
	if Input.is_physical_key_pressed(KEY_A):
		x -= AXIS_MAX
	if Input.is_physical_key_pressed(KEY_S):
		z += AXIS_MAX  # the camera looks toward -z, so S moves toward it
	if Input.is_physical_key_pressed(KEY_W):
		z -= AXIS_MAX
	client.send_input(PackedByteArray([x & 0xFF, z & 0xFF]))


func _on_entity_appeared(id: int) -> void:
	var sphere := MeshInstance3D.new()
	var mesh := SphereMesh.new()
	mesh.radius = 1.0
	mesh.height = 2.0
	sphere.mesh = mesh
	# Only a player has an owner. Yours is green and the others are orange.
	var owner = client.get_property(id, "owner")
	if owner != null:
		var material := StandardMaterial3D.new()
		var is_own: bool = owner == client.get_connection_id()
		material.albedo_color = OWN_COLOR if is_own else OTHER_PLAYER_COLOR
		sphere.material_override = material
		if is_own:
			own_entity = id
			# The prediction starts with the first state that arrives after this, which the server sends as soon
			# as it has an input to confirm.
			if client.set_prediction(id, PackedStringArray(["position"]), predict) != OK:
				push_warning("No prediction: " + client.get_last_error())
	add_child(sphere)
	spheres[id] = sphere


# The movement rule of the example server, for one input and one step of dt seconds: the two bytes are the axes
# along x and z (-127 to 127), the speed is at most PLAYER_SPEED and the player stays inside the world. This has
# to stay the same as the rule on the server; where the two differ, the server wins and the sphere is corrected.
# The two agree for the 2-byte inputs this script sends. A shorter input is ignored by the server, which keeps
# the last axes, while this rule does not move at all; the demo never sends one.
func predict(state: Dictionary, input: PackedByteArray, dt: float) -> Dictionary:
	if input.size() < 2:
		return state
	var position: Vector3 = state["position"]
	var dx := maxi(input.decode_s8(0), -127) / 127.0
	var dz := maxi(input.decode_s8(1), -127) / 127.0
	var length := sqrt(dx * dx + dz * dz)
	if length == 0.0:
		return state
	if length > 1.0:
		dx /= length
		dz /= length
	position.x = clampf(position.x + dx * PLAYER_SPEED * dt, 0.0, WORLD_SIDE)
	position.z = clampf(position.z + dz * PLAYER_SPEED * dt, 0.0, WORLD_SIDE)
	return {"position": position}


func _on_entity_removed(id: int) -> void:
	if id == own_entity:
		own_entity = -1
	if spheres.has(id):
		spheres[id].queue_free()
		spheres.erase(id)


# How far in the past the other spheres are, in whole milliseconds (2 ticks at 30 a second is 66).
func _delay_ms() -> int:
	return floori(INTERPOLATION_DELAY_TICKS * 1000.0 / TICK_RATE)


func _state_name() -> String:
	match client.get_state():
		TesseraClient.State.CONNECTING:
			return "connecting"
		TesseraClient.State.HANDSHAKING:
			return "handshaking"
		TesseraClient.State.CONNECTED:
			return "connected"
		TesseraClient.State.REJECTED:
			return "rejected"
	return "disconnected"


func _build_scene() -> void:
	# The example server keeps its entities in a 200 by 200 square.
	var camera := Camera3D.new()
	add_child(camera)
	camera.position = Vector3(100, 170, 260)
	camera.look_at_from_position(camera.position, Vector3(100, 0, 100))
	camera.far = 1000.0

	var light := DirectionalLight3D.new()
	add_child(light)
	light.rotation_degrees = Vector3(-60, 30, 0)

	var layer := CanvasLayer.new()
	add_child(layer)
	status.position = Vector2(12, 8)
	layer.add_child(status)
