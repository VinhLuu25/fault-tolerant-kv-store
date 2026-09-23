# Tests intentionally use only the standard library so a fresh checkout remains easy to build.
function(ftkv_add_test target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE ftkv::core)
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
