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
include(IsolineInstallRows)

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
    # Whether the generator's row can install module information (ISOLINE_MODULE_GENERATORS), which
    # cmake/IsolineInstall.cmake reads.
    set(isInstallable OFF)
    if(installedModules STREQUAL "yes")
        set(isInstallable ON)
    endif()
    set(ISOLINE_MODULES_INSTALLABLE ${isInstallable} PARENT_SCOPE)
endfunction()

# Whether this build installs its module targets: ON where ISOLINE_MODULES is on and the generator can
# install module information; OFF otherwise.
set(ISOLINE_MODULES_INSTALLABLE OFF)
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

# E2 (D13): what clang-tidy reports falsely on Isoline's module sources, as (source kind, check) rows. A
# target of a listed kind runs clang-tidy with exactly its kind's checks lifted
# (isoline_lift_module_tidy_checks()), and a comment beside each call names its rows: every other check
# still runs on it. clang-tidy-module-tripwire runs the
# build's clang-tidy on one source of each kind (ISOLINE_MODULE_TIDY_SOURCES) and fails once a listed
# check no longer fires. Measured with clang-tidy 22.1.8 and the project's .clang-tidy (2026-11-27):
#  - interface-unit: 93 of module isoline's 103 using-declarations get misc-unused-using-decls; an
#    exporting using-declaration is a use, which the check does not know.
#  - import-only: 4 readability-redundant-declaration, on the global operator new, new[], delete and
#    delete[] of an importer that includes a standard header (<memory>) before `import isoline;`: its
#    implicit declarations are taken for redeclarations of <new>'s, which the BMI's global module fragment
#    holds too. A bare `import isoline;` declares neither, and reports nothing.
#  - include-then-import: an Isoline header and `import isoline;` in one TU (D7): 362
#    readability-redundant-declaration and 51 performance-trivially-destructible. All but four (the global
#    operator new and delete, as for import-only) are in src/isoline headers, on their declarations, which
#    the BMI's global module fragment holds as well: each is taken for a redeclaration, and a class with an
#    implicit destructor for one whose destructor is defaulted on a later declaration.
#  - import-then-include: the other order (D7), which compiles on Clang only (module-import-then-include's
#    probe): 129 readability-redundant-declaration, for the same reason, and no
#    performance-trivially-destructible, so only the one check is lifted there.
#  - import-reexporting: an importer of a module that re-exports another (`import isoline.recipe_fixture;`,
#    whose unit does `export import isoline;`), and of nothing else: 366 readability-redundant-declaration
#    and 51 performance-trivially-destructible, as for include-then-import. Two imported BMIs hold the same
#    Isoline declarations in their global module fragments, isoline's and the re-exporting module's, whose
#    headers include Isoline's: each is taken for a redeclaration of the other. isoline_add_module_consumer()
#    lifts it on every importer of a row with IMPORTS.
#  - importing-interface-unit: the interface unit of a row with IMPORTS, whose global module fragment
#    includes its library's headers, which include Isoline's, before `export import isoline;`: an
#    include-then-import TU as well as an interface unit. On isoline.recipe_fixture's: 1
#    misc-unused-using-decls, 366 readability-redundant-declaration, 51 performance-trivially-destructible.
set(ISOLINE_MODULE_TIDY_FALSE_POSITIVES
    "interface-unit" "misc-unused-using-decls"
    "import-only" "readability-redundant-declaration"
    "include-then-import" "readability-redundant-declaration"
    "include-then-import" "performance-trivially-destructible"
    "import-then-include" "readability-redundant-declaration"
    "import-reexporting" "readability-redundant-declaration"
    "import-reexporting" "performance-trivially-destructible"
    "importing-interface-unit" "misc-unused-using-decls"
    "importing-interface-unit" "readability-redundant-declaration"
    "importing-interface-unit" "performance-trivially-destructible")

# Whether E1 (D13) applies: CMake drives Clang's GNU front end, whose synthetic BMI command carries `-c`,
# and the Clang is of a version that warns about it. The lowest version measured to warn is the row:
# Clang 22.1.8 does, Clang 19.1.0 (the floor row) accepts the `-c` silently, and the versions between are
# unmeasured. Below the row there is no E1 and no tripwire, so a Clang there that warns fails its -Werror
# build, which is the measurement that lowers the row.
set(ISOLINE_MODULE_PRECOMPILE_C_WARNS_FROM_CLANG "22")
set(ISOLINE_MODULE_PRECOMPILE_TAKES_C OFF)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU"
   AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL ISOLINE_MODULE_PRECOMPILE_C_WARNS_FROM_CLANG)
    set(ISOLINE_MODULE_PRECOMPILE_TAKES_C ON)
endif()

## @brief Lets clang-tidy run on @p target, a module target or an importer of one, with the checks of
## ISOLINE_MODULE_TIDY_FALSE_POSITIVES' rows of each @p kind lifted (E2, D13) and every other check kept:
## they join the target's ISOLINE_LIFTED_TIDY_CHECKS, which isoline_apply_tidy_command() reads, before or
## after isoline_apply_target_policy(). Without clang-tidy it changes no command. A kind without a row
## refuses the configure.
##
## isoline_lift_module_tidy_checks(<target> <kind>...)
function(isoline_lift_module_tidy_checks target)
    if(NOT ARGN)
        message(FATAL_ERROR "isoline_lift_module_tidy_checks(${target}): no kind named")
    endif()
    set(lifted "")
    foreach(kind IN LISTS ARGN)
        set(isKnown OFF)
        set(rows "${ISOLINE_MODULE_TIDY_FALSE_POSITIVES}")
        while(rows)
            list(POP_FRONT rows rowKind check)
            if(rowKind STREQUAL kind)
                set(isKnown ON)
                list(APPEND lifted "-${check}")
            endif()
        endwhile()
        if(NOT isKnown)
            message(FATAL_ERROR "isoline_lift_module_tidy_checks(${target}): ISOLINE_MODULE_TIDY_FALSE_POSITIVES "
                                "has no row of kind ${kind}")
        endif()
    endforeach()
    get_property(alreadyLifted TARGET ${target} PROPERTY ISOLINE_LIFTED_TIDY_CHECKS)
    list(APPEND alreadyLifted ${lifted})
    list(REMOVE_DUPLICATES alreadyLifted)
    set_property(TARGET ${target} PROPERTY ISOLINE_LIFTED_TIDY_CHECKS "${alreadyLifted}")
    isoline_apply_tidy_command(${target})
endfunction()

## @brief Registers the tripwires of E1 and E2 (D13), as CTests of label `lint`. Called by
## tests/modules/CMakeLists.txt once its sources are targets.
##  - cmake-precompile-tripwire, where E1 applies and Ninja builds: it reads the build's commands.
##  - clang-tidy-module-tripwire, where clang-tidy runs: it runs it on one source of each kind of
##    ISOLINE_MODULE_TIDY_FALSE_POSITIVES. A kind of a row without a source refuses the configure. The
##    BUILD_TARGETS, targets out of `all` that compile some of those sources, it builds first.
##
## isoline_add_module_exception_tripwires(TIDY_SOURCES <kind> <source>... [BUILD_TARGETS <target>...])
function(isoline_add_module_exception_tripwires)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "" "TIDY_SOURCES;BUILD_TARGETS")
    if(ISOLINE_MODULE_PRECOMPILE_TAKES_C AND CMAKE_GENERATOR MATCHES "^Ninja")
        add_test(NAME cmake-precompile-tripwire
                 COMMAND "${CMAKE_COMMAND}" "-DNINJA=${CMAKE_MAKE_PROGRAM}" "-DBUILD_DIR=${PROJECT_BINARY_DIR}"
                         "-DBUILD_CMAKE_VERSION=${CMAKE_VERSION}" "-DCOMPILER=${CMAKE_CXX_COMPILER}"
                         -P "${PROJECT_SOURCE_DIR}/tests/cmake/expect-precompile-with-c.cmake")
        # It reads the build tree, which module-import-then-include builds in (RESOURCE_LOCK build-tree).
        set_tests_properties(cmake-precompile-tripwire PROPERTIES LABELS lint TIMEOUT 60 RESOURCE_LOCK build-tree)
    endif()
    if(NOT ISOLINE_CLANG_TIDY_COMMAND)
        return()
    endif()
    set(kinds "")
    set(sources "${arg_TIDY_SOURCES}")
    while(sources)
        list(POP_FRONT sources kind source)
        list(APPEND kinds "${kind}")
    endwhile()
    set(rows "${ISOLINE_MODULE_TIDY_FALSE_POSITIVES}")
    while(rows)
        list(POP_FRONT rows kind check)
        if(NOT kind IN_LIST kinds)
            message(FATAL_ERROR "[isoline] ISOLINE_MODULE_TIDY_FALSE_POSITIVES row (${kind}, ${check}) has no "
                                "source of kind ${kind} for clang-tidy-module-tripwire")
        endif()
    endwhile()
    set(argumentsFile "${PROJECT_BINARY_DIR}/clang-tidy-module-tripwire/arguments.cmake")
    file(CONFIGURE OUTPUT "${argumentsFile}" CONTENT "set(ROWS [==[@ISOLINE_MODULE_TIDY_FALSE_POSITIVES@]==])
set(SOURCES [==[@arg_TIDY_SOURCES@]==])
set(BUILD_TARGETS [==[@arg_BUILD_TARGETS@]==])
" @ONLY)
    add_test(NAME clang-tidy-module-tripwire
             COMMAND "${CMAKE_COMMAND}" "-DCLANG_TIDY=${ISOLINE_CLANG_TIDY_EXE}" "-DBUILD_DIR=${PROJECT_BINARY_DIR}"
                     "-DARGUMENTS_FILE=${argumentsFile}" "-DCONFIG=$<CONFIG>"
                     -P "${PROJECT_SOURCE_DIR}/tests/cmake/expect-module-tidy-false-positives.cmake")
    # clang-tidy over the whole header set, once per kind, under a sanitizer build's load too. It reads the
    # build tree's module maps, which module-import-then-include builds in (RESOURCE_LOCK build-tree).
    set_tests_properties(clang-tidy-module-tripwire PROPERTIES LABELS lint TIMEOUT 600 RESOURCE_LOCK build-tree)
endfunction()

## @brief Creates the module target of a row (D2): <LIBRARY>_module, a static library that holds only the
## interface unit, in FILE_SET CXX_MODULES, and links LIBRARY and the module targets of IMPORTS. Its alias
## and export name are LIBRARY's without the `isoline_` prefix, plus `_module` (isoline::core_module). Sets
## the global property ISOLINE_MODULE_<NAME>_TARGET, which isoline_add_module_consumer() reads, and installs
## the target beside LIBRARY (isoline_install_module_target()).
##
## The module target belongs to LIBRARY alone. A copy of LIBRARY made after the row, such as a
## position-independent twin for the C API's shared library to link (P17a), copies its sources and its
## HEADERS file set but no FILE_SET CXX_MODULES, and is no module library: it gets no row and no module
## target, and an importer keeps linking this one, which links LIBRARY. A twin given an install row of its
## own installs no module beside it.
##
## _isoline_add_module_target(NAME <module> LIBRARY <target> SOURCE <absolute interface unit>
##                            [IMPORTS <module>...])
function(_isoline_add_module_target)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "NAME;LIBRARY;SOURCE" "IMPORTS")
    set(target ${arg_LIBRARY}_module)
    string(REGEX REPLACE "^isoline_" "" exportName "${arg_LIBRARY}")
    set(importTargets "")
    foreach(imported IN LISTS arg_IMPORTS)
        get_property(importTarget GLOBAL PROPERTY ISOLINE_MODULE_${imported}_TARGET)
        if(NOT importTarget)
            message(FATAL_ERROR "isoline_add_module(${arg_NAME}): IMPORTS ${imported} is no module row before it")
        endif()
        list(APPEND importTargets ${importTarget})
    endforeach()

    add_library(${target} STATIC)
    add_library(isoline::${exportName}_module ALIAS ${target})
    set_target_properties(${target} PROPERTIES EXPORT_NAME ${exportName}_module)
    target_sources(${target} PUBLIC FILE_SET CXX_MODULES FILES "${arg_SOURCE}")
    target_link_libraries(${target} PUBLIC ${arg_LIBRARY} ${importTargets})
    target_compile_features(${target} PUBLIC cxx_std_23)
    isoline_apply_target_policy(${target})
    if(ISOLINE_MODULE_PRECOMPILE_TAKES_C)
        # E1 (D13): CMake 4.3 builds an in-project importer's BMI in a synthetic target
        # (<target>@synth_<hash>) by `clang++ ... --precompile ... -o <bmi> -c <unit>.cppm`. The `-c` is
        # CMake's, not ours, and under -Werror Clang 22 refuses it (measured 2026-11-27):
        #   clang++: error: argument unused during compilation: '-c' [-Werror,-Wunused-command-line-argument]
        # Only that error is lifted, on module targets only. It cannot be narrower on CMake 4.3: the
        # synthetic target has no property of its own and takes the module target's compile options (a
        # source property of the .cppm reaches the same two commands), and the module target's one other
        # command, the object compile, uses its `-c`, so the lift changes nothing there. The warning still
        # prints, three times per synthetic target (its command, and clang-scan-deps' two scans of it), which
        # keeps the cause in sight. cmake-precompile-tripwire goes red once CMake passes no `-c` with
        # --precompile, or Clang no longer warns about it. Follow-up: report the redundant `-c` to CMake
        # upstream.
        isoline_apply_build_options(${target} COMPILE -Wno-error=unused-command-line-argument)
    endif()
    # E2 (D13), rows interface-unit of ISOLINE_MODULE_TIDY_FALSE_POSITIVES (misc-unused-using-decls), or for a
    # unit that imports, importing-interface-unit (that, readability-redundant-declaration and
    # performance-trivially-destructible).
    set(tidyKind interface-unit)
    if(arg_IMPORTS)
        set(tidyKind importing-interface-unit)
    endif()
    isoline_lift_module_tidy_checks(${target} ${tidyKind})
    set_property(GLOBAL PROPERTY ISOLINE_MODULE_${arg_NAME}_TARGET ${target})
    set_property(GLOBAL PROPERTY ISOLINE_MODULE_${arg_NAME}_IMPORTS "${arg_IMPORTS}")
    set_property(GLOBAL PROPERTY ISOLINE_MODULE_${arg_NAME}_SOURCE "${arg_SOURCE}")
    # Installed here, in the directory that creates it, beside LIBRARY (cmake/IsolineInstallRows.cmake).
    isoline_install_module_target(${target} ${arg_LIBRARY})
endfunction()

## @brief Makes @p target an importer of the named modules: it is scanned for module dependencies
## (CXX_SCAN_FOR_MODULES; the project's policies leave CMP0155 OLD, so no other target is) and links each
## module's target. An importer of a module whose row has IMPORTS gets E2's import-reexporting lift
## (isoline_lift_module_tidy_checks()), read from the row; the kinds of its own sources (an include before
## the import) remain its CMakeLists.txt's to name.
##
## isoline_add_module_consumer(<target> <module>...)
function(isoline_add_module_consumer target)
    if(NOT ARGN)
        message(FATAL_ERROR "isoline_add_module_consumer(${target}): no module named")
    endif()
    set_target_properties(${target} PROPERTIES CXX_SCAN_FOR_MODULES ON)
    foreach(module IN LISTS ARGN)
        get_property(moduleTarget GLOBAL PROPERTY ISOLINE_MODULE_${module}_TARGET)
        if(NOT moduleTarget)
            message(FATAL_ERROR "isoline_add_module_consumer(${target}): ${module} is no module row "
                                "(isoline_add_module(), with ISOLINE_MODULES ON)")
        endif()
        target_link_libraries(${target} PRIVATE ${moduleTarget})
        get_property(moduleImports GLOBAL PROPERTY ISOLINE_MODULE_${module}_IMPORTS)
        if(moduleImports)
            # E2 (D13), rows import-reexporting of ISOLINE_MODULE_TIDY_FALSE_POSITIVES.
            isoline_lift_module_tidy_checks(${target} import-reexporting)
        endif()
    endforeach()
endfunction()

## @brief Declares a C++ module of a library: one row per public library target (D1, D12).
##
## isoline_add_module(NAME <module> LIBRARY <target> SOURCE <interface unit>
##                    NAMESPACES <namespace>... INTERNAL_NAMESPACES <namespace>...
##                    [IMPORTS <module>...] [FOREIGN_NAMES <qualified name>...])
##  - NAME is what consumers import; LIBRARY's HEADER_SET is the module's surface; SOURCE is the interface
##    unit, relative to the current source directory, generated by the lint's --write and committed.
##  - NAMESPACES are public: their names are exported. INTERNAL_NAMESPACES are not, and neither is any
##    namespace named `testing` or `detail`, nor one under either; a namespace under a public one that is
##    neither fails the lint, and so does a public one that is internal by these rules.
##  - IMPORTS are re-exported modules (`export import`), FOREIGN_NAMES names of other libraries the module
##    re-exports (`export namespace core::async { using core::async::Task; }`).
##  - The module target installs with LIBRARY, in the export set and install component of LIBRARY's row
##    of ISOLINE_INSTALL_LIBRARIES (cmake/IsolineInstallRows.cmake): the component is that row's, so the
##    module row names none. With ISOLINE_INSTALL, a LIBRARY without a row refuses the configure.
## Writes ${PROJECT_BINARY_DIR}/modules/<NAME>.surface.json and appends NAME to the global property
## ISOLINE_MODULE_ROWS. With ISOLINE_TESTING and the export lint (ISOLINE_MODULE_LINT_CLANG, Python 3), it
## registers the test module-exports-<NAME> (label lint) and the target isoline_write_module_exports_<NAME>,
## which the target isoline_write_module_exports runs with every other row's.
function(isoline_add_module)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "NAME;LIBRARY;SOURCE"
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
    if(ISOLINE_MODULES)
        _isoline_add_module_target(NAME "${arg_NAME}" LIBRARY "${arg_LIBRARY}" SOURCE "${source}"
                                   IMPORTS ${arg_IMPORTS})
    endif()

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
