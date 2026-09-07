# tests/mock_tests/CMakeLists.txt
add_executable(multiple_venue_ws
    multiple_venue_ws.cpp
)

target_link_libraries(multiple_venue_ws PRIVATE
    md_provider
)
