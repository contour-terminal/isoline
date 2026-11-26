# SPDX-License-Identifier: Apache-2.0

# The project version, as constants Version.hpp reads.
configure_file(VersionConfig.hpp.in "${PROJECT_BINARY_DIR}/include/isoline/VersionConfig.hpp" @ONLY)

add_library(isoline_core STATIC)
add_library(isoline::core ALIAS isoline_core)
target_sources(isoline_core
    PRIVATE
        Version.cpp
        binary/ByteReader.cpp
        binary/Decoder.cpp
        binary/Opcode.cpp
    PUBLIC FILE_SET HEADERS
        BASE_DIRS "${PROJECT_SOURCE_DIR}/src" "${PROJECT_BINARY_DIR}/include"
        FILES
            Features.hpp
            binary/ByteReader.hpp
            binary/DecodeError.hpp
            binary/MemoryAccess.hpp
            binary/Decoder.hpp
            binary/Module.hpp
            binary/Opcode.hpp
            binary/Opcodes.def
            binary/Types.hpp
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
    set_tests_properties(isoline_test PROPERTIES LABELS unit)
endif()
