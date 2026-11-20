# mini-cache —— CMakeLists.txt(M0)
#
# 用法:
#   cmake -S . -B build            配置(生成 build/)
#   cmake --build build            编译服务器 build/mini-cache
#   ctest --test-dir build         编译并运行单元测试
#   ./build/mini-cache             启动服务器
#   cmake -S . -B build -DDEBUG=1  ASan/UBSan 调试构建(docs/development-plan.md §6)
#   rm -rf build                   清理构建产物

cmake_minimum_required(VERSION 3.13)
project(mini-cache C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)  # 严格 -std=c17

# 与原 Makefile 一致:默认 -O2;DEBUG=1 时 -g -O0
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_C_FLAGS_RELEASE "-O2")
set(CMAKE_C_FLAGS_DEBUG   "-g -O0")

# -std=c17 严格模式下,POSIX 声明(gettimeofday/localtime_r、后续的 socket/epoll)
# 需要显式开启特性宏才会暴露
add_compile_definitions(_POSIX_C_SOURCE=200809L)

add_compile_options(-Wall -Wextra -Werror)

# make DEBUG=1 的等价物(-DDEBUG=1)
option(DEBUG "ASan/UBSan 调试构建" OFF)
if(DEBUG)
  set(CMAKE_BUILD_TYPE Debug)
  add_compile_options(-fsanitize=address,undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

include_directories(src)

add_executable(mini-cache
  src/main.c
  src/util.c
  src/sds.c
)

enable_testing()

add_executable(test_util
  tests/test_util.c
  src/util.c
)
add_test(NAME test_util COMMAND test_util)

add_executable(test_sds
  tests/test_sds.c
  src/sds.c
  src/util.c
)
add_test(NAME test_sds COMMAND test_sds)
