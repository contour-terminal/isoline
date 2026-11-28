# SPDX-License-Identifier: Apache-2.0
#
# The self-test of cmake/IsolineLanguageFeatures.cmake's decisions, run in script mode:
#     cmake -P tests/cmake/language-features_test.cmake
# It drives isoline_language_feature_decision() and isoline_consumer_floor() with one row per case, and
# isoline_resolve_language_features(), the selection the configure runs, with rows and a stub probe. It
# reports every case whose outcome differs, so a broken precedence or requirement names each case it
# breaks. No probe compiles here: the probe's outcome and its first diagnostic are inputs.

cmake_minimum_required(VERSION 3.25)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/IsolineLanguageFeatures.cmake")

# Decision rows:
#     option|compiler|version|minimum|probe|diagnostic|requiredPath|expectedPath|expectedReason
# An empty minimum is a compiler without a measured row. requiredPath is the path the feature's
# requirement resolved to (empty: the feature requires nothing). expectedReason is a regular expression
# the reason must match; a refused decision is the configure's FATAL_ERROR.
set(decisionRows
    "AUTO|GNU|16.2.1|16.2|passed|||native|^GNU 16[.]2 meets GNU 16[.]2, and cmake/probes/reflection[.]cpp compiles$"
    "AUTO|GNU|16.2.1|16.2|failed|probe.cpp:3:1: error: expected unqualified-id||fallback|^probe failed: probe[.]cpp:3:1: error: expected unqualified-id$"
    "AUTO|Clang|23.0.0||passed|||fallback|^the probe passes: measure Clang 23[.]0 and add its row$"
    "AUTO|GNU|16.1.0|16.2|passed|||fallback|^GNU 16[.]1 is below GNU 16[.]2$"
    "ON|GNU|16.2.1|16.2|failed|probe.cpp:3:1: error: expected unqualified-id||refused|^ISOLINE_REFLECTION=ON, but cmake/probes/reflection[.]cpp failed: probe[.]cpp:3:1: error: expected unqualified-id$"
    "ON|Clang|23.0.0||passed|||refused|^ISOLINE_REFLECTION=ON, but there is no measured row for Clang 23[.]0 [(]the probe passes: measure Clang 23[.]0 and add its row[)]$"
    "OFF|GNU|16.2.1|16.2|passed|||fallback|^ISOLINE_REFLECTION=OFF$"
    "ON|GNU|16.2.1|16.2|passed||fallback|refused|^ISOLINE_REFLECTION=ON, but it needs reflection, which is fallback$"
    "maybe|GNU|16.2.1|16.2|passed|||refused|^ISOLINE_REFLECTION must be AUTO, ON or OFF, not 'maybe'$"
    # The precedence, OFF > no row > below minimum > probe > requirement: GCC 14 is below the row
    # before its probe fails, Clang without a row fails its probe too, and AUTO follows a requirement
    # that fell back.
    "AUTO|GNU|14.2.0|16.2|failed|probe.cpp:1:10: error: 'meta' file not found|fallback|fallback|^GNU 14[.]2 is below GNU 16[.]2$"
    "AUTO|Clang|22.1.8||failed|clang++: error: unknown argument: '-freflection'||fallback|^no measured row for Clang 22[.]1$"
    "AUTO|GNU|16.2.1|16.2|passed||fallback|fallback|^needs reflection, which is fallback$"
    "OFF|Clang|22.1.8||failed|clang++: error: unknown argument: '-freflection'||fallback|^ISOLINE_REFLECTION=OFF$"
    "ON|GNU|14.2.0|16.2|failed|probe.cpp:1:10: error: 'meta' file not found||refused|^ISOLINE_REFLECTION=ON, but GNU 14[.]2 is below GNU 16[.]2$"
    "ON|GNU|16.2.1|16.2|failed|probe.cpp:3:1: error: expected unqualified-id|fallback|refused|^ISOLINE_REFLECTION=ON, but cmake/probes/reflection[.]cpp failed: "
    # CMake's boolean spellings, in any case, are ON and OFF; auto is AUTO; nothing else is a value.
    "yes|GNU|16.2.1|16.2|failed|probe.cpp:3:1: error: expected unqualified-id||refused|^ISOLINE_REFLECTION=ON, but cmake/probes/reflection[.]cpp failed: "
    "True|GNU|16.2.1|16.2|passed|||native|^GNU 16[.]2 meets GNU 16[.]2"
    "1|Clang|22.1.8||failed|||refused|^ISOLINE_REFLECTION=ON, but there is no measured row for Clang 22[.]1$"
    "false|GNU|16.2.1|16.2|passed|||fallback|^ISOLINE_REFLECTION=OFF$"
    "n|GNU|16.2.1|16.2|passed|||fallback|^ISOLINE_REFLECTION=OFF$"
    "0|GNU|16.2.1|16.2|passed|||fallback|^ISOLINE_REFLECTION=OFF$"
    "auto|Clang|22.1.8||failed|||fallback|^no measured row for Clang 22[.]1$"
    "|GNU|16.2.1|16.2|passed|||refused|^ISOLINE_REFLECTION must be AUTO, ON or OFF, not ''$"
    "NOTFOUND|GNU|16.2.1|16.2|passed|||refused|^ISOLINE_REFLECTION must be AUTO, ON or OFF, not 'NOTFOUND'$"
    "2|GNU|16.2.1|16.2|passed|||refused|^ISOLINE_REFLECTION must be AUTO, ON or OFF, not '2'$")

# Consumer-floor rows: a floor table (its compilerId|release rows joined by ',', with '=' for '|'),
# whether it is reached, and the line isoline_consumer_floor() reports for it.
set(floorRows
    "GNU=16.2,Clang=,AppleClang=,MSVC=|OFF|^pending Clang, AppleClang, MSVC$"
    "GNU=16.2,Clang=23.1,AppleClang=,MSVC=19.60|OFF|^pending AppleClang$"
    "GNU=16.2,Clang=23.1,AppleClang=18.0,MSVC=19.60|ON|^reached: GNU 16[.]2, Clang 23[.]1, AppleClang 18[.]0, MSVC 19[.]60$")

set(failures "")
set(caseCount 0)

foreach(row IN LISTS decisionRows)
    # By a regular expression, not as a list: a reason's pattern may hold brackets.
    if(NOT row MATCHES "^([^|]*)[|]([^|]*)[|]([^|]*)[|]([^|]*)[|]([^|]*)[|]([^|]*)[|]([^|]*)[|]([^|]*)[|](.*)$")
        message(FATAL_ERROR "language-features: a decision row needs nine columns: ${row}")
    endif()
    set(option "${CMAKE_MATCH_1}")
    set(compiler "${CMAKE_MATCH_2}")
    set(version "${CMAKE_MATCH_3}")
    set(minimum "${CMAKE_MATCH_4}")
    set(probe "${CMAKE_MATCH_5}")
    set(diagnostic "${CMAKE_MATCH_6}")
    set(requiredPath "${CMAKE_MATCH_7}")
    set(expectedPath "${CMAKE_MATCH_8}")
    set(expectedReason "${CMAKE_MATCH_9}")
    set(requires "")
    if(NOT "${requiredPath}" STREQUAL "")
        set(requires reflection)
    endif()
    isoline_language_feature_decision(
        FEATURE reflection
        OPTION "${option}"
        COMPILER "${compiler}"
        VERSION "${version}"
        MINIMUM "${minimum}"
        PROBE "${probe}"
        PROBE_SOURCE cmake/probes/reflection.cpp
        DIAGNOSTIC "${diagnostic}"
        REQUIRES "${requires}"
        REQUIRED_PATH "${requiredPath}"
        OUT_PATH path
        OUT_REASON reason)
    math(EXPR caseCount "${caseCount} + 1")
    if(NOT path STREQUAL expectedPath OR NOT reason MATCHES "${expectedReason}")
        string(APPEND failures
               "\n  ${row}\n    got ${path}: ${reason}")
    endif()
endforeach()

foreach(row IN LISTS floorRows)
    string(REGEX MATCH "^([^|]*)[|]([^|]*)[|](.*)$" matched "${row}")
    set(table "${CMAKE_MATCH_1}")
    set(expectedReached "${CMAKE_MATCH_2}")
    set(expectedLine "${CMAKE_MATCH_3}")
    # The table's own form: one compilerId|release row per compiler.
    string(REPLACE "=" "|" table "${table}")
    string(REPLACE "," ";" table "${table}")
    isoline_consumer_floor(ROWS ${table} OUT_LINE line OUT_REACHED reached)
    math(EXPR caseCount "${caseCount} + 1")
    if(NOT line MATCHES "${expectedLine}" OR NOT reached STREQUAL expectedReached)
        string(APPEND failures "\n  floor ${row}\n    got ${reached}: ${line}")
    endif()
endforeach()

# The selection: isoline_resolve_language_features() over rows, the real ones unless a case gives its
# own, with a stub probe in place of try_compile. The stub passes where the compiler accepts every flag
# it is given (the case's ACCEPTS) and records what it was asked to probe, so a case can tell which
# probes ran and with which flags. This is the wiring between the features: each requirement read from
# the right row and checked against the path that feature resolved to.
function(stubProbe probe standard flags libraries prefix)
    set(result passed)
    foreach(flag IN LISTS flags)
        if(NOT flag IN_LIST stubAcceptedFlags)
            set(result failed)
        endif()
    endforeach()
    list(JOIN flags " " flagText)
    set_property(GLOBAL APPEND PROPERTY stubProbeCalls "${probe}:${flagText}")
    set(${prefix}_RESULT ${result} PARENT_SCOPE)
    set(${prefix}_DIAGNOSTIC "stub.cpp:1:1: error: a flag is not accepted" PARENT_SCOPE)
endfunction()

# expectSelection(<name> OPTIONS <ISOLINE_X=value>... COMPILER <id> VERSION <version> ACCEPTS <flag>...
#                 [ROWS <row>...] [FEATURES <feature>...] [EXPECT <feature|path|reasonRegex>...]
#                 [TARGET <feature|flags|libraries>] [PROBED <probe:flags>...] [NOT_PROBED <probe>...]
#                 [ERROR <regex>])
# Appends to the global property selectionFailures what differs.
function(expectSelection name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "COMPILER;VERSION;ERROR;TARGET"
                          "OPTIONS;ACCEPTS;ROWS;FEATURES;EXPECT;PROBED;NOT_PROBED")
    foreach(assignment IN LISTS arg_OPTIONS)
        string(REGEX MATCH "^([^=]+)=(.*)$" matched "${assignment}")
        set(${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
    endforeach()
    set(stubAcceptedFlags ${arg_ACCEPTS})
    if(NOT arg_ROWS)
        set(arg_ROWS ${ISOLINE_LANGUAGE_FEATURE_ROWS})
    endif()
    set_property(GLOBAL PROPERTY stubProbeCalls "")
    isoline_resolve_language_features(ROWS ${arg_ROWS} COMPILER "${arg_COMPILER}" VERSION "${arg_VERSION}"
                                      PROBE_COMMAND stubProbe OUT_PREFIX got)
    get_property(calls GLOBAL PROPERTY stubProbeCalls)
    set(problems "")
    if(DEFINED arg_ERROR)
        if(NOT got_ERROR MATCHES "${arg_ERROR}")
            string(APPEND problems "\n    error '${got_ERROR}' does not match '${arg_ERROR}'")
        endif()
    elseif(NOT got_ERROR STREQUAL "")
        string(APPEND problems "\n    unexpected error '${got_ERROR}'")
    endif()
    if(DEFINED arg_FEATURES AND NOT got_FEATURES STREQUAL arg_FEATURES)
        string(APPEND problems "\n    features '${got_FEATURES}', expected '${arg_FEATURES}'")
    endif()
    foreach(expectation IN LISTS arg_EXPECT)
        string(REGEX MATCH "^([^|]+)[|]([^|]+)[|](.*)$" matched "${expectation}")
        set(feature "${CMAKE_MATCH_1}")
        set(expectedPath "${CMAKE_MATCH_2}")
        set(expectedReason "${CMAKE_MATCH_3}")
        string(TOUPPER "${feature}" upper)
        if(NOT "${got_${upper}_PATH}" STREQUAL expectedPath OR NOT "${got_${upper}_REASON}" MATCHES "${expectedReason}")
            string(APPEND problems "\n    ${feature}: got ${got_${upper}_PATH}: ${got_${upper}_REASON}")
        endif()
    endforeach()
    if(DEFINED arg_TARGET)
        string(REGEX MATCH "^([^|]+)[|]([^|]*)[|](.*)$" matched "${arg_TARGET}")
        string(TOUPPER "${CMAKE_MATCH_1}" upper)
        list(JOIN got_${upper}_FLAGS " " flags)
        list(JOIN got_${upper}_LINK_LIBRARIES " " libraries)
        if(NOT flags STREQUAL CMAKE_MATCH_2 OR NOT libraries STREQUAL CMAKE_MATCH_3)
            string(APPEND problems "\n    ${CMAKE_MATCH_1} selects '${flags}' and '${libraries}'")
        endif()
    endif()
    foreach(call IN LISTS arg_PROBED)
        if(NOT call IN_LIST calls)
            string(APPEND problems "\n    not probed as ${call} (probed: ${calls})")
        endif()
    endforeach()
    foreach(probe IN LISTS arg_NOT_PROBED)
        foreach(call IN LISTS calls)
            if(call MATCHES "^${probe}:")
                string(APPEND problems "\n    probed ${call}, which is switched OFF")
            endif()
        endforeach()
    endforeach()
    if(NOT problems STREQUAL "")
        set_property(GLOBAL APPEND_STRING PROPERTY selectionFailures "\n  selection '${name}':${problems}")
    endif()
    set_property(GLOBAL APPEND PROPERTY selectionCases "${name}")
endfunction()

set(gccFlags -freflection -fcontracts -fcontract-evaluation-semantic=enforce)

expectSelection("all AUTO on GCC 16.2: every feature native, the contracts row's data selected"
    OPTIONS ISOLINE_REFLECTION=AUTO ISOLINE_ANNOTATIONS=AUTO ISOLINE_CONTRACTS=AUTO
    COMPILER GNU VERSION 16.2.1 ACCEPTS ${gccFlags}
    FEATURES reflection annotations contracts
    EXPECT "reflection|native|^GNU 16[.]2 meets" "annotations|native|^GNU 16[.]2 meets" "contracts|native|^GNU 16[.]2 meets"
    TARGET "contracts|-freflection -fcontracts -fcontract-evaluation-semantic=enforce|stdc++exp")
expectSelection("reflection OFF, contracts ON: refused for the requirement, reflection not probed"
    OPTIONS ISOLINE_REFLECTION=OFF ISOLINE_ANNOTATIONS=AUTO ISOLINE_CONTRACTS=ON
    COMPILER GNU VERSION 16.2.1 ACCEPTS ${gccFlags}
    NOT_PROBED cmake/probes/reflection.cpp
    ERROR "^\\[isoline\\] C[+][+]26 contracts: ISOLINE_CONTRACTS=ON, but it needs reflection, which is fallback$")
expectSelection("reflection OFF, the others AUTO: both follow it to the fallback"
    OPTIONS ISOLINE_REFLECTION=off ISOLINE_ANNOTATIONS=AUTO ISOLINE_CONTRACTS=AUTO
    COMPILER GNU VERSION 16.2.1 ACCEPTS ${gccFlags}
    EXPECT "reflection|fallback|^ISOLINE_REFLECTION=OFF$" "annotations|fallback|^needs reflection, which is fallback$"
           "contracts|fallback|^needs reflection, which is fallback$")
expectSelection("reflection's own probe fails, annotations AUTO: annotations follows it"
    OPTIONS ISOLINE_REFLECTION=AUTO ISOLINE_ANNOTATIONS=AUTO
    ROWS "reflection|GNU|16.2|26|-fbroken|||cmake/probes/reflection.cpp|for a test"
         "annotations|GNU|16.2|26|-freflection||reflection|cmake/probes/annotations.cpp|for a test"
    COMPILER GNU VERSION 16.2.1 ACCEPTS ${gccFlags}
    EXPECT "reflection|fallback|^probe failed: stub[.]cpp" "annotations|fallback|^needs reflection, which is fallback$")
expectSelection("a requirement no row defines"
    OPTIONS ISOLINE_REFLECTION=AUTO ISOLINE_ANNOTATIONS=AUTO
    ROWS "reflection|GNU|16.2|26|-freflection|||cmake/probes/reflection.cpp|for a test"
         "annotations|GNU|16.2|26|-freflection||introspection|cmake/probes/annotations.cpp|for a test"
    COMPILER GNU VERSION 16.2.1 ACCEPTS ${gccFlags}
    ERROR "^\\[isoline\\] C[+][+]26 annotations: its rows require 'introspection', which no earlier feature row defines$")
expectSelection("a requirement whose rows come after its user's"
    OPTIONS ISOLINE_REFLECTION=AUTO ISOLINE_ANNOTATIONS=AUTO
    ROWS "annotations|GNU|16.2|26|-freflection||reflection|cmake/probes/annotations.cpp|for a test"
         "reflection|GNU|16.2|26|-freflection|||cmake/probes/reflection.cpp|for a test"
    COMPILER GNU VERSION 16.2.1 ACCEPTS ${gccFlags}
    ERROR "^\\[isoline\\] C[+][+]26 annotations: its rows require 'reflection', which no earlier feature row defines$")
expectSelection("Clang without a row: probed with the first row's flags, and told to measure itself"
    OPTIONS ISOLINE_REFLECTION=AUTO ISOLINE_ANNOTATIONS=AUTO ISOLINE_CONTRACTS=AUTO
    COMPILER Clang VERSION 23.0.0 ACCEPTS -freflection
    PROBED "cmake/probes/reflection.cpp:-freflection"
           "cmake/probes/contracts.cpp:-freflection -fcontracts -fcontract-evaluation-semantic=enforce"
    EXPECT "reflection|fallback|^the probe passes: measure Clang 23[.]0 and add its row$"
           "annotations|fallback|^the probe passes: measure Clang 23[.]0 and add its row$"
           "contracts|fallback|^no measured row for Clang 23[.]0$")
expectSelection("a feature is one row: a fourth one is decided after its requirement"
    OPTIONS ISOLINE_REFLECTION=AUTO ISOLINE_ANNOTATIONS=AUTO ISOLINE_CONTRACTS=AUTO ISOLINE_WIDGETS=AUTO
    ROWS ${ISOLINE_LANGUAGE_FEATURE_ROWS} "widgets|GNU|16.2|26|-fwidgets||contracts|cmake/probes/widgets.cpp|for a test"
    COMPILER GNU VERSION 16.2.1 ACCEPTS ${gccFlags} -fwidgets
    FEATURES reflection annotations contracts widgets
    EXPECT "widgets|native|^GNU 16[.]2 meets GNU 16[.]2, and cmake/probes/widgets[.]cpp compiles$")

# The options' help text, from the feature's first row: what Isoline uses it for, and what it needs.
set(helpRows
    "reflection|^C[+][+]26 reflection for Isoline's table checks: AUTO, ON or OFF [(]docs/language-features[.]md[)]$"
    "contracts|^C[+][+]26 contracts for preconditions in noexcept functions [(]needs reflection native[)]: AUTO, ON or OFF ")
foreach(row IN LISTS helpRows)
    string(REGEX MATCH "^([^|]+)[|](.*)$" matched "${row}")
    set(expectedHelp "${CMAKE_MATCH_2}")
    isoline_language_feature_help(${CMAKE_MATCH_1} help ${ISOLINE_LANGUAGE_FEATURE_ROWS})
    math(EXPR caseCount "${caseCount} + 1")
    if(NOT help MATCHES "${expectedHelp}")
        string(APPEND failures "\n  help ${row}\n    got ${help}")
    endif()
endforeach()

# The contract semantic: rows compilerId|semantic|expectedFlag|expectedErrorRegex, decided over the real
# semantic rows. Only enforce is selected; every other semantic is a configure error naming its reason,
# and a compiler without semantic rows is one too.
set(semanticRows
    "GNU|enforce|-fcontract-evaluation-semantic=enforce|"
    "GNU|observe||^\\[isoline\\] the contract semantic observe is refused on GNU: it continues past a violated precondition$"
    "GNU|ignore||^\\[isoline\\] the contract semantic ignore is refused on GNU: it does not evaluate the check$"
    "GNU|quick_enforce||^\\[isoline\\] the contract semantic quick_enforce is not offered on GNU: it terminates without a report, so no canary can tell which check fired$"
    "GNU|maybe||^\\[isoline\\] no contract-semantic row names maybe for GNU$"
    "Clang|enforce||^\\[isoline\\] no contract-semantic row names enforce for Clang$")
foreach(row IN LISTS semanticRows)
    string(REGEX MATCH "^([^|]*)[|]([^|]*)[|]([^|]*)[|](.*)$" matched "${row}")
    set(expectedFlag "${CMAKE_MATCH_3}")
    set(expectedError "${CMAKE_MATCH_4}")
    isoline_contract_semantic(COMPILER "${CMAKE_MATCH_1}" SEMANTIC "${CMAKE_MATCH_2}"
                              ROWS ${ISOLINE_CONTRACT_SEMANTIC_ROWS} OUT_FLAG flag OUT_ERROR error)
    math(EXPR caseCount "${caseCount} + 1")
    if(NOT flag STREQUAL expectedFlag
       OR (expectedError STREQUAL "" AND NOT error STREQUAL "")
       OR (NOT expectedError STREQUAL "" AND NOT error MATCHES "${expectedError}"))
        string(APPEND failures "\n  semantic ${row}\n    got flag '${flag}', error '${error}'")
    endif()
endforeach()

# The overrides the configure scan refuses: rows compilerId|text|expectedFlag|expectedReasonRegex, where
# text is what a variable or property holds (generator expressions are matched as text). An empty
# expectedFlag is text that overrides nothing.
set(overrideRows
    "GNU|-O2 -fcontract-evaluation-semantic=ignore -g|-fcontract-evaluation-semantic=ignore|^does not evaluate the check$"
    "GNU|-fcontract-evaluation-semantic=observe|-fcontract-evaluation-semantic=observe|^continues past a violated precondition$"
    "GNU|-fcontract-evaluation-semantic=quick_enforce|-fcontract-evaluation-semantic=quick_enforce|^terminates without a report"
    "GNU|$<$<CONFIG:Debug>:-fcontracts-definition-check=off>|-fcontracts-definition-check=off|^silences a definition's pre"
    "GNU|-fno-contracts|-fno-contracts|^turns contracts off"
    "GNU|-fcontract-evaluation-semantic=enforce -fcontracts||"
    "GNU|-fcontracts-definition-check=on||"
    "GNU|||"
    "Clang|-fcontract-evaluation-semantic=ignore||")
foreach(row IN LISTS overrideRows)
    string(REGEX MATCH "^([^|]*)[|]([^|]*)[|]([^|]*)[|](.*)$" matched "${row}")
    set(expectedFlag "${CMAKE_MATCH_3}")
    set(expectedReason "${CMAKE_MATCH_4}")
    isoline_contract_override_in(COMPILER "${CMAKE_MATCH_1}" TEXT "${CMAKE_MATCH_2}" OUT_FLAG flag OUT_REASON reason)
    math(EXPR caseCount "${caseCount} + 1")
    if(NOT flag STREQUAL expectedFlag OR NOT reason MATCHES "${expectedReason}")
        string(APPEND failures "\n  override ${row}\n    got flag '${flag}', reason '${reason}'")
    endif()
endforeach()

# The refused flags per compiler: rows compilerId|expectedFlags (space-separated). A compiler without
# rows refuses nothing, which the configure scan turns into an error for a native-contracts target.
set(refusedRows
    "GNU|-fcontract-evaluation-semantic=quick_enforce -fcontract-evaluation-semantic=observe -fcontract-evaluation-semantic=ignore -fcontracts-definition-check=off -fno-contracts"
    "Clang|"
    "|")
foreach(row IN LISTS refusedRows)
    string(REGEX MATCH "^([^|]*)[|](.*)$" matched "${row}")
    set(expectedFlags "${CMAKE_MATCH_2}")
    isoline_contract_refused_flags(COMPILER "${CMAKE_MATCH_1}" OUT refused)
    set(flags "")
    foreach(entry IN LISTS refused)
        string(REGEX MATCH "^[^|]*" flag "${entry}")
        list(APPEND flags "${flag}")
    endforeach()
    list(JOIN flags " " flags)
    math(EXPR caseCount "${caseCount} + 1")
    if(NOT flags STREQUAL expectedFlags)
        string(APPEND failures "\n  refused ${row}\n    got '${flags}'")
    endif()
endforeach()

# The generators a native-contracts build accepts: those that write compile_commands.json, which the
# build-time check reads. Rows generator|accepted.
set(generatorRows "Ninja|ON" "Ninja Multi-Config|ON" "Unix Makefiles|ON" "Visual Studio 17 2022|OFF" "Xcode|OFF")
foreach(row IN LISTS generatorRows)
    string(REGEX MATCH "^([^|]*)[|](.*)$" matched "${row}")
    set(accepted OFF)
    if(CMAKE_MATCH_1 IN_LIST ISOLINE_CONTRACT_GENERATORS)
        set(accepted ON)
    endif()
    math(EXPR caseCount "${caseCount} + 1")
    if(NOT accepted STREQUAL CMAKE_MATCH_2)
        string(APPEND failures "\n  generator ${row}\n    got ${accepted}")
    endif()
endforeach()

# The canary's report rows: compilerId|path|spelling|expectedPrefix|expectedSuffix (the expected values
# are the rows' regular expressions, compared as text), or NONE where no row applies.
set(reportRows
    "GNU|native|contract|contract violation in function [^\n]* at [^\n]*: |\n\\[assertion_kind: assert, semantic: enforce, mode: predicate_false, terminating: yes\\]"
    "GNU|native|require|Precondition failed[.] |"
    "GNU|fallback|contract|Precondition failed[.] |"
    "Clang|fallback|require|Precondition failed[.] |"
    "GNU|native|pre|contract violation in function [^\n]* at [^\n]*: |\n\\[assertion_kind: pre, semantic: enforce, mode: predicate_false, terminating: yes\\]"
    "GNU|native|post|contract violation in function [^\n]* at [^\n]*: |\n\\[assertion_kind: post, semantic: enforce, mode: predicate_false, terminating: yes\\]"
    "GNU|fallback|pre|Precondition failed[.] |"
    "MSVC|native|contract|NONE|")
foreach(row IN LISTS reportRows)
    string(REGEX MATCH "^([^|]*)[|]([^|]*)[|]([^|]*)[|]([^|]*)[|](.*)$" matched "${row}")
    set(expectedPrefix "${CMAKE_MATCH_4}")
    set(expectedSuffix "${CMAKE_MATCH_5}")
    isoline_contract_report_pattern(COMPILER "${CMAKE_MATCH_1}" PATH "${CMAKE_MATCH_2}" SPELLING "${CMAKE_MATCH_3}"
                                    OUT_PREFIX prefix OUT_SUFFIX suffix)
    if(NOT DEFINED prefix)
        set(prefix NONE)
    endif()
    math(EXPR caseCount "${caseCount} + 1")
    if(NOT prefix STREQUAL expectedPrefix OR NOT "${suffix}" STREQUAL expectedSuffix)
        string(APPEND failures "\n  report ${row}\n    got '${prefix}' and '${suffix}'")
    endif()
endforeach()

# Where a spelling compiles to no check: compilerId|path|spelling|expected (ON or OFF).
set(uncheckedRows
    "GNU|fallback|post|ON"
    "MSVC|fallback|post|ON"
    "GNU|native|post|OFF"
    "GNU|fallback|pre|OFF"
    "GNU|fallback|contract|OFF")
foreach(row IN LISTS uncheckedRows)
    string(REGEX MATCH "^([^|]*)[|]([^|]*)[|]([^|]*)[|](.*)$" matched "${row}")
    set(expected "${CMAKE_MATCH_4}")
    isoline_contract_is_unchecked(COMPILER "${CMAKE_MATCH_1}" PATH "${CMAKE_MATCH_2}" SPELLING "${CMAKE_MATCH_3}"
                                  OUT unchecked)
    math(EXPR caseCount "${caseCount} + 1")
    if(NOT unchecked STREQUAL expected)
        string(APPEND failures "\n  unchecked ${row}\n    got ${unchecked}")
    endif()
endforeach()

# What a native-contracts package asks of its consumers: compilerId|expectedStandard, NONE where no row does.
set(consumerRows
    "GNU|26"
    "MSVC|NONE"
    "Clang|NONE")
foreach(row IN LISTS consumerRows)
    string(REGEX MATCH "^([^|]*)[|](.*)$" matched "${row}")
    set(expected "${CMAKE_MATCH_2}")
    isoline_contract_consumer_standard(COMPILER "${CMAKE_MATCH_1}" OUT_STANDARD standard)
    if(NOT DEFINED standard)
        set(standard NONE)
    endif()
    math(EXPR caseCount "${caseCount} + 1")
    if(NOT standard STREQUAL expected)
        string(APPEND failures "\n  consumer ${row}\n    got ${standard}")
    endif()
endforeach()

# What a lane expects of the selected paths: rows expected|features|paths|expectedError (features and paths
# separated by spaces; an empty expectedError is no error, otherwise a regular expression).
set(expectationRows
    "|reflection annotations contracts|native fallback native|"
    "native|reflection annotations contracts|native native native|"
    "fallback|reflection annotations contracts|fallback fallback fallback|"
    "native|reflection annotations contracts|native fallback native|^ISOLINE_EXPECT_LANGUAGE_FEATURES=native, but annotations is fallback$"
    "fallback|reflection annotations contracts|native native fallback|^ISOLINE_EXPECT_LANGUAGE_FEATURES=fallback, but reflection is native, annotations is native$"
    "native|reflection annotations contracts|native native|^ISOLINE_EXPECT_LANGUAGE_FEATURES=native, but 2 paths were selected for 3 features$"
    "native|||^ISOLINE_EXPECT_LANGUAGE_FEATURES=native, but 0 paths were selected for 0 features$"
    "maybe|reflection|native|^ISOLINE_EXPECT_LANGUAGE_FEATURES must be empty, native or fallback, not 'maybe'$")
foreach(row IN LISTS expectationRows)
    string(REGEX MATCH "^([^|]*)[|]([^|]*)[|]([^|]*)[|](.*)$" matched "${row}")
    set(expectedError "${CMAKE_MATCH_4}")
    string(REPLACE " " ";" features "${CMAKE_MATCH_2}")
    string(REPLACE " " ";" paths "${CMAKE_MATCH_3}")
    isoline_language_feature_expectation(EXPECTED "${CMAKE_MATCH_1}" FEATURES ${features} PATHS ${paths}
                                         OUT_ERROR error)
    math(EXPR caseCount "${caseCount} + 1")
    if((expectedError STREQUAL "" AND NOT error STREQUAL "") OR NOT error MATCHES "${expectedError}")
        string(APPEND failures "\n  expectation ${row}\n    got '${error}'")
    endif()
endforeach()

get_property(selectionCases GLOBAL PROPERTY selectionCases)
list(LENGTH selectionCases selectionCaseCount)
math(EXPR caseCount "${caseCount} + ${selectionCaseCount}")
get_property(selectionFailures GLOBAL PROPERTY selectionFailures)
string(APPEND failures "${selectionFailures}")

if(NOT failures STREQUAL "")
    message(FATAL_ERROR "language-features: rows whose outcome differs:${failures}")
endif()
message(STATUS "language-features: all ${caseCount} rows hold")
