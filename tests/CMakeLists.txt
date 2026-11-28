# SPDX-License-Identifier: Apache-2.0

# Suites that are not unit tests of one module.
add_subdirectory(spec)
# The differential harness: every engine on the same modules, and Wasmtime as the oracle.
add_subdirectory(differential)
add_subdirectory(encoding)
# Compiled code is byte-identical across two processes.
add_subdirectory(determinism)

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

# The api's Require() checks, which end the process, one case of the canary each: Engine's constructor
# refuses a feature set not closed over its prerequisites, the accessors of a handle whose store is gone
# that return a reference into the store refuse to read it, and Imports refuses a host function of a type
# no store could define. Each case is judged by its check's own
# text (PASS_REGULAR_EXPRESSION, which ignores the exit status), so that an abort elsewhere does not
# pass for it; the line the canary prints when the check let the call through fails it.
add_executable(isoline_require_canary canary/RequireCanary.cpp)
target_link_libraries(isoline_require_canary PRIVATE isoline::core)
isoline_apply_target_policy(isoline_require_canary TEST)
target_link_libraries(isoline_require_canary PRIVATE Catch2::Catch2) # testing/EngineRows.hpp's StringMaker
# The cases, read from the table the canary itself includes, one ISOLINE_REQUIRE_CANARY_CASE line each.
set(isolineRequireCanaryTable "${CMAKE_CURRENT_SOURCE_DIR}/canary/RequireCanaryCases.def")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${isolineRequireCanaryTable}")
file(STRINGS "${isolineRequireCanaryTable}" isolineRequireCanaryCases REGEX "^ISOLINE_REQUIRE_CANARY_CASE[(]")
foreach(canaryCase IN LISTS isolineRequireCanaryCases)
    if(NOT canaryCase MATCHES "^ISOLINE_REQUIRE_CANARY_CASE[(][A-Za-z]+, \"([a-z-]+)\", \"([^\"]+)\"[)]$")
        message(FATAL_ERROR "RequireCanaryCases.def: malformed case: ${canaryCase}")
    endif()
    set(canaryName "${CMAKE_MATCH_1}")
    set(canaryCheck "${CMAKE_MATCH_2}")
    add_test(NAME isoline_require_canary.${canaryName} COMMAND isoline_require_canary ${canaryName})
    set_tests_properties(isoline_require_canary.${canaryName} PROPERTIES LABELS unit TIMEOUT 60
        PASS_REGULAR_EXPRESSION "Precondition failed[.] ${canaryCheck}"
        FAIL_REGULAR_EXPRESSION "the check let the call through")
endforeach()

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
    # The x86-64 encoding oracle's checker, against a fake llvm-mc: every way a disassembly must fail it.
    add_test(NAME check-encodings-selftest
             COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/scripts/check-encodings_test.py")
    set_tests_properties(check-encodings-selftest PROPERTIES LABELS lint)
    # The differential corpus generator's entropy, smith flags and validation, with a stand-in wasm-tools.
    add_test(NAME make-differential-corpus-selftest
             COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/scripts/make-differential-corpus_test.py")
    set_tests_properties(make-differential-corpus-selftest PROPERTIES LABELS lint)
endif()

# The compiler's translation handles every instruction of the features it compiles: no core::todo is
# left under it (MemoryTranslation.everyCompiledOpcodeTranslates runs each of them).
add_test(NAME compiler-translation-no-todo
         COMMAND "${CMAKE_COMMAND}" "-DROOT=${PROJECT_SOURCE_DIR}/src/isoline/compiler/translation"
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckNoTodo.cmake")
set_tests_properties(compiler-translation-no-todo PROPERTIES LABELS lint)

# How long the text memory suite may run before its timeout stops it: five minutes natively. Under ASan
# it takes 66 seconds on a workstation and 92 to more than 120 on a CI runner, whose speed varies by
# 1.7 times between runs; its reader cases are a third of that, 200,000 items each. An emulator (qemu,
# wine) runs it slower still, so a cross build allows ten minutes.
set(isolineMemoryTestTimeout 300)
if(CMAKE_CROSSCOMPILING_EMULATOR)
    set(isolineMemoryTestTimeout 600)
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

# The host-stack rows: unit cases run again under a small main-thread stack.
include("${CMAKE_CURRENT_SOURCE_DIR}/stack/StackLimit.cmake")

# The contract checks of the unit tests: each case tagged [.contract] in isoline_test violates one
# Require on purpose, which ends its process, so the unit run hides it and it runs here, alone in a
# process. Their names start with "Contract.", so no wildcard over an area ("Instantiate.*") reaches
# them. A row passes on the Require's own text, so ending for any other reason fails it, as does a
# case that survives (its FAIL names SURVIVED). The row is the case, then the Require's condition as
# a regular expression.
set(isolineContractRows
    "Contract.Instantiate.codeMustMatchTheExecutor|engineOf\\(code\\.get\\(\\)\\) == executor\\.engine\\(\\)"
    "Contract.Instantiate.nullCodeNeedsTheReference|engineOf\\(code\\.get\\(\\)\\) == executor\\.engine\\(\\)"
    "Contract.Instantiate.referenceCodeIsNull|code == nullptr \\|\\| code->engine\\(\\) != ExecutionEngine::Reference"
    "Contract.PollGate.observedPollNeedsTheStoresCountdown|stepsLeft == reload\\(\\)"
    "Contract.PollGate.commitWithinTheBudget|stepsLeft <= _maxSteps"
    "Contract.PollGate.lendOnlyWhenSettled|_store->_budgetState == BudgetState::Settled"
    "Contract.PollGate.settleOnlyALease|_store->_budgetState == BudgetState::Lent"
    "Contract.PollGate.settleNoMoreStepsThanLent|left\\.steps <= lent\\.steps"
    "Contract.PollGate.settleWithinTheStackBudget|left\\.stackValues <= _maxStackValues"
    "Contract.SlotCodec.storeIntoItsWidth|slots\\.size\\(\\) == width"
    "Contract.SlotCodec.loadFromItsWidth|slots\\.size\\(\\) == width"
    "Contract.SlotCodec.nullHeapIsTheInstances|typeIndex < instanceTypes\\.size\\(\\)"
    "Contract.Code.everyContainerUsesTheArena|allocateFrom\\(_arena\\.get\\(\\), _functions\\)"
    "Contract.CountingArena.releaseOnlyWhatIsReserved|bytes <= _reserved"
    "Contract.HostCall.onlyHostFunctions|function\\.host != nullptr"
    "Contract.HostCall.argumentsAsWideAsTheParams|args\\.size\\(\\) == type\\.params\\.size\\(\\)"
    "Contract.HostCall.resultsAsWideAsTheResults|results\\.size\\(\\) == type\\.results\\.size\\(\\)"
    "Contract.RawValue.globalAssignsItsOwnKind|kindOf\\(value\\) == type\\.type\\.kind"
    "Contract.Store.addGlobalOfItsOwnKind|kindOf\\(value\\) == type\\.type\\.kind"
    "Contract.Store.grownMemoryKeepsItsTail|keepsItsTail\\(\\*_memory\\)"
    "Contract.Lowering.dispatchMustBeAvailable|isAvailable\\(config\\.dispatch\\)"
    # IPollObserver: one row per row of runtime::ObserverRefusedEntryPoints.
    "Contract.Observer.growMemory|_pollObservation == PollObservation::Idle"
    "Contract.Observer.growTable|_pollObservation == PollObservation::Idle"
    "Contract.Observer.internType|_pollObservation == PollObservation::Idle"
    "Contract.Observer.addHostFunction|_pollObservation == PollObservation::Idle"
    "Contract.Observer.addModuleFunction|_pollObservation == PollObservation::Idle"
    "Contract.Observer.addTable|_pollObservation == PollObservation::Idle"
    "Contract.Observer.addMemory|_pollObservation == PollObservation::Idle"
    "Contract.Observer.addGlobal|_pollObservation == PollObservation::Idle"
    "Contract.Observer.addTag|_pollObservation == PollObservation::Idle"
    "Contract.Observer.addException|_pollObservation == PollObservation::Idle"
    "Contract.Observer.addModuleInstance|_pollObservation == PollObservation::Idle"
    "Contract.Observer.releaseTrailing|_pollObservation == PollObservation::Idle"
    "Contract.Observer.poll|_pollObservation == PollObservation::Idle"
    "Contract.Observer.chargeFrame|_pollObservation == PollObservation::Idle"
    "Contract.Observer.releaseFrame|_pollObservation == PollObservation::Idle"
    "Contract.Observer.bindDispatch|_pollObservation == PollObservation::Idle"
    "Contract.Observer.invocation|_pollObservation == PollObservation::Idle"
    "Contract.Observer.setGlobal|_pollObservation == PollObservation::Idle"
    "Contract.Observer.tableSet|_pollObservation == PollObservation::Idle"
    "Contract.Observer.memoryRange|_pollObservation == PollObservation::Idle"
    "Contract.Observer.instanceCall|_pollObservation == PollObservation::Idle"
    "Contract.Observer.instanceMemoryRange|_pollObservation == PollObservation::Idle"
    "Contract.Store.lentBudgetForbidsStorePolls|_budgetState == BudgetState::Settled"
    "Contract.Store.lentBudgetForbidsChargingFrames|_budgetState == BudgetState::Settled"
    "Contract.Store.lentBudgetForbidsReleasingFrames|_budgetState == BudgetState::Settled"
    "Contract.Store.invocationEndsSettled|_store->_budgetState == BudgetState::Settled"
    "Contract.Store.bindOnlyModuleFunctions|!_functions\\[address\\]\\.isHost\\(\\)"
    "Contract.Store.bindOnlyExistingFunctions|address < _dispatchView\\.rows"
    "Contract.Store.bindOnlyTheInstancesOwnFunctions|_functions\\[address\\]\\.moduleInstance == moduleInstance"
    "Contract.Store.bindOnlyWhereRowsAreKept|_dispatchRows == DispatchRows::Kept"
    "Contract.Store.refusedGrowLeavesTheMemory|_view\\.bytes == before\\.bytes && \\(before\\.bytes == 0 \\|\\| _view\\.base == before\\.base\\)"
    "Contract.Store.grantedGrowReachesItsSize|_view\\.bytes == newBytes"
    "Contract.Listing.wholeOperations|row\\.words <= code\\.size\\(\\)"
    "Contract.FastInterpreter.dispatchMustBeAvailable|isAvailable\\(dispatch\\)"
    "Contract.Engine.fastDispatchMustBeAvailable|engines::rowOf\\(_config\\.engine\\)->dispatch == engines::DispatchUse::None \\|\\| fastinterpreter::isAvailable\\(_config\\.dispatch\\)"
    "Contract.Engine.knownEngine|engines::rowOf\\(_config\\.engine\\) != nullptr"
    "Contract.Function.moveAssignmentStaysInItsArena|_memory == other\\._memory"
    "Contract.Registers.registerRowOfARegister|reg\\.number < RegistersPerFile"
    "Contract.Function.removeBlocksKeepsTheEntry|index != 0 && index < _blocks\\.size\\(\\) && \\(!previous \\|\\| \\*previous < index\\)"
    "Contract.Function.removeBlocksLeavesNoEdgeIntoThem|target != Unnumbered"
    "Contract.Rewrite.substitutionRefusesACycle|resolve\\(to\\) != from"
    "Contract.Rewrite.substitutionReplacesOnce|index < _next\\.size\\(\\) && _next\\[index\\] == from"
    "Contract.Rewrite.dropsNoParameterOfTheEntry|!isDropped\\(value\\)"
    # Removed in P5 Task 16, which gives the compiled engine its row of api/ExecutionEngines.cpp.
    "Contract.Engine.refusesTheCompiledEngine|engines::rowOf\\(_config\\.engine\\) != nullptr")
set(isolineContractCases)
foreach(contract IN LISTS isolineContractRows)
    string(FIND "${contract}" "|" separator)
    string(SUBSTRING "${contract}" 0 ${separator} contractCase)
    math(EXPR textBegin "${separator} + 1")
    string(SUBSTRING "${contract}" ${textBegin} -1 contractText)
    list(APPEND isolineContractCases "${contractCase}")
    # The target's name, not its file: only then does CMake run it through CMAKE_CROSSCOMPILING_EMULATOR
    # (qemu, wine) where the tests are cross-compiled.
    add_test(NAME ${contractCase} COMMAND isoline_test "${contractCase}")
    set_tests_properties(${contractCase} PROPERTIES
                         LABELS unit
                         PASS_REGULAR_EXPRESSION "Precondition failed\\. ${contractText}"
                         FAIL_REGULAR_EXPRESSION "SURVIVED")
endforeach()
# Every [.contract] case of isoline_test has a row above, and every row a case: a case without a row
# would never run.
string(REPLACE ";" "," isolineContractCaseList "${isolineContractCases}")
# A cross build's emulator runs the binary there too; a list, passed with | between its items.
string(REPLACE ";" "|" isolineTestEmulator "${CMAKE_CROSSCOMPILING_EMULATOR}")
add_test(NAME contract-rows-complete
         COMMAND ${CMAKE_COMMAND} -DTEST_BINARY=$<TARGET_FILE:isoline_test> "-DTEST_EMULATOR=${isolineTestEmulator}"
                 "-DEXPECTED=${isolineContractCaseList}" -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckContractRows.cmake")
set_tests_properties(contract-rows-complete PROPERTIES LABELS unit)

# The memory-bound cases of the unit tests: each case tagged [.hostile] in isoline_test builds a hostile
# shape that a broken cap would let grow without bound, so the unit run hides them and they run here,
# capped. A sanitizer build caps its resident memory through ASan; any other build its address space
# through prlimit where the host has it; an emulated cross build (wine, qemu) runs them uncapped, every
# case also drawing on a counting arena of a small budget.
if(ISOLINE_SANITIZE STREQUAL "address")
    add_test(NAME isoline_hostile COMMAND isoline_test "[.hostile]")
    set_tests_properties(isoline_hostile PROPERTIES
                         ENVIRONMENT "ASAN_OPTIONS=hard_rss_limit_mb=4096:allocator_may_return_null=1")
else()
    find_program(ISOLINE_PRLIMIT prlimit)
    if(ISOLINE_PRLIMIT AND NOT CMAKE_CROSSCOMPILING AND NOT ISOLINE_SANITIZE STREQUAL "thread")
        add_test(NAME isoline_hostile
                 COMMAND "${ISOLINE_PRLIMIT}" --as=6442450944 -- $<TARGET_FILE:isoline_test> "[.hostile]")
    else()
        add_test(NAME isoline_hostile COMMAND isoline_test "[.hostile]")
    endif()
endif()
set_tests_properties(isoline_hostile PROPERTIES LABELS unit TIMEOUT 300)

# The time-bound cases of the unit tests: each case tagged [.timing] in isoline_test pins a ratio of
# times on a shape that a quadratic pass would blow up, so it runs alone, serially, with a generous
# timeout.
add_test(NAME isoline_timing COMMAND isoline_test "[.timing]")
set_tests_properties(isoline_timing PROPERTIES LABELS unit TIMEOUT 900 RUN_SERIAL TRUE)
