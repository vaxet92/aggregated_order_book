# aggregator/CMakeLists.txt
add_library(aggregator STATIC
    wire_translation.cpp
    aggregator_service.cpp
)

target_include_directories(aggregator PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(aggregator PUBLIC
    md_core
    md_proto
)

# App-wiring shared by md_provider_app: turns config + endpoints into a
# constructed Provider. Kept out of the `aggregator` gRPC-translation library so
# that library stays free of the boost/asio dependency md_provider pulls in, and
# so the core-only binary can build without it.
add_library(md_app STATIC
    provider_factory.cpp
)

target_include_directories(md_app PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(md_app PUBLIC
    md_provider
    config
    logger
    types
)

# One provider process for one (venue, market), talking to md_core over the
# DEALER/ROUTER socket instead of an in-process callback. No gRPC.
add_executable(md_provider_app
    md_provider_main.cpp
)

target_link_libraries(md_provider_app PRIVATE
    md_app
    md_wire
    md_provider
    config
    logger
)

# The consolidation + publish process: Core + gRPC + one ZmqCoreIngress bound on
# the ROUTER. No providers, so no md_app / md_provider. `aggregator`
# already brings md_core, md_proto and gRPC.
add_executable(md_core_app
    md_core_main.cpp
)

target_link_libraries(md_core_app PRIVATE
    aggregator
    md_wire
    config
    logger
)
