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
        canonical/GuestMemory.cpp
        canonical/Layout.cpp
        canonical/LiftLower.cpp
        canonical/LiftSteps.hpp
        canonical/Strings.cpp
        canonical/Trap.cpp
        canonical/Unicode.hpp
        canonical/Values.cpp
        validate/CoreTypes.cpp
        validate/Names.cpp
        validate/Subtyping.cpp
        runtime/Builtins.cpp
        runtime/HandleTable.cpp
        runtime/ComponentPlan.cpp
        runtime/Resources.cpp
        runtime/ComponentStore.cpp
        runtime/Calls.cpp
        validate/Validator.cpp
        api/Component.cpp
        api/ComponentStore.cpp
        api/Linker.cpp
    PUBLIC FILE_SET HEADERS
        BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
        FILES
            Features.hpp
            Limits.hpp
            binary/Component.hpp
            binary/Decoder.hpp
            binary/Encoder.hpp
            types/TypeArena.hpp
            canonical/GuestMemory.hpp
            canonical/Layout.hpp
            canonical/LiftLower.hpp
            canonical/Strings.hpp
            canonical/Trap.hpp
            canonical/Values.hpp
            validate/CoreTypes.hpp
            validate/Names.hpp
            validate/Subtyping.hpp
            validate/ValidatedComponent.hpp
            runtime/Builtins.hpp
            runtime/HandleTable.hpp
            runtime/ComponentPlan.hpp
            runtime/Resources.hpp
            runtime/ComponentStore.hpp
            runtime/Calls.hpp
            validate/Validator.hpp
            api/Component.hpp
            api/CallError.hpp
            api/ComponentStore.hpp
            api/Linker.hpp)
target_compile_features(isoline_component PUBLIC cxx_std_23)
target_link_libraries(isoline_component PUBLIC isoline::core)
isoline_apply_target_policy(isoline_component)

if(ISOLINE_TESTING)
    # Every *_test.cpp under src/isoline/component/ is one translation unit of the component unit-test
    # binary; isoline_test leaves them out.
    file(GLOB_RECURSE isolineComponentTestSources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
    add_executable(isoline_component_test ${isolineComponentTestSources})
    # The store and call tests write their core modules in the text format.
    target_link_libraries(isoline_component_test PRIVATE isoline::component isoline::text Catch2::Catch2WithMain)
    isoline_apply_target_policy(isoline_component_test TEST)
    # The deep-nesting call tests run on threads of a measured stack size (runtime/testing/StackThread.hpp).
    if(WIN32)
        target_sources(isoline_component_test PRIVATE runtime/testing/StackThreadWindows.cpp)
    else()
        target_sources(isoline_component_test PRIVATE runtime/testing/StackThreadPosix.cpp)
    endif()
    add_test(NAME isoline_component_test COMMAND isoline_component_test)
    set_tests_properties(isoline_component_test PROPERTIES LABELS unit TIMEOUT 600)
endif()
