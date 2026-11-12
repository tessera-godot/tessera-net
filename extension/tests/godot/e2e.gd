# SPDX-License-Identifier: Apache-2.0
# Runs the extension against the example server in a headless Godot. Started by run_godot_e2e.py, which gives the
# port of a running example server and the port of a place where nothing listens.
extends SceneTree

# The hash of the example server's schema (one type with a position of step 0.01 and range 2048).
const EXAMPLE_SCHEMA_HASH := 1615724867110258213
const WORLD_SIDE := 200.0
const STEP := 0.01
const LIMIT_MS := 20000

var port := 0
var dead_port := 0
var failures := 0
var started_ms := 0


func _initialize() -> void:
	for argument in OS.get_cmdline_user_args():
		if argument.begins_with("--port="):
			port = int(argument.substr(7))
		elif argument.begins_with("--dead_port="):
			dead_port = int(argument.substr(12))
	if port == 0 or dead_port == 0:
		print("FAIL setup: --port and --dead_port are needed")
		quit(2)
		return
	started_ms = Time.get_ticks_msec()
	await run_all()
	if failures == 0:
		print("E2E PASS")
		quit(0)
	else:
		print("E2E FAIL")
		quit(1)


func run_all() -> void:
	check_classes_exist()
	check_misuse_before_connect()
	check_schema_hash()
	check_schema_errors_name_the_field()
	check_connect_to_bad_arguments()
	var main_client: TesseraClient = await check_connects_and_sees_entities()
	if main_client != null:
		check_values_are_right(main_client)
		await check_converges_with_the_server(main_client)
	await check_signal_poll_inside_signal_is_ignored()
	await check_signal_disconnect_inside_signal_stops_the_rest()
	await check_schema_mismatch_is_rejected()
	await check_several_clients_at_once()
	await check_server_unreachable_is_a_state_not_a_crash()


func ok(check: String) -> void:
	print("OK ", check)


func fail(check: String, detail: String) -> void:
	failures += 1
	print("FAIL ", check, ": ", detail)


func expect(check: String, condition: bool, detail: String = "") -> bool:
	if condition:
		return true
	fail(check, detail)
	return false


func finish(check: String, before: int) -> void:
	if failures == before:
		ok(check)


# Reads the clients every frame until the condition holds or the time is up.
func pump(clients: Array, condition: Callable, seconds: float) -> bool:
	var deadline := Time.get_ticks_msec() + int(seconds * 1000.0)
	while Time.get_ticks_msec() < deadline:
		if Time.get_ticks_msec() - started_ms > LIMIT_MS + 10000:
			return false
		for client in clients:
			client.poll()
		if condition.call():
			return true
		OS.delay_msec(2)
		await process_frame
	return false


func make_schema(step: float = STEP) -> TesseraSchema:
	var schema := TesseraSchema.new()
	schema.add_type(1, [{"name": "position", "kind": "position", "step": step, "range": 2048.0}])
	return schema


func make_client(step: float = STEP) -> TesseraClient:
	var client := TesseraClient.new()
	client.schema = make_schema(step)
	return client


func within_world(position: Vector3) -> bool:
	return (
		position.x >= -STEP - 0.001 and position.x <= WORLD_SIDE + STEP + 0.001
		and position.z >= -STEP - 0.001 and position.z <= WORLD_SIDE + STEP + 0.001
		and absf(position.y) <= STEP + 0.001
	)


func check_classes_exist() -> void:
	var name := "classes_exist"
	var before := failures
	expect(name, ClassDB.class_exists("TesseraClient"), "TesseraClient is missing")
	expect(name, ClassDB.class_exists("TesseraSchema"), "TesseraSchema is missing")
	finish(name, before)


func check_misuse_before_connect() -> void:
	var name := "misuse_before_connect"
	var before := failures
	var client := TesseraClient.new()
	expect(name, client.get_state() == TesseraClient.State.DISCONNECTED, "a new client is not disconnected")
	for _pass in 3:
		client.poll()
		expect(name, client.get_entities().size() == 0, "entities before connecting")
		expect(name, client.get_property(1, "position") == null, "a property before connecting")
		expect(name, client.get_snapshot().is_empty(), "a snapshot before connecting")
		expect(name, client.get_rejection() == "", "a rejection before connecting")
		client.disconnect_from_server()
	expect(name, client.get_state() == TesseraClient.State.DISCONNECTED, "the state changed")
	finish(name, before)


func check_schema_hash() -> void:
	var name := "schema_hash"
	var before := failures
	var schema := make_schema()
	expect(name, schema.is_valid(), "the schema of the demo is not valid: " + schema.get_error())
	expect(name, schema.get_hash() == EXAMPLE_SCHEMA_HASH, "hash %d, the example server has %d" % [schema.get_hash(), EXAMPLE_SCHEMA_HASH])
	finish(name, before)


func check_schema_errors_name_the_field() -> void:
	var name := "schema_errors_name_the_field"
	var before := failures
	var position := {"name": "position", "kind": "position", "step": 0.01, "range": 2048.0}
	var bad_cases := [
		{"label": "unknown kind", "properties": [{"name": "hp", "kind": "float"}], "needle": "hp"},
		{"label": "step 0", "properties": [{"name": "position", "kind": "position", "step": 0.0, "range": 2048.0}], "needle": "position"},
		{"label": "min above max", "properties": [{"name": "hp", "kind": "integer", "min": 5, "max": 1, "signed": true}], "needle": "hp"},
		{"label": "repeated name", "properties": [position, {"name": "position", "kind": "flag"}], "needle": "position"},
		{"label": "no properties", "properties": [], "needle": "type 1"},
		{"label": "unknown key", "properties": [{"name": "hp", "kind": "integer", "mn": 1}], "needle": "mn"},
	]
	for bad in bad_cases:
		var schema := TesseraSchema.new()
		var result: int = schema.add_type(1, bad["properties"])
		expect(name, result == ERR_INVALID_PARAMETER, bad["label"] + ": add_type answered " + str(result))
		expect(name, schema.get_error().contains(bad["needle"]), bad["label"] + ": the message is \"" + schema.get_error() + "\"")
		expect(name, not schema.is_valid(), bad["label"] + ": the schema is valid after a refused type")
	var schema := TesseraSchema.new()
	schema.add_type(1, [{"name": "hp", "kind": "float"}])
	expect(name, schema.add_type(1, [position]) == OK, "a good type is refused after a bad one")
	expect(name, schema.is_valid(), "the schema is not valid after a good type")
	finish(name, before)


func check_connect_to_bad_arguments() -> void:
	var name := "connect_to_bad_arguments"
	var before := failures
	var client := make_client()
	for bad_port in [0, 70000, -5]:
		expect(name, client.connect_to("127.0.0.1", bad_port) == ERR_INVALID_PARAMETER, "port %d was accepted" % bad_port)
		expect(name, client.get_last_error() != "", "no message for port %d" % bad_port)
	var no_schema := TesseraClient.new()
	expect(name, no_schema.connect_to("127.0.0.1", port) == ERR_INVALID_PARAMETER, "connected without a schema")
	expect(name, no_schema.get_last_error() != "", "no message without a schema")
	var empty_schema := TesseraClient.new()
	empty_schema.schema = TesseraSchema.new()
	expect(name, empty_schema.connect_to("127.0.0.1", port) == ERR_INVALID_PARAMETER, "connected with an empty schema")
	finish(name, before)


func check_connects_and_sees_entities() -> TesseraClient:
	var name := "connects_and_sees_entities"
	var before := failures
	var client := make_client()
	var counts := {"connected": 0, "appeared": 0, "removed": 0, "disconnected": 0}
	client.connected.connect(func(): counts["connected"] += 1)
	client.entity_appeared.connect(func(_id): counts["appeared"] += 1)
	client.entity_removed.connect(func(_id): counts["removed"] += 1)
	client.disconnected.connect(func(): counts["disconnected"] += 1)
	expect(name, client.connect_to("127.0.0.1", port) == OK, "connect_to failed: " + client.get_last_error())
	var arrived: bool = await pump([client], func(): return client.get_state() == TesseraClient.State.CONNECTED and client.get_entities().size() == 100, 10.0)
	expect(name, arrived, "state %d with %d entities after 10 s" % [client.get_state(), client.get_entities().size()])
	expect(name, counts["connected"] == 1, "connected was emitted %d times" % counts["connected"])
	expect(name, counts["appeared"] == 100, "entity_appeared was emitted %d times" % counts["appeared"])
	expect(name, counts["removed"] == 0 and counts["disconnected"] == 0, "unexpected removal or disconnect")
	finish(name, before)
	return client if arrived else null


func check_values_are_right(client: TesseraClient) -> void:
	var name := "values_are_right"
	var before := failures
	var entities := client.get_entities()
	for id in entities:
		var position = client.get_property(id, "position")
		if not expect(name, position is Vector3, "the position of %d is %s" % [id, str(position)]):
			break
		if not expect(name, within_world(position), "the position of %d is outside the world: %s" % [id, str(position)]):
			break
	expect(name, client.get_property(entities[0], "nothing") == null, "an unknown property is not null")
	expect(name, client.get_property(12345, "position") == null, "an unknown entity is not null")
	var snapshot := client.get_snapshot()
	expect(name, snapshot.size() == entities.size(), "the snapshot has %d entities, not %d" % [snapshot.size(), entities.size()])
	for id in entities:
		if not expect(name, snapshot.has(id), "the snapshot lacks %d" % id):
			break
		expect(name, snapshot[id]["position"] is Vector3, "the snapshot value is not a Vector3")
	finish(name, before)


func check_converges_with_the_server(client: TesseraClient) -> void:
	# The server is another process, so there is no exact position to compare with: what is checked is that the
	# state arrives and keeps changing while the entities walk. The exact comparison is done in C++.
	var name := "converges_with_the_server"
	var before := failures
	var entities := client.get_entities()
	var first := {}
	for id in entities:
		first[id] = client.get_property(id, "position")
	await pump([client], func(): return false, 2.0)
	var changed := 0
	for id in entities:
		var now = client.get_property(id, "position")
		if now == null:
			fail(name, "entity %d lost its state" % id)
			break
		if not within_world(now):
			fail(name, "entity %d left the world: %s" % [id, str(now)])
			break
		if not now.is_equal_approx(first[id]):
			changed += 1
	expect(name, changed > 0, "no entity moved in 2 s")
	expect(name, client.get_state() == TesseraClient.State.CONNECTED, "the client is no longer connected")
	finish(name, before)
	client.disconnect_from_server()


func check_signal_poll_inside_signal_is_ignored() -> void:
	var name := "signal_poll_inside_signal_is_ignored"
	var before := failures
	var client := make_client()
	var counts := {"connected": 0, "appeared": 0, "appeared_inside_poll": 0}
	client.connected.connect(func():
		counts["connected"] += 1
		# The entities arrive after the connection is made. Reading again from inside this signal, for a while,
		# must not send any of their signals from in here.
		var appeared_before: int = counts["appeared"]
		for _pass in 30:
			client.poll()
			OS.delay_msec(10)
		counts["appeared_inside_poll"] += counts["appeared"] - appeared_before)
	client.entity_appeared.connect(func(_id):
		counts["appeared"] += 1
		client.poll())
	client.connect_to("127.0.0.1", port)
	var arrived: bool = await pump([client], func(): return client.get_state() == TesseraClient.State.CONNECTED and client.get_entities().size() == 100, 10.0)
	expect(name, arrived, "the client did not get its entities")
	expect(name, counts["connected"] == 1, "connected was emitted %d times" % counts["connected"])
	expect(name, counts["appeared"] == 100, "entity_appeared was emitted %d times" % counts["appeared"])
	expect(name, counts["appeared_inside_poll"] == 0, "poll() inside a signal sent %d signals" % counts["appeared_inside_poll"])
	client.disconnect_from_server()
	finish(name, before)


func check_signal_disconnect_inside_signal_stops_the_rest() -> void:
	var name := "signal_disconnect_inside_signal_stops_the_rest"
	var before := failures
	var client := make_client()
	var counts := {"appeared": 0, "disconnected": 0}
	client.entity_appeared.connect(func(_id):
		counts["appeared"] += 1
		if counts["appeared"] == 1:
			client.disconnect_from_server())
	client.disconnected.connect(func(): counts["disconnected"] += 1)
	client.connect_to("127.0.0.1", port)
	await pump([client], func(): return counts["appeared"] > 0, 10.0)
	await pump([client], func(): return false, 0.5)
	expect(name, counts["appeared"] == 1, "entity_appeared was emitted %d times" % counts["appeared"])
	expect(name, client.get_state() == TesseraClient.State.DISCONNECTED, "the client is not disconnected")
	expect(name, counts["disconnected"] == 0, "disconnected was emitted for a call the script made")
	expect(name, client.get_entities().size() == 0, "the client still has entities")
	finish(name, before)


func check_schema_mismatch_is_rejected() -> void:
	var name := "schema_mismatch_is_rejected"
	var before := failures
	var client := make_client(0.02)
	var reasons := []
	client.rejected.connect(func(reason): reasons.append(reason))
	client.connect_to("127.0.0.1", port)
	var rejected: bool = await pump([client], func(): return client.get_state() == TesseraClient.State.REJECTED, 10.0)
	expect(name, rejected, "the client was not rejected")
	expect(name, reasons.size() == 1, "rejected was emitted %d times" % reasons.size())
	if reasons.size() == 1:
		expect(name, reasons[0].contains("schema"), "the reason does not talk about the schema: " + reasons[0])
		expect(name, client.get_rejection() == reasons[0], "get_rejection() differs from the signal")
	expect(name, client.get_entities().size() == 0, "a rejected client has entities")
	client.disconnect_from_server()
	finish(name, before)


func check_several_clients_at_once() -> void:
	var name := "several_clients_at_once"
	var before := failures
	var a := make_client()
	var b := make_client()
	var c := make_client()
	for client in [a, b, c]:
		client.connect_to("127.0.0.1", port)
	var ready: bool = await pump([a, b, c], func(): return a.get_entities().size() == 100 and b.get_entities().size() == 100 and c.get_entities().size() == 100, 10.0)
	expect(name, ready, "the three clients did not get their entities")
	# Let go of them in another order than they were made, without disconnecting: the extension has to do it.
	b = null
	await pump([a, c], func(): return false, 0.3)
	var before_c = c.get_property(c.get_entities()[0], "position")
	a = null
	await pump([c], func(): return false, 1.0)
	var after_c = c.get_property(c.get_entities()[0], "position")
	expect(name, c.get_state() == TesseraClient.State.CONNECTED, "the last client lost its connection")
	expect(name, before_c != null and after_c != null and not before_c.is_equal_approx(after_c), "the last client stopped receiving")
	c = null
	finish(name, before)


func check_server_unreachable_is_a_state_not_a_crash() -> void:
	var name := "server_unreachable_is_a_state_not_a_crash"
	var before := failures
	var client := make_client()
	var counts := {"disconnected": 0}
	client.disconnected.connect(func(): counts["disconnected"] += 1)
	expect(name, client.connect_to("127.0.0.1", dead_port) == OK, "connect_to failed: " + client.get_last_error())
	var gone: bool = await pump([client], func(): return client.get_state() == TesseraClient.State.DISCONNECTED, 10.0)
	expect(name, gone, "the state is still %d after 10 s" % client.get_state())
	expect(name, counts["disconnected"] == 1, "disconnected was emitted %d times" % counts["disconnected"])
	# A name that does not exist must not throw; whether it fails at once or later is up to the system.
	var strange := make_client()
	var result: int = strange.connect_to("host.invalid", 7777)
	expect(name, result == OK or result == ERR_INVALID_PARAMETER, "connect_to answered %d" % result)
	strange.poll()
	strange.disconnect_from_server()
	finish(name, before)
