# =============================================================================
# apps/cv610_ev_board/samples/base/rootfs/CMakeLists.txt
#
# base rootfs 发布：从 BSP rootfs 骨架组装临时 rootfs，并重新生成 251M UBIFS。
#
#   [build]   cmake --build ... --target rootfs_prepare
#   [install] cmake --install build（由 build.sh 自动触发）
#               1. 触发 rootfs_prepare（组装临时 rootfs）
#               2. mkubiimg.sh 生成 rootfs_hi3516cv610_2k_128k_251M.ubifs
#               3. 拷贝 BSP 镜像骨架（排除其他 rootfs/nor_*）到
#                  CMAKE_INSTALL_PREFIX/image/media_player/
#               4. 用 nand_env.txt 自产 nand_env.bin / nand_burn_table.xml（251M 分区）
#
# 在 BSP rootfs 之上额外覆盖/部署：
#   - dropbear SSH 服务（/sbin/dropbear、/bin/dropbearkey 等）
#   - S80network（udhcpc DHCP）/ S90dropbear / udhcpc default.script
#   - MPP ko → /drv/ko、SDK 动态库 → /lib；
#   - HelloWorld → /root/hello（不自动执行，仅手动运行）
#
# 前提：BSP 已编译；CONFIG_OS_DROPBEAR=y 已开启（app.defconfig）；
# =============================================================================

# -----------------------------------------------------------------------------
# UBI 镜像参数（251M，与 base 的 nand_env.txt / nand_burn_table.xml 保持一致）
# -----------------------------------------------------------------------------
set(UBI_CHIP       "hi3516cv610" CACHE STRING "UBI chip name")
set(UBI_PAGE_SIZE  "2k"          CACHE STRING "UBI NAND page size")
set(UBI_BLOCK_SIZE "128k"        CACHE STRING "UBI NAND block size")
set(UBI_FS_SIZE    "251M"         CACHE STRING "UBIFS partition size")

set(_ROOTFS_IMG_NAME
    "rootfs_${UBI_CHIP}_${UBI_PAGE_SIZE}_${UBI_BLOCK_SIZE}_${UBI_FS_SIZE}.ubifs")

# -----------------------------------------------------------------------------
# 路径定义
# -----------------------------------------------------------------------------
set(_ROOTFS_PRIV_PATH "${CMAKE_CURRENT_SOURCE_DIR}/rootfs_priv")
set(_ROOTFS_TMP "${CMAKE_BINARY_DIR}/rootfs/base_rootfs_tmp")
set(_UBIIMG_SH  "${SDK_UBIIMG_TOOL}")

# BSP rootfs 骨架目录（debug/release 目录名不同，按配置选择）
#
# 注意：kconfig2cmake.py 的 BOOL_PREFIXES 只含 OS_/SDK_OS_/SDK_BUILD_/COMP_/DRV_，
# CONFIG_SDK_BSP_DEBUG（前缀 SDK_BSP_）不会转成 ENABLE_SDK_BSP_DEBUG，
# 因此这里直接读取 .config，与 sdk/build.cmake 判定 DEBUG 的行为保持一致。
set(_BSP_DEBUG_ENABLED OFF)
if(DEFINED ENABLE_SDK_BSP_DEBUG AND ENABLE_SDK_BSP_DEBUG)
    set(_BSP_DEBUG_ENABLED ON)
endif()
set(_BASE_DOT_CONFIG "${CMAKE_SOURCE_DIR}/.config")
if(EXISTS "${_BASE_DOT_CONFIG}")
    file(STRINGS "${_BASE_DOT_CONFIG}" _BASE_CFG_SDK_DEBUG
         REGEX "^CONFIG_SDK_BSP_DEBUG=y$")
    if(_BASE_CFG_SDK_DEBUG)
        set(_BSP_DEBUG_ENABLED ON)
    endif()
endif()

if(_BSP_DEBUG_ENABLED)
    set(_BSP_ROOTFS_NAME "rootfs_debug_musl_arm")
else()
    set(_BSP_ROOTFS_NAME "rootfs_musl_arm")
endif()
set(_BSP_ROOTFS_SKEL "${SDK_BSP_PUB_DIR}/${_BSP_ROOTFS_NAME}")

# dropbear 编译产物（源码直放后产物路径经路径中枢获取）
get_open_source_output(DROPBEAR_RELEASE_DIR dropbear)
set(_DROPBEAR_BIN    "${DROPBEAR_RELEASE_DIR}/sbin/dropbear")
set(_DROPBEARKEY_BIN "${DROPBEAR_RELEASE_DIR}/bin/dropbearkey")
set(_DBCLIENT_BIN    "${DROPBEAR_RELEASE_DIR}/bin/dbclient")
set(_DBSCP_BIN       "${DROPBEAR_RELEASE_DIR}/bin/scp")

# MPP ko 树根目录：hispark → output/mpp/ko，hisupport → smp/a7_linux/source/out/ko
get_filename_component(_KO_SRC "${SDK_KO_DIR}" DIRECTORY)
set(_KO_OUT "${_KO_SRC}")

set(_ENV_TXT_SRC "${CMAKE_CURRENT_SOURCE_DIR}/nand_env.txt")
set(_ENV2BURN    "${SDK_BSP_DIR}/tools/pc/uboot_env/scripts/env2burn.py")
set(_ENV_BIN_SIZE 0x40000)

# sdk_server：hispark → samples/mpp，hisupport → smp/a7_linux/source/mpp/sample
set(_SDK_SERVER "${SDK_SAMPLE_DIR}/sdk_server/sdk_server")
if(SDK_TYPE STREQUAL "hispark")
    set(_SDK_SERVER "${HI_SDK_ROOT}/samples/mpp/sdk_server/sdk_server")
endif()

# =============================================================================
# [BUILD] rootfs_prepare — 组装临时 rootfs
# =============================================================================
add_custom_target(rootfs_prepare
    # 1. 清理并创建临时目录（避免旧文件残留）
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${_ROOTFS_TMP}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_ROOTFS_TMP}"

    # 2. 拷贝 BSP 基础根文件系统（cp -raf 保留符号链接）
    COMMAND bash -c "cp -raf '${_BSP_ROOTFS_SKEL}/.' '${_ROOTFS_TMP}/'"

    # 3. 覆盖私有 etc / usr（网络、dropbear、udhcpc 脚本）
    COMMAND chmod 755
        "${_ROOTFS_PRIV_PATH}/etc/init.d/S80network"
        "${_ROOTFS_PRIV_PATH}/etc/init.d/S81load3516cv610"
        "${_ROOTFS_PRIV_PATH}/etc/init.d/S90dropbear"
        "${_ROOTFS_PRIV_PATH}/usr/share/udhcpc/default.script"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${_ROOTFS_PRIV_PATH}/etc" "${_ROOTFS_TMP}/etc"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${_ROOTFS_PRIV_PATH}/usr" "${_ROOTFS_TMP}/usr"

    # 4. dropbear SSH 服务
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_DROPBEAR_BIN}" "${_ROOTFS_TMP}/sbin/dropbear"
    COMMAND chmod 755 "${_ROOTFS_TMP}/sbin/dropbear"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_DROPBEARKEY_BIN}" "${_ROOTFS_TMP}/bin/dropbearkey"
    COMMAND chmod 755 "${_ROOTFS_TMP}/bin/dropbearkey"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_DBCLIENT_BIN}" "${_ROOTFS_TMP}/bin/dbclient"
    COMMAND chmod 755 "${_ROOTFS_TMP}/bin/dbclient"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_DBSCP_BIN}" "${_ROOTFS_TMP}/bin/scp"
    COMMAND chmod 755 "${_ROOTFS_TMP}/bin/scp"

    # 5. set_eth_mac.sh → /root（S80network 在 eth0 up 前调用，固化 MAC 防 IP 漂移）
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_ROOTFS_TMP}/root"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_ROOTFS_PRIV_PATH}/root/set_eth_mac.sh"
        "${_ROOTFS_TMP}/root/set_eth_mac.sh"
    COMMAND chmod 755 "${_ROOTFS_TMP}/root/set_eth_mac.sh"

    # 6. 播放器 → /root/media_player（开机由 S99media_player 拉起）
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:media_player>" "${_ROOTFS_TMP}/root/media_player"
    COMMAND chmod 755 "${_ROOTFS_TMP}/root/media_player"

    # 7. MPP ko → /drv/ko（hispark 与 hisupport 目录结构不同，见上方 _KO_SRC 说明）
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_ROOTFS_TMP}/drv/ko"
    COMMAND bash -c "cp -raf '${_KO_OUT}'/*.ko '${_ROOTFS_TMP}/drv/ko/' 2>/dev/null || true"
    COMMAND bash -c "cp -raf '${_KO_OUT}'/extdrv '${_ROOTFS_TMP}/drv/ko/' 2>/dev/null || true"
    COMMAND bash -c "cp -raf '${_KO_SRC}/.' '${_ROOTFS_TMP}/drv/ko/'"

    # 8. SDK 动态库 → /lib（sdk_server 运行时依赖）
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_ROOTFS_TMP}/lib"
    COMMAND bash -c "cp -af ${SDK_LIB_DIR}/*.so* ${_ROOTFS_TMP}/lib/"

    # 9. sdk_server → /root/sdk_server（hisupport 无此文件，跳过）
    COMMAND ${CMAKE_COMMAND} "-DSRC=${_SDK_SERVER}" "-DDST=${_ROOTFS_TMP}/root/sdk_server"
        -P "${CMAKE_CURRENT_LIST_DIR}/cmake/copy_if_exists.cmake"

    # 10. LCD 面板驱动 → /drv/lcd_panel.ko（CONFIG_DRV_LCD_JD9853=y 时由 SDK 编出）
    COMMAND bash -c "SRC='${CMAKE_BINARY_DIR}/drivers/lcd/jd9853/jd9853.ko'; \
        if [ -f \"$SRC\" ]; then cp -f \"$SRC\" '${_ROOTFS_TMP}/drv/lcd_panel.ko'; \
        else echo '[media_player] 警告：jd9853.ko 未生成，屏不会亮（检查 CONFIG_DRV_LCD_JD9853）'; fi"

    # 10b. 演示视频 → /root/badapple.mp4（存在才拷，方便替换）
    COMMAND bash -c "SRC='${CMAKE_CURRENT_SOURCE_DIR}/../media/badapple.mp4'; \
        if [ -f \"$SRC\" ]; then cp -f \"$SRC\" '${_ROOTFS_TMP}/root/badapple.mp4'; \
        else echo '[media_player] 提示：未提供 media/badapple.mp4，开机自启会找不到视频'; fi"

    # 10c. 板级 IO 配置 ko（board_io_config.dtsi 的 pin-cfg 由它应用）
    COMMAND bash -c "SRC='${CMAKE_BINARY_DIR}/drivers/board_io_cfg/ot_board_io_cfg.ko'; \
        if [ -f \"$SRC\" ]; then cp -f \"$SRC\" '${_ROOTFS_TMP}/drv/ko/ot_board_io_cfg.ko'; \
        else echo '[media_player] 警告：ot_board_io_cfg.ko 未生成'; fi"

    # 11. 启动脚本 + 开机自启
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_ROOTFS_PRIV_PATH}/root/start_media_player.sh"
        "${_ROOTFS_TMP}/root/start_media_player.sh"
    COMMAND chmod 755 "${_ROOTFS_TMP}/root/start_media_player.sh"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_ROOTFS_PRIV_PATH}/etc/init.d/S99media_player"
        "${_ROOTFS_TMP}/etc/init.d/S99media_player"
    COMMAND chmod 755 "${_ROOTFS_TMP}/etc/init.d/S99media_player"

    DEPENDS media_player dropbear_ext
    COMMENT "[media_player/rootfs] prepare ..."
    VERBATIM
)

# =============================================================================
# [INSTALL] cmake --install — 生成 251M UBIFS 并写入发布目录
# =============================================================================
install(CODE "
    # ---- 前置存在性检查（BSP 必须已编译）-----------------------------
    if(NOT EXISTS \"${SDK_BSP_PUB_DIR}\")
        message(FATAL_ERROR
            \"[media_player/rootfs] 未找到 BSP 产物目录：${SDK_BSP_PUB_DIR}\\n\"
            \"请先编译 BSP：bash scripts/build.sh build ...\")
    endif()
    if(NOT EXISTS \"${_BSP_ROOTFS_SKEL}\")
        message(FATAL_ERROR
            \"[media_player/rootfs] 未找到 BSP rootfs 骨架：${_BSP_ROOTFS_SKEL}\\n\"
            \"当前选择的 rootfs 名由 .config 的 CONFIG_SDK_BSP_DEBUG 决定，\\n\"
            \"请确认该配置与 BSP 实际产物（rootfs_debug_musl_arm / rootfs_musl_arm）一致。\")
    endif()

    # ---- 确保 rootfs_prepare 已构建（幂等，已构建则快速跳过）----------
    message(STATUS \"[media_player/rootfs] 确保 rootfs_prepare 已构建...\")
    execute_process(
        COMMAND ${CMAKE_COMMAND}
            --build ${CMAKE_BINARY_DIR}
            --target rootfs_prepare
        RESULT_VARIABLE _prepare_result
    )
    if(NOT _prepare_result EQUAL 0)
        message(FATAL_ERROR \"[media_player/rootfs] rootfs_prepare 构建失败\")
    endif()

    # ---- 定位 BSP 镜像目录（uImage / nand_* 所在目录）----------------
    file(GLOB _img_dirs LIST_DIRECTORIES true \"${SDK_BSP_PUB_DIR}/*image*\")
    if(NOT _img_dirs)
        message(FATAL_ERROR
            \"[media_player/rootfs] 未找到 BSP 镜像目录：${SDK_BSP_PUB_DIR}/*image*（BSP 未编译？）\")
    endif()
    list(GET _img_dirs 0 _src_img)

    # ---- 生成 251M UBIFS（在 build/rootfs/ 下执行）----------------------
    message(STATUS \"[media_player/rootfs] 生成 UBIFS 镜像（${_ROOTFS_IMG_NAME}）...\")
    set(_ubifs_out \"${CMAKE_BINARY_DIR}/rootfs/${_ROOTFS_IMG_NAME}\")
    file(REMOVE \"\${_ubifs_out}\")
    execute_process(
        COMMAND chmod +x \"${_UBIIMG_SH}\"
    )
    # mkubiimg.sh 不带第 6 个参数(Res) 时，成功也会 exit 1（清理中间文件后退出），
    # 故忽略退出码（|| true），改以“产物文件是否真生成”判定成败。
    execute_process(
        COMMAND bash -c
            \"cd ${CMAKE_BINARY_DIR}/rootfs && \
              '${_UBIIMG_SH}' \
                  ${UBI_CHIP} ${UBI_PAGE_SIZE} ${UBI_BLOCK_SIZE} \
                  '${_ROOTFS_TMP}' ${UBI_FS_SIZE} ${SDK_UBI_TOOL_DIR} || true\"
        RESULT_VARIABLE _ubi_result
    )
    if(NOT EXISTS \"\${_ubifs_out}\")
        message(FATAL_ERROR
            \"[media_player/rootfs] UBIFS 镜像生成失败（exit=\${_ubi_result}）：\${_ubifs_out} 未生成\")
    endif()

    # ---- 写入发布目录 ------------------------------------------------
    set(_dest \"\${CMAKE_INSTALL_PREFIX}/image/media_player\")
    message(STATUS \"[media_player/rootfs] 写入 \${_dest} ...\")
    file(REMOVE_RECURSE \"\${_dest}\")
    file(MAKE_DIRECTORY \"\${_dest}\")
    file(COPY \"\${_src_img}/\" DESTINATION \"\${_dest}\"
         REGEX \"rootfs_.*\\\\.(jffs2|ext4|ubifs)$\" EXCLUDE
         REGEX \"nor_(env\\\\.bin|burn_table\\\\.xml)$\" EXCLUDE)

    # ---- 自产 env / 烧写表（251M rootfs 分区）----------------------------
    find_program(_MKENVIMAGE mkenvimage)
    if(NOT _MKENVIMAGE)
        message(FATAL_ERROR \"[media_player/rootfs] 未找到 mkenvimage（请安装 u-boot-tools）\")
    endif()
    message(STATUS \"[media_player/rootfs] 自产 env/烧写表（251M rootfs）...\")
    execute_process(
        COMMAND \${_MKENVIMAGE} -s ${_ENV_BIN_SIZE} -o \"\${_dest}/nand_env.bin\" \"${_ENV_TXT_SRC}\"
        RESULT_VARIABLE _re)
    if(NOT _re EQUAL 0)
        message(FATAL_ERROR \"[media_player/rootfs] mkenvimage 生成 nand_env.bin 失败\")
    endif()
    execute_process(
        COMMAND python3 \"${_ENV2BURN}\" \"${_ENV_TXT_SRC}\" \"\${_dest}/nand_burn_table.xml\"
        RESULT_VARIABLE _rb)
    if(NOT _rb EQUAL 0)
        message(FATAL_ERROR \"[media_player/rootfs] env2burn.py 生成 nand_burn_table.xml 失败\")
    endif()
    set(_burn \"\${_dest}/nand_burn_table.xml\")
    if(EXISTS \"\${_burn}\")
        file(READ \"\${_burn}\" _xml)
        string(REGEX REPLACE
            \"(PartitionName=\\\"rootfs\\\"[^>]*SelectFile=\\\")[^\\\"]*\"
            \"\\\\1${_ROOTFS_IMG_NAME}\"
            _xml \"\${_xml}\")
        file(WRITE \"\${_burn}\" \"\${_xml}\")
    endif()

    file(COPY \"\${_ubifs_out}\" DESTINATION \"\${_dest}\")
    message(STATUS \"[media_player/rootfs] 发布完成：\${_dest}\")
" COMPONENT rootfs)
