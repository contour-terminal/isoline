# SPDX-License-Identifier: Apache-2.0

# isoline::fastinterpreter: the fast interpreter, which runs code lowered once, at load, from a
# validated module into register-based operations over frame slots.

# The dispatch rows: the dispatches this build compiles, from the most portable to the fastest.
# Switch is portable C++ and always the first row.
set(isolineFastDispatchRows Switch)
list(TRANSFORM isolineFastDispatchRows PREPEND "Dispatch::" OUTPUT_VARIABLE isolineFastDispatches)
list(JOIN isolineFastDispatches ", " ISOLINE_FAST_DISPATCHES)
message(STATUS "[isoline] fast interpreter dispatches: ${isolineFastDispatchRows}")
configure_file(DispatchAvailability.hpp.in
               "${PROJECT_BINARY_DIR}/include/isoline/fastinterpreter/DispatchAvailability.hpp" @ONLY)

target_sources(isoline_core
    PRIVATE
        Code.cpp
        Dispatch.cpp
        Listing.cpp
        Lowering.cpp
        LoweringArena.cpp
        SlotCodec.cpp
    PUBLIC FILE_SET HEADERS
        BASE_DIRS "${PROJECT_SOURCE_DIR}/src" "${PROJECT_BINARY_DIR}/include"
        FILES
            Code.hpp
            Dispatch.hpp
            LittleEndian.hpp
            Listing.hpp
            Lowering.hpp
            LoweringArena.hpp
            LoweringTable.hpp
            Operations.def
            SlotCodec.hpp
            "${PROJECT_BINARY_DIR}/include/isoline/fastinterpreter/DispatchAvailability.hpp")
