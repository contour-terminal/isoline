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
        Version.cpp
        api/Engine.cpp
        api/Module.cpp
        binary/BranchHints.cpp
        binary/ByteReader.cpp
        binary/Decoder.cpp
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
            api/Engine.hpp
            api/EngineIdentity.hpp
            api/Module.hpp
            binary/BranchHints.hpp
            binary/ByteReader.hpp
            binary/DecodeError.hpp
            binary/DecodeLimits.hpp
            binary/MemoryAccess.hpp
            binary/Decoder.hpp
            binary/Module.hpp
            binary/Opcode.hpp
            binary/Opcodes.def
            binary/TypeCanon.hpp
            binary/Types.hpp
            interpreter/Numerics.hpp
            interpreter/ReferenceInterpreter.hpp
            interpreter/Simd.hpp
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
            validate/Subtyping.hpp
            validate/ValidatedModule.hpp
            validate/ValidationError.hpp
            validate/Validator.hpp
            Version.hpp
            "${PROJECT_BINARY_DIR}/include/isoline/VersionConfig.hpp")
target_compile_features(isoline_core PUBLIC cxx_std_23)
target_link_libraries(isoline_core PUBLIC core::base)
isoline_apply_target_policy(isoline_core)

if(ISOLINE_TESTING)
    # Every *_test.cpp under src/isoline/ is one translation unit of the single unit-test binary.
    file(GLOB_RECURSE isolineTestSources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
    add_executable(isoline_test ${isolineTestSources})
    target_link_libraries(isoline_test PRIVATE isoline::core Catch2::Catch2WithMain)
    isoline_apply_target_policy(isoline_test TEST)
    add_test(NAME isoline_test COMMAND isoline_test)
    # A bound, so a test that would hang (an interrupt test whose poll went missing) fails instead.
    set_tests_properties(isoline_test PROPERTIES LABELS unit TIMEOUT 600)
endif()
