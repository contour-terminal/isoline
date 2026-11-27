# SPDX-License-Identifier: Apache-2.0

# The project version, as constants Version.hpp reads.
configure_file(VersionConfig.hpp.in "${PROJECT_BINARY_DIR}/include/isoline/VersionConfig.hpp" @ONLY)

add_library(isoline_core STATIC)

# The target this build's code runs on, chosen here from the compiler's target (its predefined macros, so
# cross and Visual Studio -A builds choose right), so that no source selects with #ifdef: the
# floating-point control register's row (runtime/FloatEnvironment.hpp), exactly one per build, and the
# platform row (platform/PlatformTable.hpp, cmake/IsolinePlatform.cmake), which names the implementation of
# every platform seam. MSVC has no inline assembly for arm64's FPCR and defines no __aarch64__, so it takes
# the portable float row there; so does a build for several architectures at once, where one object serves
# them all and no one architecture is the host's (platform row Other). ARM64EC runs x86-64 code under an
# arm64 ABI no row describes (Other too).
include(CheckCXXSymbolExists)
include(IsolinePlatform)
list(LENGTH CMAKE_OSX_ARCHITECTURES isolineOsxArchitectureCount)
set(isolineTargetArchitecture "")
if(isolineOsxArchitectureCount GREATER 1)
    message(STATUS "[isoline] several target architectures (${CMAKE_OSX_ARCHITECTURES}): portable float control row")
    set(isolineFloatControl runtime/FloatControlPortable.cpp)
    set(isolineTargetArchitecture Other)
else()
    check_cxx_symbol_exists(__x86_64__ "" ISOLINE_TARGET_X86_64_GNU)
    check_cxx_symbol_exists(_M_X64 "" ISOLINE_TARGET_X86_64_MSVC)
    check_cxx_symbol_exists(_M_ARM64EC "" ISOLINE_TARGET_ARM64EC)
    check_cxx_symbol_exists(__aarch64__ "" ISOLINE_TARGET_ARM64_GNU)
    check_cxx_symbol_exists(_M_ARM64 "" ISOLINE_TARGET_ARM64_MSVC)
    if((ISOLINE_TARGET_X86_64_GNU OR ISOLINE_TARGET_X86_64_MSVC) AND NOT ISOLINE_TARGET_ARM64EC)
        set(isolineFloatControl runtime/FloatControlX86_64.cpp)
        set(isolineTargetArchitecture X64)
    elseif(ISOLINE_TARGET_ARM64_GNU)
        set(isolineFloatControl runtime/FloatControlArm64.cpp)
        set(isolineTargetArchitecture Arm64)
    elseif(ISOLINE_TARGET_ARM64_MSVC AND NOT ISOLINE_TARGET_ARM64EC)
        set(isolineFloatControl runtime/FloatControlPortable.cpp)
        set(isolineTargetArchitecture Arm64)
    else()
        set(isolineFloatControl runtime/FloatControlPortable.cpp)
        set(isolineTargetArchitecture Other)
    endif()
endif()
# __APPLE__ before __linux__/__FreeBSD__, none of which an Apple target defines. A system none of the macros
# names selects with a name no row has, and fails the configure: no row falls back silently. CMake's name
# for the system only goes into the message, never into the selection.
check_cxx_symbol_exists(_WIN32 "" ISOLINE_TARGET_WINDOWS)
check_cxx_symbol_exists(__APPLE__ "" ISOLINE_TARGET_APPLE)
check_cxx_symbol_exists(__linux__ "" ISOLINE_TARGET_LINUX)
check_cxx_symbol_exists(__FreeBSD__ "" ISOLINE_TARGET_FREEBSD)
if(ISOLINE_TARGET_WINDOWS)
    set(isolineTargetSystem Windows)
elseif(ISOLINE_TARGET_APPLE)
    set(isolineTargetSystem MacOS)
elseif(ISOLINE_TARGET_LINUX)
    set(isolineTargetSystem Linux)
elseif(ISOLINE_TARGET_FREEBSD)
    set(isolineTargetSystem FreeBSD)
else()
    set(isolineTargetSystem "<unrecognized>")
endif()
isoline_platform_row("${isolineTargetSystem}" "${isolineTargetArchitecture}"
                     ISOLINE_PLATFORM_SYSTEM ISOLINE_PLATFORM_ARCHITECTURE isolinePlatformError)
if(isolinePlatformError)
    message(FATAL_ERROR "${isolinePlatformError} (CMake names the target system '${CMAKE_SYSTEM_NAME}')")
endif()
message(STATUS "[isoline] platform row: ${ISOLINE_PLATFORM_SYSTEM}, ${ISOLINE_PLATFORM_ARCHITECTURE}")
message(STATUS "[isoline] float control row: ${isolineFloatControl}")
add_library(isoline::core ALIAS isoline_core)
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
        runtime/FloatEnvironment.cpp
        runtime/HostFunction.cpp
        runtime/Instantiate.cpp
        runtime/LinearMemory.cpp
        runtime/Store.cpp
        runtime/ValueCheck.cpp
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
            binary/ByteWriter.hpp
            binary/DecodeError.hpp
            binary/DecodeLimits.hpp
            binary/MemoryAccess.hpp
            binary/Decoder.hpp
            binary/Encoder.hpp
            binary/Encodings.hpp
            binary/Module.hpp
            binary/NameSection.hpp
            binary/Opcode.hpp
            binary/OpcodeNames.hpp
            binary/Opcodes.def
            binary/TypeCanon.hpp
            binary/Types.hpp
            interpreter/Numerics.hpp
            interpreter/ReferenceInterpreter.hpp
            interpreter/Simd.hpp
            meta/EnumRows.hpp
            meta/FixedText.hpp
            runtime/Abrupt.hpp
            runtime/FloatEnvironment.hpp
            runtime/HostFunction.hpp
            runtime/Instantiate.hpp
            runtime/Interrupt.hpp
            runtime/Limits.hpp
            runtime/LinearMemory.hpp
            runtime/Segment.hpp
            runtime/Store.hpp
            runtime/Trap.hpp
            runtime/Value.hpp
            runtime/ValueCheck.hpp
            runtime/ValueCheckTables.hpp
            validate/Subtyping.hpp
            validate/ValidatedModule.hpp
            validate/ValidationError.hpp
            validate/Validator.hpp
            Version.hpp
            "${PROJECT_BINARY_DIR}/include/isoline/VersionConfig.hpp")
# The platform layer (isoline::platform): its sources and headers, the row's implementation files among them.
add_subdirectory(platform)
target_compile_features(isoline_core PUBLIC cxx_std_23)
# Header-based like every target but the module consumers: never scanned for module dependencies, whatever
# CMP0155 says (module isoline is a target of its own, isoline_core_module).
set_target_properties(isoline_core PROPERTIES CXX_SCAN_FOR_MODULES OFF)
target_link_libraries(isoline_core PUBLIC core::base)
isoline_apply_target_policy(isoline_core)
# After the policy: the native features' flags follow the warning set (cmake/IsolineLanguageFeatures.cmake).
isoline_apply_language_features(isoline_core)

add_subdirectory(text)
add_subdirectory(cli)

# Module isoline: the public names of isoline_core's header set, re-exported by the generated isoline.cppm.
# binary, runtime, validate, interpreter and meta are internal: isoline::api re-exports what of them is public.
isoline_add_module(
    NAME isoline
    LIBRARY isoline_core
    SOURCE isoline.cppm
    NAMESPACES isoline isoline::api isoline::config
    INTERNAL_NAMESPACES isoline::binary isoline::runtime isoline::validate isoline::interpreter isoline::meta isoline::platform)

# The C API, a library of its own over isoline_core, with its own C++ tests (src/isoline/c/).
if(ISOLINE_C_API)
    add_subdirectory(c)
endif()

if(ISOLINE_TESTING)
    # Every *_test.cpp under src/isoline/ is one translation unit of the single unit-test binary,
    # except in the directories whose tests link a target of their own and build their own binary
    # (meta/reflection's only where reflection is native, below).
    # Those under a posix/ directory test what only POSIX systems have (signal dispositions), and are
    # added on those alone, so that no test source checks for the platform.
    set(isolineSeparateTestDirectories text cli meta/reflection)
    file(GLOB_RECURSE isolineTestSources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
    foreach(directory IN LISTS isolineSeparateTestDirectories)
        list(FILTER isolineTestSources EXCLUDE REGEX "/${directory}/[^/]*_test\\.cpp$")
    endforeach()
    list(FILTER isolineTestSources EXCLUDE REGEX "/posix/[^/]*_test\\.cpp$")
    # The C API's C++ tests link the C library, so they are a binary of their own (src/isoline/c/).
    list(FILTER isolineTestSources EXCLUDE REGEX "^${CMAKE_CURRENT_SOURCE_DIR}/c/")
    if(UNIX)
        file(GLOB_RECURSE isolinePosixTestSources CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/*/posix/*_test.cpp")
        list(APPEND isolineTestSources ${isolinePosixTestSources})
    endif()
    # A separate directory's posix/ tests are its own binary's too.
    foreach(directory IN LISTS isolineSeparateTestDirectories)
        list(FILTER isolineTestSources EXCLUDE REGEX "/${directory}/posix/[^/]*_test\\.cpp$")
    endforeach()
    # The platform row's test helpers (platform/testing/), one file per seam (cmake/IsolinePlatform.cmake).
    isoline_platform_sources(test "${ISOLINE_PLATFORM_SYSTEM}" "${ISOLINE_PLATFORM_ARCHITECTURE}"
                             isolinePlatformTestSources)
    list(TRANSFORM isolinePlatformTestSources PREPEND platform/)
    list(APPEND isolineTestSources ${isolinePlatformTestSources})
    add_executable(isoline_test ${isolineTestSources})
    target_link_libraries(isoline_test PRIVATE isoline::core Catch2::Catch2WithMain)
    # The source tree, from which tests derive the paths of their data: tests/spec, and docs/embedding.md,
    # whose generated blocks PublishedLimits_test checks against the tables.
    # Whether this is a build for several architectures at once, whose platform row is Other although
    # each architecture's compile names its own: what PlatformTable_test expects of ThisPlatform then.
    if(isolineOsxArchitectureCount GREATER 1)
        set(isolineTestSeveralArchitectures 1)
    else()
        set(isolineTestSeveralArchitectures 0)
    endif()
    target_compile_definitions(isoline_test PRIVATE ISOLINE_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
                                                   ISOLINE_TEST_SEVERAL_ARCHITECTURES=${isolineTestSeveralArchitectures})
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

    # The reflection checks (docs/language-features.md), only where reflection is native: their test,
    # and the exhaustive check of meta/EnumTables.def over every header of the libraries, installed or
    # private, but the test helpers' and the checks' own.
    isoline_add_reflection_check(isoline_meta_reflection_test TEST LABELS unit
        SOURCES meta/reflection/EnumReflection_test.cpp
        LINK_LIBRARIES isoline::core)
    # The annotations rehearsal (P19 Task 7): rows derived from enumerator annotations, only where
    # annotations are native.
    isoline_add_reflection_check(isoline_meta_annotations_test TEST LABELS unit FEATURE annotations
        SOURCES meta/reflection/AnnotatedRows_test.cpp
        LINK_LIBRARIES isoline::core)
    # The C API's private headers include its C headers (wasm.h among them): reached through its static
    # library where the C API is built, and left out where it is not.
    set(isolineEnumTablesLinks isoline::cli)
    set(isolineEnumTablesExcludes "/testing/" "^isoline/meta/reflection/" "^isoline/meta/(fallback/)?contracts/")
    if(TARGET isoline::c_static)
        list(APPEND isolineEnumTablesLinks isoline::c_static)
    else()
        list(APPEND isolineEnumTablesExcludes "^isoline/c/")
    endif()
    isoline_add_reflection_check(isoline_enum_tables_check
        SOURCES meta/reflection/EnumTablesCheck.cpp
        LINK_LIBRARIES ${isolineEnumTablesLinks}
        UMBRELLA isoline/meta/reflection/EnumTablesUmbrella.hpp
        HEADER_TARGETS isoline_core isoline_text isoline_cli
        EXCLUDE ${isolineEnumTablesExcludes})
endif()
