# SPDX-License-Identifier: Apache-2.0

# The WebAssembly text format, isoline::text: a library of its own, so that an embedder that loads
# binary modules only never links it. It builds on isoline::core, which never links it back.
add_library(isoline_text STATIC)
add_library(isoline::text ALIAS isoline_text)
target_sources(isoline_text
    PRIVATE
        Annotations.cpp
        FieldReader.cpp
        FloatLiteral.cpp
        FunctionTypes.cpp
        InstructionReader.cpp
        Instructions.cpp
        IntegerLiteral.cpp
        Keywords.cpp
        Lexer.cpp
        ModuleReader.cpp
        ModuleWriter.cpp
        SExpression.cpp
        TopLevel.cpp
    PUBLIC FILE_SET HEADERS
        BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
        FILES
            Annotations.hpp
            Digits.hpp
            FloatLiteral.hpp
            FunctionTypes.hpp
            Instructions.hpp
            IntegerLiteral.hpp
            Keywords.hpp
            Lexer.hpp
            LiteralError.hpp
            ModuleReader.hpp
            ModuleWriter.hpp
            SExpression.hpp
            TextError.hpp
            TextLimits.hpp
            TopLevel.hpp)
target_link_libraries(isoline_text PUBLIC isoline::core)
isoline_apply_target_policy(isoline_text)

if(ISOLINE_TESTING)
    # Every *_test.cpp in this directory is one translation unit of the text format's test binary.
    file(GLOB isolineTextTestSources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
    add_executable(isoline_text_test ${isolineTextTestSources})
    # core::testing_dialogs sends a Windows CRT assert or abort to stderr instead of a modal dialog, which
    # would hold the run until its timeout.
    target_link_libraries(isoline_text_test PRIVATE isoline::text Catch2::Catch2WithMain
                          core::testing_dialogs)
    # The source tree, from which the tests derive the paths of their data: tests/spec and tests/text.
    target_compile_definitions(isoline_text_test PRIVATE ISOLINE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    isoline_apply_target_policy(isoline_text_test TEST)
    # Five minutes natively: under ASan it takes 54 to 95 seconds on a CI runner, whose speed varies by
    # 1.7 times between runs. An emulator (qemu, wine) runs it several times slower, 114 seconds under
    # qemu-aarch64, so a cross build allows ten minutes.
    set(isolineTextTestTimeout 300)
    if(CMAKE_CROSSCOMPILING_EMULATOR)
        set(isolineTextTestTimeout 600)
    endif()
    add_test(NAME isoline_text_test COMMAND isoline_text_test "~[linear-time]")
    set_tests_properties(isoline_text_test PROPERTIES LABELS unit TIMEOUT ${isolineTextTestTimeout})
    # The [linear-time] cases time hostile inputs against their controls, each the fastest of three runs:
    # a minute under a sanitizer, longer under an emulator, so they run apart, with room for that.
    add_test(NAME isoline_text_linear_time_test COMMAND isoline_text_test "[linear-time]")
    set_tests_properties(isoline_text_linear_time_test PROPERTIES LABELS unit TIMEOUT 900)
endif()
