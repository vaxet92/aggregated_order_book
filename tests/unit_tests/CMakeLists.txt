# unit_tests/CMakeLists.txt
find_package(GTest REQUIRED)

add_executable(unit_tests
    src/test_flat_order_book.cpp
    src/test_consolidated_bbo.cpp
    src/test_binance_parser.cpp
    src/test_bybit_parser.cpp
    src/test_okx_parser.cpp
    src/test_conflated_channel.cpp
    src/test_wire_translation.cpp
    src/test_continuity.cpp
    src/test_seq_dedup.cpp
    src/test_consolidated_book.cpp
    src/test_consolidated_state.cpp
    src/test_venue_health.cpp
    src/test_venue_registry.cpp
    src/test_instrument_registry.cpp
    src/test_core_venue_lifecycle.cpp
    src/test_spsc_queue.cpp
    src/test_provider_message.cpp
    src/test_core_queued_path.cpp
    src/test_config_json.cpp
    src/test_aggregator_routing.cpp
    src/test_wire_codec.cpp
    src/test_zmq_provider_sink.cpp
    src/test_zmq_core_ingress.cpp
)

target_link_libraries(unit_tests PRIVATE
    GTest::gtest_main
    aggregator
    md_provider
    md_wire
    config
)

include(GoogleTest)

# Under the thread sanitizer, point TSan at the checked-in suppressions file
# (uninstrumented gRPC internals only - see tsan.supp). No effect on the
# Release/ASan builds, which never set SANITIZER=thread.
if(SANITIZER STREQUAL "thread")
    gtest_discover_tests(unit_tests
        PROPERTIES ENVIRONMENT "TSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/tsan.supp")
else()
    gtest_discover_tests(unit_tests)
endif()
