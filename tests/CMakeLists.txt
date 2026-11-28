# SPDX-License-Identifier: Apache-2.0

# Suites that are not unit tests of one module.
add_subdirectory(spec)
# Isoline through `import isoline;` (ISOLINE_MODULES): the in-tree consumers of the module targets.
if(ISOLINE_MODULES)
    add_subdirectory(modules)
endif()
# The C API's C tests and its lints.
if(ISOLINE_C_API)
    add_subdirectory(c)
endif()

# The spec runner's self-test: a script with wrong assertions the runner must report and fail on.
# Two are wrong in their values (1 + 2 is not 4, and dividing by zero is no overflow). The rest pin
# each failure assertion's stage: for every kind, one module that fails at its stage (passes) and
# one that fails with the right message at another stage (fails) — a malformed module to
# assert_invalid, an invalid one to assert_malformed, an uninstantiable one to assert_unlinkable and
# an unlinkable one to assert_uninstantiable. Then a definition, instantiated under a name and invoked
# by it and by an either, passes; an assert_exception of a function that returns, an
# assert_invalid_custom of a module without custom sections, and an instance of an unknown
# definition fail. Last, an instance of a named module, which names a definition too, passes.
add_test(NAME spec-selftest COMMAND isoline_spec_runner "${CMAKE_CURRENT_SOURCE_DIR}/spec-selftest" 0 --features wasm2)
# Each failure is pinned by its line, in order, so that one failing for another reason does not pass.
set_tests_properties(spec-selftest PROPERTIES LABELS unit
                     PASS_REGULAR_EXPRESSION "spec-selftest.wast:8: 'add': expected i32:4, got i32:3.*spec-selftest.wast:9: expected execute failure 'integer overflow', got execute failure 'integer divide by zero'.*spec-selftest.wast:13: expected validate failure 'magic header not detected', got decode failure.*spec-selftest.wast:15: expected decode failure 'type mismatch', got validate failure.*spec-selftest.wast:17: expected instantiate failure 'unknown import', got link failure.*spec-selftest.wast:19: expected link failure 'out of bounds memory access', got instantiate failure.*spec-selftest.wast:28: expected exception failure '', but it succeeded.*spec-selftest.wast:29: expected custom failure 'x', but it succeeded.*spec-selftest.wast:32: no module definition named X.*passed 13, failed 9, skipped 0")
# The same script, checked for its exit status: a failing script must make the runner fail.
add_test(NAME spec-selftest-exit
         COMMAND isoline_spec_runner "${CMAKE_CURRENT_SOURCE_DIR}/spec-selftest" 0 --features wasm2)
set_tests_properties(spec-selftest-exit PROPERTIES LABELS unit WILL_FAIL TRUE)

# assert_invalid_custom, under 3.0 (whose branch hints are the custom section Isoline interprets): a
# module whose hint names a non-branch passes with "invalid target", fails with "invalid order" (a
# wrong message), and a malformed module fails it (a load failure is never a custom error).
add_test(NAME spec-selftest-custom
         COMMAND isoline_spec_runner "${CMAKE_CURRENT_SOURCE_DIR}/spec-selftest-custom" 0 --features wasm3)
set_tests_properties(spec-selftest-custom PROPERTIES LABELS unit
                     PASS_REGULAR_EXPRESSION "spec-selftest-custom.wast:11: expected custom failure '@metadata.code.branch_hint annotation: invalid order', got custom failure '@metadata.code.branch_hint annotation: invalid target'.*spec-selftest-custom.wast:17: expected custom failure '@metadata.code.branch_hint annotation: invalid target', got decode failure 'magic header not detected'.*passed 1, failed 2, skipped 0")

# The native runner: a .wast script against its oracle. Its first module is given as text and its
# oracle binary agrees, as does the assert_return after it; the oracle's binary of the quote module is
# deliberately another module's, so its parity check fails at the first byte they differ in; the
# oracle skips the last command, which counts against one pinned skip.
add_test(NAME spec-native-parity
         COMMAND isoline_spec_runner "${CMAKE_CURRENT_SOURCE_DIR}/spec-native/parity" 1 --features wasm3)
set_tests_properties(spec-native-parity PROPERTIES LABELS unit
                     PASS_REGULAR_EXPRESSION "encoding differs from the oracle: 0x20.*passed 2, failed 1, skipped 1")
# An expected divergence: the oracle records the message Isoline reports where it reports another error
# of a malformed module first than the reference does. The assertion with that entry passes, the same
# assertion without it fails, and so does one whose entry is only a prefix of Isoline's message.
add_test(NAME spec-native-divergence
         COMMAND isoline_spec_runner "${CMAKE_CURRENT_SOURCE_DIR}/spec-native/divergence" 0 --features wasm3)
set_tests_properties(spec-native-divergence PROPERTIES LABELS unit
                     PASS_REGULAR_EXPRESSION "divergence.wast:7: expected decode failure 'unexpected token', got decode failure 'unknown local [$]x at 1:22 of its quoted text'.*divergence.wast:8: expected the recorded divergence 'unknown local' [(]decode stage[)], got 'unknown local [$]x'.*passed 1, failed 2, skipped 0")
# A parity divergence the oracle records, where its binary is known to be wrong: the parity check must
# fail at exactly the recorded offset, and Isoline's encoding must have exactly the recorded digest. The
# modules whose records hold pass; one whose divergence is at another offset fails, and so do one whose
# divergence does not occur, one whose encoding changed behind the offset, and one with no encoding. The
# assert_return still runs.
add_test(NAME spec-native-recorded-parity
         COMMAND isoline_spec_runner "${CMAKE_CURRENT_SOURCE_DIR}/spec-native/recorded-parity" 0 --features wasm3)
set_tests_properties(spec-native-recorded-parity PROPERTIES LABELS unit
                     PASS_REGULAR_EXPRESSION "recorded-parity.wast:13: expected the recorded parity divergence at 0x1f, got: encoding differs from the oracle: 0x20.*recorded-parity.wast:14: the recorded parity divergence at 0x20 does not occur: the encodings agree.*recorded-parity.wast:16: the recorded parity divergence at 0x17 is stale: Isoline's encoding is now sha256:3bb6f35edaf603fa5e8679c551c1a37bdbb9264115714e5b9fed3dac45566706, not the recorded sha256:5a9722fe6067e5a2dab1195f5875cbdcc684f8956457e5d02d28c8e7cbdc19d6.*recorded-parity.wast:17: the recorded parity divergence at 0x17 cannot be checked: Isoline produces no encoding.*passed 3, failed 4, skipped 0")
# The stages of text errors: a syntax error answers assert_malformed, not assert_malformed_custom; and a
# module given as text reports its error at its line and column in the script, not in the module.
add_test(NAME spec-native-stages
         COMMAND isoline_spec_runner "${CMAKE_CURRENT_SOURCE_DIR}/spec-native/stages" 0 --features wasm3)
set_tests_properties(spec-native-stages PROPERTIES LABELS unit
                     PASS_REGULAR_EXPRESSION "expected malformed custom failure 'unknown operator', got decode failure 'unknown operator x at 1:18 of its quoted text'.*stages.wast:6: unexpected failure: unknown operator x at 8:22.*passed 1, failed 2, skipped 0")
# An oracle whose entries sit on the wrong commands fails the script as a harness failure: here the
# module's binary is moved onto the assert_return after it.
add_test(NAME spec-native-shifted
         COMMAND isoline_spec_runner "${CMAKE_CURRENT_SOURCE_DIR}/spec-native/shifted" 0 --features wasm3)
set_tests_properties(spec-native-shifted PROPERTIES LABELS unit
                     PASS_REGULAR_EXPRESSION "harness failure: the oracle's entry for command 1 [(]line 4[)] names a binary of a module given as text, but the command on line 6 is an assert_return")
# An oracle that counts other than its script fails the script as a harness failure, running nothing.
add_test(NAME spec-native-miscount
         COMMAND isoline_spec_runner "${CMAKE_CURRENT_SOURCE_DIR}/spec-native/miscount" 0 --features wasm3)
set_tests_properties(spec-native-miscount PROPERTIES LABELS unit
                     PASS_REGULAR_EXPRESSION "harness failure: the script holds 2 commands, but its oracle counts 3")
add_test(NAME spec-native-miscount-exit
         COMMAND isoline_spec_runner "${CMAKE_CURRENT_SOURCE_DIR}/spec-native/miscount" 0 --features wasm3)
set_tests_properties(spec-native-miscount-exit PROPERTIES LABELS unit WILL_FAIL TRUE)

# The pinned skip count: a script whose oracle skips its one command passes with one skip pinned, and
# fails with none pinned or two.
add_test(NAME spec-skips COMMAND isoline_spec_runner "${CMAKE_CURRENT_SOURCE_DIR}/spec-skips" 1 --features wasm2)
set_tests_properties(spec-skips PROPERTIES LABELS unit PASS_REGULAR_EXPRESSION "passed 0, failed 0, skipped 1")
add_test(NAME spec-skips-none COMMAND isoline_spec_runner "${CMAKE_CURRENT_SOURCE_DIR}/spec-skips" 0 --features wasm2)
set_tests_properties(spec-skips-none PROPERTIES LABELS unit PASS_REGULAR_EXPRESSION "skipped 1, but 0 skips are pinned")
add_test(NAME spec-skips-fewer COMMAND isoline_spec_runner "${CMAKE_CURRENT_SOURCE_DIR}/spec-skips" 2 --features wasm2)
set_tests_properties(spec-skips-fewer PROPERTIES LABELS unit WILL_FAIL TRUE)

# The feature sets: a module of extended constant expressions, a 3.0 feature, instantiates under
# wasm3 and all, and fails, with the read of its global, under wasm2.
foreach(features IN ITEMS wasm3 all)
    add_test(NAME spec-features-${features}
             COMMAND isoline_spec_runner "${CMAKE_CURRENT_SOURCE_DIR}/spec-features" 0 --features ${features})
    set_tests_properties(spec-features-${features} PROPERTIES LABELS unit
                         PASS_REGULAR_EXPRESSION "passed 2, failed 0, skipped 0")
endforeach()
add_test(NAME spec-features-wasm2
         COMMAND isoline_spec_runner "${CMAKE_CURRENT_SOURCE_DIR}/spec-features" 0 --features wasm2)
set_tests_properties(spec-features-wasm2 PROPERTIES LABELS unit PASS_REGULAR_EXPRESSION "passed 0, failed 2, skipped 0")

# The command line: a skip count and a feature set are required, and the set must be one it knows.
foreach(usage IN ITEMS "1" "1;--features" "1;--features;wasm4" "x;--features;wasm2" "1;--feature;wasm2")
    string(REPLACE ";" "-" usageName "${usage}")
    add_test(NAME spec-usage-${usageName}
             COMMAND isoline_spec_runner "${CMAKE_CURRENT_SOURCE_DIR}/spec-skips" ${usage})
    set_tests_properties(spec-usage-${usageName} PROPERTIES LABELS unit PASS_REGULAR_EXPRESSION "usage: ")
endforeach()

# Every test that builds a target of this tree (a compile-fail test, the refused lambda) runs ninja in
# it, and two ninja runs in one tree under ctest -j race on its log and its outputs: they share a lock.
set(isolineMainBuildTreeLock isoline-main-build-tree)

# The api's preconditions, which end the process, one case of the canary each: Engine's constructor
# refuses a feature set not closed over its prerequisites, the accessors of a handle whose store is gone
# that return a reference into the store refuse to read it, and Imports refuses a host function of a type
# no store could define. Each case is judged by its check's own text (PASS_REGULAR_EXPRESSION, which
# ignores the exit status), so that an abort elsewhere does not pass for it; the line the canary prints
# when the check let the call through fails it. The text around the check is the report row of the
# case's spelling (cmake/IsolineLanguageFeatures.cmake): a Require() reports through core-cpp's fail
# handler, a contract natively through the compiler's violation handler.
# The positions a contract-spelled check must be evaluated in, compiled as the libraries are.
add_library(isoline_canary_check_positions OBJECT canary/CheckPositions.cpp)
target_link_libraries(isoline_canary_check_positions PRIVATE isoline::core)
isoline_apply_target_policy(isoline_canary_check_positions)
_isoline_compile_with_language_features(isoline_canary_check_positions)
if(ISOLINE_CONTRACTS_PATH STREQUAL "native")
    # Compiled with the contracts' flags, which only GCC has: clang-tidy never sees it natively.
    set_target_properties(isoline_canary_check_positions PROPERTIES CXX_CLANG_TIDY "")
endif()
add_executable(isoline_require_canary canary/RequireCanary.cpp)
target_link_libraries(isoline_require_canary PRIVATE isoline_canary_check_positions isoline::core)
isoline_apply_target_policy(isoline_require_canary TEST)
target_link_libraries(isoline_require_canary PRIVATE Catch2::Catch2) # testing/EngineRows.hpp's StringMaker
# Where contracts are native, the same canary with a violation handler that throws
# (canary/ThrowingHandler.cpp), for the contract-spelled cases: every contract sits in a noexcept
# function, so the exception ends the process where it is thrown, and never reaches main()'s catch.
if(ISOLINE_CONTRACTS_PATH STREQUAL "native")
    add_library(isoline_canary_throwing_handler OBJECT canary/ThrowingHandler.cpp)
    isoline_apply_target_policy(isoline_canary_throwing_handler)
    _isoline_compile_with_language_features(isoline_canary_throwing_handler)
    set_target_properties(isoline_canary_throwing_handler PROPERTIES CXX_CLANG_TIDY "")
    add_executable(isoline_require_canary_throwing_handler canary/RequireCanary.cpp)
    target_link_libraries(isoline_require_canary_throwing_handler
                          PRIVATE isoline_canary_throwing_handler isoline_canary_check_positions isoline::core
                                  Catch2::Catch2)
    isoline_apply_target_policy(isoline_require_canary_throwing_handler TEST)
    # The same, its calls compiled with the compiler's caller-side checking of declaration contracts, which a
    # consumer may turn on: a caller-side `pre` must still end the process, not let the handler's exception
    # reach the canary's try (docs/embedding.md). Rows: compilerId|flag.
    set(isolineContractClientCheckRows "GNU|-fcontracts-client-check=all")
    foreach(row IN LISTS isolineContractClientCheckRows)
        string(REPLACE "|" ";" columns "${row}")
        list(GET columns 0 clientCheckCompiler)
        list(GET columns 1 clientCheckFlag)
        if(clientCheckCompiler STREQUAL CMAKE_CXX_COMPILER_ID)
            add_executable(isoline_require_canary_client_check canary/RequireCanary.cpp)
            target_link_libraries(isoline_require_canary_client_check
                                  PRIVATE isoline_canary_throwing_handler isoline_canary_check_positions isoline::core
                                          Catch2::Catch2)
            target_compile_options(isoline_require_canary_client_check PRIVATE ${clientCheckFlag})
            isoline_apply_target_policy(isoline_require_canary_client_check TEST)
        endif()
    endforeach()
    # The other half of the lambda rule: a macro in a lambda that is not noexcept, inside a noexcept
    # function's try block, does not compile (canary/LambdaMayThrow.cpp). The target is built by its test
    # alone, which passes on the macro's static_assert.
    add_library(isoline_canary_lambda_may_throw OBJECT EXCLUDE_FROM_ALL canary/LambdaMayThrow.cpp)
    target_link_libraries(isoline_canary_lambda_may_throw PRIVATE isoline::core)
    isoline_apply_target_policy(isoline_canary_lambda_may_throw)
    _isoline_compile_with_language_features(isoline_canary_lambda_may_throw)
    set_target_properties(isoline_canary_lambda_may_throw PROPERTIES CXX_CLANG_TIDY "")
    add_test(NAME contract-lambda-may-throw-refused
             COMMAND "${CMAKE_COMMAND}" --build "${PROJECT_BINARY_DIR}" --target isoline_canary_lambda_may_throw)
    set_tests_properties(contract-lambda-may-throw-refused PROPERTIES LABELS unit TIMEOUT 300
        RESOURCE_LOCK "${isolineMainBuildTreeLock}"
        PASS_REGULAR_EXPRESSION "ISOLINE_PRECONDITION/ISOLINE_INVARIANT need a noexcept function")
endif()
# Sources that must not compile, each built by its test alone, which passes on the message naming why.
# Rows: name|source|feature (empty: on every compiler; else only where that feature is native)|message.
set(isolineCompileFailRows
    "fixed-text-unterminated|compile-fail/FixedTextUnterminated.cpp||fixedTextNeedsANulTerminatedLiteral"
    "annotation-missing|compile-fail/AnnotationMissing.cpp|annotations|Unannotated::Bare has no FixedText annotation"
    "annotation-twice|compile-fail/AnnotationTwice.cpp|annotations|Doubled::Twice has 2 FixedText annotations"
    "declaration-contract-may-throw|compile-fail/DeclarationContractMayThrow.cpp|contracts|ISOLINE_PRE/ISOLINE_POST need a noexcept member function"
    "declaration-contract-on-free-function|compile-fail/DeclarationContractOnFreeFunction.cpp|contracts|ISOLINE_PRE/ISOLINE_POST need a noexcept member function"
    "declaration-contract-on-class-template|compile-fail/DeclarationContractOnClassTemplate.cpp|contracts|ISOLINE_PRE/ISOLINE_POST stay off a class template's members"
    "c-mirror-constant-without-row|compile-fail/CMirrorConstantWithoutRow.cpp|reflection|KindRows: fixture_kind_enum::FIXTURE_KIND_ADDED [(]2[)] has no row"
    "c-layout-member-without-row|compile-fail/CLayoutMemberWithoutRow.cpp|reflection|fixture_limits_t: member added has no ISOLINE_LAYOUT_FIELD row")
foreach(row IN LISTS isolineCompileFailRows)
    string(REPLACE "|" ";" columns "${row}")
    list(GET columns 0 failName)
    list(GET columns 1 failSource)
    list(GET columns 2 failFeature)
    list(GET columns 3 failMessage)
    set(failTarget isoline_compile_fail_${failName})
    string(REPLACE "-" "_" failTarget "${failTarget}")
    if(failFeature STREQUAL "")
        add_library(${failTarget} OBJECT ${failSource})
        target_link_libraries(${failTarget} PRIVATE isoline::core)
        isoline_apply_target_policy(${failTarget})
        set_target_properties(${failTarget} PROPERTIES CXX_CLANG_TIDY "")
    else()
        isoline_add_reflection_check(${failTarget} FEATURE ${failFeature} SOURCES ${failSource}
                                     LINK_LIBRARIES isoline::core)
        if(NOT TARGET ${failTarget})
            continue()
        endif()
        if(failFeature STREQUAL "contracts")
            # Read as the noexcept rule's check reads the headers: through the proving expansion.
            target_include_directories(${failTarget} BEFORE PRIVATE "${ISOLINE_CONTRACTS_PROOF_DIR}")
        endif()
    endif()
    set_target_properties(${failTarget} PROPERTIES EXCLUDE_FROM_ALL ON)
    add_test(NAME compile-fail-${failName}
             COMMAND "${CMAKE_COMMAND}" --build "${PROJECT_BINARY_DIR}" --target ${failTarget})
    set_tests_properties(compile-fail-${failName} PROPERTIES LABELS unit TIMEOUT 300
                         RESOURCE_LOCK "${isolineMainBuildTreeLock}"
                         PASS_REGULAR_EXPRESSION "${failMessage}")
endforeach()

# The consumers of isoline/Contracts.hpp, the generated header that fixes the package's declaration
# contracts, one compile each (tests/contracts-consumer/Consumer.cpp), built by its test alone and never
# linked to isoline::core, whose interface would give it the package's flags. Rows:
# name|path|compilerId|standard|flags|message: where contracts took that path and the compiler is
# compilerId (`*` any), the unit compiles at the standard with the flags; an empty message expects it to
# compile, any other the build to fail naming it. A native package refuses a unit compiled without
# contracts; the fallback asks nothing, so a unit compiled with them reads the same nothing.
set(isolineContractsConsumerRefusal "isoline was built with declaration contracts [(]ISOLINE_DECLARATION_CONTRACTS is 1[)]")
set(isolineContractsConsumerRows
    "cxx23|native|GNU|23||${isolineContractsConsumerRefusal}"
    "without-contracts|native|GNU|26|-fno-contracts|${isolineContractsConsumerRefusal}"
    "as-the-package-asks|native|GNU|26||"
    "cxx23|fallback|*|23||"
    "with-contracts|fallback|GNU|26|-fcontracts|")
foreach(row IN LISTS isolineContractsConsumerRows)
    if(NOT row MATCHES "^([a-z0-9-]+)[|](native|fallback)[|]([^|]+)[|]([0-9]+)[|]([^|]*)[|](.*)$")
        message(FATAL_ERROR "isolineContractsConsumerRows: malformed row: ${row}")
    endif()
    set(consumerName "${CMAKE_MATCH_1}")
    set(consumerPath "${CMAKE_MATCH_2}")
    set(consumerCompiler "${CMAKE_MATCH_3}")
    set(consumerStandard "${CMAKE_MATCH_4}")
    separate_arguments(consumerFlags UNIX_COMMAND "${CMAKE_MATCH_5}")
    set(consumerMessage "${CMAKE_MATCH_6}")
    if(NOT consumerPath STREQUAL ISOLINE_CONTRACTS_PATH
       OR NOT (consumerCompiler STREQUAL "*" OR consumerCompiler STREQUAL CMAKE_CXX_COMPILER_ID))
        continue()
    endif()
    set(consumerTarget "isoline_contracts_consumer_${consumerName}")
    string(REPLACE "-" "_" consumerTarget "${consumerTarget}")
    add_library(${consumerTarget} OBJECT EXCLUDE_FROM_ALL contracts-consumer/Consumer.cpp)
    target_include_directories(${consumerTarget} PRIVATE "${PROJECT_BINARY_DIR}/include")
    set(consumerExpected 0)
    if(consumerPath STREQUAL "native")
        set(consumerExpected 1)
    endif()
    target_compile_definitions(${consumerTarget} PRIVATE ISOLINE_EXPECTED_DECLARATION_CONTRACTS=${consumerExpected})
    set_target_properties(${consumerTarget} PROPERTIES CXX_STANDARD ${consumerStandard} CXX_STANDARD_REQUIRED ON
                                                       CXX_EXTENSIONS OFF)
    target_compile_options(${consumerTarget} PRIVATE ${consumerFlags})
    isoline_apply_target_policy(${consumerTarget})
    set_target_properties(${consumerTarget} PROPERTIES CXX_CLANG_TIDY "")
    add_test(NAME contracts-consumer-${consumerName}
             COMMAND "${CMAKE_COMMAND}" --build "${PROJECT_BINARY_DIR}" --target ${consumerTarget})
    set_tests_properties(contracts-consumer-${consumerName} PROPERTIES LABELS unit TIMEOUT 300
                         RESOURCE_LOCK "${isolineMainBuildTreeLock}")
    if(NOT consumerMessage STREQUAL "")
        set_tests_properties(contracts-consumer-${consumerName} PROPERTIES PASS_REGULAR_EXPRESSION "${consumerMessage}")
    endif()
endforeach()

# Where contracts are native, the proving <isoline/Contracts.hpp> expands the macros as the installed one does,
# its noexcept proof aside: the proof reads the declarations as they are installed.
if(ISOLINE_CONTRACTS_PATH STREQUAL "native")
    add_test(NAME contracts-expansion
             COMMAND "${CMAKE_COMMAND}" "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
                     "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/contracts-consumer/Expansion.cpp"
                     "-DINSTALLED_DIR=${PROJECT_BINARY_DIR}/include" "-DPROOF_DIR=${ISOLINE_CONTRACTS_PROOF_DIR}"
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/contracts-expansion.cmake")
    set_tests_properties(contracts-expansion PROPERTIES LABELS lint TIMEOUT 60)
    # A consumer of the other mode's header does not link with this mode's library.
    add_executable(isoline_contracts_mode_mismatch EXCLUDE_FROM_ALL contracts-consumer/Mismatch.cpp)
    target_include_directories(isoline_contracts_mode_mismatch BEFORE PRIVATE "${ISOLINE_CONTRACTS_SUBSTITUTE_DIR}")
    target_link_libraries(isoline_contracts_mode_mismatch PRIVATE isoline::core)
    isoline_apply_target_policy(isoline_contracts_mode_mismatch)
    set_target_properties(isoline_contracts_mode_mismatch PROPERTIES CXX_CLANG_TIDY "")
    add_test(NAME contracts-mode-mismatch
             COMMAND "${CMAKE_COMMAND}" --build "${PROJECT_BINARY_DIR}" --target isoline_contracts_mode_mismatch)
    set_tests_properties(contracts-mode-mismatch PROPERTIES LABELS unit TIMEOUT 300
                         RESOURCE_LOCK "${isolineMainBuildTreeLock}"
                         PASS_REGULAR_EXPRESSION "undefined (reference to|symbol:?) [^\n]*declaration_contracts_0")
endif()

# The cases, read from the table the canary itself includes, one ISOLINE_REQUIRE_CANARY_CASE line each.
set(isolineRequireCanaryTable "${CMAKE_CURRENT_SOURCE_DIR}/canary/RequireCanaryCases.def")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${isolineRequireCanaryTable}")
file(STRINGS "${isolineRequireCanaryTable}" isolineRequireCanaryCases REGEX "^ISOLINE_REQUIRE_CANARY_CASE[(]")
foreach(canaryCase IN LISTS isolineRequireCanaryCases)
    if(NOT canaryCase MATCHES "^ISOLINE_REQUIRE_CANARY_CASE[(][A-Za-z]+, \"([a-z-]+)\", (contract|require|pre|post), \"([^\"]+)\"[)]$")
        message(FATAL_ERROR "RequireCanaryCases.def: malformed case: ${canaryCase}")
    endif()
    set(canaryName "${CMAKE_MATCH_1}")
    set(canarySpelling "${CMAKE_MATCH_2}")
    set(canaryCheck "${CMAKE_MATCH_3}")
    isoline_contract_report_pattern(COMPILER "${CMAKE_CXX_COMPILER_ID}" PATH "${ISOLINE_CONTRACTS_PATH}"
                                    SPELLING ${canarySpelling} OUT_PREFIX canaryPrefix OUT_SUFFIX canarySuffix)
    if(NOT DEFINED canaryPrefix)
        message(FATAL_ERROR "RequireCanaryCases.def: no report row for ${CMAKE_CXX_COMPILER_ID}, "
                            "${ISOLINE_CONTRACTS_PATH}, ${canarySpelling} (cmake/IsolineLanguageFeatures.cmake)")
    endif()
    add_test(NAME isoline_require_canary.${canaryName} COMMAND isoline_require_canary ${canaryName})
    isoline_contract_is_unchecked(COMPILER "${CMAKE_CXX_COMPILER_ID}" PATH "${ISOLINE_CONTRACTS_PATH}"
                                  SPELLING ${canarySpelling} OUT canaryUnchecked)
    if(canaryUnchecked)
        # Nothing is compiled to check it on this path: the case passes on the call going through, and a
        # report of its check fails it.
        set_tests_properties(isoline_require_canary.${canaryName} PROPERTIES LABELS unit TIMEOUT 60
            PASS_REGULAR_EXPRESSION "require-canary: ${canaryName}: the check let the call through"
            FAIL_REGULAR_EXPRESSION "${canaryCheck}")
    else()
        set_tests_properties(isoline_require_canary.${canaryName} PROPERTIES LABELS unit TIMEOUT 60
            PASS_REGULAR_EXPRESSION "${canaryPrefix}${canaryCheck}${canarySuffix}"
            FAIL_REGULAR_EXPRESSION "the check let the call through")
    endif()
    if(TARGET isoline_require_canary_throwing_handler AND NOT canarySpelling STREQUAL "require")
        add_test(NAME isoline_require_canary_throwing_handler.${canaryName}
                 COMMAND isoline_require_canary_throwing_handler ${canaryName})
        set_tests_properties(isoline_require_canary_throwing_handler.${canaryName} PROPERTIES LABELS unit TIMEOUT 60
            PASS_REGULAR_EXPRESSION "throwing-handler: ${canaryCheck}"
            FAIL_REGULAR_EXPRESSION "the check let the call through")
    endif()
    if(TARGET isoline_require_canary_client_check AND canarySpelling MATCHES "^(pre|post)$")
        add_test(NAME isoline_require_canary_client_check.${canaryName}
                 COMMAND isoline_require_canary_client_check ${canaryName})
        set_tests_properties(isoline_require_canary_client_check.${canaryName} PROPERTIES LABELS unit TIMEOUT 60
            PASS_REGULAR_EXPRESSION "throwing-handler: ${canaryCheck}"
            FAIL_REGULAR_EXPRESSION "the check let the call through")
    endif()
endforeach()

# The C++ module support table (cmake/IsolineModuleSupport.cmake), in script mode: its floors, refusals
# and generator rows, and the same-LLVM rule for clang-tidy, each a row of expected verdicts.
add_test(NAME module-support
         COMMAND "${CMAKE_COMMAND}" -P "${PROJECT_SOURCE_DIR}/tests/cmake/module-support_test.cmake")
set_tests_properties(module-support PROPERTIES LABELS lint TIMEOUT 60)

# How check-install tells that an installed file names a path of the build (tests/cmake/check-install-paths.cmake),
# in script mode, beside the install tests that use it and registered with or without them.
add_test(NAME check-install-paths
         COMMAND "${CMAKE_COMMAND}" "-DWORK_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/check-install-paths"
                 -P "${PROJECT_SOURCE_DIR}/tests/cmake/check-install-paths_test.cmake")
set_tests_properties(check-install-paths PROPERTIES LABELS lint TIMEOUT 60)
# A compiler the table refuses because CMake cannot scan its modules (Apple Clang, clang-cl) gets the
# tripwire that probes CMake itself, module-scanning-tripwire (cmake/IsolineModules.cmake).
isoline_add_module_tripwires()
# The opt-in, from the other side: a build with ISOLINE_MODULES OFF builds nothing of modules (no module
# target, no scan). It reads the Ninja build files, so it is registered under a Ninja generator only.
if(NOT ISOLINE_MODULES AND CMAKE_GENERATOR MATCHES "^Ninja")
    add_test(NAME modules-off-build
             COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${PROJECT_BINARY_DIR}"
                     -P "${PROJECT_SOURCE_DIR}/tests/cmake/expect-no-module-build.cmake")
    set_tests_properties(modules-off-build PROPERTIES LABELS lint TIMEOUT 60)
endif()

# The binaries the coverage report reads (cmake/IsolineCoverage.cmake): every test executable, and the C
# API's shared library, whose code the C tests reach through it. A test executable of each kind stands for
# the rule that lists it.
set(isolineRequiredCoverageBinaries "$<TARGET_FILE:isoline_test>" "$<TARGET_FILE:isoline_spec_runner>")
if(TARGET isoline_c_shared)
    list(APPEND isolineRequiredCoverageBinaries "$<TARGET_FILE:isoline_c_shared>" "$<TARGET_FILE:isoline_c_test>")
endif()
add_test(NAME coverage-binaries
         COMMAND "${CMAKE_COMMAND}" "-DLIST=${ISOLINE_COVERAGE_BINARIES_FILE}"
                 "-DREQUIRED=${isolineRequiredCoverageBinaries}" -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckCoverageBinaries.cmake")
set_tests_properties(coverage-binaries PROPERTIES LABELS lint)

# The coverage ratchet's checker, tested on fixture reports: it needs Python, not a coverage build.
# The Python tests name the interpreter by its path, not as the imported target Python3::Interpreter:
# CMake runs an executable target through CMAKE_CROSSCOMPILING_EMULATOR, and a cross build's emulator
# (qemu, wine) cannot run the host's python. A path runs as it is, on the host, in every tree.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME coverage-check-selftest
             COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/scripts/coverage-check_test.py")
    set_tests_properties(coverage-check-selftest PROPERTIES LABELS lint)
    # The spec importer's translation: which commands become skip lines, and which fail the import.
    add_test(NAME import-spec-tests-selftest
             COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/scripts/import-spec-tests_test.py")
    set_tests_properties(import-spec-tests-selftest PROPERTIES LABELS lint)
    # docs/conformance.md lists exactly the excluded files and the assertions the GC cut skips.
    add_test(NAME conformance-doc-current
             COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/scripts/check-conformance.py")
    set_tests_properties(conformance-doc-current PROPERTIES LABELS lint)
    add_test(NAME check-conformance-selftest
             COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/scripts/check-conformance_test.py")
    set_tests_properties(check-conformance-selftest PROPERTIES LABELS lint)
    # The pinned-tool fetcher's lock rows, digest check and cache, on fixture archives: offline.
    add_test(NAME fetch-tools-selftest
             COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/scripts/fetch-tools_test.py")
    set_tests_properties(fetch-tools-selftest PROPERTIES LABELS lint)
    # A contract is spelled through the seam's macros, never as a raw contract_assert
    # (scripts/check-contract-spelling.py), and the checker's own fixtures.
    add_test(NAME contract-spelling
             COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/scripts/check-contract-spelling.py")
    set_tests_properties(contract-spelling PROPERTIES LABELS lint)
    add_test(NAME contract-spelling-selftest
             COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/scripts/check-contract-spelling_test.py")
    set_tests_properties(contract-spelling-selftest PROPERTIES LABELS lint)
    # Every enum-first table reflection cannot reach (in a source file, or in a header's function or
    # class) is a source-local row of src/isoline/meta/EnumTables.def, and docs/language-features.md
    # lists those rows. The lint reads Clang's AST of every source file of this tree, so it runs on Clang
    # trees with the GNU command line alone; the other compilers' sources are the same files, linted on
    # CI's Clang lanes. Each translation unit's result is cached in the build tree.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU"
       AND CMAKE_EXPORT_COMPILE_COMMANDS)
        add_test(NAME enum-tables-current
                 COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/scripts/check-enum-tables.py"
                         --compile-commands "${PROJECT_BINARY_DIR}/compile_commands.json"
                         --clang "${CMAKE_CXX_COMPILER}" --jobs 4
                         --cache "${CMAKE_CURRENT_BINARY_DIR}/enum-tables-cache")
        set_tests_properties(enum-tables-current PROPERTIES LABELS lint PROCESSORS 4 TIMEOUT 600)
        add_test(NAME check-enum-tables-selftest
                 COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/scripts/check-enum-tables_test.py"
                         --clang "${CMAKE_CXX_COMPILER}")
        set_tests_properties(check-enum-tables-selftest PROPERTIES LABELS lint TIMEOUT 300)
    endif()
endif()

# The build-time check of the contracts' flags (cmake/IsolineContractFlags.cmake) on synthetic compile
# commands, in script mode: what it refuses, and that 5,000 entries take it under two seconds.
add_test(NAME contract-flags-selftest
         COMMAND "${CMAKE_COMMAND}" "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/contract-flags-selftest"
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/contract-flags_test.cmake")
set_tests_properties(contract-flags-selftest PROPERTIES LABELS lint TIMEOUT 120)

# The language-feature decisions (cmake/IsolineLanguageFeatures.cmake), one row per case, in script
# mode: the host's cmake runs it, so it needs no compiler and runs in every tree.
add_test(NAME language-features-selftest
         COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/language-features_test.cmake")
set_tests_properties(language-features-selftest PROPERTIES LABELS lint)
# Whether the reflected module surface's test registers, and when its absence fails the configure
# (cmake/IsolineModuleSurface.cmake), one row per case.
add_test(NAME module-surface-selftest
         COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/module-surface_test.cmake")
set_tests_properties(module-surface-selftest PROPERTIES LABELS lint)

# The header check's seal, on a fixture project configured with this build's compiler and generator: a
# target registered after the check fails the configure, naming it; registered before, it configures.
# Natively only: a cross build's compiler needs its toolchain file, and the seal is the same CMake code.
if(NOT CMAKE_CROSSCOMPILING)
    foreach(isolineFixtureOrder IN ITEMS late early)
        add_test(NAME language-features-header-check-sealed-${isolineFixtureOrder}
                 COMMAND "${CMAKE_COMMAND}"
                         -S "${CMAKE_CURRENT_SOURCE_DIR}/cmake/header-check-sealed"
                         -B "${CMAKE_CURRENT_BINARY_DIR}/header-check-sealed-${isolineFixtureOrder}"
                         -G "${CMAKE_GENERATOR}"
                         "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
                         "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
                         "-DISOLINE_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
                         -DISOLINE_FIXTURE_ORDER=${isolineFixtureOrder}
                         -DISOLINE_REFLECTION=OFF -DISOLINE_ANNOTATIONS=OFF -DISOLINE_CONTRACTS=OFF)
    endforeach()
    set(isolineSealMessage "isoline_apply_language_features[(]fixture_late[)] runs after isoline_add_public_header_check[(]fixture_headers_cxx23[)]")
    # CMake wraps a FATAL_ERROR's text at spaces.
    string(REPLACE " " "[ \n]+" isolineSealMessage "${isolineSealMessage}")
    set_tests_properties(language-features-header-check-sealed-late PROPERTIES LABELS lint
                         PASS_REGULAR_EXPRESSION "${isolineSealMessage}")
    set_tests_properties(language-features-header-check-sealed-early PROPERTIES LABELS lint
                         FAIL_REGULAR_EXPRESSION "${isolineSealMessage}")
    # The declaration-contracts proof reads the test helpers' headers too: a throwing ISOLINE_PRE in a
    # testing/ header of the fixture fails its build (where contracts are native).
    if(ISOLINE_CONTRACTS_PATH STREQUAL "native")
        add_test(NAME language-features-contracts-proof-reads-testing
                 COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
                         "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/contracts-proof-testing"
                         "-DGENERATOR=${CMAKE_GENERATOR}" "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
                         -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/contracts-proof-testing.cmake")
        set_tests_properties(language-features-contracts-proof-reads-testing PROPERTIES LABELS lint TIMEOUT 300
            PASS_REGULAR_EXPRESSION "testing/Throwing[.]hpp:[0-9]+:[0-9]+:[^\n]*required from here")
    endif()
    # isoline_add_reflection_check()'s FEATURE names a feature row, or the configure fails naming it.
    add_test(NAME language-features-reflection-check-feature
             COMMAND "${CMAKE_COMMAND}"
                     -S "${CMAKE_CURRENT_SOURCE_DIR}/cmake/reflection-check-feature"
                     -B "${CMAKE_CURRENT_BINARY_DIR}/reflection-check-feature"
                     -G "${CMAKE_GENERATOR}"
                     "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
                     "-DISOLINE_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
                     -DISOLINE_REFLECTION=OFF -DISOLINE_ANNOTATIONS=OFF -DISOLINE_CONTRACTS=OFF)
    set_tests_properties(language-features-reflection-check-feature PROPERTIES LABELS lint
                         PASS_REGULAR_EXPRESSION "no feature row[ \n]+\"annotation\"") # CMake wraps at spaces
    # Its options that exclude each other, the same fixture's other cases: each refusal names both.
    foreach(case IN ITEMS test-and-program header-sets-only-and-exclude)
        add_test(NAME language-features-reflection-check-${case}
                 COMMAND "${CMAKE_COMMAND}"
                         -S "${CMAKE_CURRENT_SOURCE_DIR}/cmake/reflection-check-feature"
                         -B "${CMAKE_CURRENT_BINARY_DIR}/reflection-check-${case}"
                         -G "${CMAKE_GENERATOR}"
                         "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
                         "-DISOLINE_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
                         "-DCASE=${case}"
                         -DISOLINE_REFLECTION=OFF -DISOLINE_ANNOTATIONS=OFF -DISOLINE_CONTRACTS=OFF)
    endforeach()
    set_tests_properties(language-features-reflection-check-test-and-program PROPERTIES LABELS lint
                         PASS_REGULAR_EXPRESSION "TEST[ \n]+and[ \n]+PROGRAM[ \n]+exclude[ \n]+each[ \n]+other")
    set_tests_properties(language-features-reflection-check-header-sets-only-and-exclude PROPERTIES LABELS lint
                         PASS_REGULAR_EXPRESSION "EXCLUDE[ \n]+filters[ \n]+the[ \n]+private[ \n]+headers")
endif()

# The contracts' two refusals, on a parent project with its own core-cpp that adds Isoline with contracts
# native: the configure scan, and the build-time check of the compile commands, which
# tests/cmake/contract-override/Run.cmake runs after a configure that passed. Each way the parent can
# override the semantic fails one of them, naming where it came from; overriding nothing passes both.
# Natively, and where contracts are native: the refusals concern the native path's flags, which only such
# a compiler has.
if(NOT CMAKE_CROSSCOMPILING AND ISOLINE_CONTRACTS_PATH STREQUAL "native")
    set(isolineContractOverrides
        "interface-option|INTERFACE_COMPILE_OPTIONS of core::base [(]fixture_core_base[)], which isoline_core links: -fcontract-evaluation-semantic=ignore"
        "transitive-option|INTERFACE_COMPILE_OPTIONS of fixture_core_detail, which isoline_core links: -fcontract-evaluation-semantic=observe"
        "nested-genex-link|INTERFACE_COMPILE_OPTIONS of fixture_core_nested, which isoline_core links: -fcontract-evaluation-semantic=ignore"
        "direct-link|INTERFACE_COMPILE_OPTIONS of fixture_core_direct, which isoline_core links: -fno-contracts"
        "config-flags|CMAKE_CXX_FLAGS_DEBUG, as isoline_core's directory sees it: -fcontract-evaluation-semantic=ignore"
        "target-option|COMPILE_OPTIONS of isoline_core: -fcontracts-definition-check=off"
        "target-flags|COMPILE_FLAGS of isoline_core: -fno-contracts"
        "source-option|COMPILE_OPTIONS of api/Extern[.]cpp in isoline_core: -fcontract-evaluation-semantic=quick_enforce"
        "parent-directory|CMAKE_CXX_FLAGS, as isoline_core's directory sees it: -fcontract-evaluation-semantic=observe"
        "c-parent|INTERFACE_COMPILE_OPTIONS of core::base [(]fixture_core_base[)], which isoline_core links: -fcontract-evaluation-semantic=ignore"
        "add-definitions|src/isoline/[A-Za-z/]+[.]cpp is compiled with -fcontract-evaluation-semantic=observe, which is refused"
        "generated-flag|src/isoline/api/Extern[.]cpp is compiled with -fcontract-evaluation-semantic=ignore, which is refused"
        "missing-response-file|missing[.]rsp, which cannot be read"
        "specs-file|src/isoline/[A-Za-z/]+[.]cpp is compiled with -fcontract-evaluation-semantic=ignore [(]from the specs file"
        "edited-response-file|configured and checked.*src/isoline/[A-Za-z/]+[.]cpp is compiled with -fcontract-evaluation-semantic=ignore [(]from the response file"
        "none|")
    foreach(override IN LISTS isolineContractOverrides)
        string(REGEX MATCH "^([^|]+)[|](.*)$" matched "${override}")
        set(overrideCase "${CMAKE_MATCH_1}")
        set(overrideMessage "${CMAKE_MATCH_2}")
        add_test(NAME contract-override-${overrideCase}
                 COMMAND "${CMAKE_COMMAND}"
                         "-DFIXTURE_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/cmake/contract-override"
                         "-DFIXTURE_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/contract-override-${overrideCase}"
                         "-DFIXTURE_GENERATOR=${CMAKE_GENERATOR}"
                         "-DFIXTURE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
                         "-DISOLINE_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
                         -DFIXTURE_OVERRIDE=${overrideCase}
                         -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/contract-override/Run.cmake")
        if(overrideMessage STREQUAL "")
            set_tests_properties(contract-override-${overrideCase} PROPERTIES LABELS lint TIMEOUT 300
                                 PASS_REGULAR_EXPRESSION "contract-override: configured and checked"
                                 FAIL_REGULAR_EXPRESSION "CMake Error")
        else()
            # CMake wraps a FATAL_ERROR's text at spaces.
            string(REPLACE " " "[ \n]+" overrideMessage "${overrideMessage}")
            set_tests_properties(contract-override-${overrideCase} PROPERTIES LABELS lint TIMEOUT 300
                                 PASS_REGULAR_EXPRESSION "${overrideMessage}")
        endif()
    endforeach()
    # This tree's own compile commands, through the same check the build runs before compiling Isoline.
    add_test(NAME contract-flags-current
             COMMAND "${CMAKE_COMMAND}" "-DCOMPILE_COMMANDS=${CMAKE_BINARY_DIR}/compile_commands.json"
                     "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
                     "-DREFUSED_FLAGS=${PROJECT_BINARY_DIR}/isoline-contract-refused-flags.txt"
                     -P "${PROJECT_SOURCE_DIR}/cmake/IsolineContractFlags.cmake")
    set_tests_properties(contract-flags-current PROPERTIES LABELS lint FAIL_REGULAR_EXPRESSION "CMake Error")
endif()

# How long the text memory suite may run before its timeout stops it: five minutes natively. Under ASan
# it takes 66 seconds on a workstation and 92 to more than 120 on a CI runner, whose speed varies by
# 1.7 times between runs; its reader cases are a third of that, 200,000 items each. An emulator (qemu,
# wine) runs it slower still, so a cross build allows ten minutes.
set(isolineMemoryTestTimeout 300)
if(CMAKE_CROSSCOMPILING_EMULATOR)
    set(isolineMemoryTestTimeout 600)
endif()

# The export lint's self-test, on its fixture cases (scripts/testdata/module-exports/): registered where the
# lint itself is (module-exports-*, cmake/IsolineModules.cmake), so a lane without the pinned LLVM's Clang
# gains no skipped test. Run by hand without --clang, it exits 77.
if(ISOLINE_MODULE_LINT_CLANG)
    add_test(NAME check-module-exports-selftest
             COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/scripts/check-module-exports_test.py"
                     --clang "${ISOLINE_MODULE_LINT_CLANG}"
                     --work-directory "${CMAKE_CURRENT_BINARY_DIR}/check-module-exports-selftest")
    # Two dozen parses of a few lines each.
    set_tests_properties(check-module-exports-selftest PROPERTIES LABELS lint TIMEOUT 300 SKIP_RETURN_CODE 77)
    # The reflected module surface's comparison (meta-module-surface.reflection), on the lint's fixture module
    # and stand-ins for the reflection program (scripts/testdata/module-surface/): on every lane with the lint,
    # reflection native or not, since the comparison is Python.
    add_test(NAME compare-module-surface-selftest
             COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/scripts/compare-module-surface_test.py"
                     --clang "${ISOLINE_MODULE_LINT_CLANG}"
                     --work-directory "${CMAKE_CURRENT_BINARY_DIR}/compare-module-surface-selftest")
    set_tests_properties(compare-module-surface-selftest PROPERTIES LABELS lint TIMEOUT 300 SKIP_RETURN_CODE 77)
endif()

# The memory bounds: what decoding a module, and instantiating it, may hold, measured by a replaced
# global operator new (AllocationCounter.cpp). The replacement is program-wide, hence a binary of its
# own. TSan's runtime defines operator new itself, not replaceably, so a TSan build leaves it out; the
# bounds hold under every other build.
if(NOT ISOLINE_SANITIZE STREQUAL "thread")
    add_executable(isoline_memory_test
                   memory/AllocationCounter.cpp memory/AmplifyingModules.cpp memory/MemoryBounds_test.cpp)
    target_link_libraries(isoline_memory_test PRIVATE isoline::core Catch2::Catch2WithMain)
    isoline_apply_target_policy(isoline_memory_test TEST)
    # The [host-memory] cases refuse allocations at every budget. MSVC's debug STL allocates a container
    # proxy in container constructors that are noexcept, where a refusal terminates (a debug-CRT dialog
    # under wine) before Engine::load or Store::instantiate can refuse it, so the Debug configuration of
    # an MSVC or clang-cl build (clangcl-cross-debug, msvc-debug) leaves them out. Every other build runs
    # them, a Windows release build included (clangcl-cross-release, under wine).
    # It measures memory bounds, so it runs under the build's memory cap (cmake/IsolineCappedTest.cmake).
    isoline_add_capped_test(NAME isoline_memory_test TARGET isoline_memory_test LABELS unit
        ARGS "$<IF:$<AND:$<BOOL:${MSVC}>,$<CONFIG:Debug>>,~[host-memory]~[branch-cost],~[branch-cost]>")
    # The [branch-cost] cases bound validation time: branches whose labels carry a thousand operands,
    # which typed per operand per branch take about 18 s on clang-release, and far longer under a sanitizer
    # or an emulator. Checked in place, as the validator does, they take seconds, which the timeout holds
    # them to. Validator_test counts their type checks, which holds in every build.
    add_test(NAME isoline_branch_cost_test COMMAND isoline_memory_test "[branch-cost]")
    set_tests_properties(isoline_branch_cost_test PROPERTIES LABELS unit TIMEOUT 120)

    # What reading text may hold, measured the same way: a binary of its own, since only it links the
    # text format, which the core memory test must not.
    add_executable(isoline_text_memory_test
                   memory/AllocationCounter.cpp memory/AmplifyingModules.cpp memory/TextMemoryBounds_test.cpp)
    # core::testing_dialogs sends a Windows CRT assert or abort to stderr instead of a modal dialog, which
    # would hold the run until its timeout.
    target_link_libraries(isoline_text_memory_test PRIVATE isoline::text Catch2::Catch2WithMain
                          core::testing_dialogs)
    isoline_apply_target_policy(isoline_text_memory_test TEST)
    add_test(NAME isoline_text_memory_test COMMAND isoline_text_memory_test)
    set_tests_properties(isoline_text_memory_test PROPERTIES LABELS unit TIMEOUT ${isolineMemoryTestTimeout})
endif()

# The isoline tool, end to end. Compiling tests/cli/hello.wat gives exactly tests/cli/hello.wasm, which
# the pinned wasm-tools made once (`wasm-tools parse tests/cli/hello.wat -o tests/cli/hello.wasm`) and
# which is canonical for this fixture; disassembling it to standard output prints its export; and the
# tool without arguments prints its usage.
add_test(NAME cli-compile
         COMMAND isoline compile "${CMAKE_CURRENT_SOURCE_DIR}/cli/hello.wat" -o "${CMAKE_CURRENT_BINARY_DIR}/hello.wasm")
set_tests_properties(cli-compile PROPERTIES LABELS unit FIXTURES_SETUP cli-hello)
add_test(NAME cli-compile-matches
         COMMAND "${CMAKE_COMMAND}" -E compare_files "${CMAKE_CURRENT_SOURCE_DIR}/cli/hello.wasm"
                 "${CMAKE_CURRENT_BINARY_DIR}/hello.wasm")
set_tests_properties(cli-compile-matches PROPERTIES LABELS unit FIXTURES_REQUIRED cli-hello)
# A path outside ASCII and outside every ANSI code page, given on the command line, names that file: on
# Windows the tool takes its command line in UTF-16 (wmain), which the cross builds check under wine.
add_test(NAME cli-compile-non-ascii
         COMMAND isoline compile "${CMAKE_CURRENT_SOURCE_DIR}/cli/hello.wat"
                 -o "${CMAKE_CURRENT_BINARY_DIR}/héllo-λ-世界.wasm")
set_tests_properties(cli-compile-non-ascii PROPERTIES LABELS unit FIXTURES_SETUP cli-non-ascii)
add_test(NAME cli-compile-non-ascii-matches
         COMMAND "${CMAKE_COMMAND}" -E compare_files "${CMAKE_CURRENT_SOURCE_DIR}/cli/hello.wasm"
                 "${CMAKE_CURRENT_BINARY_DIR}/héllo-λ-世界.wasm")
set_tests_properties(cli-compile-non-ascii-matches PROPERTIES LABELS unit FIXTURES_REQUIRED cli-non-ascii)
add_test(NAME cli-disassemble
         COMMAND isoline disassemble "${CMAKE_CURRENT_SOURCE_DIR}/cli/hello.wasm" -o -)
set_tests_properties(cli-disassemble PROPERTIES LABELS unit
                     PASS_REGULAR_EXPRESSION "\\(export \"hello\" \\(func 0\\)\\)")
add_test(NAME cli-usage COMMAND isoline)
set_tests_properties(cli-usage PROPERTIES LABELS unit
                     PASS_REGULAR_EXPRESSION "isoline: no verb given.*compile.*disassemble")
# PASS_REGULAR_EXPRESSION ignores the exit status: this checks that it is a failure (IsolineApp_test pins
# its value, UsageOrIo).
add_test(NAME cli-usage-exit COMMAND isoline)
set_tests_properties(cli-usage-exit PROPERTIES LABELS unit WILL_FAIL TRUE)
# compile refuses to write a binary to a terminal: run under a pseudo-terminal by Python's pty module,
# on a POSIX host that runs what it builds.
if(UNIX AND NOT CMAKE_CROSSCOMPILING AND Python3_Interpreter_FOUND)
    add_test(NAME cli-binary-to-terminal
             COMMAND "${Python3_EXECUTABLE}" -I -c "import pty, sys; pty.spawn(sys.argv[1:])"
                     "$<TARGET_FILE:isoline>" compile "${CMAKE_CURRENT_SOURCE_DIR}/cli/hello.wat" -o -)
    set_tests_properties(cli-binary-to-terminal PROPERTIES LABELS unit
                         PASS_REGULAR_EXPRESSION "a binary is not written to a terminal")
endif()
