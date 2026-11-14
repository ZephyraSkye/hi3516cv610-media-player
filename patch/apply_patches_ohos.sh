#!/usr/bin/env bash
# =============================================================================
# apply_patches_ohos.sh — ws73_lcd 案例补丁 OHOS 直打版（hisupport）
#
# OHOS 构建流程专用（scripts/build_ohos.sh do_app_bsp 调用）。
# Linux 流程用共享 apply_patches.sh（wrapper 注入模式）；hispark OHOS 流程
# 也沿用共享脚本（其 hispark 分支本就是直打）。本脚本只服务 hisupport 直打。
#
#   bash apply_patches_ohos.sh <SDK_DIR> [SDK_TYPE]
#
# 直打对象（hisupport_sdk_prepare 已解压 + 官方补丁）：
#   ws73_lcd_kernel.patch  内核树 -p4 --fuzz=3（spi-pl022 RX-DMA 修改）
#   ws73_lcd_dts.patch     内核树 -p1
#   ws73_lcd_pin_mux.patch SDK 树  -p2（smp 源码，build_soc.sh 编 ko 消费）
#
# （kernel.mk 用 OHOS 配置树的 defconfig）。
#
# marker 幂等（树内 .ohos_ws73_*）；SDK 重解压后 marker 消失自动重打。
# =============================================================================

set -euo pipefail

PATCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

SDK_DIR="${1:-}"
if [[ -z "${SDK_DIR}" || ! -d "${SDK_DIR}" ]]; then
    echo "[ws73_lcd/patch/ohos] ERROR: SDK directory not found: '${SDK_DIR}'"
    echo "Usage: $0 <SDK_DIR> [SDK_TYPE]"
    exit 1
fi

_sdk_type="${2:-hispark}"
echo "[ws73_lcd/patch/ohos] SDK type: ${_sdk_type}"

if [[ "${_sdk_type}" != "hisupport" ]]; then
    echo "[ws73_lcd/patch/ohos] hispark: handled by shared apply_patches.sh, done."
    exit 0
fi

LINUX_TREE="${SDK_DIR}/open_source/linux/linux-5.10.y"
BSP_TOOLS_DIR="${SDK_DIR}/smp/a7_linux/source/bsp/tools"

# patch 已打过时返回非零，区分"已打"与"真失败"
# 命令串经 eval 执行（重定向需在展开后重新解析）
apply_patch_checked() {
    local _out
    if ! _out="$(eval "${1}" 2>&1)"; then
        if echo "${_out}" | grep -q "previously applied\|Reversed"; then
            echo "${_out}"
            echo "[ws73_lcd/patch/ohos] NOTE: patch already applied, ok."
        else
            echo "${_out}"
            echo "[ws73_lcd/patch/ohos] ERROR: patch failed: ${2}" >&2
            return 1
        fi
    fi
}

# =============================================================================
# Step 1 — busybox 配置（do_sdk busybox 编译消费：BUSYBOX_CFG 由 bsp_make_opts
# 解析传入，配置需先拷到 BSP 工具目录——busybox wrapper 的
# BUSYBOX_CFG_PATH=$(OSDRV_DIR)/tools/build_scripts/busybox/$(BUSYBOX_CFG)）
# =============================================================================
BB_MARKER="${SDK_DIR}/.ohos_ws73_busybox"
if [[ -f "${BB_MARKER}" ]]; then
    echo "[ws73_lcd/patch/ohos/busybox] Already copied, skipping."
else
    echo "[ws73_lcd/patch/ohos/busybox] Copying busybox config ..."
    cp "${PATCH_DIR}/config_debug_arm_a7_softfp_neon_udhcpc_tftp" "${BSP_TOOLS_DIR}/build_scripts/busybox/"
    touch "${BB_MARKER}"
    echo "[ws73_lcd/patch/ohos/busybox] Done."
fi

# =============================================================================
# Step 2 — kernel patch（spi-pl022 RX-DMA）
# =============================================================================
KERNEL_MARKER="${LINUX_TREE}/.ohos_ws73_kernel"
if [[ -f "${KERNEL_MARKER}" ]]; then
    echo "[ws73_lcd/patch/ohos/kernel] Already patched, skipping."
else
    echo "[ws73_lcd/patch/ohos/kernel] Applying kernel patch to ${LINUX_TREE} ..."
    [[ -d "${LINUX_TREE}" ]] || { echo "[ws73_lcd/patch/ohos/kernel] ERROR: tree not found: ${LINUX_TREE}" >&2; exit 1; }
    apply_patch_checked "patch -N --fuzz=3 -p4 -d ${LINUX_TREE} < ${PATCH_DIR}/ws73_lcd_kernel.patch" \
        "ws73_lcd_kernel.patch"
    touch "${KERNEL_MARKER}"
    echo "[ws73_lcd/patch/ohos/kernel] Done."
fi

# =============================================================================
# Step 3 — dts patch
# =============================================================================
DTS_MARKER="${LINUX_TREE}/.ohos_ws73_dts"
if [[ -f "${DTS_MARKER}" ]]; then
    echo "[ws73_lcd/patch/ohos/dts] Already patched, skipping."
else
    echo "[ws73_lcd/patch/ohos/dts] Applying dts patch to ${LINUX_TREE} ..."
    [[ -d "${LINUX_TREE}" ]] || { echo "[ws73_lcd/patch/ohos/dts] ERROR: tree not found: ${LINUX_TREE}" >&2; exit 1; }
    apply_patch_checked "patch -N -p1 -d ${LINUX_TREE} < ${PATCH_DIR}/ws73_lcd_dts.patch" \
        "ws73_lcd_dts.patch"
    touch "${DTS_MARKER}"
    echo "[ws73_lcd/patch/ohos/dts] Done."
fi

# =============================================================================
# Step 4 — pin_mux patch（SPI LCD + I2C touch pin_mux；smp 源码树）
# =============================================================================
PINMUX_MARKER="${SDK_DIR}/.ohos_ws73_pinmux"
if [[ -f "${PINMUX_MARKER}" ]]; then
    echo "[ws73_lcd/patch/ohos/pin_mux] Already patched, skipping."
else
    echo "[ws73_lcd/patch/ohos/pin_mux] Applying pin_mux patch to ${SDK_DIR} ..."
    apply_patch_checked "patch -N -p2 -d ${SDK_DIR} < ${PATCH_DIR}/ws73_lcd_pin_mux.patch" \
        "ws73_lcd_pin_mux.patch"
    touch "${PINMUX_MARKER}"
    echo "[ws73_lcd/patch/ohos/pin_mux] Done."
fi

echo "[ws73_lcd/patch/ohos] All patches applied successfully."
