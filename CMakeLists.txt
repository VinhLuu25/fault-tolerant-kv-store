cmake_minimum_required(VERSION 3.20)

project(
    fault_tolerant_kv_store
    VERSION 0.1.0
    DESCRIPTION "A fault-tolerant distributed key-value store"
    LANGUAGES CXX
)

include(GNUInstallDirs)
find_package(Threads REQUIRED)

option(FTKV_ENABLE_WARNINGS "Enable strict compiler warnings" ON)

# Set one language standard for every target in this project.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(ftkv_core STATIC
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

add_executable(ftkv_server src/main.cpp)
target_link_libraries(ftkv_server PRIVATE ftkv::core)

if(FTKV_ENABLE_WARNINGS)
    if(MSVC)
        target_compile_options(ftkv_core PRIVATE /W4 /permissive-)
        target_compile_options(ftkv_server PRIVATE /W4 /permissive-)
    else()
        set(FTKV_WARNING_FLAGS -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
        target_compile_options(ftkv_core PRIVATE ${FTKV_WARNING_FLAGS})
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
