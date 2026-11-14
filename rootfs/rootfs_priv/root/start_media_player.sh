#!/bin/sh
# start_media_player.sh — 启动播放器
#   用法：/root/start_media_player.sh [视频文件]
#   默认播放 /root/badapple.mp4

MEDIA="${1:-/root/badapple.mp4}"
LOG=/root/media_player.log

if [ ! -x /root/media_player ]; then
    echo "[mp] /root/media_player 不存在"
    exit 1
fi
if [ ! -f "$MEDIA" ]; then
    echo "[mp] 找不到视频：$MEDIA"
    exit 1
fi

# 若官方 demo 还在跑，先让出 DRM
killall factory_demo_main 2>/dev/null

# MPP UDS 守护进程（AO 通过它与驱动通信）
if [ -x /root/sdk_server ]; then
    pidof sdk_server >/dev/null 2>&1 || { /root/sdk_server & sleep 1; }
fi

echo "[mp] 播放 $MEDIA"
exec /root/media_player "$MEDIA"
