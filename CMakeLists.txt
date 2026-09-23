cmake_minimum_required(VERSION 3.20)

project(
    fault_tolerant_kv_store
    VERSION 0.1.0
    DESCRIPTION "A fault-tolerant distributed key-value store"
    LANGUAGES CXX
)

include(GNUInstallDirs)
find_package(Threads REQUIRED)
find_package(Protobuf CONFIG QUIET)
if(NOT Protobuf_FOUND)
    find_package(Protobuf REQUIRED)
endif()
find_package(gRPC CONFIG REQUIRED)

option(FTKV_ENABLE_WARNINGS "Enable strict compiler warnings" ON)
option(FTKV_ENABLE_DOCKER_TESTS "Enable Docker Compose integration tests" ON)

# Set one language standard for every target in this project.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(FTKV_PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/proto/kvstore.proto)
set(FTKV_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(FTKV_PROTO_SOURCES
    ${FTKV_GENERATED_DIR}/kvstore.pb.cc
    ${FTKV_GENERATED_DIR}/kvstore.pb.h
    ${FTKV_GENERATED_DIR}/kvstore.grpc.pb.cc
    ${FTKV_GENERATED_DIR}/kvstore.grpc.pb.h
)
file(MAKE_DIRECTORY ${FTKV_GENERATED_DIR})

add_custom_command(
    OUTPUT ${FTKV_PROTO_SOURCES}
    COMMAND $<TARGET_FILE:protobuf::protoc>
    ARGS
        --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
        --cpp_out=${FTKV_GENERATED_DIR}
        --grpc_out=${FTKV_GENERATED_DIR}
        --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
        ${FTKV_PROTO_FILE}
    DEPENDS ${FTKV_PROTO_FILE} protobuf::protoc gRPC::grpc_cpp_plugin
    COMMENT "Generating C++ gRPC bindings for kvstore.proto"
    VERBATIM
)

add_library(ftkv_proto STATIC ${FTKV_PROTO_SOURCES})
add_library(ftkv::proto ALIAS ftkv_proto)
target_include_directories(ftkv_proto PUBLIC ${FTKV_GENERATED_DIR})
target_link_libraries(ftkv_proto PUBLIC protobuf::libprotobuf gRPC::grpc++)
target_compile_features(ftkv_proto PUBLIC cxx_std_20)

add_library(ftkv_core STATIC
    src/raft/raft.cpp
    src/raft/raft.h
    src/raft/raft_node.cpp
    src/raft/raft_node.h
    src/store/in_memory_key_value_store.cpp
    src/storage/kv_store.cpp
    src/storage/kv_store.h
)
add_library(ftkv::core ALIAS ftkv_core)

target_include_directories(ftkv_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(ftkv_core PUBLIC cxx_std_20)
target_link_libraries(ftkv_core PUBLIC Threads::Threads)

add_library(ftkv_grpc STATIC
    src/grpc/grpc_client.cpp
    src/grpc/grpc_client.h
    src/grpc/grpc_server.cpp
    src/grpc/grpc_server.h
)
add_library(ftkv::grpc ALIAS ftkv_grpc)
target_include_directories(ftkv_grpc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ftkv_grpc PUBLIC ftkv::core ftkv::proto gRPC::grpc++)
target_compile_features(ftkv_grpc PUBLIC cxx_std_20)

add_executable(
    ftkv_server
    src/main.cpp
    src/node/node_runtime.cpp
    src/node/node_runtime.h
)
target_link_libraries(ftkv_server PRIVATE ftkv::grpc)

if(FTKV_ENABLE_WARNINGS)
    if(MSVC)
        target_compile_options(ftkv_core PRIVATE /W4 /permissive-)
        target_compile_options(ftkv_grpc PRIVATE /W4 /permissive-)
        target_compile_options(ftkv_server PRIVATE /W4 /permissive-)
    else()
        set(FTKV_WARNING_FLAGS -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
        target_compile_options(ftkv_core PRIVATE ${FTKV_WARNING_FLAGS})
        target_compile_options(ftkv_grpc PRIVATE ${FTKV_WARNING_FLAGS})
        target_compile_options(ftkv_server PRIVATE ${FTKV_WARNING_FLAGS})
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(
    TARGETS ftkv_core ftkv_server
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
