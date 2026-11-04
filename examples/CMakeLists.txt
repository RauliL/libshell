add_executable(
  shell
  ./shell.cpp
)

dash_configure_tool(shell)

target_link_libraries(
  shell
  PRIVATE
    dash_lib
)
