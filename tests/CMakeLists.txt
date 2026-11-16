include(FetchContent)
FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest
  GIT_TAG v2.5.3
)
FetchContent_MakeAvailable(doctest)

include("${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")

add_executable(detour_tests
    test.cpp
)

target_compile_features(detour_tests PRIVATE cxx_std_23)

target_compile_options(detour_tests PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /utf-8>
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -fno-rtti
        -fno-unwind-tables -fno-asynchronous-unwind-tables>
)

target_link_libraries(detour_tests
    PRIVATE
        ReZero::ReZeroDetour
        doctest::doctest_with_main
)

doctest_discover_tests(
    detour_tests
    PROPERTIES
        LABELS "unit"
)
