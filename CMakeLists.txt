cmake_minimum_required(VERSION 3.16)

project(dash VERSION 0.5.13.5 LANGUAGES CXX)

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

option(WITH_LIBEDIT "Build with libedit line editing" OFF)
option(WITH_LINENO "Track LINENO" ON)
option(ENABLE_FNMATCH "Use fnmatch(3) from libc" OFF)
option(ENABLE_GLOB "Use glob(3) from libc" OFF)
option(ENABLE_TEE "Use tee(2) when it is available" ON)
option(ENABLE_MEMFD_CREATE "Use memfd_create(3) when it is available" ON)
option(ENABLE_STATIC "Link the shell statically" OFF)

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
dash_have_header(sys/wait.h HAVE_SYS_WAIT_H)

if(HAVE_PATHS_H)
  dash_have_symbol(_PATH_BSHELL paths.h HAVE_DECL__PATH_BSHELL)
  dash_have_symbol(_PATH_DEVNULL paths.h HAVE_DECL__PATH_DEVNULL)
  dash_have_symbol(_PATH_TTY paths.h HAVE_DECL__PATH_TTY)
endif()

dash_have_symbol(isblank cctype HAVE_DECL_ISBLANK)
if(HAVE_DECL_ISBLANK)
  set(HAVE_DECL_ISBLANK 1)
else()
  set(HAVE_DECL_ISBLANK 0)
endif()

function(dash_probe variable includes code)
  set(source "${includes}\nint main() { ${code} }\n")
  check_cxx_source_compiles("${source}" "${variable}")
  set("${variable}" "${${variable}}" PARENT_SCOPE)
endfunction()

dash_probe(HAVE_BSEARCH "#include <cstdlib>"
  "const char *k = \"a\"; return std::bsearch(&k, &k, 1, sizeof k, nullptr) == nullptr;")
dash_probe(HAVE_FACCESSAT "#include <fcntl.h>\n#include <unistd.h>"
  "return faccessat(AT_FDCWD, \"/\", F_OK, 0);")
dash_probe(HAVE_GETPWNAM "#include <pwd.h>"
  "return getpwnam(\"root\") == nullptr;")
dash_probe(HAVE_GETRLIMIT "#include <sys/resource.h>"
  "struct rlimit r; return getrlimit(RLIMIT_NOFILE, &r);")
dash_probe(HAVE_ISALPHA "#include <cctype>"
  "return std::isalpha(static_cast<unsigned char>('a'));")
dash_probe(HAVE_KILLPG "#include <csignal>"
  "return killpg(0, 0);")
dash_probe(HAVE_MEMFD_CREATE "#include <sys/mman.h>"
  "return memfd_create(\"dash\", 0);")
dash_probe(HAVE_MEMRCHR "#include <cstring>"
  "return memrchr(\"a\", 'a', 1) == nullptr;")
dash_probe(HAVE_MEMPCPY "#include <cstring>"
  "char d[2]; return mempcpy(d, \"a\", 1) == nullptr;")
dash_probe(HAVE_SIGSETMASK "#include <csignal>"
  "return sigsetmask(0);")
dash_probe(HAVE_STPCPY "#include <cstring>"
  "char d[2]; return stpcpy(d, \"\") == nullptr;")
dash_probe(HAVE_STRCHRNUL "#include <cstring>"
  "return strchrnul(\"a\", 'b') == nullptr;")
dash_probe(HAVE_STRSIGNAL "#include <cstring>"
  "return strsignal(0) == nullptr;")
dash_probe(HAVE_STRTOD "#include <cstdlib>"
  "char *end = nullptr; return std::strtod(\"1\", &end) == 0;")
dash_probe(HAVE_STRTOIMAX "#include <cinttypes>"
  "char *end = nullptr; return std::strtoimax(\"1\", &end, 10) == 0;")
dash_probe(HAVE_STRTOUMAX "#include <cinttypes>"
  "char *end = nullptr; return std::strtoumax(\"1\", &end, 10) == 0;")
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

if(WITH_LIBEDIT)
  find_path(EDIT_INCLUDE_DIR histedit.h)
  find_library(EDIT_LIBRARY edit)
  if(NOT EDIT_INCLUDE_DIR OR NOT EDIT_LIBRARY)
    message(FATAL_ERROR "libedit was requested but histedit.h or -ledit was not found")
  endif()
else()
  set(SMALL 1)
endif()

configure_file(config.h.cmake config.h)

set(DASH_COMPILE_DEFS BSD=1 SHELL _GNU_SOURCE)
set(DASH_INCLUDE_DIRS
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_BINARY_DIR}")

function(dash_configure_tool target)
  target_compile_definitions("${target}" PRIVATE ${DASH_COMPILE_DEFS})
  target_include_directories("${target}" PRIVATE ${DASH_INCLUDE_DIRS})
  target_compile_options("${target}" PRIVATE -Wall
    "SHELL:-include ${CMAKE_CURRENT_BINARY_DIR}/config.h")
endfunction()

set(DASH_SOURCES
  src/alias.cpp
  src/arith_yacc.cpp
  src/arith_yylex.cpp
  src/cd.cpp
  src/error.cpp
  src/eval.cpp
  src/exec.cpp
  src/expand.cpp
  src/histedit.cpp
  src/input.cpp
  src/jobs.cpp
  src/mail.cpp
  src/main.cpp
  src/memalloc.cpp
  src/miscbltin.cpp
  src/mystring.cpp
  src/options.cpp
  src/output.cpp
  src/parser.cpp
  src/redir.cpp
  src/show.cpp
  src/system.cpp
  src/trap.cpp
  src/var.cpp
  src/bltin/printf.cpp
  src/bltin/test.cpp
  src/bltin/times.cpp
)

set(MKINIT_INPUTS)
foreach(src ${DASH_SOURCES})
  list(APPEND MKINIT_INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/${src}")
endforeach()

add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/token.h"
         "${CMAKE_CURRENT_BINARY_DIR}/token_vars.h"
  COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/src/mktokens"
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/mktokens"
  COMMENT "Generating token tables"
)

add_executable(mkinit src/mkinit.cpp)
add_executable(mknodes src/mknodes.cpp)
add_executable(mksyntax src/mksyntax.cpp "${CMAKE_CURRENT_BINARY_DIR}/token.h")
add_executable(mksignames src/mksignames.cpp)
dash_configure_tool(mkinit)
dash_configure_tool(mknodes)
dash_configure_tool(mksyntax)
dash_configure_tool(mksignames)

add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/builtins.def"
  COMMAND "${CMAKE_CXX_COMPILER}" -E -P -x c++
          -D_GNU_SOURCE -DJOBS=1 -DBSD=1 -DSHELL
          -include "${CMAKE_CURRENT_BINARY_DIR}/config.h"
          "${CMAKE_CURRENT_SOURCE_DIR}/src/builtins.def.in"
          -o "${CMAKE_CURRENT_BINARY_DIR}/builtins.def"
  DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/config.h"
          "${CMAKE_CURRENT_SOURCE_DIR}/src/builtins.def.in"
  COMMENT "Preprocessing builtin table"
)

add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/builtins.cpp"
         "${CMAKE_CURRENT_BINARY_DIR}/builtins.h"
  COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/src/mkbuiltins"
          "${CMAKE_CURRENT_BINARY_DIR}/builtins.def"
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/mkbuiltins"
          "${CMAKE_CURRENT_BINARY_DIR}/builtins.def"
  COMMENT "Generating builtin command table"
)

add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/init.cpp"
  COMMAND mkinit ${MKINIT_INPUTS}
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  DEPENDS mkinit ${MKINIT_INPUTS}
  COMMENT "Generating init.cpp"
)

add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/nodes.cpp"
         "${CMAKE_CURRENT_BINARY_DIR}/nodes.h"
  COMMAND mknodes
          "${CMAKE_CURRENT_SOURCE_DIR}/src/nodetypes"
          "${CMAKE_CURRENT_SOURCE_DIR}/src/nodes.c.pat"
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  DEPENDS mknodes
          "${CMAKE_CURRENT_SOURCE_DIR}/src/nodetypes"
          "${CMAKE_CURRENT_SOURCE_DIR}/src/nodes.c.pat"
  COMMENT "Generating parse-node tables"
)

add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/syntax.cpp"
         "${CMAKE_CURRENT_BINARY_DIR}/syntax.h"
  COMMAND mksyntax
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  DEPENDS mksyntax
  COMMENT "Generating syntax tables"
)

add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/signames.cpp"
  COMMAND mksignames "${CMAKE_CURRENT_BINARY_DIR}/signames.cpp"
  DEPENDS mksignames
  COMMENT "Generating signal names"
)

set_source_files_properties(
  ${DASH_SOURCES}
  "${CMAKE_CURRENT_BINARY_DIR}/builtins.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/init.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/nodes.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/signames.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/syntax.cpp"
  PROPERTIES OBJECT_DEPENDS
  "${CMAKE_CURRENT_BINARY_DIR}/token.h;${CMAKE_CURRENT_BINARY_DIR}/nodes.h;${CMAKE_CURRENT_BINARY_DIR}/builtins.h;${CMAKE_CURRENT_BINARY_DIR}/syntax.h")

add_executable(dash
  ${DASH_SOURCES}
  "${CMAKE_CURRENT_BINARY_DIR}/builtins.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/init.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/nodes.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/signames.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/syntax.cpp"
)
dash_configure_tool(dash)
if(WITH_LIBEDIT)
  target_include_directories(dash PRIVATE "${EDIT_INCLUDE_DIR}")
  target_link_libraries(dash PRIVATE "${EDIT_LIBRARY}")
endif()
if(ENABLE_STATIC)
  target_link_options(dash PRIVATE -static)
endif()

install(TARGETS dash RUNTIME DESTINATION bin)
install(FILES src/dash.1 DESTINATION share/man/man1)

enable_testing()
add_test(NAME smoke COMMAND dash -c "echo ok")
set_tests_properties(smoke PROPERTIES PASS_REGULAR_EXPRESSION "^ok\n$")
