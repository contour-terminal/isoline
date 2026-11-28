# SPDX-License-Identifier: Apache-2.0
#
# The reflected module surface (docs/language-features.md, "The reflected module surface"): a module's surface
# as the export lint reads it through Clang (scripts/check-module-exports.py --list), compared with what a
# reflection program prints of the same header set through another compiler's std::meta::members_of
# (scripts/compare-module-surface.py). Script-safe: it defines a row and functions only, so that
# tests/cmake/module-surface_test.cmake drives the decision in script mode.

# An empty value is a value (no export lint's Clang): cmake_parse_arguments() keeps it. A function keeps the
# policies it was defined under.
if(POLICY CMP0174)
    cmake_policy(SET CMP0174 NEW)
endif()

# The declaration kinds, as the export lint names them (check-module-exports.py, LISTED_KINDS), that
# std::meta::members_of does not report over a namespace, so that the reflected surface cannot show their
# names. Measured on GCC 16.2.1 20260819 (2026-11-27), over a namespace holding a declaration of each kind
# the lint lists: a using-declaration (`using other::Name;`, of a type or a function) is not a member; every
# other kind is (classes, enumerations, variables, functions, aliases, typedefs, concepts and the templates
# of each). An unscoped enumeration's enumerators are not members either, but the reflected surface reads
# them through the enumeration, as the lint does. compare-module-surface.py fails where the program
# reflects a name the lint lists only by such a kind, so a compiler that starts reporting one makes this
# row stale visibly.
set(ISOLINE_MODULE_SURFACE_UNREFLECTED_KINDS UsingDecl)

## @brief Decides whether the surface test registers: `register`, `off` (the configure says why) or `refused`
## (a FATAL_ERROR, where EXPECTED asks for the test and it cannot run).
##
## isoline_module_surface_decision(EXPECTED <bool> REFLECTION_PATH <native|fallback> PYTHON_FOUND <bool>
##                                 LINT_CLANG <path or ""> COMPILER_ID <CMAKE_CXX_COMPILER_ID>
##                                 OUT_VERDICT <var> OUT_REASON <var>)
##
## The test needs, in this order of precedence: reflection native (the program exists only then); a compiler
## other than Clang, whose reading differs from the lint's (Clang compared with Clang cannot find a declaration
## that only one compiler compiles); Python 3; and the export lint's Clang. The first that fails is the reason.
## EXPECTED is ISOLINE_EXPECT_MODULE_SURFACE, which the native CI rows set, so that the test cannot vanish from
## them unseen.
function(isoline_module_surface_decision)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "EXPECTED;REFLECTION_PATH;PYTHON_FOUND;LINT_CLANG;COMPILER_ID;OUT_VERDICT;OUT_REASON" "")
    set(reason "")
    if(NOT arg_REFLECTION_PATH STREQUAL "native")
        set(reason "reflection takes the fallback")
    elseif(arg_COMPILER_ID STREQUAL "Clang")
        set(reason "the compiler is Clang, as the export lint's is: one compiler's reading compared with itself "
                   "finds no declaration that only one compiler compiles")
    elseif(NOT arg_PYTHON_FOUND)
        set(reason "no Python 3")
    elseif(NOT arg_LINT_CLANG)
        set(reason "no export lint (ISOLINE_MODULE_LINT_CLANG)")
    endif()
    if(reason STREQUAL "")
        set(verdict register)
    elseif(arg_EXPECTED)
        set(verdict refused)
        set(reason "ISOLINE_EXPECT_MODULE_SURFACE=ON, but meta-module-surface.reflection cannot run: ${reason}")
    else()
        set(verdict off)
    endif()
    set(${arg_OUT_VERDICT} "${verdict}" PARENT_SCOPE)
    set(${arg_OUT_REASON} "${reason}" PARENT_SCOPE)
endfunction()

## @brief Adds the test <NAME> (label lint), which compares module <MODULE>'s surface as the export lint
## lists it with the names <PROGRAM> prints: another compiler's std::meta::members_of over the row's
## NAMESPACES (scripts/compare-module-surface.py), less the names of the kinds
## ISOLINE_MODULE_SURFACE_UNREFLECTED_KINDS names. A name only one of the two compilers sees is a declaration
## only one compiler compiles.
##
## isoline_add_module_surface_reflection(NAME <test> MODULE <module> PROGRAM <target>)
##  - MODULE is a row of isoline_add_module(), declared before this call.
##  - PROGRAM is a reflection program (isoline_add_reflection_check(<target> PROGRAM ...)), which exists only
##    where reflection is native.
## The comparison also checks that <PROGRAM>'s umbrella includes exactly the header set the lint reads.
## Generates <PROGRAM>'s row file isoline/meta/reflection/ModuleSurfaceNamespaces.def, one
## `{^^<namespace>, "<namespace>"},` per public namespace of the row, on its private include path. Whether the
## test registers is isoline_module_surface_decision()'s: where it does not, the configure prints why, or fails
## where ISOLINE_EXPECT_MODULE_SURFACE is ON.
function(isoline_add_module_surface_reflection)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "NAME;MODULE;PROGRAM" "")
    foreach(required IN ITEMS NAME MODULE PROGRAM)
        if(NOT arg_${required})
            message(FATAL_ERROR "isoline_add_module_surface_reflection: ${required} is required")
        endif()
    endforeach()
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "isoline_add_module_surface_reflection(${arg_NAME}): unknown arguments "
                            "${arg_UNPARSED_ARGUMENTS}")
    endif()
    get_property(manifest GLOBAL PROPERTY ISOLINE_MODULE_${arg_MODULE}_MANIFEST)
    if(NOT manifest)
        message(FATAL_ERROR "isoline_add_module_surface_reflection(${arg_NAME}): ${arg_MODULE} is no module row "
                            "(isoline_add_module(), before this call)")
    endif()
    if(TARGET ${arg_PROGRAM})
        get_target_property(umbrella ${arg_PROGRAM} ISOLINE_REFLECTION_UMBRELLA)
        if(NOT umbrella)
            message(FATAL_ERROR "isoline_add_module_surface_reflection(${arg_NAME}): ${arg_PROGRAM} has no UMBRELLA "
                                "(isoline_add_reflection_check(... UMBRELLA ... HEADER_SETS_ONLY))")
        endif()
        get_property(namespaces GLOBAL PROPERTY ISOLINE_MODULE_${arg_MODULE}_NAMESPACES)
        set(rows "// Generated by isoline_add_module_surface_reflection(${arg_NAME}): module ${arg_MODULE}'s NAMESPACES.\n")
        foreach(namespace IN LISTS namespaces)
            string(APPEND rows "{^^${namespace}, \"${namespace}\"},\n")
        endforeach()
        set(rowDirectory "${CMAKE_CURRENT_BINARY_DIR}/${arg_PROGRAM}.rows")
        file(CONFIGURE OUTPUT "${rowDirectory}/isoline/meta/reflection/ModuleSurfaceNamespaces.def"
             CONTENT "${rows}" @ONLY)
        target_include_directories(${arg_PROGRAM} PRIVATE "${rowDirectory}")
        set(reflectionPath native)
    else()
        set(reflectionPath fallback)
    endif()

    isoline_module_surface_decision(EXPECTED "${ISOLINE_EXPECT_MODULE_SURFACE}" REFLECTION_PATH "${reflectionPath}"
                                    PYTHON_FOUND "${Python3_Interpreter_FOUND}"
                                    LINT_CLANG "${ISOLINE_MODULE_LINT_CLANG}" COMPILER_ID "${CMAKE_CXX_COMPILER_ID}"
                                    OUT_VERDICT verdict OUT_REASON reason)
    if(verdict STREQUAL "refused")
        message(FATAL_ERROR "[isoline] ${reason}")
    elseif(verdict STREQUAL "off")
        message(STATUS "[isoline] ${arg_NAME}: off, ${reason}")
        return()
    endif()
    message(STATUS "[isoline] ${arg_NAME}: ${ISOLINE_MODULE_LINT_CLANG} against ${CMAKE_CXX_COMPILER_ID} "
                   "${CMAKE_CXX_COMPILER_VERSION}")
    add_test(NAME ${arg_NAME}
             COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/scripts/compare-module-surface.py"
                     --clang "${ISOLINE_MODULE_LINT_CLANG}" --manifest "${manifest}"
                     --work-directory "${PROJECT_BINARY_DIR}/modules/${arg_MODULE}.surface-reflection"
                     --umbrella "${umbrella}"
                     --unreflected-kinds ${ISOLINE_MODULE_SURFACE_UNREFLECTED_KINDS}
                     -- "$<TARGET_FILE:${arg_PROGRAM}>")
    # One parse of the header set, as module-exports-<MODULE>'s.
    set_tests_properties(${arg_NAME} PROPERTIES LABELS lint TIMEOUT 300)
endfunction()
