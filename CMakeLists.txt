cmake_minimum_required(VERSION 3.16)

project(
  libshell
  VERSION 0.5.13.5
  LANGUAGES CXX
)

# Statement expressions, the omitted-middle ?: operator, and alloca are part
# of this shell. gnu++20 keeps those extensions with the C++20 library.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

include(CheckCXXSourceCompiles)
include(CheckCXXSymbolExists)
include(CheckIncludeFileCXX)
include(CheckStructHasMember)
include(CheckSymbolExists)

set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)

option(BUILD_EXAMPLES "Whether to build example programs or not" OFF)
option(WITH_LINENO "Track LINENO" ON)
option(ENABLE_FNMATCH "Use fnmatch(3) from libc" OFF)
option(ENABLE_GLOB "Use glob(3) from libc" OFF)
option(ENABLE_TEE "Use tee(2) when it is available" ON)
option(ENABLE_MEMFD_CREATE "Use memfd_create(3) when it is available" ON)

function(dash_have_header header variable)
  check_include_file_cxx("${header}" "${variable}")
  set("${variable}" "${${variable}}" PARENT_SCOPE)
endfunction()

function(dash_have_symbol symbol header variable)
  check_cxx_symbol_exists("${symbol}" "${header}" "${variable}")
  set("${variable}" "${${variable}}" PARENT_SCOPE)
endfunction()

dash_have_header(alloca.h HAVE_ALLOCA_H)
dash_have_header(paths.h HAVE_PATHS_H)

if(HAVE_PATHS_H)
  dash_have_symbol(_PATH_BSHELL paths.h HAVE_DECL__PATH_BSHELL)
  dash_have_symbol(_PATH_DEVNULL paths.h HAVE_DECL__PATH_DEVNULL)
  dash_have_symbol(_PATH_TTY paths.h HAVE_DECL__PATH_TTY)
endif()

function(dash_probe variable includes code)
  set(source "${includes}\nint main() { ${code} }\n")
  check_cxx_source_compiles("${source}" "${variable}")
  set("${variable}" "${${variable}}" PARENT_SCOPE)
endfunction()

dash_probe(HAVE_FACCESSAT "#include <fcntl.h>\n#include <unistd.h>"
  "return faccessat(AT_FDCWD, \"/\", F_OK, 0);")
dash_probe(HAVE_GETPWNAM "#include <pwd.h>"
  "return getpwnam(\"root\") == nullptr;")
dash_probe(HAVE_GETRLIMIT "#include <sys/resource.h>"
  "struct rlimit r; return getrlimit(RLIMIT_NOFILE, &r);")
dash_probe(HAVE_MEMFD_CREATE "#include <sys/mman.h>"
  "return memfd_create(\"dash\", 0);")
dash_probe(HAVE_MEMRCHR "#include <cstring>"
  "return memrchr(\"a\", 'a', 1) == nullptr;")
dash_probe(HAVE_MEMPCPY "#include <cstring>"
  "char d[2]; return mempcpy(d, \"a\", 1) == nullptr;")
dash_probe(HAVE_SIGSETMASK "#include <csignal>"
  "return sigsetmask(0);")
dash_probe(HAVE_STRCHRNUL "#include <cstring>"
  "return strchrnul(\"a\", 'b') == nullptr;")
dash_probe(HAVE_STRSIGNAL "#include <cstring>"
  "return strsignal(0) == nullptr;")
dash_probe(HAVE_SYSCONF "#include <unistd.h>"
  "return sysconf(_SC_CLK_TCK) == 0;")
dash_probe(HAVE_TEE "#include <fcntl.h>"
  "return tee(0, 1, 0, 0);")
dash_probe(HAVE_WAIT3 "#include <sys/wait.h>"
  "return wait3(nullptr, WNOHANG, nullptr);")

check_cxx_symbol_exists(PRIdMAX cinttypes HAVE_PRIDMAX)
if(NOT HAVE_PRIDMAX)
  set(PRIdMAX "lld")
endif()

check_struct_has_member("struct stat" st_mtim sys/stat.h HAVE_ST_MTIM LANGUAGE CXX)

check_cxx_source_compiles("
#include <fcntl.h>
#include <unistd.h>
int main() { return fcntl(0, F_DUPFD_CLOEXEC, 0); }
" HAVE_F_DUPFD_CLOEXEC_PROBE)
if(HAVE_F_DUPFD_CLOEXEC_PROBE)
  set(HAVE_F_DUPFD_CLOEXEC 1)
else()
  set(HAVE_F_DUPFD_CLOEXEC 0)
endif()

check_cxx_source_compiles("
#include <sys/stat.h>
int main() {
  struct stat64 st;
  return stat64(\"/\", &st);
}
" HAVE_STAT64)

check_cxx_source_compiles("
#include <fcntl.h>
int main() { return open64(\"/\", 0); }
" HAVE_OPEN64)

check_cxx_source_compiles("
#include <glob.h>
int main() {
  glob64_t g{};
  return glob64(\"*\", 0, nullptr, &g);
}
" HAVE_GLOB64)

check_cxx_symbol_exists(signal csignal HAVE_SIGNAL)
if(NOT HAVE_SIGNAL)
  check_cxx_symbol_exists(bsd_signal csignal HAVE_BSD_SIGNAL_ALIAS)
endif()

if(CMAKE_SYSTEM_NAME MATCHES "FreeBSD|kFreeBSD")
  set(HAVE_TRADITIONAL_FACCESSAT 1)
endif()

if(ENABLE_FNMATCH)
  dash_have_symbol(fnmatch fnmatch.h HAVE_FNMATCH)
endif()
if(ENABLE_GLOB AND HAVE_FNMATCH)
  dash_have_symbol(glob glob.h HAVE_GLOB)
endif()

if(ENABLE_TEE AND HAVE_TEE)
  set(USE_TEE 1)
else()
  set(USE_TEE 0)
endif()

if(ENABLE_MEMFD_CREATE)
  set(USE_MEMFD_CREATE 1)
else()
  set(USE_MEMFD_CREATE 0)
endif()

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/config.hpp.in"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/config.hpp"
)

include(FetchContent)
FetchContent_Declare(
  expected
  GIT_REPOSITORY https://github.com/TartanLlama/expected.git
  GIT_TAG v1.1.0
)
set(EXPECTED_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(EXPECTED_BUILD_PACKAGE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(expected)

set(DASH_COMPILE_DEFS BSD=1 SHELL _GNU_SOURCE)
set(
  DASH_INCLUDE_DIRS
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_BINARY_DIR}"
)

function(dash_configure_tool target)
  target_compile_definitions("${target}" PRIVATE ${DASH_COMPILE_DEFS})
  target_include_directories("${target}" PRIVATE ${DASH_INCLUDE_DIRS})
  target_compile_options(
    "${target}"
    PRIVATE
      -Wall
      -Werror
  )
  # result.h (via parser.h / arith.h) needs tl::expected headers
  target_link_libraries("${target}" PRIVATE expected)
endfunction()

set(
  DASH_SOURCES
  src/alias.cpp
  src/arith.cpp
  src/builtins.cpp
  src/nodes.cpp
  src/cd.cpp
  src/error.cpp
  src/eval.cpp
  src/exec.cpp
  src/expand.cpp
  src/init.cpp
  src/input.cpp
  src/jobs.cpp
  src/main.cpp
  src/memalloc.cpp
  src/miscbltin.cpp
  src/mystring.cpp
  src/options.cpp
  src/output.cpp
  src/parser.cpp
  src/redir.cpp
  src/shell_context.cpp
  src/signames.cpp
  src/syntax.cpp
  src/system.cpp
  src/trap.cpp
  src/var.cpp
  src/bltin/printf.cpp
  src/bltin/test.cpp
  src/bltin/times.cpp
)

add_library(
  shell
  STATIC
    ${DASH_SOURCES}
)
dash_configure_tool(shell)
target_link_libraries(shell PUBLIC expected)

include(GNUInstallDirs)

target_include_directories(
  shell
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

install(
  TARGETS shell
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(
  FILES include/libshell.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

if(BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(BUILD_TESTING)
  enable_testing()
  add_subdirectory(tests)
endif()
