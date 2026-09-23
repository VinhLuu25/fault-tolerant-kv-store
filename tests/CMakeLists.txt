# Tests intentionally use only the standard library so a fresh checkout remains easy to build.
add_executable(ftkv_unit_tests key_value_store_test.cpp)
target_link_libraries(ftkv_unit_tests PRIVATE ftkv::core)
target_compile_features(ftkv_unit_tests PRIVATE cxx_std_20)

if(FTKV_ENABLE_WARNINGS)
    if(MSVC)
        target_compile_options(ftkv_unit_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(
            ftkv_unit_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
endif()

add_test(NAME ftkv.unit COMMAND ftkv_unit_tests)
