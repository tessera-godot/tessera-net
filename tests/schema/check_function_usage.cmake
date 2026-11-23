# SPDX-License-Identifier: Apache-2.0
# Calls the CMake functions of tessera_schema the wrong way, each call in a script of its own, and checks that it
# stops with the message that says what is wrong and how to write it. Run with:
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

if(failures)
    message(FATAL_ERROR "The functions of tessera_schema did not fail as they should:\n${failures}")
endif()
message(STATUS "${case_count} wrong calls, ${case_count} messages as expected")
