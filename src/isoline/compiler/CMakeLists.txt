# SPDX-License-Identifier: Apache-2.0

# isoline::compiler: the optimizing compiler from validated modules to native code, and what describes
# it: its configuration, limits and errors, the features it compiles, the target it generates for, the
# layout through which generated code reaches the runtime, (intermediate/) its SSA representation,
# (translation/) the translation of WebAssembly into it, (optimization/) the passes over it and
# (machine/) the target seam and the target-independent machine layer: conventions, layout, liveness, and
# (registerallocation/) linear-scan register allocation.
# Its sources take the target policy's floating-point contraction row (cmake/IsolinePedantic.cmake), so
# that constant folding is exact.
target_sources(isoline_core
    PRIVATE
        CompileError.cpp
        FeatureSupport.cpp
        intermediate/Dominators.cpp
        intermediate/Function.cpp
        intermediate/MemoryStretches.cpp
        intermediate/Printer.cpp
        intermediate/Verifier.cpp
        machine/CallingConvention.cpp
        machine/CodeBuffer.cpp
        machine/Linearize.cpp
        machine/Liveness.cpp
        machine/MachineFunction.cpp
        machine/MachineVerifier.cpp
        optimization/BoundsChecks.cpp
        optimization/ConstantFolding.cpp
        optimization/ControlFlow.cpp
        optimization/DeadCode.cpp
        optimization/PassTable.cpp
        optimization/Rewrite.cpp
        optimization/TrivialParameters.cpp
        registerallocation/LinearScan.cpp
        registerallocation/ParallelMoves.cpp
        registerallocation/SpillSlots.cpp
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
            intermediate/MemoryStretches.hpp
            intermediate/Operation.hpp
            intermediate/Operations.def
            intermediate/Printer.hpp
            intermediate/Verifier.hpp
            machine/CallingConvention.hpp
            machine/CodeBuffer.hpp
            machine/FrameRule.hpp
            machine/Linearize.hpp
            machine/Liveness.hpp
            machine/MachineFunction.hpp
            machine/MachineVerifier.hpp
            machine/Registers.hpp
            machine/Target.hpp
            machine/TargetDescription.hpp
            optimization/FoldingTable.hpp
            optimization/PassTable.hpp
            optimization/Rewrite.hpp
            registerallocation/LinearScan.hpp
            registerallocation/ParallelMoves.hpp
            registerallocation/SpillSlots.hpp
            translation/LoopAssignments.hpp
            translation/MemoryTranslation.hpp
            translation/NumericRules.hpp
            translation/Translator.hpp)
