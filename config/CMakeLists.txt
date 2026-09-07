# config/CMakeLists.txt
find_package(simdjson CONFIG REQUIRED)

add_library(config STATIC
    config.cpp
    venues_config.cpp
)

target_include_directories(config PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(config PUBLIC
    types
)

# PRIVATE: simdjson is an implementation detail of config.cpp's JSON parsing.
# ServerConfig, ConfigLoadResult and every declaration in config.h are plain
# types with no simdjson in their signatures, so nothing that links config
# needs simdjson's headers on its own include path.
target_link_libraries(config PRIVATE
    simdjson::simdjson
)
