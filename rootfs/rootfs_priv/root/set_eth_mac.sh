#!/bin/sh
# set_eth_mac.sh —— 固化 eth0 MAC，跨重启稳定
#
# 问题：Hi3516CV610 hleth/femac-v2 驱动在设备树无 mac-address、板上无 EFUSE/chipid
#       可读时，每次 probe 调用 eth_hw_addr_random() 生成首字节 bit1=1 的 LAA 随机
#       地址，导致每次开机 MAC 变化、DHCP 分配的 IP 跟着漂移。
#
# 方案：首次开机随机生成一个合规 LAA MAC（首字节 0x02），落盘到 /root/.eth_mac
#       （UBIFS，掉电不丢），以后每次开机读取复用。等价于 /etc/machine-id 思路。
#
# MAC 来源优先级：
#   1) /root/.eth_mac（已落盘）—— 首选
#   2) 上述文件不存在时，本脚本随机生成并落盘
#
# 生成的 MAC 首字节固定 0x02：
#   bit0(组播)=0、bit1(U/L 本地管理)=1，符合 IEEE 802 单播 LAA 规范。
#
# 用法：在 /etc/init.d/S80network 中、ifconfig eth0 up 之前调用本脚本。
#      本脚本会自行完成 eth0 down → 设 MAC → up。
#
# 注意：DHCP（udhcpc）在本脚本之后拉起，故 MAC 必须先定型。

MAC_FILE=/root/.eth_mac

if [ ! -f "$MAC_FILE" ]; then
    # 首次：生成 6 字节随机，首字节强制 0x02（LAA 单播）
    RAW=$(head -c 6 /dev/urandom | od -An -tx1 | tr -d ' \n')
    MAC="02:$(echo "$RAW" | cut -c3-4):$(echo "$RAW" | cut -c5-6):$(echo "$RAW" | cut -c7-8):$(echo "$RAW" | cut -c9-10):$(echo "$RAW" | cut -c11-12)"
    echo "$MAC" > "$MAC_FILE"
fi

MAC=$(cat "$MAC_FILE")

ifconfig eth0 down 2>/dev/null
ifconfig eth0 hw ether "$MAC" 2>/dev/null
ifconfig eth0 up

echo "[set_eth_mac] eth0 MAC = $MAC (from $MAC_FILE)"
