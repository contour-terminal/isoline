# SPDX-License-Identifier: Apache-2.0
#
# C++ modules, opt-in: ISOLINE_MODULES, default OFF.
#
# This is the gate. With ISOLINE_MODULES ON it feeds the real toolchain to the support table
# (cmake/IsolineModuleSupport.cmake) and stops the configure on a toolchain the table refuses, naming
# why; beside a module build it also holds clang-tidy to the compiler's own LLVM build. With
# ISOLINE_MODULES OFF it changes nothing about the build: no target, no flag. Either way it knows the
# tripwire of a refused compiler, which tests/CMakeLists.txt registers (isoline_add_module_tripwires()).
#
# A module is a row, isoline_add_module(), in its library's CMakeLists.txt. Whatever ISOLINE_MODULES says,
# the row writes the module's surface manifest and, in a testing build that has the export lint's Clang,
# registers the lint (scripts/check-module-exports.py) and the target that regenerates the interface unit.

include_guard(GLOBAL)
include(IsolineModuleSupport)

# The target ABI the table's rows name (isoline_module_target_abi()).
isoline_module_target_abi(COMPILER_ID "${CMAKE_CXX_COMPILER_ID}" SIMULATE_ID "${CMAKE_CXX_SIMULATE_ID}"
                          OUT_TARGET_ABI ISOLINE_CXX_TARGET_ABI)

# The tripwires a refused compiler row names: (tripwire, CTest name, script). Each probes CMake itself,
# not the table, and goes red once CMake builds modules for that compiler: the row should then become a
# lane.
set(ISOLINE_MODULE_TRIPWIRES
    "scanning-probe" "module-scanning-tripwire" "tests/cmake/expect-no-module-scanning.cmake")

# The cache variables a tripwire's probe project takes over from this build, so that it configures the
# same toolchain: the compiler and its toolchain file, the build tool, and what the msvc-wine toolchain
# files read.
set(ISOLINE_MODULE_PROBE_CACHE_VARIABLES
    CMAKE_TOOLCHAIN_FILE
    CMAKE_CXX_COMPILER
    CMAKE_MAKE_PROGRAM
    MSVC_WINE_ROOT
    MSVC_WINE_TOOLSET_VERSION
    MSVC_WINE_SDK_VERSION)

## @brief Refuses the configure unless the toolchain builds Isoline's C++ modules, and unless clang-tidy,
## where it runs, is the compiler's own LLVM build. Prints the verdict.
function(_isoline_module_gate)
    set(scanDepsDefined "no")
    if(DEFINED CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS)
        set(scanDepsDefined "yes")
    endif()
    # The generator's build tool is CMAKE_MAKE_PROGRAM; its row says whether it has a floor.
    isoline_module_generator_tool("${CMAKE_GENERATOR}" buildTool)
    set(buildToolOutput "")
    if(buildTool)
        execute_process(COMMAND "${CMAKE_MAKE_PROGRAM}" --version OUTPUT_VARIABLE buildToolOutput ERROR_QUIET)
    endif()
    isoline_module_scan_inputs(
        SCAN_RULE "${CMAKE_CXX_SCANDEP_SOURCE}" SCAN_DEPS_DEFINED "${scanDepsDefined}"
        SCAN_DEPS "${CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS}" BUILD_TOOL_OUTPUT "${buildToolOutput}"
        OUT_SCAN_RULE_DEFINED scanRuleDefined OUT_SCAN_DEPS_FOUND scanDepsFound
        OUT_BUILD_TOOL_VERSION buildToolVersion)
    isoline_module_support(
        COMPILER_ID "${CMAKE_CXX_COMPILER_ID}" FRONTEND "${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}"
        TARGET_ABI "${ISOLINE_CXX_TARGET_ABI}" VERSION "${CMAKE_CXX_COMPILER_VERSION}"
        GENERATOR "${CMAKE_GENERATOR}" CMAKE_VERSION "${CMAKE_VERSION}" NINJA_VERSION "${buildToolVersion}"
        SCAN_RULE_DEFINED "${scanRuleDefined}" SCAN_DEPS_FOUND "${scanDepsFound}"
        OUT_VERDICT verdict OUT_REASON reason OUT_INSTALLED_MODULES installedModules)
    if(NOT verdict STREQUAL "supported")
        message(FATAL_ERROR "[isoline] ISOLINE_MODULES: ${reason}")
    endif()

    if(ISOLINE_CLANG_TIDY)
        execute_process(COMMAND "${ISOLINE_CLANG_TIDY_EXE}" --version
                        OUTPUT_VARIABLE tidyVersionOutput ERROR_QUIET)
        isoline_module_tidy_refusal(COMPILER_ID "${CMAKE_CXX_COMPILER_ID}" VERSION "${CMAKE_CXX_COMPILER_VERSION}"
                                    TIDY_VERSION_OUTPUT "${tidyVersionOutput}" OUT_REASON tidyRefusal)
        if(tidyRefusal)
            message(FATAL_ERROR "[isoline] ISOLINE_MODULES with ISOLINE_CLANG_TIDY: ${tidyRefusal} "
                                "(ISOLINE_CLANG_TIDY_EXE=${ISOLINE_CLANG_TIDY_EXE})")
        endif()
    endif()

    message(STATUS "[isoline] C++ modules: on (${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}, "
                   "installed modules: ${installedModules})")
endfunction()

if(ISOLINE_MODULES)
    _isoline_module_gate()
endif()

## @brief Registers the tripwire of the compiler's row, where it is refused with one, as a CTest of label
## `lint`. Called by tests/CMakeLists.txt, whatever ISOLINE_MODULES says: the probe is a project of its own
## and does not configure Isoline, so it tests CMake, never the table.
function(isoline_add_module_tripwires)
    isoline_module_compiler_tripwire(COMPILER_ID "${CMAKE_CXX_COMPILER_ID}"
                                     FRONTEND "${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}"
                                     TARGET_ABI "${ISOLINE_CXX_TARGET_ABI}" CMAKE_VERSION "${CMAKE_VERSION}"
                                     OUT_TRIPWIRE tripwire)
    if(NOT tripwire)
        return()
    endif()
    set(rows "${ISOLINE_MODULE_TRIPWIRES}")
    set(testName "")
    while(rows)
        list(POP_FRONT rows rowTripwire rowTestName rowScript)
        if(rowTripwire STREQUAL tripwire)
            set(testName "${rowTestName}")
            set(script "${rowScript}")
            break()
        endif()
    endwhile()
    if(NOT testName)
        message(FATAL_ERROR "[isoline] ISOLINE_MODULE_TRIPWIRES has no row for the tripwire ${tripwire}")
    endif()

    set(probeDirectory "${PROJECT_BINARY_DIR}/${testName}")
    set(initialCache "")
    foreach(variable IN LISTS ISOLINE_MODULE_PROBE_CACHE_VARIABLES)
        if(DEFINED ${variable})
            string(APPEND initialCache "set(${variable} [==[${${variable}}]==] CACHE STRING \"\")\n")
        endif()
    endforeach()
    file(CONFIGURE OUTPUT "${probeDirectory}/initial-cache.cmake" CONTENT "${initialCache}" @ONLY)
    add_test(NAME ${testName}
             COMMAND "${CMAKE_COMMAND}"
                     "-DPROBE_BINARY_DIR=${probeDirectory}/build"
                     "-DINITIAL_CACHE=${probeDirectory}/initial-cache.cmake"
                     "-DGENERATOR=${CMAKE_GENERATOR}"
                     "-DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
                     "-DGENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
                     "-DCOMPILER=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
                     -P "${PROJECT_SOURCE_DIR}/${script}")
    # A configure of its own, plus a build where that succeeds: seconds, but a toolchain under wine is slow
    # to start.
    set_tests_properties(${testName} PROPERTIES LABELS lint TIMEOUT 300)
endfunction()

# The export lint: scripts/check-module-exports.py, run by Python 3 with the Clang of the pinned LLVM major
# (ISOLINE_MODULE_LINT_LLVM_MAJOR), which dumps the header set's AST. It belongs to the testing build, so a
# parent project that embeds Isoline neither looks for Python nor for that Clang.
# Raising the major re-baselines the selftest cases whose expected.txt quotes that Clang's diagnostic
# wording (scripts/testdata/module-exports/): global-scope-internal-reexported, def-included-two-levels and
# user-include-warning-fails.
set(ISOLINE_MODULE_LINT_CLANG "" CACHE FILEPATH
    "The clang++ of LLVM ${ISOLINE_MODULE_LINT_LLVM_MAJOR} the export lint runs; empty: the compiler where it \
is that Clang, else clang++-${ISOLINE_MODULE_LINT_LLVM_MAJOR}")

## @brief Sets ISOLINE_MODULE_LINT_CLANG, in the caller's scope, to the export lint's Clang, or to "" where
## there is none, and prints which: the cache's value where it is set, else the compiler where it is Clang
## of the GNU front end, else clang++-<major> on the PATH. A candidate that is Apple's Clang, or another
## LLVM major, is refused with its reason.
function(_isoline_module_lint_clang)
    set(candidates "")
    if(ISOLINE_MODULE_LINT_CLANG)
        list(APPEND candidates "${ISOLINE_MODULE_LINT_CLANG}")
    else()
        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
            list(APPEND candidates "${CMAKE_CXX_COMPILER}")
        endif()
        find_program(programClang NAMES clang++-${ISOLINE_MODULE_LINT_LLVM_MAJOR} NO_CACHE)
        if(programClang)
            list(APPEND candidates "${programClang}")
        endif()
    endif()
    set(accepted "")
    set(refusals "")
    foreach(candidate IN LISTS candidates)
        execute_process(COMMAND "${candidate}" --version OUTPUT_VARIABLE versionOutput ERROR_QUIET)
        isoline_module_lint_clang_refusal(VERSION_OUTPUT "${versionOutput}" OUT_REASON reason)
        if(NOT reason)
            set(accepted "${candidate}")
            break()
        endif()
        list(APPEND refusals "${candidate}: ${reason}")
    endforeach()
    foreach(refusal IN LISTS refusals)
        message(STATUS "[isoline] export lint: refused ${refusal}")
    endforeach()
    if(accepted)
        message(STATUS "[isoline] export lint: ${accepted}")
    elseif(NOT candidates)
        message(STATUS "[isoline] export lint: off, no clang++-${ISOLINE_MODULE_LINT_LLVM_MAJOR} found "
                       "(ISOLINE_MODULE_LINT_CLANG)")
    else()
        message(STATUS "[isoline] export lint: off")
    endif()
    set(ISOLINE_MODULE_LINT_CLANG "${accepted}" PARENT_SCOPE)
endfunction()

if(ISOLINE_TESTING)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    _isoline_module_lint_clang()
    if(NOT Python3_Interpreter_FOUND)
        message(STATUS "[isoline] export lint: off, no Python 3")
        set(ISOLINE_MODULE_LINT_CLANG "")
    endif()
else()
    set(ISOLINE_MODULE_LINT_CLANG "")
endif()

## @brief Sets @p outVar to a generator expression that writes @p values as the elements of a JSON array,
## empty and repeated ones left out. The values are written as they are: a manifest value holds no `"` or `\` (paths
## in CMake's own form, plain definitions), and one that does makes the manifest unreadable, which the
## lint reports as a tool error.
function(_isoline_module_json_array outVar values)
    set(filtered "$<REMOVE_DUPLICATES:$<FILTER:${values},EXCLUDE,^$>>")
    set(${outVar} "[$<$<BOOL:${filtered}>:\"$<JOIN:${filtered},\"$<COMMA> \">\">]" PARENT_SCOPE)
endfunction()

## @brief Declares a C++ module of a library: one row per public library target (D1, D12).
##
## isoline_add_module(NAME <module> LIBRARY <target> SOURCE <interface unit>
##                    NAMESPACES <namespace>... INTERNAL_NAMESPACES <namespace>...
##                    [IMPORTS <module>...] [FOREIGN_NAMES <qualified name>...]
##                    [INSTALL_COMPONENT <component>])
##  - NAME is what consumers import; LIBRARY's HEADER_SET is the module's surface; SOURCE is the interface
##    unit, relative to the current source directory, generated by the lint's --write and committed.
##  - NAMESPACES are public: their names are exported. INTERNAL_NAMESPACES are not, and neither is any
##    namespace named `testing` or `detail`, nor one under either; a namespace under a public one that is
##    neither fails the lint, and so does a public one that is internal by these rules.
##  - IMPORTS are re-exported modules (`export import`), FOREIGN_NAMES names of other libraries the module
##    re-exports (`export namespace core::async { using core::async::Task; }`).
##  - INSTALL_COMPONENT is the install component of the module's files, `isoline` by default.
## Writes ${PROJECT_BINARY_DIR}/modules/<NAME>.surface.json and appends NAME to the global property
## ISOLINE_MODULE_ROWS. With ISOLINE_TESTING and the export lint (ISOLINE_MODULE_LINT_CLANG, Python 3), it
## registers the test module-exports-<NAME> (label lint) and the target isoline_write_module_exports_<NAME>,
## which the target isoline_write_module_exports runs with every other row's.
function(isoline_add_module)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "NAME;LIBRARY;SOURCE;INSTALL_COMPONENT"
                          "NAMESPACES;INTERNAL_NAMESPACES;IMPORTS;FOREIGN_NAMES")
    foreach(required IN ITEMS NAME LIBRARY SOURCE NAMESPACES)
        if(NOT arg_${required})
            message(FATAL_ERROR "isoline_add_module: ${required} is required")
        endif()
    endforeach()
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "isoline_add_module(${arg_NAME}): unknown arguments ${arg_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT TARGET ${arg_LIBRARY})
        message(FATAL_ERROR "isoline_add_module(${arg_NAME}): LIBRARY ${arg_LIBRARY} is no target")
    endif()
    if(NOT arg_INSTALL_COMPONENT)
        set(arg_INSTALL_COMPONENT "isoline")
    endif()
    # The standard the module's consumers compile with, which the lint parses and precompiles the headers in:
    # the highest cxx_std_<N> of the library's interface compile features (its PUBLIC
    # target_compile_features()), not its own CXX_STANDARD, which builds the library and reaches no consumer.
    get_target_property(interfaceFeatures ${arg_LIBRARY} INTERFACE_COMPILE_FEATURES)
    set(cxxStandard "")
    foreach(feature IN LISTS interfaceFeatures)
        if(feature MATCHES "^cxx_std_([0-9]+)$" AND (cxxStandard STREQUAL "" OR CMAKE_MATCH_1 GREATER cxxStandard))
            set(cxxStandard "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    if(NOT cxxStandard)
        message(FATAL_ERROR "isoline_add_module(${arg_NAME}): ${arg_LIBRARY} names no cxx_std_<N> interface compile "
                            "feature (target_compile_features(${arg_LIBRARY} PUBLIC cxx_std_23)): the standard its "
                            "consumers compile with")
    endif()

    # The include directories the headers compile with: the library's own, which carry its link
    # interface's. Those of a dependency that is SYSTEM (as the vendored core-cpp is) or imported, and every
    # dependency's INTERFACE_SYSTEM_INCLUDE_DIRECTORIES, are system ones, as in the library's own build.
    get_target_property(linkLibraries ${arg_LIBRARY} LINK_LIBRARIES)
    get_target_property(noSystemFromImported ${arg_LIBRARY} NO_SYSTEM_FROM_IMPORTED)
    set(systemIncludeDirectories "")
    foreach(dependency IN LISTS linkLibraries)
        if(NOT TARGET ${dependency})
            continue()
        endif()
        get_target_property(aliased ${dependency} ALIASED_TARGET)
        if(aliased)
            set(dependency "${aliased}")
        endif()
        get_target_property(isSystem ${dependency} SYSTEM)
        get_target_property(isImported ${dependency} IMPORTED)
        if(isSystem OR (isImported AND NOT noSystemFromImported))
            list(APPEND systemIncludeDirectories "$<TARGET_PROPERTY:${dependency},INTERFACE_INCLUDE_DIRECTORIES>")
        endif()
        list(APPEND systemIncludeDirectories "$<TARGET_PROPERTY:${dependency},INTERFACE_SYSTEM_INCLUDE_DIRECTORIES>")
    endforeach()

    set(manifest "${PROJECT_BINARY_DIR}/modules/${arg_NAME}.surface.json")
    set(importManifests "")
    foreach(imported IN LISTS arg_IMPORTS)
        list(APPEND importManifests "${PROJECT_BINARY_DIR}/modules/${imported}.surface.json")
    endforeach()
    cmake_path(ABSOLUTE_PATH arg_SOURCE BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" OUTPUT_VARIABLE source)
    _isoline_module_json_array(headerSet "$<TARGET_PROPERTY:${arg_LIBRARY},HEADER_SET>")
    _isoline_module_json_array(headerBaseDirectories "$<TARGET_PROPERTY:${arg_LIBRARY},HEADER_DIRS>")
    _isoline_module_json_array(namespaces "${arg_NAMESPACES}")
    _isoline_module_json_array(internalNamespaces "${arg_INTERNAL_NAMESPACES}")
    _isoline_module_json_array(imports "${importManifests}")
    _isoline_module_json_array(foreignNames "${arg_FOREIGN_NAMES}")
    _isoline_module_json_array(includeDirectories "$<TARGET_PROPERTY:${arg_LIBRARY},INCLUDE_DIRECTORIES>")
    _isoline_module_json_array(systemIncludes "${systemIncludeDirectories}")
    _isoline_module_json_array(compileDefinitions "$<TARGET_PROPERTY:${arg_LIBRARY},COMPILE_DEFINITIONS>")
    file(GENERATE OUTPUT "${manifest}" CONTENT "{
  \"name\": \"${arg_NAME}\",
  \"source\": \"${source}\",
  \"headerSet\": ${headerSet},
  \"headerBaseDirectories\": ${headerBaseDirectories},
  \"namespaces\": ${namespaces},
  \"internalNamespaces\": ${internalNamespaces},
  \"imports\": ${imports},
  \"foreignNames\": ${foreignNames},
  \"includeDirectories\": ${includeDirectories},
  \"systemIncludeDirectories\": ${systemIncludes},
  \"compileDefinitions\": ${compileDefinitions},
  \"cxxStandard\": \"${cxxStandard}\"
}
" TARGET ${arg_LIBRARY})
    set_property(GLOBAL APPEND PROPERTY ISOLINE_MODULE_ROWS "${arg_NAME}")
    set_property(GLOBAL PROPERTY ISOLINE_MODULE_${arg_NAME}_INSTALL_COMPONENT "${arg_INSTALL_COMPONENT}")

    if(NOT ISOLINE_MODULE_LINT_CLANG)
        return()
    endif()
    set(lint "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/scripts/check-module-exports.py"
             --clang "${ISOLINE_MODULE_LINT_CLANG}" --manifest "${manifest}"
             --work-directory "${PROJECT_BINARY_DIR}/modules/${arg_NAME}.lint")
    add_test(NAME module-exports-${arg_NAME} COMMAND ${lint} --check)
    # One parse of the header set and one precompile of the interface unit: seconds, under a sanitizer
    # build's load too.
    set_tests_properties(module-exports-${arg_NAME} PROPERTIES LABELS lint TIMEOUT 300)
    add_custom_target(isoline_write_module_exports_${arg_NAME}
                      COMMAND ${lint} --write
                      COMMENT "Generating the interface unit of module ${arg_NAME}: ${arg_SOURCE}"
                      VERBATIM)
    if(NOT TARGET isoline_write_module_exports)
        add_custom_target(isoline_write_module_exports)
    endif()
    add_dependencies(isoline_write_module_exports isoline_write_module_exports_${arg_NAME})
endfunction()
