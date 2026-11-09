add_executable(
  shell-example
  ./shell.cpp
)

dash_configure_tool(shell-example)

target_link_libraries(
  shell-example
  PRIVATE
    shell
)
