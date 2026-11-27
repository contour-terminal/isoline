# SPDX-License-Identifier: Apache-2.0
#
# C++26 reflection, annotations and contracts: where a compiler has them, Isoline uses them; everywhere
# else it builds the same library through a C++23 fallback. See docs/language-features.md.
#
# Which path a feature takes is decided by data: one feature row per (feature, compiler), each with a
# measured minimum version and a probe that compiles, and links, what Isoline uses. A compiler without a
# row, or below its row, takes the fallback even where its probe passes: every flag, library and report
# format is that compiler's data, measured before it is trusted. ISOLINE_<FEATURE> (AUTO, ON or OFF)
# decides what happens when the native path is out of reach. A feature is its rows: the feature list
# and the options are derived from them.
#
# Included at configure time, the module defines the options, selects each feature's path and sets
# ISOLINE_<FEATURE>_PATH (native or fallback). Included in script mode
# (tests/cmake/language-features_test.cmake), it only defines the functions, and the selection runs
# there with rows and a probe the test supplies.

include_guard(GLOBAL)

# An empty keyword value (no row's minimum, no diagnostic) is an empty string, not an unset variable.
# CMake before 3.31 has no such policy and unsets it: every comparison below quotes its expansion, so
# both read the same.
if(POLICY CMP0174)
    cmake_policy(SET CMP0174 NEW)
endif()

## Feature rows: feature|compilerId|minimumVersion|standard|flags|linkLibraries|requires|probe|purpose
## flags and linkLibraries are separated by spaces; probe is relative to the project's source tree;
## purpose says what Isoline uses the feature for, in the option's help text (a feature's first row
## gives it). Features are decided in the order they first appear, so a requirement's rows come before
## its users'. The contracts row's flags pin the semantic, so its probe tests the one Isoline uses.
set(ISOLINE_LANGUAGE_FEATURE_ROWS
    "reflection|GNU|16.2|26|-freflection|||cmake/probes/reflection.cpp|for Isoline's table checks"
    "annotations|GNU|16.2|26|-freflection||reflection|cmake/probes/annotations.cpp|for table rows derived from enumerator annotations"
    "contracts|GNU|16.2|26|-freflection -fcontracts -fcontract-evaluation-semantic=enforce|stdc++exp|reflection|cmake/probes/contracts.cpp|for preconditions in noexcept functions")

## The values an ISOLINE_<FEATURE> option takes: spelling|value, the spelling compared case-insensitively.
## CMake's boolean constants count as ON and OFF; any other spelling, the empty one included, is refused.
set(ISOLINE_LANGUAGE_FEATURE_OPTION_SPELLINGS
    "AUTO|AUTO"
    "ON|ON" "YES|ON" "TRUE|ON" "Y|ON" "1|ON"
    "OFF|OFF" "NO|OFF" "FALSE|OFF" "N|OFF" "0|OFF")

## Consumer-floor rows (owner, Q2): compilerId|firstReleaseWithReflectionAndContracts. Consumers move to
## C++26, and the fallback is deleted, once no release is empty: every compiler has shipped both.
set(ISOLINE_CONSUMER_FLOOR_ROWS
    "GNU|16.2"
    "Clang|"
    "AppleClang|"
    "MSVC|")

## The features whose native path changes how Isoline's libraries compile (isoline_apply_language_features).
## Reflection and annotations only add check translation units of their own.
set(ISOLINE_TARGET_LANGUAGE_FEATURES contracts)

## Contract-semantic rows: compilerId|semantic|verdict|flag|reason. The verdict is `selected`, `not
## offered` or `refused`; reason says what the semantic does, and why one that is not selected is
## refused. Every flag of a row not `selected` is refused wherever the configure scan finds it
## (isoline_refuse_contract_overrides()), so that a check never continues or is skipped.
set(ISOLINE_CONTRACT_SEMANTIC_ROWS
    "GNU|enforce|selected|-fcontract-evaluation-semantic=enforce|reports the condition, then terminates"
    "GNU|quick_enforce|not offered|-fcontract-evaluation-semantic=quick_enforce|terminates without a report, so no canary can tell which check fired"
    "GNU|observe|refused|-fcontract-evaluation-semantic=observe|continues past a violated precondition"
    "GNU|ignore|refused|-fcontract-evaluation-semantic=ignore|does not evaluate the check")

## The semantic Isoline's contracts are checked under: pinned, never an option (docs/language-features.md).
set(ISOLINE_CONTRACT_SEMANTIC enforce)

## Refused-flag rows beyond the semantics': compilerId|flag|reason. The configure scan refuses each.
set(ISOLINE_CONTRACT_REFUSED_FLAG_ROWS
    "GNU|-fcontracts-definition-check=off|silences a definition's pre (measured), which the preconditions move into"
    "GNU|-fno-contracts|turns contracts off in a build whose sources were selected to use them")

## Report rows: compilerId|path|spelling|prefix|suffix, `*` matching any. What a failed check prints
## around its condition, as CMake regular expressions, by the path contracts took and the spelling of the
## check's site (ISOLINE_PRECONDITION and ISOLINE_INVARIANT are `contract`, Require() is `require`). The
## first row that matches applies; the canary's tests are built from them (tests/CMakeLists.txt).
set(ISOLINE_CONTRACT_REPORT_ROWS
    "*|fallback|*|Precondition failed[.] |"
    "GNU|native|require|Precondition failed[.] |"
    "GNU|native|contract|contract violation in function [^\n]* at [^\n]*: |\n\\[assertion_kind: assert, semantic: enforce, mode: predicate_false, terminating: yes\\]")

## @brief Decides the contract semantic @p SEMANTIC on @p COMPILER from the semantic rows.
##
## isoline_contract_semantic(COMPILER <compilerId> SEMANTIC <semantic> ROWS <row>... OUT_FLAG <var>
##                           OUT_ERROR <var>)
## OUT_FLAG is the row's flag where its verdict is `selected`, else empty; OUT_ERROR is empty, or the
## configure error naming why the semantic is not taken. Pure.
function(isoline_contract_semantic)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "COMPILER;SEMANTIC;OUT_FLAG;OUT_ERROR" "ROWS")
    set(${arg_OUT_FLAG} "" PARENT_SCOPE)
    foreach(row IN LISTS arg_ROWS)
        if(NOT row MATCHES "^([^|]*)[|]([^|]*)[|]([^|]*)[|]([^|]*)[|](.*)$")
            message(FATAL_ERROR "[isoline] a contract-semantic row has five columns: ${row}")
        endif()
        if(NOT CMAKE_MATCH_1 STREQUAL arg_COMPILER OR NOT CMAKE_MATCH_2 STREQUAL arg_SEMANTIC)
            continue()
        endif()
        if(CMAKE_MATCH_3 STREQUAL "selected")
            set(${arg_OUT_FLAG} "${CMAKE_MATCH_4}" PARENT_SCOPE)
            set(${arg_OUT_ERROR} "" PARENT_SCOPE)
        else()
            set(${arg_OUT_ERROR}
                "[isoline] the contract semantic ${arg_SEMANTIC} is ${CMAKE_MATCH_3} on ${arg_COMPILER}: it ${CMAKE_MATCH_5}"
                PARENT_SCOPE)
        endif()
        return()
    endforeach()
    set(${arg_OUT_ERROR} "[isoline] no contract-semantic row names ${arg_SEMANTIC} for ${arg_COMPILER}" PARENT_SCOPE)
endfunction()

## @brief Sets @p OUT to the flags Isoline's contracts refuse on @p COMPILER, one `flag|reason` each: the
## flag of every semantic row not `selected`, then every refused-flag row. Empty for a compiler without
## rows. Pure.
##
## isoline_contract_refused_flags(COMPILER <compilerId> OUT <var>)
function(isoline_contract_refused_flags)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "COMPILER;OUT" "")
    set(refused "")
    foreach(row IN LISTS ISOLINE_CONTRACT_SEMANTIC_ROWS)
        string(REGEX MATCH "^([^|]*)[|]([^|]*)[|]([^|]*)[|]([^|]*)[|](.*)$" matched "${row}")
        if(CMAKE_MATCH_1 STREQUAL arg_COMPILER AND NOT CMAKE_MATCH_3 STREQUAL "selected")
            list(APPEND refused "${CMAKE_MATCH_4}|${CMAKE_MATCH_5}")
        endif()
    endforeach()
    foreach(row IN LISTS ISOLINE_CONTRACT_REFUSED_FLAG_ROWS)
        string(REGEX MATCH "^([^|]*)[|]([^|]*)[|](.*)$" matched "${row}")
        if(CMAKE_MATCH_1 STREQUAL arg_COMPILER)
            list(APPEND refused "${CMAKE_MATCH_2}|${CMAKE_MATCH_3}")
        endif()
    endforeach()
    set(${arg_OUT} "${refused}" PARENT_SCOPE)
endfunction()

## @brief Finds the first flag the configure scan refuses on @p COMPILER in @p TEXT
## (isoline_contract_refused_flags()). Generator expressions are matched as text, so a flag under a
## condition counts, whatever the condition.
##
## isoline_contract_override_in(COMPILER <compilerId> TEXT <text> OUT_FLAG <var> OUT_REASON <var>)
## OUT_FLAG and OUT_REASON are empty where @p TEXT holds none. Pure.
function(isoline_contract_override_in)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "COMPILER;TEXT;OUT_FLAG;OUT_REASON" "")
    isoline_contract_refused_flags(COMPILER "${arg_COMPILER}" OUT refused)
    foreach(row IN LISTS refused)
        string(REGEX MATCH "^([^|]*)[|](.*)$" matched "${row}")
        set(flag "${CMAKE_MATCH_1}")
        set(reason "${CMAKE_MATCH_2}")
        string(FIND "${arg_TEXT}" "${flag}" position)
        if(NOT position EQUAL -1)
            set(${arg_OUT_FLAG} "${flag}" PARENT_SCOPE)
            set(${arg_OUT_REASON} "${reason}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${arg_OUT_FLAG} "" PARENT_SCOPE)
    set(${arg_OUT_REASON} "" PARENT_SCOPE)
endfunction()

## @brief Sets the report a failed check prints around its condition, from the first report row that
## matches.
##
## isoline_contract_report_pattern(COMPILER <compilerId> PATH <native|fallback> SPELLING <contract|require>
##                                 OUT_PREFIX <var> OUT_SUFFIX <var>)
## Both are CMake regular expressions; both are unset where no row matches. Pure.
function(isoline_contract_report_pattern)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "COMPILER;PATH;SPELLING;OUT_PREFIX;OUT_SUFFIX" "")
    foreach(row IN LISTS ISOLINE_CONTRACT_REPORT_ROWS)
        if(NOT row MATCHES "^([^|]*)[|]([^|]*)[|]([^|]*)[|]([^|]*)[|](.*)$")
            message(FATAL_ERROR "[isoline] a contract report row has five columns: ${row}")
        endif()
        if((CMAKE_MATCH_1 STREQUAL "*" OR CMAKE_MATCH_1 STREQUAL arg_COMPILER)
           AND (CMAKE_MATCH_2 STREQUAL "*" OR CMAKE_MATCH_2 STREQUAL arg_PATH)
           AND (CMAKE_MATCH_3 STREQUAL "*" OR CMAKE_MATCH_3 STREQUAL arg_SPELLING))
            set(${arg_OUT_PREFIX} "${CMAKE_MATCH_4}" PARENT_SCOPE)
            set(${arg_OUT_SUFFIX} "${CMAKE_MATCH_5}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    unset(${arg_OUT_PREFIX} PARENT_SCOPE)
    unset(${arg_OUT_SUFFIX} PARENT_SCOPE)
endfunction()

## @brief Sets @p outVar to the features of @p rows, in the order they first appear.
function(isoline_language_features_of outVar)
    set(features "")
    foreach(row IN LISTS ARGN)
        string(REGEX MATCH "^[^|]+" feature "${row}")
        if(NOT feature IN_LIST features)
            list(APPEND features "${feature}")
        endif()
    endforeach()
    set(${outVar} "${features}" PARENT_SCOPE)
endfunction()

## @brief Normalizes an ISOLINE_<FEATURE> value into @p outVar by ISOLINE_LANGUAGE_FEATURE_OPTION_SPELLINGS:
## AUTO, ON or OFF, or empty for a spelling no row names.
function(_isoline_language_feature_option value outVar)
    string(TOUPPER "${value}" upper)
    set(normal "")
    foreach(row IN LISTS ISOLINE_LANGUAGE_FEATURE_OPTION_SPELLINGS)
        string(REGEX MATCH "^([^|]+)[|](.+)$" matched "${row}")
        if(upper STREQUAL CMAKE_MATCH_1)
            set(normal "${CMAKE_MATCH_2}")
            break()
        endif()
    endforeach()
    set(${outVar} "${normal}" PARENT_SCOPE)
endfunction()

## @brief Decides one feature's path from its option, its row, its probe and its requirement.
##
## isoline_language_feature_decision(
##     FEATURE <feature> OPTION <AUTO|ON|OFF, or a CMake boolean> COMPILER <compilerId>
##     VERSION <compilerVersion> MINIMUM <row's minimum, empty without a row> PROBE <passed|failed>
##     PROBE_SOURCE <probe path> DIAGNOSTIC <the probe's first diagnostic> REQUIRES <feature or empty>
##     REQUIRED_PATH <the requirement's path, or empty> OUT_PATH <var> OUT_REASON <var>)
##
## The precedence is OFF > no row > below the minimum > probe > requirement: the first that applies
## gives the reason. Under AUTO it is a fallback; under ON it refuses, and OUT_PATH is `refused`, which
## the configure turns into a FATAL_ERROR with OUT_REASON as its message. Pure: it compiles nothing.
function(isoline_language_feature_decision)
    cmake_parse_arguments(PARSE_ARGV 0 arg ""
        "FEATURE;OPTION;COMPILER;VERSION;MINIMUM;PROBE;PROBE_SOURCE;DIAGNOSTIC;REQUIRES;REQUIRED_PATH;OUT_PATH;OUT_REASON"
        "")
    string(TOUPPER "ISOLINE_${arg_FEATURE}" option)
    string(REGEX MATCH "^[0-9]+([.][0-9]+)?" shortVersion "${arg_VERSION}")
    set(compiler "${arg_COMPILER} ${shortVersion}")
    _isoline_language_feature_option("${arg_OPTION}" value)

    set(blocker "")
    if(value STREQUAL "")
        set(${arg_OUT_PATH} refused PARENT_SCOPE)
        set(${arg_OUT_REASON} "${option} must be AUTO, ON or OFF, not '${arg_OPTION}'" PARENT_SCOPE)
        return()
    elseif(value STREQUAL "OFF")
        set(${arg_OUT_PATH} fallback PARENT_SCOPE)
        set(${arg_OUT_REASON} "${option}=OFF" PARENT_SCOPE)
        return()
    elseif("${arg_MINIMUM}" STREQUAL "" AND "${arg_PROBE}" STREQUAL "passed")
        set(blocker "the probe passes: measure ${compiler} and add its row")
        set(refusal "there is no measured row for ${compiler} (${blocker})")
    elseif("${arg_MINIMUM}" STREQUAL "")
        set(blocker "no measured row for ${compiler}")
        set(refusal "there is ${blocker}")
    elseif("${arg_VERSION}" VERSION_LESS "${arg_MINIMUM}")
        set(blocker "${compiler} is below ${arg_COMPILER} ${arg_MINIMUM}")
        set(refusal "${blocker}")
    elseif(NOT "${arg_PROBE}" STREQUAL "passed")
        set(diagnostic "${arg_DIAGNOSTIC}")
        if(diagnostic STREQUAL "")
            set(diagnostic "no diagnostic")
        endif()
        set(blocker "probe failed: ${diagnostic}")
        set(refusal "${arg_PROBE_SOURCE} failed: ${diagnostic}")
    elseif(NOT "${arg_REQUIRES}" STREQUAL "" AND NOT "${arg_REQUIRED_PATH}" STREQUAL "native")
        set(blocker "needs ${arg_REQUIRES}, which is ${arg_REQUIRED_PATH}")
        set(refusal "it ${blocker}")
    endif()

    if(blocker STREQUAL "")
        set(${arg_OUT_PATH} native PARENT_SCOPE)
        set(${arg_OUT_REASON}
            "${compiler} meets ${arg_COMPILER} ${arg_MINIMUM}, and ${arg_PROBE_SOURCE} compiles" PARENT_SCOPE)
    elseif(value STREQUAL "ON")
        set(${arg_OUT_PATH} refused PARENT_SCOPE)
        set(${arg_OUT_REASON} "${option}=ON, but ${refusal}" PARENT_SCOPE)
    else()
        set(${arg_OUT_PATH} fallback PARENT_SCOPE)
        set(${arg_OUT_REASON} "${blocker}" PARENT_SCOPE)
    endif()
endfunction()

## @brief Reports the consumer floor: which compilers still lack a release with reflection and contracts.
##
## isoline_consumer_floor(ROWS <compilerId|release>... OUT_LINE <var> OUT_REACHED <var>)
## OUT_LINE is "pending <compilers>" while a release is empty, else "reached: <compiler release>...";
## OUT_REACHED is ON once no release is empty (the trigger of the reflection and contracts switches).
function(isoline_consumer_floor)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "OUT_LINE;OUT_REACHED" "ROWS")
    set(pending "")
    set(reached "")
    foreach(row IN LISTS arg_ROWS)
        string(REGEX MATCH "^([^|]+)[|](.*)$" matched "${row}")
        if(CMAKE_MATCH_2 STREQUAL "")
            list(APPEND pending "${CMAKE_MATCH_1}")
        else()
            list(APPEND reached "${CMAKE_MATCH_1} ${CMAKE_MATCH_2}")
        endif()
    endforeach()
    if(pending)
        list(JOIN pending ", " line)
        set(${arg_OUT_LINE} "pending ${line}" PARENT_SCOPE)
        set(${arg_OUT_REACHED} OFF PARENT_SCOPE)
    else()
        list(JOIN reached ", " line)
        set(${arg_OUT_LINE} "reached: ${line}" PARENT_SCOPE)
        set(${arg_OUT_REACHED} ON PARENT_SCOPE)
    endif()
endfunction()

## @brief Finds @p feature's row for @p compilerId among the remaining arguments (the rows): sets
## <prefix>_FOUND, _MINIMUM, _STANDARD, _FLAGS, _LINK_LIBRARIES, _REQUIRES, _PROBE and _PURPOSE (always
## the feature's first row's) in the caller. A row of other than nine columns is a FATAL_ERROR.
## Without a row for the compiler, everything but the minimum is the feature's first row's: the probe
## still runs, with that row's flags, so that a compiler which accepts them is told to measure itself
## rather than told the feature is not there. Only a row's own data is ever applied to a target.
function(_isoline_language_feature_row feature compilerId prefix)
    set(found OFF)
    set(fields "")
    set(purpose "")
    foreach(row IN LISTS ARGN)
        string(REPLACE "|" ";" columns "${row}")
        list(LENGTH columns columnCount)
        if(NOT columnCount EQUAL 9)
            message(FATAL_ERROR "[isoline] a feature row has nine columns, not ${columnCount}: ${row}")
        endif()
        list(GET columns 0 rowFeature)
        list(GET columns 1 rowCompiler)
        if(NOT rowFeature STREQUAL feature)
            continue()
        endif()
        if(fields STREQUAL "")
            set(fields "${columns}")
            list(GET columns 8 purpose)
        endif()
        if(rowCompiler STREQUAL compilerId)
            set(found ON)
            set(fields "${columns}")
            break()
        endif()
    endforeach()
    if(fields STREQUAL "")
        message(FATAL_ERROR "[isoline] no feature row names '${feature}'")
    endif()
    set(minimum "")
    list(GET fields 3 standard)
    list(GET fields 4 flags)
    list(GET fields 5 libraries)
    list(GET fields 6 requires)
    list(GET fields 7 probe)
    if(found)
        list(GET fields 2 minimum)
    endif()
    separate_arguments(flags UNIX_COMMAND "${flags}")
    separate_arguments(libraries UNIX_COMMAND "${libraries}")
    set(${prefix}_FOUND ${found} PARENT_SCOPE)
    set(${prefix}_MINIMUM "${minimum}" PARENT_SCOPE)
    set(${prefix}_STANDARD "${standard}" PARENT_SCOPE)
    set(${prefix}_FLAGS "${flags}" PARENT_SCOPE)
    set(${prefix}_LINK_LIBRARIES "${libraries}" PARENT_SCOPE)
    set(${prefix}_REQUIRES "${requires}" PARENT_SCOPE)
    set(${prefix}_PROBE "${probe}" PARENT_SCOPE)
    set(${prefix}_PURPOSE "${purpose}" PARENT_SCOPE)
endfunction()

## @brief Resolves every feature of @p ROWS for one compiler, in row order, each against the path its
## requirement resolved to. Pure apart from what @p PROBE_COMMAND does: the configure passes the real
## probe, the self-test a stub.
##
## isoline_resolve_language_features(ROWS <row>... COMPILER <compilerId> VERSION <compilerVersion>
##                                   PROBE_COMMAND <function> OUT_PREFIX <prefix>)
## Reads each option from the variable ISOLINE_<FEATURE>. PROBE_COMMAND is called as
## `<function>(<probe> <standard> <flags> <libraries> <prefix>)` and sets <prefix>_RESULT (passed or
## failed) and <prefix>_DIAGNOSTIC. Sets in the caller: <OUT_PREFIX>_FEATURES; per feature
## <OUT_PREFIX>_<FEATURE>_PATH, _REASON, _STANDARD, _FLAGS and _LINK_LIBRARIES; and <OUT_PREFIX>_ERROR,
## empty, or the message of the first refusal (an option, or a requirement no earlier feature defines),
## after which nothing more is resolved.
function(isoline_resolve_language_features)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "COMPILER;VERSION;PROBE_COMMAND;OUT_PREFIX" "ROWS")
    set(out "${arg_OUT_PREFIX}")
    isoline_language_features_of(features ${arg_ROWS})
    set(${out}_FEATURES "${features}" PARENT_SCOPE)
    set(${out}_ERROR "" PARENT_SCOPE)
    set(decided "")
    foreach(feature IN LISTS features)
        string(TOUPPER "${feature}" upper)
        _isoline_language_feature_row(${feature} "${arg_COMPILER}" row ${arg_ROWS})
        set(requiredPath "")
        if(NOT "${row_REQUIRES}" STREQUAL "")
            if(NOT row_REQUIRES IN_LIST decided)
                set(${out}_ERROR
                    "[isoline] C++26 ${feature}: its rows require '${row_REQUIRES}', which no earlier feature row defines"
                    PARENT_SCOPE)
                return()
            endif()
            set(requiredPath "${path_${row_REQUIRES}}")
        endif()
        # The probe of a feature switched OFF cannot change its outcome, so it is not compiled.
        _isoline_language_feature_option("${ISOLINE_${upper}}" option)
        set(probe_RESULT failed)
        set(probe_DIAGNOSTIC "")
        if(NOT option STREQUAL "OFF")
            cmake_language(CALL ${arg_PROBE_COMMAND} "${row_PROBE}" "${row_STANDARD}" "${row_FLAGS}"
                           "${row_LINK_LIBRARIES}" probe)
        endif()
        isoline_language_feature_decision(
            FEATURE ${feature}
            OPTION "${ISOLINE_${upper}}"
            COMPILER "${arg_COMPILER}"
            VERSION "${arg_VERSION}"
            MINIMUM "${row_MINIMUM}"
            PROBE ${probe_RESULT}
            PROBE_SOURCE "${row_PROBE}"
            DIAGNOSTIC "${probe_DIAGNOSTIC}"
            REQUIRES "${row_REQUIRES}"
            REQUIRED_PATH "${requiredPath}"
            OUT_PATH path
            OUT_REASON reason)
        if(path STREQUAL "refused")
            set(${out}_ERROR "[isoline] C++26 ${feature}: ${reason}" PARENT_SCOPE)
            return()
        endif()
        set(path_${feature} ${path})
        list(APPEND decided ${feature})
        set(${out}_${upper}_PATH ${path} PARENT_SCOPE)
        set(${out}_${upper}_REASON "${reason}" PARENT_SCOPE)
        set(${out}_${upper}_STANDARD "${row_STANDARD}" PARENT_SCOPE)
        set(${out}_${upper}_FLAGS "${row_FLAGS}" PARENT_SCOPE)
        set(${out}_${upper}_LINK_LIBRARIES "${row_LINK_LIBRARIES}" PARENT_SCOPE)
    endforeach()
endfunction()

## @brief The configure's probe: compiles and links @p probe (relative to the source tree) with the
## standard, flags and libraries given, and sets <prefix>_RESULT (passed or failed) and
## <prefix>_DIAGNOSTIC (the first compiler or linker error, the source tree's prefix stripped) in the
## caller. The flags reach the compile only, as a target's do, so a missing library fails the link as it
## would in the build. A standard CMake knows no flag for on this compiler fails the probe without
## compiling it: try_compile would refuse to generate its project, which is a configure error, not a
## failed probe. An edit of the probe reconfigures.
function(_isoline_run_language_feature_probe probe standard flags libraries prefix)
    set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${PROJECT_SOURCE_DIR}/${probe}")
    if(NOT "cxx_std_${standard}" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
        set(${prefix}_RESULT failed PARENT_SCOPE)
        set(${prefix}_DIAGNOSTIC
            "CMake ${CMAKE_VERSION} knows no C++${standard} mode of ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
            PARENT_SCOPE)
        return()
    endif()
    try_compile(probeCompiles
        SOURCES "${PROJECT_SOURCE_DIR}/${probe}"
        COMPILE_DEFINITIONS ${flags}
        LINK_LIBRARIES ${libraries}
        CXX_STANDARD ${standard}
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        NO_CACHE
        OUTPUT_VARIABLE output)
    set(diagnostic "")
    if(probeCompiles)
        set(result passed)
    else()
        set(result failed)
        # GCC and Clang (`error:`), MSVC (`error C2065:`, `error LNK2019:`), and an undefined symbol.
        string(REPLACE "\r" "" output "${output}")
        string(REGEX MATCH "[^\n]*(error( [A-Z]+[0-9]+)?:|undefined reference)[^\n]*" diagnostic "${output}")
        string(REPLACE "${PROJECT_SOURCE_DIR}/" "" diagnostic "${diagnostic}")
        string(STRIP "${diagnostic}" diagnostic)
    endif()
    set(${prefix}_RESULT ${result} PARENT_SCOPE)
    set(${prefix}_DIAGNOSTIC "${diagnostic}" PARENT_SCOPE)
endfunction()

## @brief Sets @p outVar to the help text of @p feature's option, from its first row among the
## remaining arguments (the rows): the purpose, the requirement, and the values.
function(isoline_language_feature_help feature outVar)
    _isoline_language_feature_row(${feature} "" row ${ARGN})
    set(help "C++26 ${feature} ${row_PURPOSE}")
    if(NOT row_REQUIRES STREQUAL "")
        string(APPEND help " (needs ${row_REQUIRES} native)")
    endif()
    string(APPEND help ": AUTO, ON or OFF (docs/language-features.md)")
    set(${outVar} "${help}" PARENT_SCOPE)
endfunction()

## @brief Defines ISOLINE_<FEATURE> (a STRING, default AUTO) for each feature of the rows.
function(_isoline_define_language_feature_options)
    isoline_language_features_of(features ${ISOLINE_LANGUAGE_FEATURE_ROWS})
    foreach(feature IN LISTS features)
        string(TOUPPER "ISOLINE_${feature}" option)
        isoline_language_feature_help(${feature} help ${ISOLINE_LANGUAGE_FEATURE_ROWS})
        set(${option} AUTO CACHE STRING "${help}")
        # An existing entry keeps its old help text otherwise; the rows are its source.
        set_property(CACHE ${option} PROPERTY HELPSTRING "${help}")
        set_property(CACHE ${option} PROPERTY STRINGS AUTO ON OFF)
    endforeach()
endfunction()

## @brief Selects every feature's path for the C++ compiler of this build, and reports one status line
## per feature and one for the consumer floor.
##
## Sets ISOLINE_<FEATURE>_PATH (native or fallback) and ISOLINE_CONSUMER_FLOOR_REACHED (the switches'
## trigger) as INTERNAL cache entries, and, for isoline_apply_language_features(),
## ISOLINE_<FEATURE>_STANDARD, _FLAGS and _LINK_LIBRARIES, and ISOLINE_CONTRACT_SEMANTIC_FLAG (empty
## unless contracts are native), in the including directory. An option the compiler cannot honour (ON
## without the native path, or a value that is not AUTO, ON or OFF) is a FATAL_ERROR, and so is a pinned
## contract semantic its rows do not select.
function(isoline_select_language_features)
    isoline_resolve_language_features(
        ROWS ${ISOLINE_LANGUAGE_FEATURE_ROWS}
        COMPILER "${CMAKE_CXX_COMPILER_ID}"
        VERSION "${CMAKE_CXX_COMPILER_VERSION}"
        PROBE_COMMAND _isoline_run_language_feature_probe
        OUT_PREFIX selected)
    if(NOT selected_ERROR STREQUAL "")
        message(FATAL_ERROR "${selected_ERROR}")
    endif()
    foreach(feature IN LISTS selected_FEATURES)
        string(TOUPPER "${feature}" upper)
        message(STATUS "[isoline] C++26 ${feature}: ${selected_${upper}_PATH} (${selected_${upper}_REASON})")
        set(ISOLINE_${upper}_PATH ${selected_${upper}_PATH} CACHE INTERNAL
            "The path ISOLINE_${upper} selected: native or fallback")
        set(ISOLINE_${upper}_STANDARD "${selected_${upper}_STANDARD}" PARENT_SCOPE)
        set(ISOLINE_${upper}_FLAGS "${selected_${upper}_FLAGS}" PARENT_SCOPE)
        set(ISOLINE_${upper}_LINK_LIBRARIES "${selected_${upper}_LINK_LIBRARIES}" PARENT_SCOPE)
    endforeach()

    isoline_consumer_floor(ROWS ${ISOLINE_CONSUMER_FLOOR_ROWS} OUT_LINE floorLine OUT_REACHED floorReached)
    message(STATUS "[isoline] C++26 consumer floor: ${floorLine}")
    set(ISOLINE_CONSUMER_FLOOR_REACHED ${floorReached} CACHE INTERNAL
        "ON once every compiler of ISOLINE_CONSUMER_FLOOR_ROWS has shipped reflection and contracts")

    # The semantic, where contracts are native: the pinned one, or a configure error naming why not.
    set(semanticFlag "")
    if(ISOLINE_CONTRACTS_PATH STREQUAL "native")
        isoline_contract_semantic(COMPILER "${CMAKE_CXX_COMPILER_ID}" SEMANTIC "${ISOLINE_CONTRACT_SEMANTIC}"
                                  ROWS ${ISOLINE_CONTRACT_SEMANTIC_ROWS} OUT_FLAG semanticFlag OUT_ERROR semanticError)
        if(NOT semanticError STREQUAL "")
            message(FATAL_ERROR "${semanticError}")
        endif()
        message(STATUS "[isoline] C++26 contracts semantic: ${ISOLINE_CONTRACT_SEMANTIC} (${semanticFlag})")
    endif()
    set(ISOLINE_CONTRACT_SEMANTIC_FLAG "${semanticFlag}" PARENT_SCOPE)
endfunction()

## @brief Generates the contracts seam's forwarder, `isoline/meta/Check.hpp` under @p directory: one
## #include of the native header (meta/contracts/Check.hpp) where contracts are native, else of the
## fallback (meta/fallback/contracts/Check.hpp). The directory is a PRIVATE include directory of every
## target _isoline_apply_target_language_features() sees, outside every FILE_SET, so the forwarder is
## never installed; no source tests for the feature.
function(_isoline_write_check_forwarder directory)
    set(header "isoline/meta/fallback/contracts/Check.hpp")
    if(ISOLINE_CONTRACTS_PATH STREQUAL "native")
        set(header "isoline/meta/contracts/Check.hpp")
    endif()
    file(CONFIGURE OUTPUT "${directory}/isoline/meta/Check.hpp"
         CONTENT "// Generated by cmake/IsolineLanguageFeatures.cmake: the contracts seam, ${ISOLINE_CONTRACTS_PATH}.\n#pragma once\n#include <${header}>\n"
         @ONLY)
endfunction()

## The generators whose compile commands the build-time check reads (CMAKE_EXPORT_COMPILE_COMMANDS):
## a native-contracts build with any other is refused at configure time.
set(ISOLINE_CONTRACT_GENERATORS "Ninja" "Ninja Multi-Config" "Unix Makefiles" "MinGW Makefiles" "MSYS Makefiles")

## @brief Compiles @p target as each native feature of ISOLINE_TARGET_LANGUAGE_FEATURES says: its row's
## standard (the highest wins), its flags PRIVATE, and its libraries PRIVATE, which reach consumers of a
## static library as $<LINK_ONLY:…>, so an installed package links them without compiling with them;
## then, where contracts are native, the pinned semantic's flag, the last PRIVATE option this adds. On
## either path the target gets the contracts seam's forwarder, <isoline/meta/Check.hpp>, on a PRIVATE
## include directory. The target's compile features are left as they are: consumers still compile as
## C++23.
##
## Where contracts are native, the target is recorded in the GLOBAL property ISOLINE_CONTRACT_TARGETS,
## which the configure scan reads (isoline_refuse_contract_overrides(), scheduled by the first), its
## compile commands are exported, and it depends on the build-time check of them
## (_isoline_add_contract_flags_check()).
function(_isoline_apply_target_language_features target)
    set(standard "")
    foreach(feature IN LISTS ISOLINE_TARGET_LANGUAGE_FEATURES)
        string(TOUPPER "${feature}" upper)
        if(NOT ISOLINE_${upper}_PATH STREQUAL "native")
            continue()
        endif()
        if(standard STREQUAL "" OR ISOLINE_${upper}_STANDARD GREATER standard)
            set(standard "${ISOLINE_${upper}_STANDARD}")
        endif()
        # Build options (isoline_apply_build_options()): never exported with an installed target, and recorded,
        # so check-install proves it. The library stays a PRIVATE link: a static consumer links it through the
        # exported $<LINK_ONLY:...>, which the package's config checks it can (cmake/isolineConfig.cmake.in).
        isoline_apply_build_options(${target} COMPILE ${ISOLINE_${upper}_FLAGS})
        target_link_libraries(${target} PRIVATE ${ISOLINE_${upper}_LINK_LIBRARIES})
    endforeach()
    if(NOT standard STREQUAL "")
        set_target_properties(${target} PROPERTIES CXX_STANDARD ${standard} CXX_STANDARD_REQUIRED ON)
    endif()
    target_include_directories(${target} PRIVATE "${ISOLINE_PRIVATE_INCLUDE_DIR}")
    if(NOT ISOLINE_CONTRACTS_PATH STREQUAL "native")
        return()
    endif()
    if(NOT CMAKE_GENERATOR IN_LIST ISOLINE_CONTRACT_GENERATORS)
        list(JOIN ISOLINE_CONTRACT_GENERATORS ", " generators)
        message(FATAL_ERROR
            "[isoline] C++26 contracts native: the generator ${CMAKE_GENERATOR} writes no compile_commands.json, "
            "so the build cannot check the flags Isoline's contracts compile with. Use one of ${generators}, "
            "or set ISOLINE_CONTRACTS=OFF.")
    endif()
    # Not last on the command line: linked INTERFACE options and later target_compile_options() follow
    # it. That is what the scan and the build-time check are for.
    isoline_apply_build_options(${target} COMPILE ${ISOLINE_CONTRACT_SEMANTIC_FLAG})
    set_target_properties(${target} PROPERTIES EXPORT_COMPILE_COMMANDS ON)
    set_property(GLOBAL APPEND PROPERTY ISOLINE_CONTRACT_TARGETS ${target})
    get_property(scheduled GLOBAL PROPERTY ISOLINE_CONTRACT_SCAN_SCHEDULED)
    if(NOT scheduled)
        set_property(GLOBAL PROPERTY ISOLINE_CONTRACT_SCAN_SCHEDULED ON)
        # After the whole project has configured, Isoline as a subproject included: the top-level
        # directory is the last to finish.
        cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL isoline_refuse_contract_overrides)
        _isoline_add_contract_flags_check()
    endif()
    add_dependencies(${target} isoline_contract_flags_check)
endfunction()

## @brief Adds isoline_contract_flags_check, the build-time check every native-contracts target depends
## on: cmake/IsolineContractFlags.cmake reads ${CMAKE_BINARY_DIR}/compile_commands.json, as the generator
## wrote it, and fails the build when a translation unit under Isoline's src/ is compiled with a flag
## isoline_contract_refused_flags() names (response and specs files read, one that cannot be read an
## error). It reruns when the compile commands, the refused flags or a response or specs file it read change. The flags are written to a file, one `flag|reason` per line.
function(_isoline_add_contract_flags_check)
    isoline_contract_refused_flags(COMPILER "${CMAKE_CXX_COMPILER_ID}" OUT refused)
    list(JOIN refused "\n" refusedLines)
    set(refusedFile "${PROJECT_BINARY_DIR}/isoline-contract-refused-flags.txt")
    file(CONFIGURE OUTPUT "${refusedFile}" CONTENT "${refusedLines}\n" @ONLY)
    set(stamp "${PROJECT_BINARY_DIR}/isoline-contract-flags.stamp")
    set(script "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/IsolineContractFlags.cmake")
    add_custom_command(OUTPUT "${stamp}"
        COMMAND "${CMAKE_COMMAND}" "-DCOMPILE_COMMANDS=${CMAKE_BINARY_DIR}/compile_commands.json"
                "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}" "-DREFUSED_FLAGS=${refusedFile}"
                "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}" "-DSTAMP=${stamp}"
                "-DDEPFILE=${stamp}.d" -P "${script}"
        DEPENDS "${CMAKE_BINARY_DIR}/compile_commands.json" "${refusedFile}" "${script}"
        # The response and specs files the check read: editing one reruns it.
        DEPFILE "${stamp}.d"
        COMMENT "Checking the flags Isoline's contracts compile with"
        VERBATIM)
    add_custom_target(isoline_contract_flags_check DEPENDS "${stamp}")
endfunction()

## @brief Compiles @p target, a test's or a tool's, as isoline_apply_language_features() compiles a
## library, without recording it for the header check. A target with a HEADERS file set installs headers,
## which must be checked: it calls isoline_apply_language_features(), and here it is a FATAL_ERROR.
function(_isoline_compile_with_language_features target)
    get_target_property(headerSets ${target} HEADER_SETS)
    if(headerSets)
        message(FATAL_ERROR
            "[isoline] _isoline_compile_with_language_features(${target}): ${target} has a HEADERS file set, "
            "so its headers must be checked: call isoline_apply_language_features(${target})")
    endif()
    _isoline_apply_target_language_features(${target})
endfunction()

## @brief A library's call: compiles @p target as _isoline_apply_target_language_features() does, and
## records it in the GLOBAL property ISOLINE_LANGUAGE_FEATURE_TARGETS, whose installed headers
## isoline_add_public_header_check() checks. Every target passed here, on either path, is recorded.
## Once that check has run, the registry is sealed: a target arriving later would compile with the
## native features and its headers unchecked, so it is a FATAL_ERROR.
function(isoline_apply_language_features target)
    get_property(sealedBy GLOBAL PROPERTY ISOLINE_HEADER_CHECK_SEALED)
    if(sealedBy)
        message(FATAL_ERROR
            "[isoline] isoline_apply_language_features(${target}) runs after isoline_add_public_header_check(${sealedBy}),"
            " so ${target}'s headers would go unchecked: register ${target} before the check, or move the check after it")
    endif()
    set_property(GLOBAL APPEND PROPERTY ISOLINE_LANGUAGE_FEATURE_TARGETS ${target})
    _isoline_apply_target_language_features(${target})
endfunction()

## @brief Appends to @p outVar the targets @p item names: an item of LINK_LIBRARIES,
## INTERFACE_LINK_LIBRARIES or INTERFACE_LINK_LIBRARIES_DIRECT. Its generator expressions, nested to any
## depth, are read as text, conservatively: every word of it that names a target counts, under any
## condition. Aliases resolve to the target they alias.
function(_isoline_linked_targets item outVar)
    set(targets "${${outVar}}")
    # Every `$<NAME:`, every other `$<`, `>` and `,` separates words; a `:` that ends a condition
    # (`$<$<CONFIG:Debug>:name>`) is left at the front of the next one, and `::` inside a name is kept.
    string(REGEX REPLACE "\\$<[A-Za-z_]+:" ";" names "${item}")
    string(REPLACE "$<" ";" names "${names}")
    string(REGEX REPLACE "[>,]" ";" names "${names}")
    foreach(name IN LISTS names)
        string(REGEX REPLACE "^:" "" name "${name}")
        if(name STREQUAL "" OR NOT TARGET "${name}")
            continue()
        endif()
        get_target_property(aliased "${name}" ALIASED_TARGET)
        if(aliased)
            set(name "${aliased}")
        endif()
        if(NOT name IN_LIST targets)
            list(APPEND targets "${name}")
        endif()
    endforeach()
    set(${outVar} "${targets}" PARENT_SCOPE)
endfunction()

## @brief The configure scan (D6 of the plan; docs/language-features.md): refuses any flag that would
## check Isoline's contracts under another semantic than the pinned one, wherever it comes from. Deferred
## to the end of the top-level directory, so that a parent project's later changes are seen too.
##
## For every target of ISOLINE_CONTRACT_TARGETS it reads CMAKE_CXX_FLAGS and CMAKE_CXX_FLAGS_<CONFIG>
## (each configuration of the build, and CMake's four) as the target's directory sees them, the target's
## own COMPILE_OPTIONS (which hold its directories' COMPILE_OPTIONS from its creation on; one added to a
## directory later never reaches it) and COMPILE_FLAGS, those of its sources, and the
## INTERFACE_COMPILE_OPTIONS of every target in its
## link closure (LINK_LIBRARIES, then each one's INTERFACE_LINK_LIBRARIES and
## INTERFACE_LINK_LIBRARIES_DIRECT), core::base included, all with the compiler and rows the target's
## directory sees. Any refused flag (isoline_contract_override_in()) is a FATAL_ERROR naming where it
## was found and why; so is a target whose compiler no row names. The scan is early feedback: what it
## cannot read as text (a flag built by a generator expression, add_definitions()) the build-time check
## of the compile commands refuses (_isoline_add_contract_flags_check()).
function(isoline_refuse_contract_overrides)
    set(flagVariables CMAKE_CXX_FLAGS)
    foreach(configuration IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL LISTS CMAKE_BUILD_TYPE CMAKE_CONFIGURATION_TYPES)
        string(TOUPPER "${configuration}" configuration)
        list(APPEND flagVariables "CMAKE_CXX_FLAGS_${configuration}")
    endforeach()
    list(REMOVE_DUPLICATES flagVariables)

    get_property(contractTargets GLOBAL PROPERTY ISOLINE_CONTRACT_TARGETS)
    foreach(target IN LISTS contractTargets)
        # source|text: where each text was found, and what it holds.
        set(sources "")
        get_target_property(directory ${target} SOURCE_DIR)
        # The rows as the target's directory sees them: a deferred call runs in the top-level directory,
        # which, with Isoline a subproject, never saw this module's variables.
        # So is the compiler: a parent project may not have enabled C++ at all.
        foreach(variable IN ITEMS ISOLINE_CONTRACT_SEMANTIC ISOLINE_CONTRACT_SEMANTIC_ROWS
                                  ISOLINE_CONTRACT_REFUSED_FLAG_ROWS CMAKE_CXX_COMPILER_ID)
            get_directory_property(${variable} DIRECTORY "${directory}" DEFINITION ${variable})
        endforeach()
        isoline_contract_refused_flags(COMPILER "${CMAKE_CXX_COMPILER_ID}" OUT refused)
        if(refused STREQUAL "")
            message(FATAL_ERROR
                "[isoline] ${target} compiles with native contracts, but no contract row names its compiler "
                "'${CMAKE_CXX_COMPILER_ID}', so nothing it must refuse is known (cmake/IsolineLanguageFeatures.cmake)")
        endif()
        foreach(variable IN LISTS flagVariables)
            get_directory_property(text DIRECTORY "${directory}" DEFINITION ${variable})
            list(APPEND sources "${variable}, as ${target}'s directory sees it|${text}")
        endforeach()
        foreach(property IN ITEMS COMPILE_OPTIONS COMPILE_FLAGS)
            get_target_property(text ${target} ${property})
            if(text)
                list(JOIN text " " text)
                list(APPEND sources "${property} of ${target}|${text}")
            endif()
        endforeach()
        get_target_property(targetSources ${target} SOURCES)
        foreach(source IN LISTS targetSources)
            # Relative to the target's directory, not to the top-level one this runs in.
            cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY "${directory}" OUTPUT_VARIABLE sourcePath)
            foreach(property IN ITEMS COMPILE_OPTIONS COMPILE_FLAGS)
                get_source_file_property(text "${sourcePath}" TARGET_DIRECTORY ${target} ${property})
                if(text)
                    list(JOIN text " " text)
                    list(APPEND sources "${property} of ${source} in ${target}|${text}")
                endif()
            endforeach()
        endforeach()

        # The link closure, breadth first, each target with the name it was linked by.
        set(closure "")
        get_target_property(linked ${target} LINK_LIBRARIES)
        foreach(item IN LISTS linked)
            _isoline_linked_targets("${item}" closure)
        endforeach()
        set(index 0)
        list(LENGTH closure count)
        while(index LESS count)
            list(GET closure ${index} dependency)
            foreach(property IN ITEMS INTERFACE_LINK_LIBRARIES INTERFACE_LINK_LIBRARIES_DIRECT)
                get_target_property(interfaceLinked ${dependency} ${property})
                foreach(item IN LISTS interfaceLinked)
                    _isoline_linked_targets("${item}" closure)
                endforeach()
            endforeach()
            math(EXPR index "${index} + 1")
            list(LENGTH closure count)
        endwhile()
        foreach(dependency IN LISTS closure)
            get_target_property(text ${dependency} INTERFACE_COMPILE_OPTIONS)
            if(text)
                list(JOIN text " " text)
                set(name "${dependency}")
                foreach(item IN LISTS linked)
                    if(TARGET "${item}")
                        get_target_property(aliased "${item}" ALIASED_TARGET)
                        if(aliased STREQUAL dependency)
                            set(name "${item} (${dependency})")
                        endif()
                    endif()
                endforeach()
                list(APPEND sources "INTERFACE_COMPILE_OPTIONS of ${name}, which ${target} links|${text}")
            endif()
        endforeach()

        foreach(entry IN LISTS sources)
            string(REGEX MATCH "^([^|]*)[|](.*)$" matched "${entry}")
            set(source "${CMAKE_MATCH_1}")
            isoline_contract_override_in(COMPILER "${CMAKE_CXX_COMPILER_ID}" TEXT "${CMAKE_MATCH_2}"
                                         OUT_FLAG flag OUT_REASON reason)
            if(NOT flag STREQUAL "")
                message(FATAL_ERROR
                    "[isoline] ${source}: ${flag} is refused, since it ${reason}. Isoline's contracts are checked "
                    "under the ${ISOLINE_CONTRACT_SEMANTIC} semantic only (docs/language-features.md).")
            endif()
        endforeach()
    endforeach()
endfunction()

## @brief Sets @p outVar to the include names of the `.hpp` files in @p target's HEADER_SET, each
## relative to the header base directory it is under, in the set's order. Other files (an X-macro
## `.def`, which only its includers can compile) are left out.
function(_isoline_header_set_include_names target outVar)
    set(includeNames "")
    get_target_property(headers ${target} HEADER_SET)
    if(NOT headers)
        set(${outVar} "" PARENT_SCOPE)
        return()
    endif()
    get_target_property(baseDirectories ${target} HEADER_DIRS)
    get_target_property(targetSourceDirectory ${target} SOURCE_DIR)
    foreach(header IN LISTS headers)
        if(NOT header MATCHES "[.]hpp$")
            continue()
        endif()
        cmake_path(ABSOLUTE_PATH header BASE_DIRECTORY "${targetSourceDirectory}" NORMALIZE)
        set(includeName "")
        foreach(baseDirectory IN LISTS baseDirectories)
            cmake_path(IS_PREFIX baseDirectory "${header}" NORMALIZE underBase)
            if(underBase)
                file(RELATIVE_PATH includeName "${baseDirectory}" "${header}")
                break()
            endif()
        endforeach()
        if(includeName STREQUAL "")
            message(FATAL_ERROR "[isoline] ${header} is under none of ${target}'s header base directories")
        endif()
        list(APPEND includeNames "${includeName}")
    endforeach()
    set(${outVar} "${includeNames}" PARENT_SCOPE)
endfunction()

## @brief Adds @p checkTarget, an OBJECT library with one generated translation unit per `.hpp` in the
## HEADER_SET of each target of ISOLINE_LANGUAGE_FEATURE_TARGETS, each including its header alone,
## compiled as C++23 without extensions, whatever the libraries themselves compile as. Called once, after
## every such target is defined; it seals the registry (ISOLINE_HEADER_CHECK_SEALED).
##
## It proves that an installed header parses as C++23 (no reflection, annotation or contract syntax
## leaks into one) and includes what it uses. Isoline's warning set applies, and a C++26 extension is an
## error on the target itself, with or without ISOLINE_WERROR. clang-tidy does not run on the generated
## stubs: each holds one #include, and the header's code is tidied with its library. The libraries are
## linked for their usage requirements only; an OBJECT library is never linked.
function(isoline_add_public_header_check checkTarget)
    # Sealed first: every registration after this point is refused (isoline_apply_language_features).
    set_property(GLOBAL PROPERTY ISOLINE_HEADER_CHECK_SEALED ${checkTarget})
    get_property(targets GLOBAL PROPERTY ISOLINE_LANGUAGE_FEATURE_TARGETS)
    set(sources "")
    foreach(target IN LISTS targets)
        _isoline_header_set_include_names(${target} includeNames)
        foreach(includeName IN LISTS includeNames)
            set(source "${CMAKE_CURRENT_BINARY_DIR}/${checkTarget}/${includeName}.cpp")
            file(CONFIGURE OUTPUT "${source}"
                 CONTENT "// Generated by isoline_add_public_header_check(): ${includeName} compiles alone as C++23.\n#include <${includeName}>\n"
                 @ONLY)
            list(APPEND sources "${source}")
        endforeach()
    endforeach()
    if(sources STREQUAL "")
        message(FATAL_ERROR "[isoline] isoline_add_public_header_check: no recorded target has a header set")
    endif()
    add_library(${checkTarget} OBJECT ${sources})
    set_target_properties(${checkTarget} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_link_libraries(${checkTarget} PRIVATE ${targets})
    isoline_apply_target_policy(${checkTarget})
    set_target_properties(${checkTarget} PROPERTIES CXX_CLANG_TIDY "")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)
        # GCC spells the group c++26-extensions; Clang has both spellings. A flag the compiler lacks is
        # dropped. Clang rejects an annotation as a syntax error regardless.
        set(extensionErrors "")
        _isoline_supported_flags(extensionErrors -Werror=c++26-extensions -Werror=c++2c-extensions)
        target_compile_options(${checkTarget} PRIVATE ${extensionErrors})
    endif()
endfunction()

## @brief Adds a reflection check where reflection (or the FEATURE it names) takes the native path, and
## nothing elsewhere: the CMake-selected half of the seam, so that no source tests for the feature.
##
## isoline_add_reflection_check(<target> SOURCES <source>... [FEATURE <feature>] [LINK_LIBRARIES <library>...]
##                              [UMBRELLA <include name> HEADER_TARGETS <target>... [EXCLUDE <regex>...]]
##                              [TEST LABELS <label>...])
##
## Without TEST, <target> is a compile-only OBJECT library in ALL, whose static_asserts are the check;
## with TEST, a Catch2 executable registered as the test <target> with LABELS. Either compiles with the
## feature row's standard and flags, under Isoline's warning set; clang-tidy never sees it, since only a
## compiler with the feature native compiles it. FEATURE is a row of ISOLINE_LANGUAGE_FEATURE_ROWS,
## `reflection` when omitted; `annotations` adds a check whose sources annotate. LINK_LIBRARIES are
## linked PRIVATE.
##
## UMBRELLA generates the header <include name>, on the target's private include path, that includes
## every header a table may live in: the `.hpp` files of each HEADER_TARGETS' HEADER_SET (an X-macro
## `.def` is left out), then every other `.hpp` under their source directories (the private headers)
## whose path matches none of EXCLUDE. A table in a new header is therefore checked without a row. An
## EXCLUDE pattern that matches no header is a FATAL_ERROR.
function(isoline_add_reflection_check target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "TEST" "UMBRELLA;FEATURE" "SOURCES;LINK_LIBRARIES;HEADER_TARGETS;EXCLUDE;LABELS")
    if(NOT arg_FEATURE)
        set(arg_FEATURE reflection)
    endif()
    # Looked up in the rows, not by whether ISOLINE_<FEATURE>_PATH is defined: a stale cache entry would
    # pass that.
    set(known OFF)
    foreach(row IN LISTS ISOLINE_LANGUAGE_FEATURE_ROWS)
        string(REPLACE "|" ";" columns "${row}")
        list(GET columns 0 rowFeature)
        if(rowFeature STREQUAL arg_FEATURE)
            set(known ON)
            break()
        endif()
    endforeach()
    if(NOT known)
        message(FATAL_ERROR "[isoline] isoline_add_reflection_check(${target}): no feature row \"${arg_FEATURE}\"")
    endif()
    string(TOUPPER "${arg_FEATURE}" feature)
    if(NOT ISOLINE_${feature}_PATH STREQUAL "native")
        return()
    endif()
    if(arg_TEST)
        add_executable(${target} ${arg_SOURCES})
        target_link_libraries(${target} PRIVATE Catch2::Catch2WithMain)
        add_test(NAME ${target} COMMAND ${target})
        set_tests_properties(${target} PROPERTIES LABELS "${arg_LABELS}" TIMEOUT 60)
        isoline_apply_target_policy(${target} TEST)
    else()
        add_library(${target} OBJECT ${arg_SOURCES})
        isoline_apply_target_policy(${target})
    endif()
    target_link_libraries(${target} PRIVATE ${arg_LINK_LIBRARIES} ${ISOLINE_${feature}_LINK_LIBRARIES})
    target_compile_options(${target} PRIVATE ${ISOLINE_${feature}_FLAGS})
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD ${ISOLINE_${feature}_STANDARD} CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF CXX_CLANG_TIDY "")

    if(arg_UMBRELLA)
        set(includeNames "")
        set(sourceDirectories "")
        set(usedPatterns "")
        foreach(headerTarget IN LISTS arg_HEADER_TARGETS)
            _isoline_header_set_include_names(${headerTarget} names)
            list(APPEND includeNames ${names})
            get_target_property(sourceDirectory ${headerTarget} SOURCE_DIR)
            list(APPEND sourceDirectories "${sourceDirectory}")
        endforeach()
        # The private headers: every other .hpp under the libraries' sources, named from src/.
        foreach(sourceDirectory IN LISTS sourceDirectories)
            file(GLOB_RECURSE privateHeaders CONFIGURE_DEPENDS "${sourceDirectory}/*.hpp")
            foreach(header IN LISTS privateHeaders)
                file(RELATIVE_PATH includeName "${PROJECT_SOURCE_DIR}/src" "${header}")
                set(excluded OFF)
                foreach(pattern IN LISTS arg_EXCLUDE)
                    if(includeName MATCHES "${pattern}")
                        set(excluded ON)
                        list(APPEND usedPatterns "${pattern}")
                    endif()
                endforeach()
                if(NOT excluded AND NOT includeName IN_LIST includeNames)
                    list(APPEND includeNames "${includeName}")
                endif()
            endforeach()
        endforeach()
        # A pattern that excludes nothing is stale, or misspelt: then what it meant to exclude is checked
        # after all, or a rename went unnoticed.
        foreach(pattern IN LISTS arg_EXCLUDE)
            if(NOT pattern IN_LIST usedPatterns)
                message(FATAL_ERROR
                    "[isoline] isoline_add_reflection_check(${target}): EXCLUDE \"${pattern}\" matches no header")
            endif()
        endforeach()
        set(content "// Generated by isoline_add_reflection_check(${target}): every header a table may live in.\n#pragma once\n")
        foreach(includeName IN LISTS includeNames)
            string(APPEND content "#include <${includeName}>\n")
        endforeach()
        file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${target}/${arg_UMBRELLA}" CONTENT "${content}" @ONLY)
        target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/${target}")
    endif()
endfunction()

if(NOT CMAKE_SCRIPT_MODE_FILE)
    _isoline_define_language_feature_options()
    isoline_select_language_features()
    ## The PRIVATE include directory of the generated contracts seam (_isoline_write_check_forwarder()).
    set(ISOLINE_PRIVATE_INCLUDE_DIR "${PROJECT_BINARY_DIR}/private")
    _isoline_write_check_forwarder("${ISOLINE_PRIVATE_INCLUDE_DIR}")
endif()
