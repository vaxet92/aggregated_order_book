# md_wire/CMakeLists.txt
find_package(cppzmq CONFIG REQUIRED)

add_library(md_wire STATIC
    wire_codec.cpp
    zmq_provider_sink.cpp
    zmq_core_ingress.cpp
)

target_include_directories(md_wire PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(md_wire PUBLIC
    md_core
    types
    logger
    cppzmq-static
)
