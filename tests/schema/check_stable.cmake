# SPDX-License-Identifier: Apache-2.0
# Runs the real tessera_schema twice on a schema file and checks that the two headers are the same bytes, and that the
# hash it prints is the one written in the header. Run with:
#   cmake -DTOOL=<tessera_schema> -DSCHEMA=<schema.json> -DWORK=<empty folder to use> -P check_stable.cmake
foreach(variable TOOL SCHEMA WORK)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "check_stable.cmake needs -D${variable}=...")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
foreach(run a b)
    execute_process(
        COMMAND "${TOOL}" generate --cpp "${WORK}/${run}/out.hpp" --namespace stable "${SCHEMA}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "tessera_schema generate failed (run ${run}, exit code ${result}):\n${output}${errors}")
    endif()
endforeach()

execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${WORK}/a/out.hpp" "${WORK}/b/out.hpp" RESULT_VARIABLE differ)
if(NOT differ EQUAL 0)
    message(FATAL_ERROR "Generating twice from ${SCHEMA} gave two different headers: ${WORK}/a/out.hpp and ${WORK}/b/out.hpp")
endif()

execute_process(COMMAND "${TOOL}" check "${SCHEMA}" RESULT_VARIABLE check_result OUTPUT_VARIABLE check_output ERROR_VARIABLE check_errors)
if(NOT check_result EQUAL 0)
    message(FATAL_ERROR "tessera_schema check failed (exit code ${check_result}):\n${check_output}${check_errors}")
endif()

execute_process(COMMAND "${TOOL}" hash "${SCHEMA}" RESULT_VARIABLE hash_result OUTPUT_VARIABLE printed_hash ERROR_VARIABLE hash_errors)
if(NOT hash_result EQUAL 0)
    message(FATAL_ERROR "tessera_schema hash failed (exit code ${hash_result}):\n${hash_errors}")
endif()
string(STRIP "${printed_hash}" printed_hash)

file(READ "${WORK}/a/out.hpp" header)
string(REGEX MATCH "kSchemaHash = (0x[0-9a-f]+)ull" found "${header}")
if(NOT found)
    message(FATAL_ERROR "The generated header has no kSchemaHash")
endif()
if(NOT "${CMAKE_MATCH_1}" STREQUAL "${printed_hash}")
    message(FATAL_ERROR "The hash printed by the tool (${printed_hash}) is not the one in the header (${CMAKE_MATCH_1})")
endif()
message(STATUS "Two runs gave the same header, and the hash is ${printed_hash}")
