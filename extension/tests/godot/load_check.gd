# SPDX-License-Identifier: Apache-2.0
# Loads the extension in a headless Godot and checks that its classes are there and can be made.
extends SceneTree


func _init() -> void:
	for class_name_to_check in ["TesseraClient", "TesseraSchema"]:
		if not ClassDB.class_exists(class_name_to_check):
			print("LOAD missing ", class_name_to_check)
			quit(2)
			return
	var client = ClassDB.instantiate("TesseraClient")
	var schema = ClassDB.instantiate("TesseraSchema")
	if client == null or schema == null:
		print("LOAD could not make the classes")
		quit(1)
		return
	print("LOAD ok")
	quit(0)
