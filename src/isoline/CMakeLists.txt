# SPDX-License-Identifier: Apache-2.0

# The project version, as constants Version.hpp reads.
configure_file(VersionConfig.hpp.in "${PROJECT_BINARY_DIR}/include/isoline/VersionConfig.hpp" @ONLY)

add_library(isoline_core STATIC)

# The floating-point control register's row of the target (runtime/FloatEnvironment.hpp): exactly one
# per build, chosen here from the compiler's target architecture (its predefined macros, so cross and
# Visual Studio -A builds choose right), so that no source selects it with #ifdef. MSVC has no inline
# assembly for arm64's FPCR and defines no __aarch64__, so it takes the portable row there; so does a
# build for several architectures at once, where one object serves them all.
include(CheckCXXSymbolExists)
list(LENGTH CMAKE_OSX_ARCHITECTURES isolineOsxArchitectureCount)
if(isolineOsxArchitectureCount GREATER 1)
    message(STATUS "[isoline] several target architectures (${CMAKE_OSX_ARCHITECTURES}): portable float control row")
    set(isolineFloatControl runtime/FloatControlPortable.cpp)
else()
    check_cxx_symbol_exists(__x86_64__ "" ISOLINE_TARGET_X86_64_GNU)
    check_cxx_symbol_exists(_M_X64 "" ISOLINE_TARGET_X86_64_MSVC)
    check_cxx_symbol_exists(_M_ARM64EC "" ISOLINE_TARGET_ARM64EC)
    check_cxx_symbol_exists(__aarch64__ "" ISOLINE_TARGET_ARM64_GNU)
    if((ISOLINE_TARGET_X86_64_GNU OR ISOLINE_TARGET_X86_64_MSVC) AND NOT ISOLINE_TARGET_ARM64EC)
        set(isolineFloatControl runtime/FloatControlX86_64.cpp)
    elseif(ISOLINE_TARGET_ARM64_GNU)
        set(isolineFloatControl runtime/FloatControlArm64.cpp)
    else()
        set(isolineFloatControl runtime/FloatControlPortable.cpp)
    endif()
endif()
message(STATUS "[isoline] float control row: ${isolineFloatControl}")
add_library(isoline::core ALIAS isoline_core)

# The host-stack rows, one per build flavour: the bytes one nested invocation takes at the most on the
# reference and on the fast engine (runtime/NestedInvocationBytes.hpp, generated from the row), and the stack
# of the tests' small threads (runtime/testing/SmallStackThread.hpp's SmallStackBytes). The engines' bytes are
# the most the hidden case Calls.measureNestingStack measured on the flavour's builds (docs/fast-interpreter.md
# lists each), rounded up by half again to a multiple of 256; Calls.nestingStackCostIsPublished checks every
# build against its row. The small thread holds maxNestedInvocations (100) nestings of the larger engine twice
# over, rounded up to a MiB; a release build's is 512 KiB, where the dispatch files' frames are bounded
# (-Wframe-larger-than=4096, fastinterpreter/CMakeLists.txt). A row is the flavour, then the reference's, the
# fast engine's and the small thread's bytes.
set(isolineNestingRows
    "release|2304|3072|524288"
    "debug|13056|12544|3145728"
    "address|12288|11264|3145728"
    "thread|10752|9728|3145728"
    "coverage|7424|7168|2097152")
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
        list(GET fields 1 ISOLINE_NESTING_REFERENCE_BYTES)
        list(GET fields 2 ISOLINE_NESTING_FAST_BYTES)
        list(GET fields 3 isolineSmallStackBytes)
    endif()
endforeach()
if(NOT ISOLINE_NESTING_FLAVOUR)
    message(FATAL_ERROR "[isoline] no host-stack row for the build flavour '${isolineNestingFlavour}'")
endif()
message(STATUS "[isoline] host-stack row: ${ISOLINE_NESTING_FLAVOUR}")
configure_file(runtime/NestedInvocationBytes.hpp.in
               "${PROJECT_BINARY_DIR}/include/isoline/runtime/NestedInvocationBytes.hpp" @ONLY)
target_sources(isoline_core
    PRIVATE
        Version.cpp
        api/Engine.cpp
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
            api/Extern.hpp
            api/Imports.hpp
            api/Module.hpp
            api/Store.hpp
            api/Types.hpp
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
            "${PROJECT_BINARY_DIR}/include/isoline/VersionConfig.hpp"
            "${PROJECT_BINARY_DIR}/include/isoline/runtime/NestedInvocationBytes.hpp")
target_compile_features(isoline_core PUBLIC cxx_std_23)
target_link_libraries(isoline_core PUBLIC core::base)
isoline_apply_target_policy(isoline_core)

add_subdirectory(numerics)
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
    add_executable(isoline_test ${isolineTestSources})
    target_link_libraries(isoline_test PRIVATE isoline::core Catch2::Catch2WithMain)
    # The stack of the tests' small threads (SmallStackBytes): the host-stack row's, above.
    target_compile_definitions(isoline_test PRIVATE ISOLINE_SMALL_STACK_BYTES=${isolineSmallStackBytes})
    if(NOT WIN32)
        find_package(Threads REQUIRED)
        target_link_libraries(isoline_test PRIVATE Threads::Threads)
    endif()
    # The source tree, from which tests derive the paths of their data: tests/spec, and docs/embedding.md,
    # whose generated blocks PublishedLimits_test checks against the tables.
    target_compile_definitions(isoline_test PRIVATE ISOLINE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    isoline_apply_target_policy(isoline_test TEST)
    # The [memory] cases (growth past a store's budgets, hostile deltas) run apart, under a hard memory
    # cap, so that a broken bound fails small instead of starving a shared machine.
    add_test(NAME isoline_test COMMAND isoline_test "~[memory]")
    # A bound, so a test that would hang (an interrupt test whose poll went missing) fails instead.
    set_tests_properties(isoline_test PROPERTIES LABELS unit TIMEOUT 600)
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
