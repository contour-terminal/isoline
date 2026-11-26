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
target_sources(isoline_core
    PRIVATE
        Contracts.cpp
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
            runtime/ConstantExpression.hpp
            runtime/ExecutionMode.hpp
            runtime/FloatEnvironment.hpp
            runtime/HostFunction.hpp
            runtime/Instantiate.hpp
            runtime/Interrupt.hpp
            runtime/Limits.hpp
            runtime/LinearMemory.hpp
            runtime/ModuleCode.hpp
            runtime/PollGate.hpp
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
            "${PROJECT_BINARY_DIR}/include/isoline/Contracts.hpp"
            "${PROJECT_BINARY_DIR}/include/isoline/VersionConfig.hpp")
target_compile_features(isoline_core PUBLIC cxx_std_23)
# Header-based like every target but the module consumers: never scanned for module dependencies, whatever
# CMP0155 says (module isoline is a target of its own, isoline_core_module).
set_target_properties(isoline_core PROPERTIES CXX_SCAN_FOR_MODULES OFF)
target_link_libraries(isoline_core PUBLIC core::base)
isoline_apply_target_policy(isoline_core)
# After the policy: the native features' flags follow the warning set (cmake/IsolineLanguageFeatures.cmake).
isoline_apply_language_features(isoline_core)

add_subdirectory(numerics)
add_subdirectory(fastinterpreter)
add_subdirectory(text)
add_subdirectory(cli)

# Module isoline: the public names of isoline_core's header set, re-exported by the generated isoline.cppm.
# binary, runtime, validate, interpreter and meta are internal: isoline::api re-exports what of them is public.
isoline_add_module(
    NAME isoline
    LIBRARY isoline_core
    SOURCE isoline.cppm
    NAMESPACES isoline isoline::api isoline::config
    INTERNAL_NAMESPACES isoline::binary isoline::runtime isoline::validate isoline::interpreter isoline::meta
    # isoline/Contracts.hpp's: the declarations of the set spell their contracts through them.
    DEFINED_MACROS ISOLINE_DECLARATION_CONTRACTS ISOLINE_PRE ISOLINE_POST)

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
    add_executable(isoline_test ${isolineTestSources})
    target_link_libraries(isoline_test PRIVATE isoline::core Catch2::Catch2WithMain)
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

    # The reflection checks (docs/language-features.md), only where reflection is native: their test,
    # and the exhaustive check of meta/EnumTables.def over every header of the libraries, installed or
    # private, but the test helpers' and the checks' own.
    isoline_add_reflection_check(isoline_meta_reflection_test TEST LABELS unit
        SOURCES meta/reflection/EnumReflection_test.cpp meta/reflection/ModuleSurface_test.cpp
                meta/reflection/CApiReflection_test.cpp
        LINK_LIBRARIES isoline::core)
    # The annotations rehearsal (P19 Task 7): rows derived from enumerator annotations, only where
    # annotations are native.
    isoline_add_reflection_check(isoline_meta_annotations_test TEST LABELS unit FEATURE annotations
        SOURCES meta/reflection/AnnotatedRows_test.cpp
        LINK_LIBRARIES isoline::core)
    # The reflected module surface (P19 Task 9): GCC's members_of over module isoline's public namespaces,
    # compared with the export lint's names by the test meta-module-surface.reflection.
    isoline_add_reflection_check(isoline_meta_surface_reflection PROGRAM
        SOURCES meta/reflection/ModuleSurface.cpp
        LINK_LIBRARIES isoline::core
        UMBRELLA isoline/meta/reflection/ModuleSurfaceUmbrella.hpp
        HEADER_TARGETS isoline_core
        HEADER_SETS_ONLY)
    isoline_add_module_surface_reflection(NAME meta-module-surface.reflection MODULE isoline
                                          PROGRAM isoline_meta_surface_reflection)
    # The C API's private headers include its C headers (wasm.h among them): reached through its static
    # library where the C API is built, and left out where it is not.
    set(isolineEnumTablesLinks isoline::cli)
    set(isolineEnumTablesExcludes "/testing/" "^isoline/meta/reflection/" "^isoline/meta/(fallback/)?contracts/")
    if(TARGET isoline::c_static)
        list(APPEND isolineEnumTablesLinks isoline::c_static)
    else()
        list(APPEND isolineEnumTablesExcludes "^isoline/c/")
    endif()
    # The C API's enum mirrors and struct members (P19 Task 13), where the C API is built: its C headers read as
    # C++ beside the tables of the C layer that convert them.
    if(TARGET isoline::c_static)
        isoline_add_reflection_check(isoline_c_api_check
            SOURCES meta/reflection/CApiCheck.cpp
            LINK_LIBRARIES isoline::c_static isoline::core)
        # Every C header the C API installs, wasm.h among them, from its header set: a header added there is
        # one whose enumerations the discovery of CApiCheck.cpp walks, without a row.
        if(TARGET isoline_c_api_check)
            _isoline_header_set_include_names(isoline_c_static isolineCApiHeaders "[.]h$")
            set(isolineCApiUmbrella "// Generated from isoline_c_static's header set (src/isoline/CMakeLists.txt).\n#pragma once\n")
            foreach(header IN LISTS isolineCApiHeaders)
                string(APPEND isolineCApiUmbrella "#include <${header}>\n")
            endforeach()
            file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/isoline_c_api_check.headers/isoline/meta/reflection/CApiHeaders.hpp"
                 CONTENT "${isolineCApiUmbrella}" @ONLY)
            target_include_directories(isoline_c_api_check PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/isoline_c_api_check.headers")
        endif()
    endif()
    isoline_add_reflection_check(isoline_enum_tables_check
        SOURCES meta/reflection/EnumTablesCheck.cpp
        LINK_LIBRARIES ${isolineEnumTablesLinks}
        UMBRELLA isoline/meta/reflection/EnumTablesUmbrella.hpp
        HEADER_TARGETS isoline_core isoline_text isoline_cli
        EXCLUDE ${isolineEnumTablesExcludes})
    # The noexcept rule of declaration contracts, only where contracts are native: every header of the
    # libraries, the test helpers' included (Catch2 for their StringMakers), read through the proving
    # expansion of <isoline/Contracts.hpp> (isoline_add_declaration_contracts_proof()).
    set(isolineDeclarationContractsExcludes "")
    if(NOT TARGET isoline::c_static)
        list(APPEND isolineDeclarationContractsExcludes "^isoline/c/")
    endif()
    isoline_add_declaration_contracts_proof(isoline_declaration_contracts_check
        SOURCES meta/reflection/DeclarationContractsCheck.cpp
        LINK_LIBRARIES ${isolineEnumTablesLinks} Catch2::Catch2
        HEADER_TARGETS isoline_core isoline_text isoline_cli
        EXCLUDE ${isolineDeclarationContractsExcludes})
endif()
