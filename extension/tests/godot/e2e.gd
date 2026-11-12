# SPDX-License-Identifier: Apache-2.0
# Runs the extension against the example server in a headless Godot. Started by run_godot_e2e.py, which gives the
# port of a running example server, the port of a place where nothing listens and the port of a second example
# server that the script stops when this one prints STOP_SERVER (to see what a client does when its server goes).
extends SceneTree

# The hash of the example server's schema (one type with a position of step 0.01 and range 2048).
const EXAMPLE_SCHEMA_HASH := 1615724867110258213
const WORLD_SIDE := 200.0
const STEP := 0.01
const LIMIT_MS := 40000
# What the example server does with its entities (server/example/example_game.hpp): 100 of them, each starting at
# InitialPosition(index) and walking at its own speed, 30 ticks a second.
const ENTITY_COUNT := 100
const TICK := 1.0 / 30.0
const MAX_SPEED := 5.0

var port := 0
var dead_port := 0
var doomed_port := 0
var failures := 0
var started_ms := 0


func _initialize() -> void:
	for argument in OS.get_cmdline_user_args():
		if argument.begins_with("--port="):
			port = int(argument.substr(7))
		elif argument.begins_with("--dead_port="):
			dead_port = int(argument.substr(12))
		elif argument.begins_with("--doomed_port="):
			doomed_port = int(argument.substr(14))
	if port == 0 or dead_port == 0 or doomed_port == 0:
		print("FAIL setup: --port, --dead_port and --doomed_port are needed")
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
		check_values_match_the_example_game(main_client)
		await check_converges_with_the_server(main_client)
	await check_signal_poll_inside_signal_is_ignored()
	await check_signal_disconnect_inside_signal_stops_the_rest()
	await check_schema_mismatch_is_rejected()
	await check_several_clients_at_once()
	await check_server_unreachable_is_a_state_not_a_crash()
	check_unresolvable_host()
	check_schema_survives_duplicate_and_save()
	await check_server_going_away_removes_entities_then_disconnects()
	await check_works_next_to_godots_own_enet()


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
		{"label": "integer without max", "properties": [{"name": "hp", "kind": "integer", "min": 0}], "needle": "max"},
		{"label": "integer without min", "properties": [{"name": "hp", "kind": "integer", "max": 9}], "needle": "min"},
		{"label": "integer without both", "properties": [{"name": "hp", "kind": "integer"}], "needle": "hp"},
		{"label": "flag with bits", "properties": [{"name": "alive", "kind": "flag", "bits": 3}], "needle": "bits"},
		{"label": "position with count", "properties": [{"name": "position", "kind": "position", "step": 0.01, "range": 2048.0, "count": 4}], "needle": "count"},
		{"label": "angle with step", "properties": [{"name": "heading", "kind": "angle", "bits": 8, "step": 0.1}], "needle": "step"},
		{"label": "choice with signed", "properties": [{"name": "team", "kind": "choice", "count": 4, "signed": true}], "needle": "signed"},
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
	finish(name, before)


# A name that cannot be resolved: connect_to answers with an error at once (the lookup blocks until the system
# gives up), the client stays disconnected and no signal is sent, because nothing was ever connected.
func check_unresolvable_host() -> void:
	var name := "unresolvable_host"
	var before := failures
	var client := make_client()
	var signals := {"count": 0}
	client.disconnected.connect(func(): signals["count"] += 1)
	client.rejected.connect(func(_reason): signals["count"] += 1)
	var result: int = client.connect_to("host.invalid", 7777)
	expect(name, result == ERR_INVALID_PARAMETER, "connect_to answered %d" % result)
	expect(name, client.get_last_error().contains("host.invalid"), "the message is \"" + client.get_last_error() + "\"")
	expect(name, client.get_state() == TesseraClient.State.DISCONNECTED, "the state is %d" % client.get_state())
	for _pass in 5:
		client.poll()
		OS.delay_msec(10)
	expect(name, signals["count"] == 0, "%d signals were sent" % signals["count"])
	finish(name, before)



# The example game's own pseudo-random numbers (a 32-bit hash of the entity's index), written again here.
func example_unit(index: int, which: int) -> float:
	var mask := 0xFFFFFFFF
	var h: int = ((index * 4 + which + 1) * 0x9E3779B9) & mask
	h = h ^ (h >> 16)
	h = (h * 0x85EBCA6B) & mask
	h = h ^ (h >> 13)
	h = (h * 0xC2B2AE35) & mask
	h = h ^ (h >> 16)
	return float(h >> 8) / 16777216.0


# Plays the example game again on this side (start positions, speeds, bouncing on the edges of the square), one
# tick at a time, and finds the tick where it best agrees with what the client holds. A swapped axis, a wrong scale
# or a wrong step cannot agree with it at any tick.
func check_values_match_the_example_game(client: TesseraClient) -> void:
	var name := "values_match_the_example_game"
	var before := failures
	var snapshot := client.get_snapshot()
	expect(name, snapshot.size() == ENTITY_COUNT, "the snapshot has %d entities" % snapshot.size())
	var seen_x := PackedFloat64Array()
	var seen_z := PackedFloat64Array()
	seen_x.resize(ENTITY_COUNT)
	seen_z.resize(ENTITY_COUNT)
	for id in snapshot:
		var index: int = (id >> 16) & 0xFFFFFFF  # the index field of an entity id
		if not expect(name, index < ENTITY_COUNT, "entity %d has index %d" % [id, index]):
			return
		var position: Vector3 = snapshot[id]["position"]
		seen_x[index] = position.x
		seen_z[index] = position.z
		expect(name, absf(position.y) < 0.001, "the height of entity %d is %f, the example keeps it at 0" % [index, position.y])
	var x := PackedFloat64Array()
	var z := PackedFloat64Array()
	var vx := PackedFloat64Array()
	var vz := PackedFloat64Array()
	for index in ENTITY_COUNT:
		x.append(example_unit(index, 0) * WORLD_SIDE)
		z.append(example_unit(index, 1) * WORLD_SIDE)
		vx.append((example_unit(index, 2) * 2.0 - 1.0) * MAX_SPEED)
		vz.append((example_unit(index, 3) * 2.0 - 1.0) * MAX_SPEED)
	var best_error := INF
	var best_tick := -1
	for tick in 2400:
		var worst := 0.0
		for index in ENTITY_COUNT:
			worst = maxf(worst, maxf(absf(x[index] - seen_x[index]), absf(z[index] - seen_z[index])))
		if worst < best_error:
			best_error = worst
			best_tick = tick
		for index in ENTITY_COUNT:
			x[index] += vx[index] * TICK
			z[index] += vz[index] * TICK
			if x[index] < 0.0 or x[index] > WORLD_SIDE:
				vx[index] = -vx[index]
				x[index] = clampf(x[index], 0.0, WORLD_SIDE)
			if z[index] < 0.0 or z[index] > WORLD_SIDE:
				vz[index] = -vz[index]
				z[index] = clampf(z[index], 0.0, WORLD_SIDE)
	print("INFO best agreement with the example game: tick %d, largest difference %f m" % [best_tick, best_error])
	# A tick of walking is at most 5 / 30 = 0.17 m; the entities are not all read at the same tick.
	expect(name, best_error < 0.5, "the largest difference to the example game is %f m (best at tick %d)" % [best_error, best_tick])
	finish(name, before)


func check_schema_survives_duplicate_and_save() -> void:
	var name := "schema_survives_duplicate_and_save"
	var before := failures
	var schema := TesseraSchema.new()
	schema.add_type(1, [{"name": "position", "kind": "position", "step": STEP, "range": 2048.0}])
	schema.add_type(2, [{"name": "hp", "kind": "integer", "min": -5, "max": 100, "signed": true}, {"name": "alive", "kind": "flag"}])
	var hash := schema.get_hash()
	expect(name, hash != 0, "the schema has no hash")
	var copy: TesseraSchema = schema.duplicate()
	expect(name, copy.is_valid(), "the copy is not valid: " + copy.get_error())
	expect(name, copy.get_hash() == hash, "the copy has hash %d, not %d" % [copy.get_hash(), hash])
	var path := "res://roundtrip_schema.tres"
	expect(name, ResourceSaver.save(schema, path) == OK, "the schema was not saved")
	var loaded = ResourceLoader.load(path, "", ResourceLoader.CACHE_MODE_IGNORE)
	expect(name, loaded is TesseraSchema, "the saved file did not load as a TesseraSchema")
	if loaded is TesseraSchema:
		expect(name, loaded.is_valid(), "the loaded schema is not valid: " + loaded.get_error())
		expect(name, loaded.get_hash() == hash, "the loaded schema has hash %d, not %d" % [loaded.get_hash(), hash])
	DirAccess.remove_absolute(ProjectSettings.globalize_path(path))
	# The copy is its own: adding to it does not change the original.
	copy.add_type(3, [{"name": "on", "kind": "flag"}])
	expect(name, schema.get_hash() == hash, "the original changed when its copy did")
	finish(name, before)


# A second example server (own port) is stopped by the test script when it sees STOP_SERVER: the client has to
# drop every entity it knew, one signal each, and then say that it is disconnected, once.
func check_server_going_away_removes_entities_then_disconnects() -> void:
	var name := "server_going_away_removes_entities_then_disconnects"
	var before := failures
	var client := make_client()
	var events := []
	var removed := {}
	client.entity_removed.connect(func(id):
		events.append("removed")
		removed[id] = true)
	client.disconnected.connect(func(): events.append("disconnected"))
	expect(name, client.connect_to("127.0.0.1", doomed_port) == OK, "connect_to failed: " + client.get_last_error())
	var arrived: bool = await pump([client], func(): return client.get_state() == TesseraClient.State.CONNECTED and client.get_entities().size() == ENTITY_COUNT, 10.0)
	expect(name, arrived, "the client did not get its entities")
	var known := client.get_entities()
	print("STOP_SERVER")
	var gone: bool = await pump([client], func(): return client.get_state() == TesseraClient.State.DISCONNECTED, 15.0)
	expect(name, gone, "the client is still in state %d" % client.get_state())
	expect(name, events.count("removed") == ENTITY_COUNT, "entity_removed was emitted %d times" % events.count("removed"))
	expect(name, events.count("disconnected") == 1, "disconnected was emitted %d times" % events.count("disconnected"))
	expect(name, events.size() == ENTITY_COUNT + 1 and events.back() == "disconnected", "disconnected did not come last")
	var every_one := true
	for id in known:
		every_one = every_one and removed.has(id)
	expect(name, every_one, "some entity was not reported as removed")
	expect(name, client.get_entities().size() == 0, "the client still has entities")
	finish(name, before)


# Godot has its own ENet, and so does the extension. They live in one process here: a host of Godot's own talks to
# another host of Godot's own while the extension's client talks to the example server.
func check_works_next_to_godots_own_enet() -> void:
	var name := "works_next_to_godots_own_enet"
	var before := failures
	var listener := ENetConnection.new()
	expect(name, listener.create_host_bound("127.0.0.1", 0, 4) == OK, "the listening host was not created")
	var listen_port := listener.get_local_port()
	var visitor := ENetConnection.new()
	expect(name, visitor.create_host(1) == OK, "the connecting host was not created")
	var peer := visitor.connect_to_host("127.0.0.1", listen_port)
	expect(name, peer != null, "no peer for the connection")
	var client := make_client()
	expect(name, client.connect_to("127.0.0.1", port) == OK, "connect_to failed: " + client.get_last_error())
	var godot_connected := {"listener": false, "visitor": false}
	var done: bool = await pump([client], func():
		var heard: Array = listener.service(0)
		if heard[0] == ENetConnection.EVENT_CONNECT:
			godot_connected["listener"] = true
		heard = visitor.service(0)
		if heard[0] == ENetConnection.EVENT_CONNECT:
			godot_connected["visitor"] = true
		return godot_connected["listener"] and godot_connected["visitor"] and client.get_state() == TesseraClient.State.CONNECTED and client.get_entities().size() == ENTITY_COUNT, 10.0)
	expect(name, godot_connected["listener"] and godot_connected["visitor"], "Godot's own ENet hosts did not connect to each other")
	expect(name, done, "the client did not get its entities next to Godot's ENet")
	var first = client.get_property(client.get_entities()[0], "position")
	await pump([client], func(): return false, 0.5)
	var later = client.get_property(client.get_entities()[0], "position")
	expect(name, first != null and later != null and not first.is_equal_approx(later), "the client stopped receiving")
	peer.peer_disconnect_now()
	listener.destroy()
	visitor.destroy()
	client.disconnect_from_server()
	finish(name, before)
