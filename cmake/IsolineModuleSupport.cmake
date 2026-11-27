# SPDX-License-Identifier: Apache-2.0
#
# Where Isoline builds C++ modules, as data: the compilers with their floors and refusals, the generators,
# and the tool floors. A new compiler, generator or floor is a row here, nothing else.
#
# The floors are the versions a lane runs, not the versions CMake documents: Clang 19.1.0 is the lowest
# release that builds Isoline and the core-cpp it installs against (CMake documents 18.1.2; Clang 18 has
# no alias template argument deduction, which core-cpp uses), GCC 15.2 is the pinned gcc:15.2.0 image,
# MSVC 19.44 is toolset 14.44 (VS 17.14).
#
# Everything here is pure: the functions read their arguments and these tables, never the cache or the
# toolchain, so tests/cmake/module-support_test.cmake drives them in script mode. cmake/IsolineModules.cmake
# feeds them the real toolchain.

include_guard(GLOBAL)

# An empty argument (an empty Ninja version, an empty reason) is an empty string to cmake_parse_arguments(),
# not an unset variable. The functions below keep this setting: include() gives the file a policy scope of
# its own, and a function records the policies in force where it is defined.
if(POLICY CMP0174)
    cmake_policy(SET CMP0174 NEW)
endif()

# The compilers: (compiler id, front end variant, target ABI, minimum version, verdict, reason, tripwire,
# dependency scanner).
#  - The first three are CMAKE_CXX_COMPILER_ID, CMAKE_CXX_COMPILER_FRONTEND_VARIANT and the target ABI
#    (MSVC where the compiler is MSVC or simulates it, GNU otherwise).
#  - A refused row names its reason. Its tripwire, where it has one, is a test of CMake itself that goes
#    red once CMake scans modules for that compiler (ISOLINE_MODULE_TRIPWIRES in IsolineModules.cmake).
#  - The scanner is the tool CMake runs to scan the compiler's module dependencies, where it is not the
#    compiler itself; CMake records it in CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS.
# A toolchain no row names is refused: the table fails closed. Rows hold empty cells, so every walk over a
# table copies it quoted ("${...}"): an unquoted expansion drops empty elements.
set(ISOLINE_MODULE_COMPILERS
    "Clang"      "GNU"  "GNU"  "19.1.0" "supported" ""
        "-"              "clang-scan-deps"
    "GNU"        "GNU"  "GNU"  "15.2"   "supported" ""
        "-"              "-"
    "MSVC"       "MSVC" "MSVC" "19.44"  "supported" ""
        "-"              "-"
    "AppleClang" "GNU"  "GNU"  "-"      "refused"   "CMake has no module dependency scanning for Apple Clang"
        "scanning-probe" "-"
    "Clang"      "MSVC" "MSVC" "-"      "refused"   "CMake has no module dependency scanning for clang-cl"
        "scanning-probe" "-"
    "Clang"      "GNU"  "MSVC" "-"      "refused"   "Clang targeting the MSVC ABI is untested for modules"
        "-"              "-")

# The generators: (name, can consume installed modules, build tool with a floor). The Visual Studio
# generators build modules in-tree, but can neither install module information nor build BMIs for an
# imported module target (cmake-cxxmodules(7)). Ninja Multi-Config is supported as CMake documents it:
# its header-mode install is measured (check-install's multi-config handling), its installed modules are
# unmeasured, no lane running it (a follow-up row).
set(ISOLINE_MODULE_GENERATORS
    "Ninja"                 "yes" "Ninja"
    "Ninja Multi-Config"    "yes" "Ninja"
    "Visual Studio 17 2022" "no"  "-"
    "Visual Studio 18 2026" "no"  "-")

# The tool floors: (tool, minimum version). CMake 4.3 is the pinned archive of every module lane; 1.11 is
# Ninja's first release with the dyndep support CMake's module builds need, and 1.11.1 the one the floor
# lane runs.
set(ISOLINE_MODULE_TOOL_FLOORS
    "CMake" "4.3"
    "Ninja" "1.11.1")

# The compilers whose clang-tidy can read their BMIs: BMIs carry a version check, so clang-tidy must come
# from the very LLVM build that compiles them.
set(ISOLINE_MODULE_TIDY_COMPILERS
    "Clang")

# The LLVM major of the export lint's Clang (scripts/check-module-exports.py): the one CI pins. The lint's
# Clang dumps the header set's AST, whose JSON form is Clang's own and changes between majors, so another
# major (or Apple's Clang, whose versions are Xcode's) is refused rather than trusted.
set(ISOLINE_MODULE_LINT_LLVM_MAJOR 22)

## @brief Sets @p outVar to the minimum version of @p tool in ISOLINE_MODULE_TOOL_FLOORS.
function(_isoline_module_tool_floor tool outVar)
    set(rows "${ISOLINE_MODULE_TOOL_FLOORS}")
    while(rows)
        list(POP_FRONT rows rowTool rowFloor)
        if(rowTool STREQUAL tool)
            set(${outVar} "${rowFloor}" PARENT_SCOPE)
            return()
        endif()
    endwhile()
    message(FATAL_ERROR "[isoline] ISOLINE_MODULE_TOOL_FLOORS has no row for ${tool}")
endfunction()

## @brief Looks up the row of ISOLINE_MODULE_COMPILERS for a compiler.
##
## Sets <outPrefix>Found to ON or OFF and, where found, <outPrefix>Minimum, <outPrefix>Verdict,
## <outPrefix>Reason, <outPrefix>Tripwire and <outPrefix>Scanner (`-` for none).
function(_isoline_module_compiler_row compilerId frontend targetAbi outPrefix)
    set(rows "${ISOLINE_MODULE_COMPILERS}")
    while(rows)
        list(POP_FRONT rows rowId rowFrontend rowAbi rowMinimum rowVerdict rowReason rowTripwire rowScanner)
        if(rowId STREQUAL compilerId AND rowFrontend STREQUAL frontend AND rowAbi STREQUAL targetAbi)
            set(${outPrefix}Found ON PARENT_SCOPE)
            set(${outPrefix}Minimum "${rowMinimum}" PARENT_SCOPE)
            set(${outPrefix}Verdict "${rowVerdict}" PARENT_SCOPE)
            set(${outPrefix}Reason "${rowReason}" PARENT_SCOPE)
            set(${outPrefix}Tripwire "${rowTripwire}" PARENT_SCOPE)
            set(${outPrefix}Scanner "${rowScanner}" PARENT_SCOPE)
            return()
        endif()
    endwhile()
    set(${outPrefix}Found OFF PARENT_SCOPE)
endfunction()

## @brief Looks up the row of ISOLINE_MODULE_GENERATORS for @p generator.
##
## Sets <outPrefix>Found to ON or OFF and, where found, <outPrefix>InstalledModules and <outPrefix>Tool
## (`-` for none).
function(_isoline_module_generator_row generator outPrefix)
    set(rows "${ISOLINE_MODULE_GENERATORS}")
    while(rows)
        list(POP_FRONT rows rowName rowInstalledModules rowTool)
        if(rowName STREQUAL generator)
            set(${outPrefix}Found ON PARENT_SCOPE)
            set(${outPrefix}InstalledModules "${rowInstalledModules}" PARENT_SCOPE)
            set(${outPrefix}Tool "${rowTool}" PARENT_SCOPE)
            return()
        endif()
    endwhile()
    set(${outPrefix}Found OFF PARENT_SCOPE)
endfunction()

## @brief isoline_module_support()'s refusal: sets its outputs to `refused`, the reason (the arguments,
## concatenated) and `no`, then returns from it. The first check that fails names the reason.
macro(_isoline_module_refuse)
    string(CONCAT refusal ${ARGN})
    set(${arg_OUT_VERDICT} "refused" PARENT_SCOPE)
    set(${arg_OUT_REASON} "${refusal}" PARENT_SCOPE)
    set(${arg_OUT_INSTALLED_MODULES} "no" PARENT_SCOPE)
    return()
endmacro()

## @brief Decides whether a toolchain builds Isoline's C++ modules.
##
## isoline_module_support(
##     COMPILER_ID <id> FRONTEND <variant> TARGET_ABI <GNU|MSVC> VERSION <compiler version>
##     GENERATOR <generator> CMAKE_VERSION <version> NINJA_VERSION <version, or empty without Ninja>
##     SCAN_RULE_DEFINED <yes|no> SCAN_DEPS_FOUND <yes|no|not-needed>
##     OUT_VERDICT <var> OUT_REASON <var> OUT_INSTALLED_MODULES <var>)
##
## NINJA_VERSION is the version of the generator row's build tool, which is Ninja's for every row today;
## the argument is renamed when a row names a second tool.
## SCAN_RULE_DEFINED says whether CMake defines CMAKE_CXX_SCANDEP_SOURCE for the compiler, and
## SCAN_DEPS_FOUND whether CMake found clang-scan-deps (`not-needed` where CMake did not look for it).
## Sets OUT_VERDICT to `supported` or `refused`, OUT_REASON to why it is refused (empty when supported),
## and OUT_INSTALLED_MODULES to whether the generator can consume installed modules (`yes` or `no`;
## `no` when refused). Pure: reads only its arguments and the tables above.
function(isoline_module_support)
    set(inputs COMPILER_ID FRONTEND TARGET_ABI VERSION GENERATOR CMAKE_VERSION NINJA_VERSION SCAN_RULE_DEFINED
               SCAN_DEPS_FOUND)
    set(outputs OUT_VERDICT OUT_REASON OUT_INSTALLED_MODULES)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "${inputs};${outputs}" "")
    foreach(output IN LISTS outputs)
        if(NOT arg_${output})
            message(FATAL_ERROR "isoline_module_support: ${output} <var> is required")
        endif()
    endforeach()

    _isoline_module_tool_floor(CMake cmakeFloor)
    if(arg_CMAKE_VERSION VERSION_LESS cmakeFloor)
        _isoline_module_refuse("CMake ${arg_CMAKE_VERSION} is below the C++ modules floor ${cmakeFloor}")
    endif()

    _isoline_module_compiler_row("${arg_COMPILER_ID}" "${arg_FRONTEND}" "${arg_TARGET_ABI}" compiler)
    if(NOT compilerFound)
        _isoline_module_refuse("no row of the C++ module support table names ${arg_COMPILER_ID} "
                               "(front end '${arg_FRONTEND}', target ABI ${arg_TARGET_ABI}): "
                               "an unknown toolchain is refused")
    endif()
    if(NOT compilerVerdict STREQUAL "supported")
        _isoline_module_refuse("${compilerReason}")
    endif()
    if(arg_VERSION VERSION_LESS compilerMinimum)
        _isoline_module_refuse("${arg_COMPILER_ID} ${arg_VERSION} is below the C++ modules floor "
                               "${compilerMinimum}")
    endif()
    if(NOT arg_SCAN_RULE_DEFINED STREQUAL "yes")
        _isoline_module_refuse("CMake ${arg_CMAKE_VERSION} defines no module dependency scanning "
                               "(CMAKE_CXX_SCANDEP_SOURCE) for ${arg_COMPILER_ID} ${arg_VERSION}")
    endif()
    if(NOT compilerScanner STREQUAL "-" AND NOT arg_SCAN_DEPS_FOUND STREQUAL "yes")
        _isoline_module_refuse("${compilerScanner} was not found: CMake scans the module dependencies of "
                               "${arg_COMPILER_ID} with it (CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS)")
    endif()

    _isoline_module_generator_row("${arg_GENERATOR}" generator)
    if(NOT generatorFound)
        set(names "")
        set(rows "${ISOLINE_MODULE_GENERATORS}")
        while(rows)
            list(POP_FRONT rows rowName rowInstalledModules rowTool)
            list(APPEND names "${rowName}")
        endwhile()
        list(JOIN names ", " names)
        _isoline_module_refuse("the generator '${arg_GENERATOR}' builds no C++ modules here (supported: ${names})")
    endif()
    if(NOT generatorTool STREQUAL "-")
        _isoline_module_tool_floor("${generatorTool}" toolFloor)
        if(arg_NINJA_VERSION STREQUAL "")
            _isoline_module_refuse("the version of ${generatorTool} is unknown; the C++ modules floor is "
                                   "${toolFloor}")
        endif()
        if(arg_NINJA_VERSION VERSION_LESS toolFloor)
            _isoline_module_refuse("${generatorTool} ${arg_NINJA_VERSION} is below the C++ modules floor "
                                   "${toolFloor}")
        endif()
    endif()

    set(${arg_OUT_VERDICT} "supported" PARENT_SCOPE)
    set(${arg_OUT_REASON} "" PARENT_SCOPE)
    set(${arg_OUT_INSTALLED_MODULES} "${generatorInstalledModules}" PARENT_SCOPE)
endfunction()

## @brief Sets OUT_TRIPWIRE to the tripwire of the compiler's row (a key of ISOLINE_MODULE_TRIPWIRES), or
## to "" where the row has none, no row names the compiler, or CMAKE_VERSION is below the CMake row of
## ISOLINE_MODULE_TOOL_FLOORS: a tripwire asks whether the CMake the gate accepts scans that compiler, and
## below the floor its probe would test a CMake the gate refuses anyway (or, below 3.28, one without
## FILE_SET CXX_MODULES at all).
##
## isoline_module_compiler_tripwire(COMPILER_ID <id> FRONTEND <variant> TARGET_ABI <abi>
##                                  CMAKE_VERSION <version> OUT_TRIPWIRE <var>)
function(isoline_module_compiler_tripwire)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "COMPILER_ID;FRONTEND;TARGET_ABI;CMAKE_VERSION;OUT_TRIPWIRE" "")
    _isoline_module_compiler_row("${arg_COMPILER_ID}" "${arg_FRONTEND}" "${arg_TARGET_ABI}" compiler)
    _isoline_module_tool_floor(CMake cmakeFloor)
    set(tripwire "")
    if(compilerFound AND NOT compilerTripwire STREQUAL "-" AND NOT arg_CMAKE_VERSION VERSION_LESS cmakeFloor)
        set(tripwire "${compilerTripwire}")
    endif()
    set(${arg_OUT_TRIPWIRE} "${tripwire}" PARENT_SCOPE)
endfunction()

## @brief Sets OUT_TARGET_ABI to the target ABI the compiler rows name: MSVC where the compiler is MSVC or
## simulates it (clang-cl, and Clang's GNU front end targeting *-windows-msvc), GNU otherwise.
##
## isoline_module_target_abi(COMPILER_ID <CMAKE_CXX_COMPILER_ID> SIMULATE_ID <CMAKE_CXX_SIMULATE_ID>
##                           OUT_TARGET_ABI <var>)
function(isoline_module_target_abi)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "COMPILER_ID;SIMULATE_ID;OUT_TARGET_ABI" "")
    set(targetAbi "GNU")
    if(arg_COMPILER_ID STREQUAL "MSVC" OR arg_SIMULATE_ID STREQUAL "MSVC")
        set(targetAbi "MSVC")
    endif()
    set(${arg_OUT_TARGET_ABI} "${targetAbi}" PARENT_SCOPE)
endfunction()

## @brief Turns what CMake knows about a toolchain's module scanning into isoline_module_support()'s
## inputs.
##
## isoline_module_scan_inputs(SCAN_RULE <CMAKE_CXX_SCANDEP_SOURCE>
##                            SCAN_DEPS_DEFINED <yes|no: whether CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS is defined>
##                            SCAN_DEPS <CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS>
##                            BUILD_TOOL_OUTPUT <what `<build tool> --version` printed, or empty>
##                            OUT_SCAN_RULE_DEFINED <var> OUT_SCAN_DEPS_FOUND <var> OUT_BUILD_TOOL_VERSION <var>)
## CMake looks for clang-scan-deps beside a Clang of the GNU front end only, so where it left the variable
## undefined the scanner is `not-needed`; a defined but empty or *-NOTFOUND one is `no`. The build tool's
## version is the first dotted number it printed, or empty.
function(isoline_module_scan_inputs)
    set(inputs SCAN_RULE SCAN_DEPS_DEFINED SCAN_DEPS BUILD_TOOL_OUTPUT)
    set(outputs OUT_SCAN_RULE_DEFINED OUT_SCAN_DEPS_FOUND OUT_BUILD_TOOL_VERSION)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "${inputs};${outputs}" "")
    set(scanRuleDefined "no")
    if(arg_SCAN_RULE)
        set(scanRuleDefined "yes")
    endif()
    set(scanDepsFound "not-needed")
    if(arg_SCAN_DEPS_DEFINED STREQUAL "yes")
        set(scanDepsFound "no")
        if(arg_SCAN_DEPS)
            set(scanDepsFound "yes")
        endif()
    endif()
    string(REGEX MATCH "^[0-9]+(\\.[0-9]+)*" buildToolVersion "${arg_BUILD_TOOL_OUTPUT}")
    set(${arg_OUT_SCAN_RULE_DEFINED} "${scanRuleDefined}" PARENT_SCOPE)
    set(${arg_OUT_SCAN_DEPS_FOUND} "${scanDepsFound}" PARENT_SCOPE)
    set(${arg_OUT_BUILD_TOOL_VERSION} "${buildToolVersion}" PARENT_SCOPE)
endfunction()

## @brief Sets @p outVar to the build tool of @p generator's row with a floor (`Ninja`), or to "".
function(isoline_module_generator_tool generator outVar)
    _isoline_module_generator_row("${generator}" generator)
    set(tool "")
    if(generatorFound AND NOT generatorTool STREQUAL "-")
        set(tool "${generatorTool}")
    endif()
    set(${outVar} "${tool}" PARENT_SCOPE)
endfunction()

## @brief Decides whether clang-tidy may run beside a module build: it must come from the compiler's own
## LLVM build, because it reads the BMIs the compiler writes.
##
## isoline_module_tidy_refusal(COMPILER_ID <id> VERSION <compiler version>
##                             TIDY_VERSION_OUTPUT <what `clang-tidy --version` printed> OUT_REASON <var>)
## Sets OUT_REASON to why clang-tidy is refused, or to "" where it is the compiler's LLVM.
function(isoline_module_tidy_refusal)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "COMPILER_ID;VERSION;TIDY_VERSION_OUTPUT;OUT_REASON" "")
    set(reason "")
    string(REGEX MATCH "LLVM version ([0-9]+(\\.[0-9]+)*)" ignored "${arg_TIDY_VERSION_OUTPUT}")
    set(tidyVersion "${CMAKE_MATCH_1}")
    if(NOT arg_COMPILER_ID IN_LIST ISOLINE_MODULE_TIDY_COMPILERS)
        set(reason "clang-tidy cannot read the BMIs of ${arg_COMPILER_ID} ${arg_VERSION}")
    elseif(tidyVersion STREQUAL "")
        set(reason "clang-tidy printed no 'LLVM version' to tell its LLVM build from the compiler's")
    elseif(NOT tidyVersion VERSION_EQUAL arg_VERSION)
        set(reason "clang-tidy is LLVM ${tidyVersion}, but the compiler is ${arg_COMPILER_ID} ${arg_VERSION}: "
                   "the BMIs it reads must come from its own LLVM build")
    endif()
    string(JOIN "" reason ${reason})
    set(${arg_OUT_REASON} "${reason}" PARENT_SCOPE)
endfunction()

## @brief Decides whether a clang++ may be the export lint's Clang: LLVM's (not Apple's) at
## ISOLINE_MODULE_LINT_LLVM_MAJOR.
##
## isoline_module_lint_clang_refusal(VERSION_OUTPUT <what `<clang++> --version` printed> OUT_REASON <var>)
## Sets OUT_REASON to why it is refused, or to "" where it is accepted.
function(isoline_module_lint_clang_refusal)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "VERSION_OUTPUT;OUT_REASON" "")
    set(reason "")
    string(REGEX MATCH "clang version ([0-9]+)(\\.[0-9]+)*" ignored "${arg_VERSION_OUTPUT}")
    set(major "${CMAKE_MATCH_1}")
    if(arg_VERSION_OUTPUT MATCHES "Apple clang")
        set(reason "Apple's Clang is not LLVM ${ISOLINE_MODULE_LINT_LLVM_MAJOR}")
    elseif(major STREQUAL "")
        set(reason "it printed no 'clang version'")
    elseif(NOT major EQUAL ISOLINE_MODULE_LINT_LLVM_MAJOR)
        set(reason "it is LLVM ${major}, not the pinned LLVM ${ISOLINE_MODULE_LINT_LLVM_MAJOR}")
    endif()
    set(${arg_OUT_REASON} "${reason}" PARENT_SCOPE)
endfunction()
