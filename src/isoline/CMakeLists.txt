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
# reference and on the interpreter (runtime/NestedInvocationBytes.hpp, generated from the row), and the stack
# of the tests' small threads (runtime/testing/SmallStackThread.hpp's SmallStackBytes). The engines' bytes are
# the most the hidden case Calls.measureNestingStack measured on the flavour's builds (docs/interpreter.md
# lists each), rounded up by half again to a multiple of 256; Calls.nestingStackCostIsPublished checks every
# build against its row. The small thread holds maxNestedInvocations (100) nestings of the larger engine twice
# over, rounded up to a MiB; a release build's is 512 KiB, where the dispatch files' frames are bounded
# (-Wframe-larger-than=4096, interpreter/CMakeLists.txt). A row is the flavour, then the reference's, the
# interpreter's and the small thread's bytes. A flavour may have a row of its own for one target architecture,
# "<flavour>-<target>", which that target's builds take instead: s390x's ABI gives every frame a 160-byte
# register save area, so its frames are larger than any other target's.
set(isolineNestingRows
    "release|2304|3072|524288"
    "release-s390x|4864|6144|2097152"
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
# The targets with rows of their own: a row is the compiler's predefined macro that names the target, then
# the suffix of its flavours' rows. A build for that target takes the flavour's row with the suffix when there
# is one; chosen by the macros, as the float control row is, so that a cross build chooses right.
set(isolineNestingTargets
    "__s390x__|s390x")
list(TRANSFORM isolineNestingRows REPLACE "\\|.*$" "" OUTPUT_VARIABLE isolineNestingFlavours)
foreach(target IN LISTS isolineNestingTargets)
    string(REPLACE "|" ";" fields "${target}")
    list(GET fields 0 macro)
    list(GET fields 1 suffix)
    check_cxx_symbol_exists(${macro} "" ISOLINE_NESTING_TARGET_${suffix})
    if(ISOLINE_NESTING_TARGET_${suffix} AND "${isolineNestingFlavour}-${suffix}" IN_LIST isolineNestingFlavours)
        set(isolineNestingFlavour "${isolineNestingFlavour}-${suffix}")
    endif()
endforeach()
foreach(row IN LISTS isolineNestingRows)
    string(REPLACE "|" ";" fields "${row}")
    list(GET fields 0 flavour)
    if(flavour STREQUAL isolineNestingFlavour)
        set(ISOLINE_NESTING_FLAVOUR "${flavour}")
        list(GET fields 1 ISOLINE_NESTING_REFERENCE_BYTES)
        list(GET fields 2 ISOLINE_NESTING_INTERPRETER_BYTES)
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
        Contracts.cpp
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
        reference/Numerics.cpp
        reference/ReferenceInterpreter.cpp
        reference/Simd.cpp
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
            meta/EnumRows.hpp
            meta/FixedText.hpp
            reference/Numerics.hpp
            reference/ReferenceInterpreter.hpp
            reference/Simd.hpp
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
            runtime/ValueCheckTables.hpp
            runtime/ValueWords.hpp
            validate/Subtyping.hpp
            validate/ValidatedModule.hpp
            validate/ValidationError.hpp
            validate/Validator.hpp
            Version.hpp
            "${PROJECT_BINARY_DIR}/include/isoline/Contracts.hpp"
            "${PROJECT_BINARY_DIR}/include/isoline/VersionConfig.hpp"
            "${PROJECT_BINARY_DIR}/include/isoline/runtime/NestedInvocationBytes.hpp")
target_compile_features(isoline_core PUBLIC cxx_std_23)
# Header-based like every target but the module consumers: never scanned for module dependencies, whatever
# CMP0155 says (module isoline is a target of its own, isoline_core_module).
set_target_properties(isoline_core PROPERTIES CXX_SCAN_FOR_MODULES OFF)
target_link_libraries(isoline_core PUBLIC core::base)
isoline_apply_target_policy(isoline_core)
# After the policy: the native features' flags follow the warning set (cmake/IsolineLanguageFeatures.cmake).
isoline_apply_language_features(isoline_core)

add_subdirectory(numerics)
add_subdirectory(interpreter)
add_subdirectory(text)
add_subdirectory(cli)

# Module isoline: the public names of isoline_core's header set, re-exported by the generated isoline.cppm.
# binary, runtime, validate, interpreter, reference, numerics and meta are internal: isoline::api re-exports
# what of them is public. isoline::api::engines, the engine rows, is public.
isoline_add_module(
    NAME isoline
    LIBRARY isoline_core
    SOURCE isoline.cppm
    NAMESPACES isoline isoline::api isoline::api::engines isoline::config
    INTERNAL_NAMESPACES isoline::binary isoline::runtime isoline::validate isoline::interpreter isoline::reference
                        isoline::numerics isoline::meta
    # isoline/Contracts.hpp's: the declarations of the set spell their contracts through them. And
    # isoline/interpreter/DispatchAvailability.hpp's: the spelling of the tail calls the probe row passed with.
    DEFINED_MACROS ISOLINE_DECLARATION_CONTRACTS ISOLINE_PRE ISOLINE_POST ISOLINE_INTERPRETER_MUSTTAIL)

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
    # The unit run, one test per area: an area is the part of a case's name before its first dot, and
    # isoline_test.<area> runs every case of it ("<area>.*"), so that the areas run in parallel and each has a
    # bound of its own. A row is the area, then its TIMEOUT in seconds when it is not the default, a bound
    # that turns a hang (an interrupt test whose poll went missing) into a failure; ISOLINE_TEST_TIMEOUT_FACTOR
    # multiplies each under an emulator. Then a label besides unit, when it has one: interpreter for the
    # interpreter's own areas (src/isoline/interpreter), which `ctest -L interpreter` selects with the spec
    # suite on that engine. isoline_test.areas checks that every case belongs to a row's area.
    # The hidden cases run apart: the [.contract] ones in tests/CMakeLists.txt, the [perf] ones below. So do
    # the [memory] cases (growth past a store's budgets, hostile deltas), under a hard memory cap, so that a
    # broken bound fails small instead of starving a shared machine.
    set(isolineTestDefaultTimeout 120)
    set(isolineTestAreas
        "BranchHints"
        "Bulk|interpreter"
        "ByteReader"
        "Calls|300|interpreter"
        "Code|interpreter"
        "ConstantExpression"
        "Contract"
        "Contracts"
        "CountingArena"
        "DecodeError"
        "Decoder"
        "Dispatch|interpreter"
        "Encoder|300"
        "Engine"
        "EnumRows"
        "Exceptions|interpreter"
        "Externs"
        "Features"
        "FixedText"
        "FloatEnvironment"
        "HeapMemoryAllocator"
        "HostCall"
        "HostFunction"
        "Imports"
        "Instantiate"
        "Interpreter|interpreter"
        "Interrupt"
        "Interrupter"
        "Limits"
        "LinearMemory"
        "LittleEndian|interpreter"
        "Lowering|600|interpreter"
        "LoweringRows|interpreter"
        "Memory|interpreter"
        "MemoryAccess"
        "Module"
        "ModuleEquality"
        "NameSection"
        "Numerics"
        "Opcode"
        "OpcodeNames"
        "OperationTable|interpreter"
        "PollGate"
        "ProvidedMemoryAllocator"
        "PublishedLimits"
        "RawValue"
        "Readme"
        "ReallocatingMemoryAllocator"
        "ReferenceInterpreter|300"
        "Relaxed"
        "Segment"
        "Simd|300|interpreter"
        "SimdKernels"
        "SlotCodec|interpreter"
        "Store"
        "Subtyping"
        "Tables|interpreter"
        "Threads"
        "Trap"
        "TypeCanon"
        "Types"
        "ValidatedModule"
        "Validator"
        "Value"
        "ValueCheck"
        "Version")
    set(isolineTestAreaNames)
    foreach(row IN LISTS isolineTestAreas)
        if(row MATCHES "^([A-Za-z]+)(\\|([0-9]+))?(\\|(interpreter))?$")
            set(area "${CMAKE_MATCH_1}")
            set(timeout "${CMAKE_MATCH_3}")
            set(labels unit ${CMAKE_MATCH_5})
        else()
            message(FATAL_ERROR "isolineTestAreas: expected '<area>[|<seconds>][|interpreter]', got '${row}'")
        endif()
        if(NOT timeout)
            set(timeout ${isolineTestDefaultTimeout})
        endif()
        math(EXPR timeout "${timeout} * ${ISOLINE_TEST_TIMEOUT_FACTOR}")
        list(APPEND isolineTestAreaNames "${area}")
        # The tags first: Catch2 reads a name pattern to the end of the argument. A name pattern selects
        # hidden cases too, which ~[.] leaves to their own tests.
        add_test(NAME isoline_test.${area} COMMAND isoline_test "~[.] ~[memory] ${area}.*")
        set_tests_properties(isoline_test.${area} PROPERTIES LABELS "${labels}" TIMEOUT ${timeout})
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
