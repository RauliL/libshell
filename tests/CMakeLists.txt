include(CTest)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY
    https://github.com/catchorg/Catch2.git
  GIT_TAG
    v3.8.1
)
FetchContent_MakeAvailable(Catch2)

add_executable(
  parse_tests
  ./parse/parse_tests.cpp
  ./parse/ast_tests.cpp
)
target_include_directories(
  parse_tests
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/parse"
)
target_compile_definitions(
  parse_tests
  PRIVATE
  "PARSE_FIXTURE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/parse\""
)
target_link_libraries(
  parse_tests
  PRIVATE
    dash_lib
    Catch2::Catch2WithMain
)
dash_configure_tool(parse_tests)

add_executable(
  arith_tests
  ./arith/arith_eval_tests.cpp
)
target_link_libraries(
  arith_tests
  PRIVATE
    dash_lib
    Catch2::Catch2WithMain
)
dash_configure_tool(arith_tests)

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)
catch_discover_tests(parse_tests)
catch_discover_tests(arith_tests)
