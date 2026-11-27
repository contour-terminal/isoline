# SPDX-License-Identifier: Apache-2.0
#
# The C++ module support table (cmake/IsolineModuleSupport.cmake), in script mode:
#
#   cmake -P tests/cmake/module-support_test.cmake
#
# Every toolchain is a row of inputs and the verdict it must get. A `-` stands for an empty value: no
# reason, no Ninja (a Visual Studio generator). Any row that gets another verdict, a reason without the
# expected text, or another "installed modules" answer fails the script.

cmake_minimum_required(VERSION 3.25)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/IsolineModuleSupport.cmake")

# (compiler id, front end, target ABI, version, generator, CMake, Ninja, scan rule defined,
#  clang-scan-deps found → verdict, reason substring, installed modules)
set(supportCases
    "Clang"      "GNU"  "GNU"  "22.1.8"      "Ninja"                 "4.3.0" "1.13.2" "yes" "yes"
        "supported" "-"               "yes"
    "Clang"      "GNU"  "GNU"  "18.1.8"      "Ninja"                 "4.3.0" "1.13.2" "yes" "yes"
        "refused"   "19.1.0"          "no"
    "Clang"      "GNU"  "GNU"  "19.1.0"      "Ninja"                 "4.3.0" "1.13.2" "yes" "yes"
        "supported" "-"               "yes"
    "GNU"        "GNU"  "GNU"  "15.1.0"      "Ninja"                 "4.3.0" "1.13.2" "yes" "not-needed"
        "refused"   "15.2"            "no"
    "GNU"        "GNU"  "GNU"  "15.2.0"      "Ninja"                 "4.3.0" "1.13.2" "yes" "not-needed"
        "supported" "-"               "yes"
    "MSVC"       "MSVC" "MSVC" "19.43.34808" "Ninja"                 "4.3.0" "1.13.2" "yes" "not-needed"
        "refused"   "19.44"           "no"
    "MSVC"       "MSVC" "MSVC" "19.44.35207" "Ninja"                 "4.3.0" "1.13.2" "yes" "not-needed"
        "supported" "-"               "yes"
    "MSVC"       "MSVC" "MSVC" "19.51.36256" "Visual Studio 17 2022" "4.3.0" "-"      "yes" "not-needed"
        "supported" "-"               "no"
    "MSVC"       "MSVC" "MSVC" "19.51.36256" "Visual Studio 18 2026" "4.3.0" "-"      "yes" "not-needed"
        "supported" "-"               "no"
    "AppleClang" "GNU"  "GNU"  "17.0.0"      "Ninja"                 "4.3.0" "1.13.2" "no"  "not-needed"
        "refused"   "Apple Clang"     "no"
    "Clang"      "MSVC" "MSVC" "22.1.8"      "Ninja"                 "4.3.0" "1.13.2" "no"  "not-needed"
        "refused"   "clang-cl"        "no"
    "Clang"      "GNU"  "MSVC" "22.1.8"      "Ninja"                 "4.3.0" "1.13.2" "yes" "yes"
        "refused"   "MSVC ABI"        "no"
    "Clang"      "GNU"  "GNU"  "22.1.8"      "Unix Makefiles"        "4.3.0" "-"      "yes" "yes"
        "refused"   "generator"       "no"
    "Clang"      "GNU"  "GNU"  "22.1.8"      "Ninja"                 "4.2.3" "1.13.2" "yes" "yes"
        "refused"   "4.3"             "no"
    "Clang"      "GNU"  "GNU"  "22.1.8"      "Ninja"                 "4.3.0" "1.11.0" "yes" "yes"
        "refused"   "1.11.1"          "no"
    "Clang"      "GNU"  "GNU"  "22.1.8"      "Ninja"                 "4.3.0" "1.11.1" "yes" "yes"
        "supported" "-"               "yes"
    "Clang"      "GNU"  "GNU"  "22.1.8"      "Ninja"                 "4.3.0" "-"      "yes" "yes"
        "refused"   "Ninja"           "no"
    "Clang"      "GNU"  "GNU"  "22.1.8"      "Ninja Multi-Config"    "4.3.0" "1.13.2" "yes" "yes"
        "supported" "-"               "yes"
    "Clang"      "GNU"  "GNU"  "22.1.8"      "Ninja"                 "4.3.0" "1.13.2" "yes" "no"
        "refused"   "clang-scan-deps" "no"
    "Clang"      "GNU"  "GNU"  "22.1.8"      "Ninja"                 "4.3.0" "1.13.2" "yes" "not-needed"
        "refused"   "clang-scan-deps" "no"
    "Clang"      "GNU"  "GNU"  "22.1.8"      "Ninja"                 "4.3.0" "1.13.2" "no"  "yes"
        "refused"   "dependency scanning" "no"
    "GNU"        "GNU"  "GNU"  "15.2.0"      "Ninja"                 "4.3.0" "1.13.2" "no"  "not-needed"
        "refused"   "dependency scanning" "no"
    "Intel"      "GNU"  "GNU"  "2025.1.0"    "Ninja"                 "4.3.0" "1.13.2" "yes" "not-needed"
        "refused"   "unknown toolchain" "no"
    "-"          "-"    "GNU"  "-"           "Ninja"                 "4.3.0" "1.13.2" "no"  "not-needed"
        "refused"   "unknown toolchain" "no")

# (compiler id, front end, target ABI, CMake → tripwire): which refused toolchain gets a probe of CMake
# itself, and only on a CMake the gate accepts.
set(tripwireCases
    "AppleClang" "GNU"  "GNU"  "4.3.0"  "scanning-probe"
    "Clang"      "MSVC" "MSVC" "4.3.0"  "scanning-probe"
    "Clang"      "MSVC" "MSVC" "4.2.3"  "-"
    "AppleClang" "GNU"  "GNU"  "3.25.1" "-"
    "Clang"      "GNU"  "MSVC" "4.3.0"  "-"
    "Clang"      "GNU"  "GNU"  "4.3.0"  "-"
    "Intel"      "GNU"  "GNU"  "4.3.0"  "-")

# (compiler id, simulated id → target ABI): what the gate feeds the table as TARGET_ABI.
set(targetAbiCases
    "MSVC"       "-"    "MSVC"
    "Clang"      "MSVC" "MSVC"
    "Clang"      "-"    "GNU"
    "GNU"        "-"    "GNU"
    "AppleClang" "-"    "GNU")

# (scan rule, clang-scan-deps defined, clang-scan-deps, build tool output → scan rule defined,
#  clang-scan-deps found, build tool version): what the gate feeds the table from CMake's variables.
set(scanInputCases
    "clang-scan-deps -format=p1689 ..." "yes" "/usr/bin/clang-scan-deps-22"                 "1.13.2\n"
        "yes" "yes"        "1.13.2"
    "-"                                 "yes" "/usr/bin/clang-scan-deps-22"                 "1.11.1.git.kitware\n"
        "no"  "yes"        "1.11.1"
    "cl /scanDependencies ..."          "yes" "CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS-NOTFOUND" "-"
        "yes" "no"         "-"
    "clang-scan-deps -format=p1689 ..." "yes" "-"                                           "ninja 1.13\n"
        "yes" "no"         "-"
    "cl /scanDependencies ..."          "no"  "-"                                           "-"
        "yes" "not-needed" "-")

# (compiler id, version, clang-tidy --version output → reason substring): the same-LLVM rule.
set(tidyCases
    "Clang" "22.1.8" "LLVM (http://llvm.org/):\n  LLVM version 22.1.8\n  Optimized build." "-"
    "Clang" "22.1.8" "LLVM (http://llvm.org/):\n  LLVM version 20.1.8\n  Optimized build." "20.1.8"
    "Clang" "19.1.0" "LLVM version 19.1.01"                                          "19.1.01"
    "Clang" "22.1.8" "clang-tidy, some build"                                        "LLVM version"
    "GNU"   "15.2.0" "LLVM version 15.2.0"                                           "BMIs of GNU")

# (clang++ --version output → reason substring): the export lint's Clang, LLVM's at the pinned major.
set(lintClangCases
    "clang version 22.1.8 (Fedora 22.1.8-4.fc44)\nTarget: x86_64-redhat-linux-gnu"         "-"
    "Ubuntu clang version 22.1.0 (++20260301)\nTarget: x86_64-pc-linux-gnu"                 "-"
    "clang version 20.1.8\nTarget: x86_64-redhat-linux-gnu"                                 "LLVM 20"
    "Apple clang version 17.0.0 (clang-1700.0.13.5)\nTarget: arm64-apple-darwin24.5.0"      "Apple"
    "g++ (GCC) 16.2.1"                                                                       "no 'clang version'")

## @brief Sets @p outVar to @p value, or to "" where @p value is the placeholder `-`.
function(_unplaceholder outVar value)
    if(value STREQUAL "-")
        set(value "")
    endif()
    set(${outVar} "${value}" PARENT_SCOPE)
endfunction()

set(failures "")

set(cases "${supportCases}")
while(cases)
    list(POP_FRONT cases compilerId frontend targetAbi version generator cmakeVersion ninjaVersion scanRule
                         scanDeps expectedVerdict expectedReason expectedInstalled)
    foreach(column IN ITEMS compilerId frontend version ninjaVersion expectedReason)
        _unplaceholder(${column} "${${column}}")
    endforeach()
    isoline_module_support(
        COMPILER_ID "${compilerId}" FRONTEND "${frontend}" TARGET_ABI "${targetAbi}" VERSION "${version}"
        GENERATOR "${generator}" CMAKE_VERSION "${cmakeVersion}" NINJA_VERSION "${ninjaVersion}"
        SCAN_RULE_DEFINED "${scanRule}" SCAN_DEPS_FOUND "${scanDeps}"
        OUT_VERDICT verdict OUT_REASON reason OUT_INSTALLED_MODULES installed)
    string(CONCAT row "${compilerId}/${frontend}/${targetAbi} ${version}, ${generator}, CMake ${cmakeVersion}, "
                      "Ninja '${ninjaVersion}', scan rule ${scanRule}, clang-scan-deps ${scanDeps}")
    if(NOT verdict STREQUAL expectedVerdict)
        string(APPEND failures "\n  ${row}: ${verdict} (${reason}), expected ${expectedVerdict}")
        continue()
    endif()
    if(expectedReason STREQUAL "")
        if(NOT reason STREQUAL "")
            string(APPEND failures "\n  ${row}: gave the reason '${reason}', expected none")
        endif()
    else()
        string(FIND "${reason}" "${expectedReason}" position)
        if(position EQUAL -1)
            string(APPEND failures "\n  ${row}: the reason '${reason}' does not name '${expectedReason}'")
        endif()
    endif()
    if(NOT installed STREQUAL expectedInstalled)
        string(APPEND failures "\n  ${row}: installed modules ${installed}, expected ${expectedInstalled}")
    endif()
endwhile()

set(cases "${tripwireCases}")
while(cases)
    list(POP_FRONT cases compilerId frontend targetAbi cmakeVersion expectedTripwire)
    _unplaceholder(expectedTripwire "${expectedTripwire}")
    isoline_module_compiler_tripwire(COMPILER_ID "${compilerId}" FRONTEND "${frontend}" TARGET_ABI "${targetAbi}"
                                     CMAKE_VERSION "${cmakeVersion}" OUT_TRIPWIRE tripwire)
    if(NOT tripwire STREQUAL expectedTripwire)
        string(APPEND failures "\n  ${compilerId}/${frontend}/${targetAbi} on CMake ${cmakeVersion}: "
                               "tripwire '${tripwire}', expected '${expectedTripwire}'")
    endif()
endwhile()

set(cases "${targetAbiCases}")
while(cases)
    list(POP_FRONT cases compilerId simulateId expectedAbi)
    _unplaceholder(simulateId "${simulateId}")
    isoline_module_target_abi(COMPILER_ID "${compilerId}" SIMULATE_ID "${simulateId}" OUT_TARGET_ABI targetAbi)
    if(NOT targetAbi STREQUAL expectedAbi)
        string(APPEND failures "\n  ${compilerId} simulating '${simulateId}': target ABI ${targetAbi}, "
                               "expected ${expectedAbi}")
    endif()
endwhile()

set(cases "${scanInputCases}")
while(cases)
    list(POP_FRONT cases scanRule scanDepsDefined scanDeps buildToolOutput expectedScanRule expectedScanDeps
                         expectedBuildToolVersion)
    foreach(column IN ITEMS scanRule scanDeps buildToolOutput expectedBuildToolVersion)
        _unplaceholder(${column} "${${column}}")
    endforeach()
    isoline_module_scan_inputs(
        SCAN_RULE "${scanRule}" SCAN_DEPS_DEFINED "${scanDepsDefined}" SCAN_DEPS "${scanDeps}"
        BUILD_TOOL_OUTPUT "${buildToolOutput}" OUT_SCAN_RULE_DEFINED scanRuleDefined
        OUT_SCAN_DEPS_FOUND scanDepsFound OUT_BUILD_TOOL_VERSION buildToolVersion)
    string(CONCAT row "scan rule '${scanRule}', clang-scan-deps ${scanDepsDefined} '${scanDeps}', "
                      "build tool '${buildToolOutput}'")
    if(NOT scanRuleDefined STREQUAL expectedScanRule)
        string(APPEND failures "\n  ${row}: scan rule defined ${scanRuleDefined}, expected ${expectedScanRule}")
    endif()
    if(NOT scanDepsFound STREQUAL expectedScanDeps)
        string(APPEND failures "\n  ${row}: clang-scan-deps ${scanDepsFound}, expected ${expectedScanDeps}")
    endif()
    if(NOT buildToolVersion STREQUAL expectedBuildToolVersion)
        string(APPEND failures "\n  ${row}: build tool version '${buildToolVersion}', "
                               "expected '${expectedBuildToolVersion}'")
    endif()
endwhile()

set(cases "${tidyCases}")
while(cases)
    list(POP_FRONT cases compilerId version tidyOutput expectedReason)
    _unplaceholder(expectedReason "${expectedReason}")
    isoline_module_tidy_refusal(COMPILER_ID "${compilerId}" VERSION "${version}"
                                TIDY_VERSION_OUTPUT "${tidyOutput}" OUT_REASON reason)
    string(REPLACE "\n" " | " shownOutput "${tidyOutput}")
    if(expectedReason STREQUAL "")
        if(NOT reason STREQUAL "")
            string(APPEND failures "\n  ${compilerId} ${version} with '${shownOutput}': refused ('${reason}'), "
                                   "expected accepted")
        endif()
    else()
        string(FIND "${reason}" "${expectedReason}" position)
        if(reason STREQUAL "" OR position EQUAL -1)
            string(APPEND failures "\n  ${compilerId} ${version} with '${shownOutput}': reason '${reason}', "
                                   "expected one naming '${expectedReason}'")
        endif()
    endif()
endwhile()

set(cases "${lintClangCases}")
while(cases)
    list(POP_FRONT cases versionOutput expectedReason)
    _unplaceholder(expectedReason "${expectedReason}")
    isoline_module_lint_clang_refusal(VERSION_OUTPUT "${versionOutput}" OUT_REASON reason)
    string(REPLACE "\n" " | " shownOutput "${versionOutput}")
    if(expectedReason STREQUAL "")
        if(NOT reason STREQUAL "")
            string(APPEND failures "\n  lint Clang '${shownOutput}': refused ('${reason}'), expected accepted")
        endif()
    else()
        string(FIND "${reason}" "${expectedReason}" position)
        if(reason STREQUAL "" OR position EQUAL -1)
            string(APPEND failures "\n  lint Clang '${shownOutput}': reason '${reason}', "
                                   "expected one naming '${expectedReason}'")
        endif()
    endif()
endwhile()

if(failures)
    message(FATAL_ERROR "module-support: failed:${failures}")
endif()
message(STATUS "module-support: passed")
