extends Node
# Baseline benchmark of the built-in replication. Roles: server (N moving entities) and client (idle bot).
# Args after "--": role=server|client port=P entities=N clients=K run=S moving=PCT

var args := {}
var peer := ENetMultiplayerPeer.new()
var entities: Array[Node3D] = []
var velocities := PackedVector3Array()
var connected := 0
var all_connected_at := -1.0
var t0 := 0.0
var last_log := 0.0

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		var kv := a.split("=")
		if kv.size() == 2:
			args[kv[0]] = kv[1]
	t0 = Time.get_ticks_msec() / 1000.0
	Engine.physics_ticks_per_second = 30
	if args.get("role", "") == "server":
		Engine.max_fps = 60
		_start_server()
	else:
		Engine.max_fps = 30
		_start_client()

func _start_server() -> void:
	var port := int(args.get("port", "9000"))
	peer.create_server(port, 64)
	multiplayer.multiplayer_peer = peer
	multiplayer.peer_connected.connect(func(_id): connected += 1)
	multiplayer.peer_disconnected.connect(func(_id): connected -= 1)
	var n := int(args.get("entities", "1000"))
	var rng := RandomNumberGenerator.new()
	rng.seed = 1
	var scene: PackedScene = load("res://entity.tscn")
	for i in n:
		var e: Node3D = scene.instantiate()
		e.name = "E%d" % i
		e.position = Vector3(rng.randf_range(-1000, 1000), 0, rng.randf_range(-1000, 1000))
		$World.add_child(e, true)
		entities.append(e)
		velocities.append(Vector3(rng.randf_range(-5, 5), 0, rng.randf_range(-5, 5)))
	print("{\"event\":\"spawned\",\"entities\":%d}" % n)

func _start_client() -> void:
	var err := peer.create_client("127.0.0.1", int(args.get("port", "9000")))
	if err != OK:
		print("client failed: ", err)
		get_tree().quit(1)
		return
	multiplayer.multiplayer_peer = peer
	multiplayer.server_disconnected.connect(func(): get_tree().quit())

func _physics_process(delta: float) -> void:
	if args.get("role", "") != "server":
		return
	var moving := int(float(entities.size()) * float(args.get("moving", "100")) / 100.0)
	for i in moving:
		var e := entities[i]
		var p := e.position + velocities[i] * delta
		if absf(p.x) > 1000.0: velocities[i].x = -velocities[i].x
		if absf(p.z) > 1000.0: velocities[i].z = -velocities[i].z
		e.position = p

func _process(_delta: float) -> void:
	var now := Time.get_ticks_msec() / 1000.0
	if args.get("role", "") == "server":
		var want := int(args.get("clients", "1"))
		if all_connected_at < 0.0 and connected >= want:
			all_connected_at = now
			print("{\"event\":\"all_connected\"}")
		if all_connected_at < 0.0 and now - t0 > 120.0:
			get_tree().quit(2)
		if now - last_log >= 1.0:
			last_log = now
			var sent := peer.host.pop_statistic(ENetConnection.HOST_TOTAL_SENT_DATA)
			var recv := peer.host.pop_statistic(ENetConnection.HOST_TOTAL_RECEIVED_DATA)
			print("{\"event\":\"tick\",\"t\":%.1f,\"since_all\":%.1f,\"peers\":%d,\"entities\":%d,\"fps\":%d,\"process_ms\":%.3f,\"physics_ms\":%.3f,\"sent_bytes\":%d,\"recv_bytes\":%d}" % [
				now - t0, (now - all_connected_at) if all_connected_at >= 0.0 else -1.0, connected, entities.size(),
				Engine.get_frames_per_second(),
				Performance.get_monitor(Performance.TIME_PROCESS) * 1000.0,
				Performance.get_monitor(Performance.TIME_PHYSICS_PROCESS) * 1000.0,
				int(sent), int(recv)])
		if all_connected_at >= 0.0 and now - all_connected_at > float(args.get("run", "20")):
			get_tree().quit()
	else:
		if now - last_log >= 1.0:
			last_log = now
			print("{\"event\":\"client\",\"entities\":%d}" % $World.get_child_count())
