# SPDX-License-Identifier: Apache-2.0
# Connects to a Tessera server and draws a sphere for every entity it replicates. With the server started with
# --players, each connected player has a sphere too, and WASD moves yours.
extends Node3D

const DEFAULT_HOST := "127.0.0.1"
const DEFAULT_PORT := 7777
# How many times a second the input is sent, and how far the keys push (the server reads -127 to 127).
const INPUT_RATE := 30.0
const AXIS_MAX := 127
const OWN_COLOR := Color(0.2, 0.85, 0.3)
const OTHER_PLAYER_COLOR := Color(0.95, 0.6, 0.15)

var client := TesseraClient.new()
var spheres := {}  # entity id -> MeshInstance3D
var status := Label.new()
var input_clock := 0.0


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
		var position = client.get_property(id, "position")
		if position != null:
			spheres[id].position = position
	status.text = "%s, %d entities %s\nWASD moves the green sphere; it follows your input after a round trip" % [
		_state_name(), spheres.size(), client.get_rejection()]


# Sends the keys as two bytes, the axis along x and the axis along z, 30 times a second while connected. The
# server reads each as a number from -127 to 127 and moves the player's entity; this demo does not predict where
# it will be, so the sphere moves when the server's answer arrives.
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
		material.albedo_color = OWN_COLOR if owner == client.get_connection_id() else OTHER_PLAYER_COLOR
		sphere.material_override = material
	add_child(sphere)
	spheres[id] = sphere


func _on_entity_removed(id: int) -> void:
	if spheres.has(id):
		spheres[id].queue_free()
		spheres.erase(id)


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
