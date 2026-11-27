# SPDX-License-Identifier: Apache-2.0

# isoline::compiler: the optimizing compiler from validated modules to native code, and what describes
# it: its configuration, limits and errors, the features it compiles, the target it generates for, the
# layout through which generated code reaches the runtime, (intermediate/) its SSA representation and
# (translation/) the translation of WebAssembly into it.
# Its sources take the target policy's floating-point contraction row (cmake/IsolinePedantic.cmake), so
# that constant folding is exact.
target_sources(isoline_core
    PRIVATE
        CompileError.cpp
        FeatureSupport.cpp
        intermediate/Dominators.cpp
        intermediate/Function.cpp
        intermediate/Printer.cpp
        intermediate/Verifier.cpp
        translation/LoopAssignments.cpp
        translation/Translator.cpp
    PUBLIC FILE_SET HEADERS
        BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
        FILES
            CompileConfig.hpp
            CompileError.hpp
            CompileLimits.hpp
            ContextLayout.hpp
            FeatureSupport.hpp
            intermediate/Dominators.hpp
            intermediate/Function.hpp
            intermediate/Operation.hpp
            intermediate/Operations.def
            intermediate/Printer.hpp
            intermediate/Verifier.hpp
            machine/Target.hpp
            machine/TargetDescription.hpp
            translation/LoopAssignments.hpp
            translation/NumericRules.hpp
            translation/Translator.hpp)
