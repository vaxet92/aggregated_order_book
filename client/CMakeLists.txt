# client/CMakeLists.txt

# Shared client plumbing: connection lifecycle, backoff, gap detection, and
# scaled-value formatting. Extracted once three binaries needed it - the gap
# detection in particular had already drifted between copies.
add_library(client_common STATIC
    client_common.cpp
)

target_include_directories(client_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(client_common PUBLIC
    md_proto
    logger
)

# The production client: any combination of feeds, selected on the command
# line (--bbo / --notional_band / --price_band).
add_executable(client_app client_app.cpp)
target_link_libraries(client_app PRIVATE client_common)

# Fixed-subscription test binaries, one feed each, no flags. They exercise a
# single feed in isolation - useful for reproducing a problem without argument
# parsing in the way. Not what ships.
add_executable(bbo_sub bbo_sub.cpp)
target_link_libraries(bbo_sub PRIVATE client_common)

add_executable(volume_band_sub volume_band_sub.cpp)
target_link_libraries(volume_band_sub PRIVATE client_common)

add_executable(price_band_sub price_band_sub.cpp)
target_link_libraries(price_band_sub PRIVATE client_common)
