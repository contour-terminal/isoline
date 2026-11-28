# SPDX-License-Identifier: Apache-2.0

# isoline::execution: what runs compiled code. Sealed executable memory (ICodeMemory), the calling thread's
# stack bounds and the native-stack arithmetic (IStackBounds), the running CPU's features (ICpuFeatureProbe),
# and the runtime stubs generated code calls. Each platform's half is one file, chosen here from the compiler's
# target (as runtime/'s FloatControl rows are), so that no source selects with #ifdef: the code memory by
# operating system, the stack bounds by operating system, and the CPU probe by architecture and, on x86-64, by
# the compiler's frontend for CPUID's intrinsic.
# FreeBSD's thread stack query is another function in another header than Linux's (pthread_np.h).
check_cxx_symbol_exists(__FreeBSD__ "" ISOLINE_TARGET_FREEBSD)
if(ISOLINE_TARGET_WINDOWS)
    set(isolineCodeMemory CodeMemoryWindows.cpp)
    set(isolineStackBounds StackBoundsWindows.cpp)
    # <windows.h>'s min and max macros would rewrite std::numeric_limits<>::max() in the headers that follow it.
    set_source_files_properties(CodeMemoryWindows.cpp StackBoundsWindows.cpp TARGET_DIRECTORY isoline_core
        PROPERTIES COMPILE_DEFINITIONS "NOMINMAX;WIN32_LEAN_AND_MEAN")
elseif(ISOLINE_TARGET_APPLE)
    set(isolineCodeMemory CodeMemoryPosix.cpp)
    set(isolineStackBounds StackBoundsMacOS.cpp)
elseif(ISOLINE_TARGET_FREEBSD)
    set(isolineCodeMemory CodeMemoryPosix.cpp)
    set(isolineStackBounds StackBoundsFreeBsd.cpp)
else()
    set(isolineCodeMemory CodeMemoryPosix.cpp)
    set(isolineStackBounds StackBoundsPosix.cpp)
endif()
if(isolineHostArchitecture STREQUAL "X64")
    if(MSVC)
        set(isolineCpuFeatureProbe CpuFeatureProbeX86_64.cpp CpuidMsvc.cpp)
    else()
        set(isolineCpuFeatureProbe CpuFeatureProbeX86_64.cpp CpuidGnu.cpp)
    endif()
else()
    set(isolineCpuFeatureProbe CpuFeatureProbePortable.cpp)
endif()
message(STATUS "[isoline] execution rows: ${isolineCodeMemory}, ${isolineStackBounds}, ${isolineCpuFeatureProbe}")
target_sources(isoline_core
    PRIVATE
        ${isolineCodeMemory}
        ${isolineStackBounds}
        ${isolineCpuFeatureProbe}
        RuntimeStubs.cpp
    PUBLIC FILE_SET HEADERS
        BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
        FILES
            CodeMemory.hpp
            CpuFeatureProbe.hpp
            RuntimeStubs.hpp
            StackBounds.hpp)
