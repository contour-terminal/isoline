target_sources(isoline_core
    PRIVATE
        api/Extern.cpp
    PUBLIC FILE_SET HEADERS
        BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
        FILES
            api/Extern.hpp
            "${PROJECT_BINARY_DIR}/include/isoline/Contracts.hpp")
