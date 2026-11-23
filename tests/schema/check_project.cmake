# SPDX-License-Identifier: Apache-2.0
# Builds the example project (examples/game-schema) as a project of its own, the way a game would: it brings this
# repository in as a folder, with add_subdirectory, and gets its C++ and its GDScript from tessera_schema_generate.
# The steps, each with its own message when it fails:
#   1. a copy of the example is configured and built (the build writes the script, so the copy is used, and the
#      repository stays as it is), and the ctest of the project passes and holds one test;
#   2. the script that the build wrote is the one kept in the example, byte for byte;
#   3. `generate --check` says that the files are what the schema makes;
#   4. after a change in the schema file, `--check` says they are not, with the command that makes them again, and
#      the next build writes the script again, after which `--check` is happy.
# Run with:
#   cmake -DREPO=<this repository> -DEXAMPLE=<examples/game-schema> -DTOOL=<tessera_schema> -DWORK=<folder to use>
#         -DGENERATOR=<generator of this build> [-DPLATFORM=<platform>] [-DCXX=<compiler>] -DCONFIG=<Debug|Release>
#         -P check_project.cmake
foreach(variable REPO EXAMPLE TOOL WORK GENERATOR CONFIG)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "check_project.cmake needs -D${variable}=...")
    endif()
endforeach()

# Runs a command and stops with a message that says which step it was, what was run and what it printed, when its
# exit code is not `expected`. Leaves the exit code and the output in STEP_CODE and STEP_OUTPUT.
function(run_step description expected)
    execute_process(COMMAND ${ARGN}
                    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    set(STEP_CODE "${code}" PARENT_SCOPE)
    set(STEP_OUTPUT "${out}${err}" PARENT_SCOPE)
    if(NOT code EQUAL expected)
        string(REPLACE ";" " " shown "${ARGN}")
        message(FATAL_ERROR "Step \"${description}\" failed: expected exit code ${expected}, got ${code}.\n"
                            "Command: ${shown}\nOutput:\n${out}${err}")
    endif()
endfunction()

set(project "${WORK}/project")
set(build "${WORK}/build")
set(kept_script "${EXAMPLE}/godot/game_schema.gd")
set(script "${project}/godot/game_schema.gd")
set(regenerate "tessera_schema generate --gdscript ${kept_script} --gdscript-class GameSchema ${EXAMPLE}/schema.json")

file(REMOVE_RECURSE "${WORK}")
file(COPY "${EXAMPLE}/" DESTINATION "${project}")
# The build has to write the script itself, or there would be nothing to compare.
file(REMOVE "${script}")

# The same generator, platform and compiler as the build that runs this test, so that the project is built with what
# is known to work here (CXXFLAGS and the like come from the environment on their own).
set(configure -S "${project}" -B "${build}" -G "${GENERATOR}" -DTESSERA_NET_DIR=${REPO})
if(NOT "${PLATFORM}" STREQUAL "")
    list(APPEND configure -A "${PLATFORM}")
endif()
if(NOT GENERATOR MATCHES "^Visual Studio")
    list(APPEND configure -DCMAKE_BUILD_TYPE=${CONFIG})
    if(NOT "${CXX}" STREQUAL "")
        list(APPEND configure -DCMAKE_CXX_COMPILER=${CXX})
    endif()
endif()

run_step("configure the example as a project of its own" 0 "${CMAKE_COMMAND}" ${configure})
if(STEP_OUTPUT MATCHES "Downloading|git clone|FetchContent")
    message(FATAL_ERROR "Step \"configure the example\": the configure tried to download something, and it must not.\n${STEP_OUTPUT}")
endif()
run_step("build the example" 0 "${CMAKE_COMMAND}" --build "${build}" --config ${CONFIG} --parallel)
run_step("run the tests of the example" 0 "${CMAKE_CTEST_COMMAND}" --test-dir "${build}" -C ${CONFIG} --output-on-failure)
run_step("list the tests of the example" 0 "${CMAKE_CTEST_COMMAND}" --test-dir "${build}" -C ${CONFIG} -N)
if(NOT STEP_OUTPUT MATCHES "Total Tests: 1")
    message(FATAL_ERROR "Step \"the project has only its own test\": the tests of this repository must be off when it is "
                        "a folder of another project, and ctest -N said:\n${STEP_OUTPUT}")
endif()

if(NOT EXISTS "${build}/generated/game_schema.hpp")
    message(FATAL_ERROR "Step \"the header is made\": ${build}/generated/game_schema.hpp was not written by the build.")
endif()
if(NOT EXISTS "${script}")
    message(FATAL_ERROR "Step \"the script is made\": the build did not write ${script}.")
endif()
if(NOT EXISTS "${kept_script}")
    message(FATAL_ERROR "Step \"the kept script\": ${kept_script} does not exist. Make it with this command:\n  ${regenerate}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${kept_script}" "${script}" RESULT_VARIABLE differ)
if(NOT differ EQUAL 0)
    message(FATAL_ERROR "Step \"the kept script is what the build makes\": ${kept_script} is not what the build wrote "
                        "from schema.json. Make it again with this command:\n  ${regenerate}")
endif()

set(check "${TOOL}" generate --check --cpp "${build}/generated/game_schema.hpp" --namespace game
          --gdscript "${script}" --gdscript-class GameSchema "${project}/schema.json")
run_step("check says the files are what the schema makes" 0 ${check})

# A change in the schema file: the first range becomes another number.
file(READ "${project}/schema.json" schema_text)
string(REPLACE "\"range\": 1000.0" "\"range\": 2000.0" changed_text "${schema_text}")
if(changed_text STREQUAL schema_text)
    message(FATAL_ERROR "Step \"change the schema\": the text \"range\": 1000.0 is not in ${EXAMPLE}/schema.json any more; "
                        "change this test to a number that is.")
endif()
file(WRITE "${project}/schema.json" "${changed_text}")
run_step("check sees that the files are old now" 1 ${check})
if(NOT STEP_OUTPUT MATCHES "tessera_schema generate")
    message(FATAL_ERROR "Step \"check says how to make the files again\": the output has no command:\n${STEP_OUTPUT}")
endif()
file(READ "${script}" script_before)
run_step("build again after the change" 0 "${CMAKE_COMMAND}" --build "${build}" --config ${CONFIG} --parallel)
file(READ "${script}" script_after)
if(script_after STREQUAL script_before)
    message(FATAL_ERROR "Step \"the build writes the script again\": ${script} did not change after the schema file did.")
endif()
run_step("check is happy after the build" 0 ${check})

message(STATUS "The example was built as a project of its own, and its files follow its schema")
