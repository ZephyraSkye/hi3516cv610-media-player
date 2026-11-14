# =============================================================================
# apps/cv610_ev_board/samples/base/CMakeLists.txt
#
# media_player 应用入口。
#   dts/    — 案例 DTS（bsp/dts 为基础，启用屏/背光节点，必须）
#   source/ — 播放器（编译进 rootfs /root/media_player）
#   rootfs/ — 组装临时 rootfs 并重新生成 251M UBIFS 烧录镜像
# =============================================================================

add_subdirectory(dts)
add_subdirectory(source)
add_subdirectory(rootfs)
