# SPDX-License-Identifier: Apache-2.0
# Loads the extension in a headless Godot and checks that its class is there and answers.
extends SceneTree


func _init() -> void:
	if not ClassDB.class_exists("TesseraClient"):
		print("LOAD missing")
		quit(2)
		return
	var client = ClassDB.instantiate("TesseraClient")
	var answer = client.ping()
	print("LOAD ping=", answer)
	quit(0 if answer == "tessera" else 1)
