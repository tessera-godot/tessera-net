# SPDX-License-Identifier: Apache-2.0
# Makes the GDScript of a schema file again and compares it with the one that is kept in the repository, so that the
# kept one cannot be left behind when the schema file changes. Run with:
#   cmake -DTOOL=<tessera_schema> -DSCHEMA=<schema.json> -DCLASS=<ClassName> -DCOMMITTED=<kept script.gd>
#         -DWORK=<folder to use> -P check_generated_gdscript.cmake
foreach(variable TOOL SCHEMA CLASS COMMITTED WORK)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "check_generated_gdscript.cmake needs -D${variable}=...")
    endif()
endforeach()

get_filename_component(script_name "${COMMITTED}" NAME)
file(REMOVE_RECURSE "${WORK}")
execute_process(
    COMMAND "${TOOL}" generate --gdscript "${WORK}/${script_name}" --gdscript-class "${CLASS}" "${SCHEMA}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "tessera_schema generate failed (exit code ${result}):\n${output}${errors}")
endif()

set(regenerate "tessera_schema generate --gdscript examples/godot-client-demo/${script_name} --gdscript-class ${CLASS} server/example/example_schema.json")
if(NOT EXISTS "${COMMITTED}")
    message(FATAL_ERROR "${COMMITTED} does not exist. Make it with this command, from the root of the repository:\n  ${regenerate}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${COMMITTED}" "${WORK}/${script_name}" RESULT_VARIABLE differ)
if(NOT differ EQUAL 0)
    message(FATAL_ERROR "${COMMITTED} is not what the tool makes from ${SCHEMA} now. Make it again, from the root of the repository:\n  ${regenerate}")
endif()
message(STATUS "${script_name} is up to date with ${SCHEMA}")
