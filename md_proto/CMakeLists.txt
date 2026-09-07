cmake_minimum_required(VERSION 3.20)
# md_proto/CMakeLists.txt
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

add_library(md_proto STATIC)

protobuf_generate(
    TARGET md_proto
    LANGUAGE cpp
    PROTOS aggregator.proto
)

protobuf_generate(
    TARGET md_proto
    LANGUAGE grpc
    GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
    PLUGIN "protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
    PROTOS aggregator.proto
)

target_include_directories(md_proto PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(md_proto PUBLIC
    protobuf::libprotobuf
    gRPC::grpc++
)