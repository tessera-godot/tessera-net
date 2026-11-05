extends SceneTree
# Usage: godot --headless --path project -s run.gd -- 1000000 5
func _init() -> void:
	var args := OS.get_cmdline_user_args()
	var count := int(args[0]) if args.size() > 0 else 1000000
	var runs := int(args[1]) if args.size() > 1 else 5
	var bench = ClassDB.instantiate("TesseraNodeBench")
	print(JSON.stringify(bench.run(count, runs)))
	quit()
