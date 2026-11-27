# SPDX-License-Identifier: Apache-2.0
#
# The platform layer, isoline::platform: every OS resource behind an interface. Its sources build into
# isoline_core; which implementation file of each seam a build has is its platform row's
# (cmake/IsolinePlatform.cmake), whose enumerators the parent directory chose from the compiler's target.

# The row as C++: ThisPlatform, PlatformTable's row of this build.
configure_file(ThisPlatform.hpp.in "${PROJECT_BINARY_DIR}/include/isoline/platform/ThisPlatform.hpp" @ONLY)

isoline_platform_sources(core "${ISOLINE_PLATFORM_SYSTEM}" "${ISOLINE_PLATFORM_ARCHITECTURE}" isolinePlatformSources)
message(STATUS "[isoline] platform sources: ${isolinePlatformSources}")
target_sources(isoline_core
    PRIVATE
        HardwareCapabilities.cpp
        InstructionCache.cpp
        JitMemory.cpp
        VirtualMemory.cpp
        ${isolinePlatformSources}
    PUBLIC FILE_SET HEADERS
        FILES
            HardwareCapabilities.hpp
            InstructionCache.hpp
            JitMemory.hpp
            Membarrier.hpp
            Platform.hpp
            PlatformTable.hpp
            VirtualMemory.hpp
            "${PROJECT_BINARY_DIR}/include/isoline/platform/ThisPlatform.hpp")
