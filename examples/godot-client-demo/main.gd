# SPDX-License-Identifier: Apache-2.0
# Connects to a Tessera server and draws a sphere for every entity it replicates.
extends Node3D

const DEFAULT_HOST := "127.0.0.1"
const DEFAULT_PORT := 7777

var client := TesseraClient.new()
var spheres := {}  # entity id -> MeshInstance3D
var status := Label.new()


func _ready() -> void:
	# The schema has to describe the entities exactly as the server does: the same types, the same properties in
	# the same order, and the same step and range for each position.
	var schema := TesseraSchema.new()
	schema.add_type(1, [{"name": "position", "kind": "position", "step": 0.01, "range": 2048.0}])
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


func _process(_delta: float) -> void:
	# Nothing is read from the network, and no signal is sent, until poll() is called.
	client.poll()
	for id in spheres:
		var position = client.get_property(id, "position")
		if position != null:
			spheres[id].position = position
	status.text = "%s, %d entities %s" % [_state_name(), spheres.size(), client.get_rejection()]


func _on_entity_appeared(id: int) -> void:
	var sphere := MeshInstance3D.new()
	var mesh := SphereMesh.new()
	mesh.radius = 1.0
	mesh.height = 2.0
	sphere.mesh = mesh
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
