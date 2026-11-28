# SPDX-License-Identifier: Apache-2.0

# The project version, as constants Version.hpp reads.
configure_file(VersionConfig.hpp.in "${PROJECT_BINARY_DIR}/include/isoline/VersionConfig.hpp" @ONLY)

add_library(isoline_core STATIC)

# The target this build's code runs on, chosen here from the compiler's target architecture (its
# predefined macros, so cross and Visual Studio -A builds choose right), so that no source selects
# with #ifdef: the floating-point control register's row (runtime/FloatEnvironment.hpp), exactly one
# per build, and the generated isoline/HostTarget.hpp, which names the architecture and the operating
# system ABI as the compiler describes them (compiler/machine/TargetDescription.hpp). MSVC has no
# inline assembly for arm64's FPCR and defines no __aarch64__, so it takes the portable float row
# there; so does a build for several architectures at once, where one object serves them all and no
# one architecture is the host's. ARM64EC runs x86-64 code under an arm64 ABI no row describes.
include(CheckCXXSymbolExists)
list(LENGTH CMAKE_OSX_ARCHITECTURES isolineOsxArchitectureCount)
set(isolineHostArchitecture "")
if(isolineOsxArchitectureCount GREATER 1)
    message(STATUS "[isoline] several target architectures (${CMAKE_OSX_ARCHITECTURES}): portable float control row")
    set(isolineFloatControl runtime/FloatControlPortable.cpp)
else()
    check_cxx_symbol_exists(__x86_64__ "" ISOLINE_TARGET_X86_64_GNU)
    check_cxx_symbol_exists(_M_X64 "" ISOLINE_TARGET_X86_64_MSVC)
    check_cxx_symbol_exists(_M_ARM64EC "" ISOLINE_TARGET_ARM64EC)
    check_cxx_symbol_exists(__aarch64__ "" ISOLINE_TARGET_ARM64_GNU)
    check_cxx_symbol_exists(_M_ARM64 "" ISOLINE_TARGET_ARM64_MSVC)
    if((ISOLINE_TARGET_X86_64_GNU OR ISOLINE_TARGET_X86_64_MSVC) AND NOT ISOLINE_TARGET_ARM64EC)
        set(isolineFloatControl runtime/FloatControlX86_64.cpp)
        set(isolineHostArchitecture X64)
    elseif(ISOLINE_TARGET_ARM64_GNU)
        set(isolineFloatControl runtime/FloatControlArm64.cpp)
        set(isolineHostArchitecture Arm64)
    elseif(ISOLINE_TARGET_ARM64_MSVC AND NOT ISOLINE_TARGET_ARM64EC)
        set(isolineFloatControl runtime/FloatControlPortable.cpp)
        set(isolineHostArchitecture Arm64)
    else()
        set(isolineFloatControl runtime/FloatControlPortable.cpp)
    endif()
endif()
check_cxx_symbol_exists(_WIN32 "" ISOLINE_TARGET_WINDOWS)
check_cxx_symbol_exists(__APPLE__ "" ISOLINE_TARGET_APPLE)
if(ISOLINE_TARGET_WINDOWS)
    set(isolineHostAbi Windows)
elseif(ISOLINE_TARGET_APPLE)
    set(isolineHostAbi Apple)
else()
    set(isolineHostAbi SystemV)
endif()
if(isolineHostArchitecture)
    set(ISOLINE_HOST_ARCHITECTURE "compiler::machine::Architecture::${isolineHostArchitecture}")
else()
    set(ISOLINE_HOST_ARCHITECTURE "std::nullopt")
endif()
set(ISOLINE_HOST_OPERATING_SYSTEM_ABI "compiler::machine::OperatingSystemAbi::${isolineHostAbi}")
configure_file(HostTarget.hpp.in "${PROJECT_BINARY_DIR}/include/isoline/HostTarget.hpp" @ONLY)
message(STATUS "[isoline] float control row: ${isolineFloatControl}")
message(STATUS "[isoline] host target: ${ISOLINE_HOST_ARCHITECTURE}, ${ISOLINE_HOST_OPERATING_SYSTEM_ABI}")
add_library(isoline::core ALIAS isoline_core)

# The host-stack rows, one per build flavour: the bytes one nested invocation takes at the most on the
# reference and on the fast engine (runtime/NestedInvocationBytes.hpp, generated from the row), per target
# architecture, and the stack of the tests' small threads (runtime/testing/SmallStackThread.hpp's
# SmallStackBytes). The engines' bytes are the most the hidden case Calls.measureNestingStack measured on the
# flavour's builds for that architecture (docs/fast-interpreter.md lists each), rounded up by half again to a
# multiple of 256; Calls.nestingStackCostIsPublished checks every build against its row. An architecture's
# columns repeat x86-64's until a build of the flavour has measured it there. A build for several
# architectures at once (no one host architecture) takes the larger of each column. The small thread holds
# maxNestedInvocations (100) nestings of the larger engine twice over, rounded up to a MiB; a release build's
# is 512 KiB, where the dispatch files' frames are bounded (-Wframe-larger-than=4096,
# fastinterpreter/CMakeLists.txt), and holds 100 nestings of every release row once. A row is the flavour,
# then x86-64's reference bytes, fast engine bytes and small thread, then arm64's.
set(isolineNestingRows
    "release|2304|3072|524288|4352|4608|524288"
    "debug|13056|12544|3145728|13056|12544|3145728"
    "address|12288|11264|3145728|15360|23808|5242880"
    "thread|10752|9728|3145728|10752|9728|3145728"
    "coverage|7424|7168|2097152|7424|7168|2097152")
# The flavour: an instrumented build's (address, thread, coverage) first, then an optimized single-config GCC
# or Clang build's (release); every other is debug, a multi-config generator's included, which may build
# unoptimized, and MSVC's, whose frames no option bounds.
if(ISOLINE_SANITIZE)
    set(isolineNestingFlavour "${ISOLINE_SANITIZE}")
elseif(ISOLINE_COVERAGE)
    set(isolineNestingFlavour coverage)
elseif(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$"
       AND (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU"
            OR (NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")))
    set(isolineNestingFlavour release)
else()
    set(isolineNestingFlavour debug)
endif()
foreach(row IN LISTS isolineNestingRows)
    string(REPLACE "|" ";" fields "${row}")
    list(GET fields 0 flavour)
    if(flavour STREQUAL isolineNestingFlavour)
        set(ISOLINE_NESTING_FLAVOUR "${flavour}")
        # The columns of each architecture, in the row's order: reference, fast engine, small thread.
        set(isolineNestingColumns ISOLINE_NESTING_REFERENCE_BYTES ISOLINE_NESTING_FAST_BYTES
                                  isolineSmallStackBytes)
        foreach(column RANGE 2)
            list(GET isolineNestingColumns ${column} variable)
            math(EXPR x64Field "1 + ${column}")
            math(EXPR arm64Field "4 + ${column}")
            list(GET fields ${x64Field} x64Bytes)
            list(GET fields ${arm64Field} arm64Bytes)
            if(isolineHostArchitecture STREQUAL "X64")
                set(${variable} ${x64Bytes})
            elseif(isolineHostArchitecture STREQUAL "Arm64")
                set(${variable} ${arm64Bytes})
            elseif(arm64Bytes GREATER x64Bytes)
                set(${variable} ${arm64Bytes})
            else()
                set(${variable} ${x64Bytes})
            endif()
        endforeach()
    endif()
endforeach()
if(NOT ISOLINE_NESTING_FLAVOUR)
    message(FATAL_ERROR "[isoline] no host-stack row for the build flavour '${isolineNestingFlavour}'")
endif()
message(STATUS "[isoline] host-stack row: ${ISOLINE_NESTING_FLAVOUR}, ${isolineHostArchitecture}")
configure_file(runtime/NestedInvocationBytes.hpp.in
               "${PROJECT_BINARY_DIR}/include/isoline/runtime/NestedInvocationBytes.hpp" @ONLY)
target_sources(isoline_core
    PRIVATE
        Version.cpp
        api/Engine.cpp
        api/ExecutionEngines.cpp
        api/Extern.cpp
        api/Imports.cpp
        api/Module.cpp
        api/Store.cpp
        binary/BranchHints.cpp
        binary/ByteReader.cpp
        binary/Decoder.cpp
        binary/Encoder.cpp
        binary/NameSection.cpp
        binary/Opcode.cpp
        binary/TypeCanon.cpp
        interpreter/Numerics.cpp
        interpreter/ReferenceInterpreter.cpp
        interpreter/Simd.cpp
        ${isolineFloatControl}
        runtime/ConstantExpression.cpp
        runtime/CountingArena.cpp
        runtime/FloatEnvironment.cpp
        runtime/HostCall.cpp
        runtime/HostFunction.cpp
        runtime/Instantiate.cpp
        runtime/LinearMemory.cpp
        runtime/Store.cpp
        runtime/ValueCheck.cpp
        runtime/ValueWords.cpp
        validate/Subtyping.cpp
        validate/ValidatedModule.cpp
        validate/Validator.cpp
    PUBLIC FILE_SET HEADERS
        BASE_DIRS "${PROJECT_SOURCE_DIR}/src" "${PROJECT_BINARY_DIR}/include"
        FILES
            Features.hpp
            api/CallError.hpp
            api/Engine.hpp
            api/EngineIdentity.hpp
            api/ExecutionEngines.hpp
            api/Extern.hpp
            api/Imports.hpp
            api/Module.hpp
            api/Store.hpp
            api/Types.hpp
            binary/BodyUnits.hpp
            binary/BranchHints.hpp
            binary/ByteReader.hpp
            binary/DecodeError.hpp
            binary/DecodeLimits.hpp
            binary/MemoryAccess.hpp
            binary/Decoder.hpp
            binary/Encoder.hpp
            binary/Module.hpp
            binary/NameSection.hpp
            binary/Opcode.hpp
            binary/Opcodes.def
            binary/TypeCanon.hpp
            binary/Types.hpp
            interpreter/Numerics.hpp
            interpreter/ReferenceInterpreter.hpp
            interpreter/Simd.hpp
            runtime/Abrupt.hpp
            runtime/ConstantExpression.hpp
            runtime/CountingArena.hpp
            runtime/ExecutionMode.hpp
            runtime/FloatEnvironment.hpp
            runtime/HostCall.hpp
            runtime/HostFunction.hpp
            runtime/Instantiate.hpp
            runtime/Interrupt.hpp
            runtime/Limits.hpp
            runtime/LinearMemory.hpp
            runtime/ModuleCode.hpp
            runtime/PollGate.hpp
            runtime/RawValue.hpp
            runtime/Segment.hpp
            runtime/Store.hpp
            runtime/Trap.hpp
            runtime/Value.hpp
            runtime/ValueCheck.hpp
            runtime/ValueWords.hpp
            validate/Subtyping.hpp
            validate/ValidatedModule.hpp
            validate/ValidationError.hpp
            validate/Validator.hpp
            Version.hpp
            "${PROJECT_BINARY_DIR}/include/isoline/HostTarget.hpp"
            "${PROJECT_BINARY_DIR}/include/isoline/VersionConfig.hpp"
            "${PROJECT_BINARY_DIR}/include/isoline/runtime/NestedInvocationBytes.hpp")
target_compile_features(isoline_core PUBLIC cxx_std_23)
target_link_libraries(isoline_core PUBLIC core::base)
isoline_apply_target_policy(isoline_core)

add_subdirectory(numerics)
add_subdirectory(compiler)
add_subdirectory(execution)
add_subdirectory(fastinterpreter)
add_subdirectory(text)
add_subdirectory(cli)

if(ISOLINE_TESTING)
    # Every *_test.cpp under src/isoline/ is one translation unit of the single unit-test binary,
    # except in the directories whose tests link a target of their own and build their own binary.
    # Those under a posix/ directory test what only POSIX systems have (signal dispositions), and are
    # added on those alone, so that no test source checks for the platform.
    set(isolineSeparateTestDirectories text cli)
    file(GLOB_RECURSE isolineTestSources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
    foreach(directory IN LISTS isolineSeparateTestDirectories)
        list(FILTER isolineTestSources EXCLUDE REGEX "/${directory}/[^/]*_test\\.cpp$")
    endforeach()
    list(FILTER isolineTestSources EXCLUDE REGEX "/posix/[^/]*_test\\.cpp$")
    if(UNIX)
        file(GLOB_RECURSE isolinePosixTestSources CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/*/posix/*_test.cpp")
        list(APPEND isolineTestSources ${isolinePosixTestSources})
    endif()
    # A separate directory's posix/ tests are its own binary's too.
    foreach(directory IN LISTS isolineSeparateTestDirectories)
        list(FILTER isolineTestSources EXCLUDE REGEX "/${directory}/posix/[^/]*_test\\.cpp$")
    endforeach()
    # The platform's half of runtime/testing/SmallStackThread.hpp, one row per platform.
    if(WIN32)
        list(APPEND isolineTestSources runtime/testing/SmallStackThreadWindows.cpp)
    else()
        list(APPEND isolineTestSources runtime/testing/SmallStackThreadPosix.cpp)
    endif()
    # The compiler's half of runtime/testing/StackAddress.hpp: GCC and Clang's frame-address builtin, or MSVC's
    # (and clang-cl's) return-address intrinsic.
    if(MSVC)
        list(APPEND isolineTestSources runtime/testing/StackAddressMsvc.cpp)
    else()
        list(APPEND isolineTestSources runtime/testing/StackAddressGnu.cpp)
    endif()
    # The compiled-code harness (execution/testing/CompileOne.hpp), the stack painting's calibration
    # (execution/testing/StackCalibration.hpp, a translation unit of its own) and the platform's half of the page
    # protection the code memory tests read (execution/testing/PageProtection.hpp), one row per platform.
    list(APPEND isolineTestSources execution/testing/CompileOne.cpp execution/testing/StackCalibration.cpp)
    if(ISOLINE_TARGET_WINDOWS)
        list(APPEND isolineTestSources execution/testing/PageProtectionWindows.cpp)
        set_source_files_properties(execution/testing/PageProtectionWindows.cpp
            PROPERTIES COMPILE_DEFINITIONS "NOMINMAX;WIN32_LEAN_AND_MEAN")
    elseif(ISOLINE_TARGET_APPLE)
        list(APPEND isolineTestSources execution/testing/PageProtectionMacOS.cpp)
    else()
        list(APPEND isolineTestSources execution/testing/PageProtectionLinux.cpp)
    endif()
    # The encoding oracle's samples (compiler/x86_64/EncodingSamples.hpp): test support, built into the unit
    # tests and tests/encoding's isoline_encoding_samples, never into the library.
    list(APPEND isolineTestSources compiler/x86_64/EncodingSamples.cpp)
    add_executable(isoline_test ${isolineTestSources})
    # The text format, which the compiled-code suites build their modules from (execution/testing/CompileOne.hpp).
    target_link_libraries(isoline_test PRIVATE isoline::core isoline::text Catch2::Catch2WithMain)
    # The stack of the tests' small threads (SmallStackBytes): the host-stack row's, above.
    target_compile_definitions(isoline_test PRIVATE ISOLINE_SMALL_STACK_BYTES=${isolineSmallStackBytes})
    if(NOT WIN32)
        find_package(Threads REQUIRED)
        target_link_libraries(isoline_test PRIVATE Threads::Threads)
    endif()
    # The source tree, from which tests derive the paths of their data: tests/spec, and docs/embedding.md,
    # whose generated blocks PublishedLimits_test checks against the tables. The operating system the tests
    # run on, by CMake's name for it: what HostTarget_test checks the generated host ABI, which the
    # compiler's predefined macros chose, against.
    target_compile_definitions(isoline_test PRIVATE ISOLINE_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
                                                   ISOLINE_TEST_SYSTEM_NAME="${CMAKE_SYSTEM_NAME}")
    isoline_apply_target_policy(isoline_test TEST)
    # The unit run, one test per area: an area is the part of a case's name before its first dot, and
    # isoline_test.<area> runs every case of it ("<area>.*"), so that the areas run in parallel and each has a
    # bound of its own. A row is the area, then its TIMEOUT in seconds when it is not the default, a bound
    # that turns a hang (an interrupt test whose poll went missing) into a failure; ISOLINE_TEST_TIMEOUT_FACTOR
    # multiplies each under an emulator. isoline_test.areas checks that every case belongs to a row's area.
    # The hidden cases run apart: the [.contract] ones in tests/CMakeLists.txt, the [perf] ones below. So do
    # the [memory] cases (growth past a store's budgets, hostile deltas), under a hard memory cap, so that a
    # broken bound fails small instead of starving a shared machine.
    set(isolineTestDefaultTimeout 120)
    set(isolineTestAreas
        "BodyUnits"
        "BoundsChecks|300"
        "BranchHints"
        "Bulk"
        "ByteReader"
        "CallingConvention"
        "Calls|300"
        "Code"
        "CodeBuffer"
        "CodeMemory"
        "CompileCancellation"
        "CompileConfig"
        "CompileError"
        "Compiler|300"
        "ConstantExpression"
        "ContextLayout"
        "Contract|300"
        "CountingArena"
        "CpuFeatureProbe"
        "CpuFeatures"
        "DecodeError"
        "Decoder"
        "Dispatch"
        "Dominators|300"
        "Encoder|300"
        "EncodingSamples"
        "Engine"
        "Exceptions"
        "Externs"
        "FastInterpreter"
        "FeatureSupport"
        "Features"
        "FloatEnvironment"
        "Function"
        "Guards"
        "HeapMemoryAllocator"
        "HostCall"
        "HostFunction"
        "HostTarget"
        "Imports"
        "Instantiate"
        "Interpreter|300"
        "Interrupt"
        "Interrupter"
        "Limits"
        "LinearMemory"
        "LinearScan|300"
        "Linearize|300"
        "LittleEndian"
        "Liveness|300"
        "LoopAssignments|300"
        "Lowering|600"
        "LoweringRows"
        "MachineFunction"
        "MachineVerifier"
        "Memory"
        "MemoryAccess"
        "MemoryTranslation|300"
        "Module"
        "ModuleEquality"
        "NameSection"
        "Numerics"
        "Observer"
        "Opcode"
        "OperationTable"
        "Optimization|300"
        "ParallelMoves"
        "PollGate"
        "ProvidedMemoryAllocator"
        "PublishedLimits"
        "RawValue"
        "Readme"
        "ReallocatingMemoryAllocator"
        "Registers"
        "ReferenceInterpreter|300"
        "Relaxed"
        "Rewrite"
        "RuntimeStubs"
        "Segment"
        "Selector|600"
        "Simd|300"
        "SimdKernels"
        "SlotCodec"
        "SpillSlots"
        "StackBounds"
        "Store"
        "Subtyping"
        "Tables"
        "Target|1200"
        "TargetDescription"
        "Threads"
        "Translator|300"
        "Trampoline"
        "Trap"
        "TypeCanon"
        "Types"
        "ValidatedModule"
        "Validator"
        "Value"
        "ValueCheck"
        "Verifier|300"
        "Version"
        "Views")
    set(isolineTestAreaNames)
    foreach(row IN LISTS isolineTestAreas)
        if(row MATCHES "^([A-Za-z]+)(\\|([0-9]+))?$")
            set(area "${CMAKE_MATCH_1}")
            set(timeout "${CMAKE_MATCH_3}")
        else()
            message(FATAL_ERROR "isolineTestAreas: expected '<area>' or '<area>|<seconds>', got '${row}'")
        endif()
        if(NOT timeout)
            set(timeout ${isolineTestDefaultTimeout})
        endif()
        math(EXPR timeout "${timeout} * ${ISOLINE_TEST_TIMEOUT_FACTOR}")
        list(APPEND isolineTestAreaNames "${area}")
        # The tags first: Catch2 reads a name pattern to the end of the argument. A name pattern selects
        # hidden cases too, which ~[.] leaves to their own tests.
        add_test(NAME isoline_test.${area} COMMAND isoline_test "~[.] ~[memory] ${area}.*")
        set_tests_properties(isoline_test.${area} PROPERTIES LABELS unit TIMEOUT ${timeout})
    endforeach()
    # A cross build's emulator runs the binary there too; a list, passed with | between its items.
    string(REPLACE ";" "|" isolineAreaEmulator "${CMAKE_CROSSCOMPILING_EMULATOR}")
    string(REPLACE ";" "," isolineTestAreaList "${isolineTestAreaNames}")
    add_test(NAME isoline_test.areas
             COMMAND ${CMAKE_COMMAND} -DTEST_BINARY=$<TARGET_FILE:isoline_test>
                     "-DTEST_EMULATOR=${isolineAreaEmulator}" "-DSELECTION=~[memory]"
                     "-DAREAS=${isolineTestAreaList}" -P "${PROJECT_SOURCE_DIR}/tests/CheckTestAreas.cmake")
    set_tests_properties(isoline_test.areas PROPERTIES LABELS unit)
    isoline_add_capped_test(NAME isoline_test_memory TARGET isoline_test LABELS unit ARGS "[memory]")
    # The interrupt latency the specification publishes (p99 interrupt-to-return within 10 ms) is a
    # property of optimized code: measured in release builds only, by the hidden [perf] cases, which
    # the unit run above skips. It asserts wall-clock time, so every test preset but clang-release-perf
    # excludes the perf label (CMakePresets.json): a plain `ctest --preset` on a loaded box, or under an
    # emulator (wine, qemu), does not run it.
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_test(NAME isoline_interrupt_latency COMMAND isoline_test "[perf]")
        set_tests_properties(isoline_interrupt_latency PROPERTIES LABELS perf TIMEOUT 300)
    endif()
endif()
