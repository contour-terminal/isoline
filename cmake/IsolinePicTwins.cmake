# SPDX-License-Identifier: Apache-2.0
#
# isoline_pic_twins(): position-independent copies of static libraries, for a shared library to link.
#
# A static library is compiled once, for every program that links it. Compiling isoline_core position
# independent so that the shared C library can link it would therefore change the code of the CLI, the
# tests and every C++ or static C embedder too, and PIC is not free: Clang's PIC call round trip
# (Latency.callRoundTrip) measured about 2 ns of 70 slower than PIE on x86-64 (plan P17a, Unit B Task 5).
# Performance at or above Wasmtime's is Isoline's bar, so no embedder pays that for a library it may not
# use. Instead, the shared library links a twin of each static library it is built over: the same
# sources, compiled with the same settings, plus -fPIC.
#
# A twin is EXCLUDE_FROM_ALL, so it is built only when the shared library is. No twin is made where the
# build is position independent already: CMAKE_POSITION_INDEPENDENT_CODE (a consumer asking for PIC
# everywhere), or an object format whose code is position independent regardless (Mach-O; PE, where PIC
# is no compiler mode at all). Those are the rows below.
#
# tests/c/CheckPicTwins.cmake holds a twin to its original, from Ninja's own compile commands.

include_guard(GLOBAL)

# The object formats whose code a compiler makes position independent only when asked: format|reason.
set(ISOLINE_PIC_TWIN_FORMAT_ROWS
    "elf|code is position dependent (PIE, in an executable) unless compiled -fPIC")

# What a twin copies from its original. Usage requirements that name targets (LINK_LIBRARIES,
# INTERFACE_LINK_LIBRARIES) are copied with each library that has a twin replaced by it.
set(_isolinePicTwinProperties
    COMPILE_DEFINITIONS
    COMPILE_FEATURES
    COMPILE_OPTIONS
    CXX_EXTENSIONS
    CXX_SCAN_FOR_MODULES
    CXX_STANDARD
    CXX_STANDARD_REQUIRED
    CXX_VISIBILITY_PRESET
    C_EXTENSIONS
    C_STANDARD
    INCLUDE_DIRECTORIES
    INTERFACE_COMPILE_DEFINITIONS
    INTERFACE_COMPILE_FEATURES
    INTERFACE_COMPILE_OPTIONS
    INTERFACE_INCLUDE_DIRECTORIES
    INTERFACE_LINK_DIRECTORIES
    INTERFACE_LINK_OPTIONS
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
    MSVC_RUNTIME_LIBRARY
    SYSTEM
    VISIBILITY_INLINES_HIDDEN)

## @brief Sets @p outVar to whether this build makes PIC twins at all (the rows above, and no
## CMAKE_POSITION_INDEPENDENT_CODE).
function(isoline_pic_twins_wanted outVar)
    if(APPLE)
        set(format macho)
    elseif(WIN32)
        set(format pe)
    else()
        set(format elf)
    endif()
    set(wanted OFF)
    foreach(row IN LISTS ISOLINE_PIC_TWIN_FORMAT_ROWS)
        if(row MATCHES "^([a-z]+)[|]" AND CMAKE_MATCH_1 STREQUAL format)
            set(wanted ON)
        endif()
    endforeach()
    if(CMAKE_POSITION_INDEPENDENT_CODE)
        set(wanted OFF)
    endif()
    set(${outVar} ${wanted} PARENT_SCOPE)
endfunction()

## @brief Replaces, in the list @p items, each target of @p originals by its twin (<target>_pic), inside
## $<LINK_ONLY:...> too, and sets @p outVar to the result.
function(_isoline_pic_twin_map items originals outVar)
    set(mapped "")
    foreach(item IN LISTS items)
        set(inner "${item}")
        set(linkOnly OFF)
        if(item MATCHES "^[$]<LINK_ONLY:(.+)>$")
            set(inner "${CMAKE_MATCH_1}")
            set(linkOnly ON)
        endif()
        # An imported target is visible in the directory that found it alone, and the twins are made
        # in the caller's: the caller finds what the originals link (find_package) before it asks.
        if(inner MATCHES "::" AND NOT TARGET ${inner})
            message(FATAL_ERROR "isoline_pic_twins: ${inner}, which an original links, is no target here; "
                                "find it in this directory first")
        endif()
        if(TARGET ${inner})
            get_target_property(aliased ${inner} ALIASED_TARGET)
            if(aliased)
                set(inner ${aliased})
            endif()
            if(inner IN_LIST originals)
                set(item "${inner}_pic")
                if(linkOnly)
                    set(item "$<LINK_ONLY:${item}>")
                endif()
            endif()
        endif()
        list(APPEND mapped "${item}")
    endforeach()
    set(${outVar} "${mapped}" PARENT_SCOPE)
endfunction()

## @brief Declares <target>_pic for each static library target of the remaining arguments: its PIC twin.
##
## isoline_pic_twins(<target>... [EXTRA_OPTIONS <option>...])
## EXTRA_OPTIONS are compile options the twins get and their originals do not (they reach no executable).
## The twins link one another where their originals do. Clang-tidy runs on the originals alone, which
## compile the same sources.
function(isoline_pic_twins)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "" "EXTRA_OPTIONS")
    set(originals "")
    foreach(target IN LISTS arg_UNPARSED_ARGUMENTS)
        get_target_property(aliased ${target} ALIASED_TARGET)
        if(aliased)
            set(target ${aliased})
        endif()
        get_target_property(type ${target} TYPE)
        if(NOT type STREQUAL "STATIC_LIBRARY")
            message(FATAL_ERROR "isoline_pic_twins(${target}): ${type} is no static library")
        endif()
        list(APPEND originals ${target})
    endforeach()

    foreach(target IN LISTS originals)
        set(twin ${target}_pic)
        get_target_property(sourceDir ${target} SOURCE_DIR)
        get_target_property(sources ${target} SOURCES)
        set(absoluteSources "")
        foreach(source IN LISTS sources)
            if(NOT source MATCHES "^[$]<")
                cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY "${sourceDir}" NORMALIZE)
            endif()
            list(APPEND absoluteSources "${source}")
        endforeach()
        add_library(${twin} STATIC EXCLUDE_FROM_ALL ${absoluteSources})
        # A source's own options are properties of its target's directory, which the twin's is not: each is
        # copied there, so that the twin compiles every source as its original does.
        foreach(source IN LISTS absoluteSources)
            if(source MATCHES "^[$]<")
                continue()
            endif()
            foreach(sourceProperty IN ITEMS COMPILE_OPTIONS COMPILE_DEFINITIONS)
                get_property(value SOURCE "${source}" TARGET_DIRECTORY ${target} PROPERTY ${sourceProperty})
                if(value)
                    set_property(SOURCE "${source}" TARGET_DIRECTORY ${twin} PROPERTY ${sourceProperty} "${value}")
                endif()
            endforeach()
        endforeach()
        foreach(property IN LISTS _isolinePicTwinProperties)
            get_target_property(value ${target} ${property})
            if(NOT value STREQUAL "value-NOTFOUND")
                set_property(TARGET ${twin} PROPERTY ${property} "${value}")
            endif()
        endforeach()
        foreach(property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            get_target_property(value ${target} ${property})
            if(NOT value STREQUAL "value-NOTFOUND")
                _isoline_pic_twin_map("${value}" "${originals}" value)
                set_property(TARGET ${twin} PROPERTY ${property} "${value}")
            endif()
        endforeach()
        set_target_properties(${twin} PROPERTIES
            POSITION_INDEPENDENT_CODE ON
            CXX_CLANG_TIDY ""
            ISOLINE_PIC_TWIN_OF ${target})
        if(arg_EXTRA_OPTIONS)
            target_compile_options(${twin} PRIVATE ${arg_EXTRA_OPTIONS})
        endif()
    endforeach()
endfunction()
