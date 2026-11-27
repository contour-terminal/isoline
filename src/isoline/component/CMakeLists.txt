# SPDX-License-Identifier: Apache-2.0

# The component model (P11): isoline_component, on isoline_core's public layers and nothing else.
# isoline_core links no component code.
add_library(isoline_component STATIC)
add_library(isoline::component ALIAS isoline_component)
target_sources(isoline_component
    PRIVATE
        binary/Decoder.cpp
        binary/Encoder.cpp
        binary/Grammar.hpp
        types/TypeArena.cpp
        canonical/Layout.cpp
        validate/CoreTypes.cpp
        validate/Names.cpp
        validate/Subtyping.cpp
        validate/Validator.cpp
    PUBLIC FILE_SET HEADERS
        BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
        FILES
            Features.hpp
            Limits.hpp
            binary/Component.hpp
            binary/Decoder.hpp
            binary/Encoder.hpp
            types/TypeArena.hpp
            canonical/Layout.hpp
            validate/CoreTypes.hpp
            validate/Names.hpp
            validate/Subtyping.hpp
            validate/ValidatedComponent.hpp
            validate/Validator.hpp)
target_compile_features(isoline_component PUBLIC cxx_std_23)
target_link_libraries(isoline_component PUBLIC isoline::core)
isoline_apply_target_policy(isoline_component)

if(ISOLINE_TESTING)
    # Every *_test.cpp under src/isoline/component/ is one translation unit of the component unit-test
    # binary; isoline_test leaves them out.
    file(GLOB_RECURSE isolineComponentTestSources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
    add_executable(isoline_component_test ${isolineComponentTestSources})
    target_link_libraries(isoline_component_test PRIVATE isoline::component Catch2::Catch2WithMain)
    isoline_apply_target_policy(isoline_component_test TEST)
    add_test(NAME isoline_component_test COMMAND isoline_component_test)
    set_tests_properties(isoline_component_test PROPERTIES LABELS unit TIMEOUT 600)
endif()
