target_sources(isoline_core
    PRIVATE
        api/Engine.cpp
    PUBLIC FILE_SET HEADERS
        BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
        FILES
            api/Engine.hpp
            "${PROJECT_BINARY_DIR}/include/isoline/VersionConfig.hpp")
