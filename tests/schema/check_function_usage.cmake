# SPDX-License-Identifier: Apache-2.0
# Calls the CMake functions of tessera_schema the wrong way, each call in a script of its own, and checks that it
# stops with the message that says what is wrong and how to write it. It also configures two small projects with a
# right call of tessera_schema_check_test, and checks the command of the test that this registers. Run with:
#   cmake -DFUNCTIONS=<tessera_schema.cmake> -DSCHEMA=<a schema file that exists> -DWORK=<folder to use>
#         -P check_function_usage.cmake
foreach(variable FUNCTIONS SCHEMA WORK)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "check_function_usage.cmake needs -D${variable}=...")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
set(case_count 0)
set(failures "")

# Runs `call` in a script of its own, and checks that the script fails with a message that starts like `prefix`
# and then says `fragment`. CMake breaks long lines of a message, so the white space is made into single spaces
# before the text is searched.
macro(expect_error call prefix fragment)
    math(EXPR case_count "${case_count} + 1")
    file(WRITE "${WORK}/case_${case_count}.cmake" "include(\"${FUNCTIONS}\")\n${call}\n")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -P "${WORK}/case_${case_count}.cmake"
        RESULT_VARIABLE case_result
        OUTPUT_VARIABLE case_output
        ERROR_VARIABLE case_errors)
    string(REGEX REPLACE "[ \t\r\n]+" " " case_text "${case_output} ${case_errors}")
    string(FIND "${case_text}" "${prefix} " case_prefix_at)
    string(FIND "${case_text}" "${fragment}" case_fragment_at)
    if(case_result EQUAL 0)
        string(APPEND failures "case ${case_count}: ${call}\n  expected the call to fail, and it did not\n")
    elseif(case_prefix_at EQUAL -1 OR case_fragment_at EQUAL -1 OR case_fragment_at LESS case_prefix_at)
        string(APPEND failures "case ${case_count}: ${call}\n  expected a message that starts with \"${prefix}\" and says \"${fragment}\"\n  got: ${case_text}\n")
    endif()
endmacro()

# Configures a small project whose only job is one call of a function, and checks the command of the test that the
# call registers. The project has no language, so that no compiler is asked for, and the tool is a program that
# is cmake itself (a program that is there, so that ctest shows its path): only the command is looked at. `expected` is the command, without
# the quotation marks that ctest puts around each word; `not_expected` is a list of words that must not be in the
# command (empty: none).
macro(expect_test_command name call expected not_expected)
    math(EXPR case_count "${case_count} + 1")
    set(case_dir "${WORK}/${name}")
    file(MAKE_DIRECTORY "${case_dir}")
    file(COPY "${SCHEMA}" DESTINATION "${case_dir}")
    file(WRITE "${case_dir}/CMakeLists.txt"
         "cmake_minimum_required(VERSION 3.21)\nproject(case LANGUAGES NONE)\nenable_testing()\n"
         "add_executable(tessera_schema IMPORTED)\n"
         "set_target_properties(tessera_schema PROPERTIES IMPORTED_LOCATION \"${CMAKE_COMMAND}\")\n"
         "include(\"${FUNCTIONS}\")\n${call}\n")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${case_dir}" -B "${case_dir}/build"
        RESULT_VARIABLE case_result
        OUTPUT_VARIABLE case_output
        ERROR_VARIABLE case_errors)
    if(case_result EQUAL 0)
        execute_process(
            COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${case_dir}/build" -C Debug -N -V
            RESULT_VARIABLE case_result
            OUTPUT_VARIABLE case_output
            ERROR_VARIABLE case_errors)
        string(REPLACE "\"" "" case_output "${case_output}")
        string(REPLACE "\\" "/" case_output "${case_output}")
        string(REGEX REPLACE "[ \t\r\n]+" " " case_text "${case_output}")
        string(FIND "${case_text}" "Test command: ${expected} " case_found)
        set(case_unwanted -1)
        foreach(word IN LISTS not_expected)
            string(FIND "${case_text}" "${word}" case_word_at)
            if(NOT case_word_at EQUAL -1)
                set(case_unwanted ${case_word_at})
            endif()
        endforeach()
        if(case_found EQUAL -1 OR NOT case_unwanted EQUAL -1)
            string(APPEND failures "case ${case_count}: ${call}\n  expected the test command \"${expected}\" and not \"${not_expected}\"\n  got: ${case_text}\n")
        endif()
    else()
        string(APPEND failures "case ${case_count}: ${call}\n  the project did not configure:\n${case_output}${case_errors}\n")
    endif()
endmacro()

# tessera_schema_generate
expect_error("tessera_schema_generate()"
             "tessera_schema_generate():" "give the name of the target first")
expect_error("tessera_schema_generate(SCHEMA \"${SCHEMA}\" CPP out.hpp)"
             "tessera_schema_generate(SCHEMA):" "give the name of the target first")
expect_error("tessera_schema_generate(one CPP out.hpp)"
             "tessera_schema_generate(one):" "SCHEMA is missing")
expect_error("tessera_schema_generate(one SCHEMA \"${WORK}/not_there.json\" CPP out.hpp)"
             "tessera_schema_generate(one):" "${WORK}/not_there.json does not exist")
expect_error("tessera_schema_generate(one SCHEMA \"${SCHEMA}\")"
             "tessera_schema_generate(one):" "nothing to generate")
expect_error("tessera_schema_generate(one SCHEMA \"${SCHEMA}\" GDSCRIPT out.gd NAMESPACE game)"
             "tessera_schema_generate(one):" "NAMESPACE goes with CPP")
expect_error("tessera_schema_generate(one SCHEMA \"${SCHEMA}\" CPP out.hpp CLASS Game)"
             "tessera_schema_generate(one):" "CLASS goes with GDSCRIPT")
expect_error("tessera_schema_generate(one SCHEMA \"${SCHEMA}\" CPP out.hpp OUTPUT x)"
             "tessera_schema_generate(one):" "OUTPUT is not an option")
expect_error("tessera_schema_generate(one SCHEMA \"${SCHEMA}\" CPP)"
             "tessera_schema_generate(one):" "CPP needs a value")
expect_error("tessera_schema_generate(one SCHEMA \"${SCHEMA}\" CPP a.hpp CPP b.hpp)"
             "tessera_schema_generate(one):" "CPP is given twice")

# tessera_schema_check_test
expect_error("tessera_schema_check_test()"
             "tessera_schema_check_test():" "give the name of the test first")
expect_error("tessera_schema_check_test(kept GDSCRIPT out.gd)"
             "tessera_schema_check_test(kept):" "SCHEMA is missing")
expect_error("tessera_schema_check_test(kept SCHEMA \"${SCHEMA}\")"
             "tessera_schema_check_test(kept):" "nothing to check")
expect_error("tessera_schema_check_test(kept SCHEMA \"${SCHEMA}\" CPP out.hpp CLASS Game)"
             "tessera_schema_check_test(kept):" "CLASS goes with GDSCRIPT")

# What the test runs: every option in the same order, and every relative path read from the folder of the project.
get_filename_component(schema_name "${SCHEMA}" NAME)
expect_test_command(good_call
    "tessera_schema_check_test(kept SCHEMA \"${SCHEMA}\" CPP \"${WORK}/good_call/out.hpp\" NAMESPACE game GDSCRIPT \"${WORK}/good_call/out.gd\" CLASS Game)"
    "${CMAKE_COMMAND} generate --check --cpp ${WORK}/good_call/out.hpp --namespace game --gdscript ${WORK}/good_call/out.gd --gdscript-class Game ${SCHEMA}"
    "")
expect_test_command(relative_paths
    "tessera_schema_check_test(kept SCHEMA ${schema_name} CPP kept/out.hpp GDSCRIPT godot/out.gd)"
    "${CMAKE_COMMAND} generate --check --cpp ${WORK}/relative_paths/kept/out.hpp --gdscript ${WORK}/relative_paths/godot/out.gd ${WORK}/relative_paths/${schema_name}"
    "--namespace;--gdscript-class")

if(failures)
    message(FATAL_ERROR "The functions of tessera_schema did not fail as they should:\n${failures}")
endif()
message(STATUS "${case_count} calls, ${case_count} results as expected")
