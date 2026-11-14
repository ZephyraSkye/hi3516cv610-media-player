# =============================================================================
# apps/cv610_ev_board/samples/media_player/dts/CMakeLists.txt
#
# 案例侧 DTS 入口：引用 bsp/dts/CMakeLists.txt 完成 DTB 编译 + FIT 镜像组装。
#
# bsp/dts/CMakeLists.txt 所需外部变量（IMAGE_NAME / DTB_FILE / ITS_FILE /
# MKIMAGE_DIR / PUB_IMAGE）由上层构建系统（sdk/build.cmake）注入；
# 本文件不重复定义，仅做转发。
# =============================================================================

# 用绝对路径，避免案例所在层级不同导致相对路径算错
set(_BSP_DTS_DIR "${CMAKE_SOURCE_DIR}/apps/cv610_ev_board/bsp/dts")
cmake_path(NORMAL_PATH _BSP_DTS_DIR OUTPUT_VARIABLE _BSP_DTS_DIR)

if(NOT EXISTS "${_BSP_DTS_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "[media_player/dts] bsp/dts/CMakeLists.txt not found: ${_BSP_DTS_DIR}")
endif()

# fit_assemble 的落盘目录（${SDK_BSP_PUB_DIR}/${PUB_IMAGE}）。
# 不指定时 PUB_IMAGE 为空，新编的 uImage 会落到 pub/ 根下，
# 而 rootfs 组装读的是 pub/<chip>_image_<debug|release>_musl/，
# 结果就是：DTB 编对了、FIT 也打了，但打进镜像的仍是旧 uImage。
if(NOT DEFINED PUB_IMAGE OR PUB_IMAGE STREQUAL "")
    set(_mp_dbg OFF)
    if(EXISTS "${CMAKE_SOURCE_DIR}/.config")
        file(STRINGS "${CMAKE_SOURCE_DIR}/.config" _mp_dbg_line
             REGEX "^CONFIG_SDK_BSP_DEBUG=y$")
        if(_mp_dbg_line)
            set(_mp_dbg ON)
        endif()
    endif()
    if(_mp_dbg)
        set(PUB_IMAGE "hi3516cv610_image_debug_musl")
    else()
        set(PUB_IMAGE "hi3516cv610_image_musl")
    endif()
    message(STATUS "[media_player/dts] PUB_IMAGE=${PUB_IMAGE}")
endif()

# 引用 bsp/dts 子工程
# 设置 APP_DTS_DIR：本目录下的同名 dts/dtsi 将优先于 bsp/dts 被使用。
set(APP_DTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

add_subdirectory("${_BSP_DTS_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/bsp_dts")
