# SPDX-License-Identifier: Apache-2.0
# The CMake functions of tessera_schema, for a project that has this repository as a subfolder and builds the tool
# (-DTESSERA_BUILD_SCHEMA_TOOL=ON, which the project sets before add_subdirectory).
#
#   tessera_schema_generate(<target> SCHEMA <schema.json>
#                           [CPP <header.hpp> [NAMESPACE <a::b>]]
#                           [GDSCRIPT <script.gd> [CLASS <Name>]])
#
# Makes the C++ header and the GDScript of a schema file while your project is built. Give CPP, GDSCRIPT or both.
#   - SCHEMA and GDSCRIPT are read from the folder of the CMakeLists.txt that makes the call, when the path is
#     relative: the schema file is yours, and the script belongs in the folder of your Godot project.
#   - CPP is written in the build folder, when the path is relative: the header is made by the build, and is not
#     kept.
#   - NAMESPACE is the namespace of the header (the tool's default is game_schema), CLASS is the class_name of the
#     script (the tool's default is GameSchema).
# It makes three things:
#   - <target>, a library with no code that carries the folder of the header and the core library: link it to what
#     includes the header.
#   - <target>_generate, which writes the header when the schema file changes.
#   - <target>_gdscript, which writes the script every time the project is built (the tool does not write a file
#     again when its bytes would be the same). It is not a custom command with an output, because CMake deletes such
#     an output from the source folder on `clean` (seen with the Visual Studio and the Ninja generators).
#
#   tessera_schema_check_test(<test name> SCHEMA <schema.json>
#                             [CPP <header.hpp> [NAMESPACE <a::b>]]
#                             [GDSCRIPT <script.gd> [CLASS <Name>]])
#
# Registers a test for ctest that says whether files you keep in the repository (a script, a header) are what the
# tool makes from the schema file now, and fails with the command that makes them again when they are not. Every
# relative path is read from the folder of the CMakeLists.txt that makes the call (the files are yours, so none of
# them is in the build folder).

# The words that the functions take, and the one for each that has a value.
set(_TESSERA_SCHEMA_OPTIONS SCHEMA CPP NAMESPACE GDSCRIPT CLASS)

# Reads and checks the arguments that both functions take. `fn` is the name of the function that was called,
# `first` its first argument (the name of the target or of the test), `kind` says what that name is for ("target"
# or "test"), `verb` what the function does ("generate" or "check"), `cpp_base` the folder that a relative CPP is
# read from, `example` a call written the right way. The rest are the options.
# Sets _TS_SCHEMA, _TS_CPP, _TS_NAMESPACE, _TS_GDSCRIPT and _TS_CLASS in the scope of the caller: the paths as
# absolute paths, and an empty text for what was not given.
function(_tessera_schema_read_arguments fn first kind verb cpp_base example)
    set(who "${fn}(${first}):")
    if(first STREQUAL "" OR first IN_LIST _TESSERA_SCHEMA_OPTIONS)
        message(FATAL_ERROR "${who} give the name of the ${kind} first, for example ${example}")
    endif()
    set(options ${ARGN})
    cmake_parse_arguments(ARG "" "SCHEMA;CPP;NAMESPACE;GDSCRIPT;CLASS" "" ${options})
    if(ARG_UNPARSED_ARGUMENTS)
        list(GET ARG_UNPARSED_ARGUMENTS 0 word)
        message(FATAL_ERROR "${who} ${word} is not an option of ${fn}. The options are SCHEMA, CPP, NAMESPACE, "
                            "GDSCRIPT and CLASS, each followed by one value. For example ${example}")
    endif()
    if(ARG_KEYWORDS_MISSING_VALUES)
        list(GET ARG_KEYWORDS_MISSING_VALUES 0 word)
        message(FATAL_ERROR "${who} ${word} needs a value, for example ${example}")
    endif()
    foreach(keyword IN LISTS _TESSERA_SCHEMA_OPTIONS)
        set(times 0)
        foreach(word IN LISTS options)
            if(word STREQUAL keyword)
                math(EXPR times "${times} + 1")
            endif()
        endforeach()
        if(times GREATER 1)
            message(FATAL_ERROR "${who} ${keyword} is given twice. Each option goes once.")
        endif()
    endforeach()
    if(NOT ARG_SCHEMA)
        message(FATAL_ERROR "${who} SCHEMA is missing: say which schema file to use, for example ${example}")
    endif()
    if(NOT ARG_CPP AND NOT ARG_GDSCRIPT)
        message(FATAL_ERROR "${who} there is nothing to ${verb}: give CPP (the path of the C++ header), GDSCRIPT "
                            "(the path of the GDScript file), or both. For example ${example}")
    endif()
    if(ARG_NAMESPACE AND NOT ARG_CPP)
        message(FATAL_ERROR "${who} NAMESPACE goes with CPP, since it is the namespace of the C++ header. "
                            "Add CPP <header path>, or leave NAMESPACE out.")
    endif()
    if(ARG_CLASS AND NOT ARG_GDSCRIPT)
        message(FATAL_ERROR "${who} CLASS goes with GDSCRIPT, since it is the class_name of the script. "
                            "Add GDSCRIPT <script path>, or leave CLASS out.")
    endif()
    get_filename_component(schema "${ARG_SCHEMA}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT EXISTS "${schema}")
        message(FATAL_ERROR "${who} ${schema} does not exist. SCHEMA is read from the folder of this "
                            "CMakeLists.txt when the path is relative.")
    endif()
    set(_TS_SCHEMA "${schema}" PARENT_SCOPE)
    set(_TS_CPP "" PARENT_SCOPE)
    if(ARG_CPP)
        get_filename_component(cpp "${ARG_CPP}" ABSOLUTE BASE_DIR "${cpp_base}")
        set(_TS_CPP "${cpp}" PARENT_SCOPE)
    endif()
    set(_TS_GDSCRIPT "" PARENT_SCOPE)
    if(ARG_GDSCRIPT)
        get_filename_component(gdscript "${ARG_GDSCRIPT}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        set(_TS_GDSCRIPT "${gdscript}" PARENT_SCOPE)
    endif()
    set(_TS_NAMESPACE "${ARG_NAMESPACE}" PARENT_SCOPE)
    set(_TS_CLASS "${ARG_CLASS}" PARENT_SCOPE)
endfunction()

function(tessera_schema_generate)
    # No named argument: a call with none at all gets the message that says what to write, not CMake's own.
    set(options ${ARGV})
    list(POP_FRONT options target)
    _tessera_schema_read_arguments(tessera_schema_generate "${target}" target generate "${CMAKE_CURRENT_BINARY_DIR}"
        "tessera_schema_generate(game_schema SCHEMA schema.json CPP generated/game_schema.hpp)" ${options})

    add_library(${target} INTERFACE)
    if(_TS_CPP)
        get_filename_component(cpp_dir "${_TS_CPP}" DIRECTORY)
        set(command tessera_schema generate --cpp "${_TS_CPP}")
        if(_TS_NAMESPACE)
            list(APPEND command --namespace "${_TS_NAMESPACE}")
        endif()
        add_custom_command(
            OUTPUT "${_TS_CPP}"
            COMMAND ${command} "${_TS_SCHEMA}"
            DEPENDS tessera_schema "${_TS_SCHEMA}"
            COMMENT "Generating ${_TS_CPP} from ${_TS_SCHEMA}"
            VERBATIM)
        add_custom_target(${target}_generate DEPENDS "${_TS_CPP}")
        target_include_directories(${target} INTERFACE "${cpp_dir}")
        target_link_libraries(${target} INTERFACE tessera_net_core)
        add_dependencies(${target} ${target}_generate)
    endif()
    if(_TS_GDSCRIPT)
        set(command tessera_schema generate --gdscript "${_TS_GDSCRIPT}")
        if(_TS_CLASS)
            list(APPEND command --gdscript-class "${_TS_CLASS}")
        endif()
        add_custom_target(${target}_gdscript ALL
            COMMAND ${command} "${_TS_SCHEMA}"
            COMMENT "Generating ${_TS_GDSCRIPT} from ${_TS_SCHEMA}"
            VERBATIM)
        add_dependencies(${target}_gdscript tessera_schema)
        add_dependencies(${target} ${target}_gdscript)
    endif()
endfunction()

function(tessera_schema_check_test)
    set(options ${ARGV})
    list(POP_FRONT options name)
    _tessera_schema_read_arguments(tessera_schema_check_test "${name}" test check "${CMAKE_CURRENT_SOURCE_DIR}"
        "tessera_schema_check_test(game_schema_is_current SCHEMA schema.json GDSCRIPT godot/game_schema.gd)" ${options})

    set(command tessera_schema generate --check)
    if(_TS_CPP)
        list(APPEND command --cpp "${_TS_CPP}")
        if(_TS_NAMESPACE)
            list(APPEND command --namespace "${_TS_NAMESPACE}")
        endif()
    endif()
    if(_TS_GDSCRIPT)
        list(APPEND command --gdscript "${_TS_GDSCRIPT}")
        if(_TS_CLASS)
            list(APPEND command --gdscript-class "${_TS_CLASS}")
        endif()
    endif()
    add_test(NAME "${name}" COMMAND ${command} "${_TS_SCHEMA}")
endfunction()
