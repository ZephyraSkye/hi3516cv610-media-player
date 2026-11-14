#!/usr/bin/env bash
# =============================================================================
# apply_patches.sh — ws73_lcd SDK patch 应用脚本（hispark / hisupport 共用）
#
# 由 scripts/build.sh sdk 在编译 SDK 前自动调用，也可单独执行：
#   bash .../ws73_lcd/patch/apply_patches.sh <SDK_DIR> [SDK_TYPE]
#
# SDK_TYPE 由调用方显式传入（hispark / hisupport），默认为 hispark。
#
# pin mux 由 board_io_config.dtsi+ ot_board_io_cfg.ko 实现；
# DTB 由 bsp/dts 独立树编译,
# =============================================================================

set -euo pipefail

PATCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

SDK_DIR="${1:-}"
if [[ -z "${SDK_DIR}" || ! -d "${SDK_DIR}" ]]; then
    echo "[ws73_lcd/patch] ERROR: SDK directory not found: '${SDK_DIR}'"
    echo "Usage: $0 <SDK_DIR> [SDK_TYPE]"
    exit 1
fi
_sdk_type="${2:-hispark}"
echo "[ws73_lcd/patch] SDK type: ${_sdk_type}"

# 公共路径变量
if [[ "${_sdk_type}" == "hisupport" ]]; then
    LINUX_DIR="${SDK_DIR}/open_source/linux"
    BSP_TOOLS_DIR="${SDK_DIR}/smp/a7_linux/source/bsp/tools"
else
    LINUX_DIR="${SDK_DIR}/third_party/linux/linux-5.10.y"
    BSP_TOOLS_DIR="${SDK_DIR}/boards/dmeb/tools"
fi
LINUX_MARKER="${LINUX_DIR}/.linux_patched"

# =============================================================================
# Step 1 — linux patch
#   - busybox 配置 + nand_env.txt 拷到 BSP 工具目录
#   - hispark: 直接 patch -p4 到预解压内核源码 + 拷 defconfig
#   - hisupport: 拷贝 defconfig + patch wrapper Makefile（kernel 编译时注入）
# =============================================================================
if [[ -f "${LINUX_MARKER}" ]]; then
    echo "[patch/linux] Already patched, skipping."
else
    echo "[patch/linux] Applying linux patches to ${LINUX_DIR} ..."
    [[ -d "${LINUX_DIR}" ]] || { echo "[patch/linux] ERROR: directory not found: ${LINUX_DIR}"; exit 1; }

    # ---- 公共：busybox 配置 + nand_env.txt ----
    cp "${PATCH_DIR}/config_debug_arm_a7_softfp_neon_udhcpc_tftp" "${BSP_TOOLS_DIR}/build_scripts/busybox/"
    NAND_ENV_DST="${BSP_TOOLS_DIR}/pc/uboot_env/env_text/hi3516cv610/nand_env.txt"
    # 覆盖前留 .orig（若还没有），供 revert 还原 shipped 原件
    if [[ -f "${NAND_ENV_DST}" && ! -f "${NAND_ENV_DST}.orig" ]]; then
        cp -p "${NAND_ENV_DST}" "${NAND_ENV_DST}.orig"
    fi
    cp "${PATCH_DIR}/nand_env.txt"                                "${NAND_ENV_DST}"

    # ---- kernel 补丁注入（SDK_TYPE 差异） ----
    if [[ "${_sdk_type}" == "hisupport" ]]; then
        # deconfig拷到 wrapper 根：解压后由 wrapper 补丁注入内核树
        cp "${PATCH_DIR}/ws73_lcd_kernel.patch" "${LINUX_DIR}/"
        cp "${PATCH_DIR}/hi3516cv610_drm_touch_defconfig" "${LINUX_DIR}/"
        patch --fuzz=0 -p1 -d "${LINUX_DIR}" < "${PATCH_DIR}/kernel_wrapper_hisupport.patch"
    else
        patch --fuzz=3 -p4 -d "${LINUX_DIR}" < "${PATCH_DIR}/ws73_lcd_kernel.patch"
        cp "${PATCH_DIR}/hi3516cv610_drm_touch_defconfig" "${LINUX_DIR}/arch/arm/configs/"
    fi

    touch "${LINUX_MARKER}"
    # 固定内核版本号，避免 git dirty state 自动追加 "+" 导致与预编译 .ko 的 version magic 不匹配
    echo -n "" > "${LINUX_DIR}/.scmversion"
    echo "[ws73_lcd/patch/linux] Done."
fi

echo "[ws73_lcd/patch] All patches applied successfully."
