# SPDX-License-Identifier: Apache-2.0

# isoline::compiler: the optimizing compiler from validated modules to native code, and what describes
# it: its configuration, limits and errors, the features it compiles, the target it generates for, the
# layout through which generated code reaches the runtime, (intermediate/) its SSA representation,
# (translation/) the translation of WebAssembly into it and (optimization/) the passes over it.
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
        optimization/BoundsChecks.cpp
        optimization/ConstantFolding.cpp
        optimization/ControlFlow.cpp
        optimization/DeadCode.cpp
        optimization/PassTable.cpp
        optimization/Rewrite.cpp
        optimization/TrivialParameters.cpp
        translation/LoopAssignments.cpp
        translation/MemoryTranslation.cpp
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
            intermediate/Guards.hpp
            intermediate/Operation.hpp
            intermediate/Operations.def
            intermediate/Printer.hpp
            intermediate/Verifier.hpp
            machine/Target.hpp
            optimization/FoldingTable.hpp
            optimization/PassTable.hpp
            optimization/Rewrite.hpp
            machine/TargetDescription.hpp
            translation/LoopAssignments.hpp
            translation/MemoryTranslation.hpp
            translation/NumericRules.hpp
            translation/Translator.hpp)
