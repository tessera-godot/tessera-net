extends Node3D
# Benchmark entity: one node with one MultiplayerSynchronizer replicating its position.

const SYNC_INTERVAL := 1.0 / 20.0

func _enter_tree() -> void:
	var cfg := SceneReplicationConfig.new()
	cfg.add_property(NodePath(":position"))
	cfg.property_set_replication_mode(NodePath(":position"), SceneReplicationConfig.REPLICATION_MODE_ALWAYS)
	var sync := MultiplayerSynchronizer.new()
	sync.name = "Sync"
	sync.root_path = NodePath("..")
	sync.replication_config = cfg
	sync.replication_interval = SYNC_INTERVAL
	add_child(sync)
