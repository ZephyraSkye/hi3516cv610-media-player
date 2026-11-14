#!/usr/bin/env bash
# =============================================================================
# revert_patches.sh — ws73_lcd SDK patch 还原（与 apply_patches.sh 镜像，供 distclean 换案例用）
#   用法：bash .../ws73_lcd/patch/revert_patches.sh <SDK_DIR> [SDK_TYPE]
#   SDK_TYPE 缺省 hispark；各步按 marker 判断，幂等。
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

# 路径与 apply_patches.sh 一致
if [[ "${_sdk_type}" == "hisupport" ]]; then
    LINUX_DIR="${SDK_DIR}/open_source/linux"
    BSP_TOOLS_DIR="${SDK_DIR}/smp/a7_linux/source/bsp/tools"
else
    LINUX_DIR="${SDK_DIR}/third_party/linux/linux-5.10.y"
    BSP_TOOLS_DIR="${SDK_DIR}/boards/dmeb/tools"
fi
LINUX_MARKER="${LINUX_DIR}/.linux_patched"

# =============================================================================
# Step 1 反打 — linux patch
#   hispark  : 直接反打 ws73_lcd_kernel.patch（-p4）
#   hisupport: 反打 wrapper Makefile（-p1）+ 删拷入的 .patch
#   公共     ：删拷入/生成的文件（defconfig/busybox 配置/nand_env.txt/.scmversion）
# =============================================================================
if [[ ! -f "${LINUX_MARKER}" ]]; then
    echo "[revert/linux] Not patched, skipping."
else
    echo "[revert/linux] Reverting linux patches ..."

    if [[ "${_sdk_type}" == "hisupport" ]]; then
        patch --fuzz=3 -R -p1 -d "${LINUX_DIR}" < "${PATCH_DIR}/kernel_wrapper_hisupport.patch" || true
        rm -f "${LINUX_DIR}/ws73_lcd_kernel.patch"
    else
        patch --fuzz=3 -R -p4 -d "${LINUX_DIR}" < "${PATCH_DIR}/ws73_lcd_kernel.patch" || true
    fi

    rm -f \
        "${LINUX_DIR}/hi3516cv610_drm_touch_defconfig" \
        "${BSP_TOOLS_DIR}/build_scripts/busybox/config_debug_arm_a7_softfp_neon_udhcpc_tftp" \
        "${LINUX_DIR}/.scmversion"

    # nand_env.txt：有 .orig 则还原；否则保留现有文件（BSP uboot_env 必需输入）
    NAND_ENV_DST="${BSP_TOOLS_DIR}/pc/uboot_env/env_text/hi3516cv610/nand_env.txt"
    if [[ -f "${NAND_ENV_DST}.orig" ]]; then
        mv -f "${NAND_ENV_DST}.orig" "${NAND_ENV_DST}"
    else
        echo "[revert/linux] WARNING: 无 nand_env.txt.orig，保留现有文件"
    fi

    rm -f "${LINUX_MARKER}"
    echo "[revert/linux] Done."
fi

echo "[ws73_lcd/patch] All patches reverted. SDK restored."
