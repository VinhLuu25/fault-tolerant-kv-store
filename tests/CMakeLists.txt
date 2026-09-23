# Native tests use a standard-library harness to avoid another test-framework dependency.
function(ftkv_add_test target source)
    add_executable(${target} ${source})
    if(ARGN)
        target_link_libraries(${target} PRIVATE ${ARGN})
    else()
        target_link_libraries(${target} PRIVATE ftkv::core)
    endif()
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_compile_features(${target} PRIVATE cxx_std_20)

    if(FTKV_ENABLE_WARNINGS)
        if(MSVC)
            target_compile_options(${target} PRIVATE /W4 /permissive-)
        else()
            target_compile_options(
                ${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
            )
        endif()
    endif()

    add_test(NAME ${target} COMMAND ${target})
endfunction()

ftkv_add_test(ftkv_unit_tests key_value_store_test.cpp)
ftkv_add_test(ftkv_storage_tests kv_store_test.cpp)
ftkv_add_test(ftkv_raft_tests raft_test.cpp)
ftkv_add_test(ftkv_grpc_tests grpc_test.cpp ftkv::grpc)

add_test(NAME ftkv_version_test COMMAND ftkv_server --version)
set_tests_properties(
    ftkv_version_test
    PROPERTIES
        LABELS "unit;release"
        PASS_REGULAR_EXPRESSION "^ftkv_server ${PROJECT_VERSION}"
)

set_tests_properties(ftkv_unit_tests ftkv_storage_tests PROPERTIES LABELS "unit;storage")
set_tests_properties(ftkv_raft_tests PROPERTIES LABELS "unit;raft")
set_tests_properties(ftkv_grpc_tests PROPERTIES LABELS "unit;grpc")

if(FTKV_ENABLE_DOCKER_TESTS)
    unset(FTKV_DOCKER_CLI CACHE)
    unset(FTKV_BASH_CLI CACHE)
    find_program(FTKV_DOCKER_CLI NAMES docker HINTS ENV PATH NO_DEFAULT_PATH)
    find_program(FTKV_BASH_CLI NAMES bash HINTS ENV PATH NO_DEFAULT_PATH)
    if(FTKV_DOCKER_CLI AND FTKV_BASH_CLI)
        string(MD5 FTKV_TEST_BUILD_HASH "${CMAKE_BINARY_DIR}")
        string(SUBSTRING "${FTKV_TEST_BUILD_HASH}" 0 8 FTKV_TEST_BUILD_HASH)
        set(FTKV_TEST_PROJECT_NAME "ftkv-ctest-${FTKV_TEST_BUILD_HASH}")
        set(FTKV_CLUSTER_TEST_SCRIPT
            ${CMAKE_CURRENT_SOURCE_DIR}/integration/cluster_test.sh
        )
        set(FTKV_CLUSTER_TEST_ENV
            "FTKV_DOCKER_EXECUTABLE=${FTKV_DOCKER_CLI}"
            "FTKV_TEST_PROJECT_NAME=${FTKV_TEST_PROJECT_NAME}"
        )

        function(ftkv_add_cluster_test name action)
            add_test(
                NAME ${name}
                COMMAND
                    ${CMAKE_COMMAND} -E env ${FTKV_CLUSTER_TEST_ENV}
                    ${FTKV_BASH_CLI} ${FTKV_CLUSTER_TEST_SCRIPT} ${action}
            )
            set_tests_properties(
                ${name}
                PROPERTIES
                    LABELS "integration;docker"
                    RESOURCE_LOCK ftkv_docker_cluster
                    SKIP_RETURN_CODE 77
            )
        endfunction()

        ftkv_add_cluster_test(ftkv_cluster_startup startup)
        set_tests_properties(
            ftkv_cluster_startup
            PROPERTIES
                FIXTURES_SETUP ftkv_cluster
                TIMEOUT 600
        )

        ftkv_add_cluster_test(ftkv_cluster_leader_election election)
        set_tests_properties(
            ftkv_cluster_leader_election
            PROPERTIES
                DEPENDS ftkv_cluster_startup
                FIXTURES_REQUIRED ftkv_cluster
                TIMEOUT 90
        )

        ftkv_add_cluster_test(ftkv_cluster_node_failure_recovery recovery)
        set_tests_properties(
            ftkv_cluster_node_failure_recovery
            PROPERTIES
                DEPENDS ftkv_cluster_leader_election
                FIXTURES_REQUIRED ftkv_cluster
                TIMEOUT 120
        )

        ftkv_add_cluster_test(ftkv_cluster_cleanup cleanup)
        set_tests_properties(
            ftkv_cluster_cleanup
            PROPERTIES
                FIXTURES_CLEANUP ftkv_cluster
                LABELS "docker;fixture"
                TIMEOUT 60
        )
    else()
        message(STATUS "Docker or Bash was not found; Docker integration tests are unavailable")
    endif()
endif()
