cmake_minimum_required(VERSION 3.10)
project(kvspace VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

include(GNUInstallDirs)

# 默认 prefix=/usr（与 kvspace-c 对齐），前端与后端同装 <prefix>/lib/kvspace。
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "" FORCE)
endif()

add_library(kvspace SHARED src/frontend.c src/codec_abi.c src/xvalue_head.c)

include(CTest)
if(BUILD_TESTING)
    add_executable(test_xvalue_head tests/test_xvalue_head.c)
    target_include_directories(test_xvalue_head PRIVATE src include)
    target_link_libraries(test_xvalue_head PRIVATE kvspace)
    add_test(NAME xvalue_head COMMAND test_xvalue_head)
    add_executable(test_codec_abi tests/test_codec_abi.c)
    target_include_directories(test_codec_abi PRIVATE src include)
    target_link_libraries(test_codec_abi PRIVATE kvspace)
    add_test(NAME codec_abi COMMAND test_codec_abi)
endif()

# 前端 SONAME：libkvspace.so.1
set_target_properties(kvspace PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)

target_include_directories(kvspace
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# 活跃句柄表（退出兜底落盘）用 pthread 互斥；dlopen/dlsym 的宿主库 Linux 为 libdl，
# macOS 在 libSystem 内（CMAKE_DL_LIBS 两者皆正确）。
find_package(Threads REQUIRED)
target_link_libraries(kvspace PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)

target_compile_definitions(kvspace PRIVATE _GNU_SOURCE)

install(TARGETS kvspace
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/kvspace
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

configure_file(kvspace.pc.in ${CMAKE_CURRENT_BINARY_DIR}/kvspace.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/kvspace.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# ── CLI（cli/ 独立 Cargo 项目）──────────────────────────────────────
# 走本仓 dispatch 前端的 C ABI，故须在 kvspace 目标之后构建；
# 安装到 <prefix>/bin（prefix 默认 /usr → /usr/bin，与 kvlang 的 CLI 一致）。
# Embed relative RUNPATH entries:
#   $ORIGIN/../lib                      -- release tarball (bin/ + lib/)
#   $ORIGIN/../<libdir>/kvspace         -- cmake --install (<prefix>/<libdir>/kvspace)
# Use @loader_path on macOS; escape $ as $$ for CMake.
# release.yml packages this binary directly without rebuilding.
find_program(CARGO_EXECUTABLE cargo)
if(CARGO_EXECUTABLE)
  set(KVSPACE_CLI_BIN "${CMAKE_CURRENT_SOURCE_DIR}/cli/target/release/kvspace")
  if(APPLE)
    set(KVSPACE_CLI_ORIGIN "@loader_path")
  else()
    set(KVSPACE_CLI_ORIGIN "$$ORIGIN")
  endif()
  add_custom_target(kvspace-cli ALL
    COMMAND ${CMAKE_COMMAND} -E env
            "RUSTFLAGS=-L native=$<TARGET_FILE_DIR:kvspace> -C link-arg=-Wl,-rpath,'${KVSPACE_CLI_ORIGIN}/../lib' -C link-arg=-Wl,-rpath,'${KVSPACE_CLI_ORIGIN}/../${CMAKE_INSTALL_LIBDIR}/kvspace'"
            "${CARGO_EXECUTABLE}" build --release
            --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/cli/Cargo.toml"
    BYPRODUCTS "${KVSPACE_CLI_BIN}"
    DEPENDS kvspace
    COMMENT "构建 kvspace CLI（cli/）")
  install(PROGRAMS "${KVSPACE_CLI_BIN}" DESTINATION ${CMAKE_INSTALL_BINDIR})
else()
  message(STATUS "未找到 cargo，跳过 kvspace CLI（cli/）")
endif()
