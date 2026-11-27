# SPDX-License-Identifier: Apache-2.0
#
# isoline_c_export_rules(): what a shared library of the C API exports, and nothing else.
#
# The rules are data, one row per linker option: format|when|option|reason, where format is the object
# format (elf, macho; PE needs none: a DLL exports what is declared __declspec(dllexport), which
# ISOLINE_API is while the library is built) and when is `always` or `uninstrumented`. <map> stands for
# the version script, <list> for the generated Mach-O exported-symbols list.
#
# isoline_c_shared applies them, and so does the export-rules fixture of tests/c, which holds the rows to
# hiding what a C++ library defines beside its C API (the c-export-rules test).

include_guard(GLOBAL)

set(ISOLINE_C_EXPORT_RULE_ROWS
    "elf|always|-Wl,--version-script=<map>|exports wasm_* and isoline_* under the ABI's version node and makes every other symbol local"
    "elf|always|-Wl,--exclude-libs,ALL|keeps the symbols of the static libraries linked in (isoline_core, core-cpp) local"
    "elf|uninstrumented|-Wl,--no-undefined|refuses a library with an unresolved symbol; a sanitizer's or coverage's runtime is the executable's, so an instrumented build leaves it out"
    "macho|always|-Wl,-exported_symbols_list,<list>|exports the listed symbols and hides every other")

# The object format of the build's shared libraries, as the rows name it.
if(APPLE)
    set(ISOLINE_C_OBJECT_FORMAT macho)
elseif(WIN32)
    set(ISOLINE_C_OBJECT_FORMAT pe)
else()
    set(ISOLINE_C_OBJECT_FORMAT elf)
endif()

## @brief Applies the export rules to the shared library @p target.
##
## isoline_c_export_rules(<target> MAP <version script> SYMBOLS <exported-symbols file>)
## SYMBOLS lists the exported names, one per line, as src/isoline/c/exported-symbols.txt does; the Mach-O
## list is generated from it. The target's own code is compiled with hidden visibility too, so the rules
## are a second line behind it: they hide what is default visibility regardless, such as the standard
## library's template instantiations (libstdc++ declares namespace std visibility("default")).
function(isoline_c_export_rules target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "MAP;SYMBOLS" "")
    if(NOT arg_MAP OR NOT arg_SYMBOLS)
        message(FATAL_ERROR "isoline_c_export_rules(${target}): MAP and SYMBOLS are required")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${arg_SYMBOLS}")
    file(STRINGS "${arg_SYMBOLS}" symbols)
    list(TRANSFORM symbols PREPEND "_")
    list(JOIN symbols "\n" machOExports)
    set(list "${CMAKE_CURRENT_BINARY_DIR}/${target}.exported_symbols")
    file(CONFIGURE OUTPUT "${list}" CONTENT "${machOExports}\n")

    if(ISOLINE_SANITIZE OR ISOLINE_COVERAGE)
        set(instrumentation instrumented)
    else()
        set(instrumentation uninstrumented)
    endif()
    foreach(row IN LISTS ISOLINE_C_EXPORT_RULE_ROWS)
        string(REGEX MATCH "^([a-z]+)[|]([a-z]+)[|]([^|]+)[|]" matched "${row}")
        set(option "${CMAKE_MATCH_3}")
        if(CMAKE_MATCH_1 STREQUAL ISOLINE_C_OBJECT_FORMAT
           AND (CMAKE_MATCH_2 STREQUAL "always" OR CMAKE_MATCH_2 STREQUAL instrumentation))
            string(REPLACE "<map>" "${arg_MAP}" option "${option}")
            string(REPLACE "<list>" "${list}" option "${option}")
            # Recorded with the build's own options: none of them may reach an installed target's interface.
            isoline_apply_build_options(${target} LINK "${option}")
        endif()
    endforeach()
    set_target_properties(${target} PROPERTIES
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        LINK_DEPENDS "${arg_MAP};${list}"
        ISOLINE_C_OBJECT_FORMAT "${ISOLINE_C_OBJECT_FORMAT}")
endfunction()
