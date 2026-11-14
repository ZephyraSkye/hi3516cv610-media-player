# media_player

> 上海海思越影 Hi3516CV610 · 本地 mp4 播放案例（软解 + SPI 屏显 + 喇叭出声 + 音画同步）
>
> 适用于 Sunniwell CV610 DevBoard（256MB SPI NAND / 2.8" JD9853 240×320 SPI 屏）。
> 作为 [hi_aiot_solution](https://gitcode.com/HiSpark/hi_aiot_solution) 的一个 sample 存在，**不能独立编译**。

基于 Hi3516CV610 的音视频播放器案例，验证「本地 mp4 → 屏显 + 喇叭出声」的完整链路。

本案例不依赖任何硬件解码器，使用 **FFmpeg 纯软件解码**，把解码后的画面转为 RGB565 经
DRM 送到 SPI LCD，音频经 MPP AO 送到板载 codec：

- 视频：libavformat 解封装（mov）→ libavcodec 软解 H.264 → YUV420 转 RGB565 → DRM 局部刷新
- 音频：libavcodec 软解 AAC → libswresample 转 S16/48kHz/立体声 → `ss_mpi_ao_send_frame`
- 同步：以「已送入 AO 的样本数」为音频主时钟，视频帧超前则等待、落后则丢帧
- 自启：开机由 `S99media_player` 加载屏驱动、拉起 `sdk_server`、后台播放 `/root/badapple.mp4`

**上电即播**：烧录本镜像后无需任何命令，开机 5~8 秒自动播放 `/root/badapple.mp4`（含声音）。
如需关闭自启，把 `/etc/init.d/S99media_player` 改名（使其不匹配 `S[0-9][0-9]*`）即可。

> Hi3516CV610 为编码型 IPC SoC，**无视频硬件解码器**（无 VDEC 模块），因此 480×360 的
> H.264 软解约 86 ms/帧；本案例默认使用 240×180/20fps 的预转码视频，解码降至约 8 ms/帧。

---

## 一、放进 SDK 树

本案例依赖 SDK 的构建系统（`scripts/build.sh`、`sdk/build.cmake`、`apps/cv610_ev_board/bsp/dts`），
**不能单独用 CMake 编译**。请先按官方文档装好 WSL2 + 交叉工具链，然后：

```bash
# 1. 按官方文档同步源码树
repo init -u https://gitcode.com/HiSpark/hi_aiot_solution_manifest -b main -g linux
repo sync -j8

# 2. 把本仓库拷进 samples 目录（目录名建议保持 media_player，编译命令要用到）
cd hi_aiot_solution
git clone https://github.com/ZephyraSkye/hi3516cv610-media-player.git
cp -r hi3516cv610-media-player apps/cv610_ev_board/samples/media_player

# 3. 编译
cd apps/../..
bash scripts/build.sh distclean
bash scripts/build.sh build --target cv610_ev_board samples media_player
```

环境搭建（WSL2 + Ubuntu 22.04 + `arm-v01c02-linux-musleabi-gcc` 10.3.0）见
[官方环境准备文档](https://docs.hisilicon.com/projects/hi_aiot_solution/master/software-guide/environment-setup/index.html)。

## 二、目录结构

```
media_player/
├── README.md
├── CMakeLists.txt
├── app.defconfig                  # Kconfig 差异配置
├── oh_product/                    # OpenHarmony 产品配置
├── prebuilt/                      # 预编译 FFmpeg 7.1 静态库（含头文件）
│   ├── include/
│   └── lib/                       # libavformat.a / libavcodec.a / libswresample.a / libavutil.a
├── media/
│   └── badapple.mp4               # 演示视频（240×180/20fps，随镜像打包）
├── patch/                         # SDK patch
│   ├── apply_patches.sh
│   ├── ws73_lcd_dts.patch         # 背光 + JD9853 屏节点（必须）
│   ├── ws73_lcd_pin_mux.patch     # spi_lcd_pin_mux() 挂入 pin_mux()
│   ├── ws73_lcd_kernel.patch
│   ├── hi3516cv610_drm_touch_defconfig
│   └── nand_env.txt               # 251M rootfs 分区 env
├── dts/                           # 案例 DTS 入口（必须：屏/背光节点靠它进内核）
│   ├── CMakeLists.txt             # 转发到 apps/cv610_ev_board/bsp/dts 编 DTB + 组装 FIT
│   └── board_io_config.dtsi       # 板级引脚（含 SPI 屏管脚）
├── source/
│   ├── CMakeLists.txt
│   └── main.c
└── rootfs/
    ├── CMakeLists.txt
    ├── nand_env.txt
    ├── cmake/copy_if_exists.cmake
    └── rootfs_priv/
        ├── etc/init.d/S80network
        ├── etc/init.d/S81load3516cv610
        ├── etc/init.d/S90dropbear
        ├── etc/init.d/S99media_player      # 本案例新增：开机自启播放器
        └── root/start_media_player.sh      # 手动启动脚本
```

## 三、前提条件

- Hi3516CV610 开发板（NAND，251M UBIFS rootfs 分区）
- LCD 屏为 **JD9853（2.8 寸 240×320，SPI 接口）**，对应 `CONFIG_DRV_LCD_JD9853=y`
- 板载 codec 通路可用（喇叭接 19 号接口），`ot_acodec.ko` 由 MPP 加载器拉起
- 首次编译需联网/具备 BSP 编译条件（**patch 会修改内核 dts 与 MPP sysconfig，需重编 BSP**）

## 四、编译

在工程根目录执行：

```bash
bash scripts/build.sh distclean
bash scripts/build.sh build --target cv610_ev_board samples media_player
```

> 首次编译约 20~40 分钟（含内核 dts / pin_mux 补丁重编 BSP）；后续仅改应用层约 2~3 分钟。

### 编译产物

```
_build/install/image/media_player/
├── uImage                                    # 内核
├── rootfs_hi3516cv610_2k_128k_251M.ubifs     # 重新打包的 251M 根文件系统
├── boot_image.bin
├── nand_burn_table.xml
└── nand_env.bin
```

rootfs 内额外部署：

| 路径 | 内容 |
|------|------|
| `/root/media_player` | 播放器可执行文件 |
| `/root/badapple.mp4` | 演示视频 |
| `/root/sdk_server` | MPP UDS 守护进程（AO 依赖） |
| `/drv/lcd_panel.ko` | JD9853 面板驱动（由 `jd9853.ko` 改名部署） |
| `/root/start_media_player.sh` | 手动启动脚本 |
| `/etc/init.d/S99media_player` | 开机自启脚本 |

## 五、烧写

编译产物直接包含可烧写镜像，使用 Toolplatform 烧写，分区表选
`_build/install/image/media_player/nand_burn_table.xml`，具体参考
[烧录到开发板文档](https://gitcode.com/HiSpark/hi_aiot_solution/blob/master/docs/zh-CN/software-guide/build-and-burn.md)。

> 烧写前关闭串口终端占用；若报 `网络下载TFTP超时`，先退出接管路由的代理软件
> （TUN 网卡 `198.18.0.0/15` 会劫持 TFTP 使用的 UDP 69）。

## 六、上板运行

**默认行为：上电自动播放。** 不需要登录串口、不需要敲任何命令，开机后屏上直接开始放
`/root/badapple.mp4`。

系统启动时按顺序执行：

| 顺序 | 脚本 | 内容 |
|------|------|------|
| 1 | `S80network` | `eth0` up，udhcpc 动态获取 IP |
| 2 | `S81load3516cv610` | 加载 `/drv/ko` MPP 驱动；`sys_config.ko` 内执行 `spi_lcd_pin_mux()` 配置屏管脚 |
| 3 | `S90dropbear` | 启动 dropbear（22 端口） |
| 4 | `S99media_player` | `insmod /drv/lcd_panel.ko` → 启动 `sdk_server` → 后台播放 |

开机约 5~8 秒后屏上开始播放（视频与音频同时起），日志写在 `/root/media_player.log`：

```bash
~ # tail -f /root/media_player.log
[drm] DAMAGE_CLIPS 属性: 支持局部刷新
[drm] 局部刷新区域 x=0..240 y=70..250
[a] 已设 SCHED_FIFO/60
[v] 20.6 fps | 转换 1.4 | 刷屏 21.2 | 音频 12.40 s | 丢 0
```

手动运行（会先让出 DRM，再重新拉起）：

```bash
~ # killall media_player
~ # /root/start_media_player.sh                      # 默认播放 /root/badapple.mp4
~ # /root/start_media_player.sh /root/your.mp4       # 指定文件
```

## 七、功能说明

### 视频源要求

解码耗时与**像素数**成正比，与码率关系不大，建议预先转码：

```bash
ffmpeg -i badapple.mp4 \
  -vf "scale=240:180:flags=bilinear,fps=20" \
  -c:v libx264 -profile:v baseline -level 3.0 -preset veryfast \
  -crf 28 -g 20 -bf 0 \
  -c:a aac -b:a 96k -ac 2 -ar 48000 \
  -movflags +faststart badapple_small.mp4
```

| 参数 | 作用 |
|------|------|
| `scale=240:180` | 像素降到 1/4，解码快 4 倍（屏为 240×320，上下留黑边） |
| `-profile:v baseline -bf 0` | 无 B 帧、无重排序缓冲，软解最省 |
| `fps=20` | 与 SPI 屏刷新能力匹配，避免持续丢帧 |
| `-c:a aac -ar 48000` | AO 只接受 48kHz，源为其他采样率时靠 swresample 转换 |

容器仅支持 MP4/MOV；需其他容器时在 `prebuilt/` 的 ffmpeg 编译参数中追加 demuxer。

### 帧率与性能

| 阶段 | 耗时 | 说明 |
|------|------|------|
| 视频软解（240×180） | ~8 ms | CPU 软解，无硬件可用 |
| YUV420→RGB565 | ~1.4 ms | 查表实现，内循环无整数除法（Cortex-A7 无硬件除法） |
| SPI 刷屏（局部 240×180） | ~21 ms | 86KB @50MHz，仅传视频区域 |
| SPI 刷屏（整帧 240×320） | ~37 ms | 未启用 `DAMAGE_CLIPS` 时的表现 |

实测 **20~25 fps**。上限由 SPI 屏带宽决定：dts 中 `spi-max-frequency = <50000000>`，
整帧 153KB 的物理传输时间无法压缩。

### 音视频同步

- **主时钟**：`ss_mpi_ao_send_frame` 为阻塞调用，其返回节奏即音频播放进度，累计样本数换算成秒
- **视频节流**：帧 PTS 与音频时钟比较，超前 >5 ms 则 `usleep`，落后 >200 ms 丢帧追赶
- **线程隔离**：解封装、视频、音频、AO 各一个线程；音频线程 `SCHED_FIFO/60`
- **缓冲**：PCM 环形缓冲 4 秒，抗解码抖动

### AO 送帧的三个硬约束

| 约束 | 现象 |
|------|------|
| 缓冲区必须在 **MMZ**（`ss_mpi_sys_mmz_alloc`） | `ao_check_frame_inmmb ... check frame failed` |
| 立体声为**平面布局**（`virt_addr[0]` 左、`[1]` 右） | `virt_addr[1] is null` |
| `frame.len` 为**每声道**字节数 | 声音变速或杂音 |

## 八、故障排查

| 现象 | 排查方向 |
|------|----------|
| 屏黑但有背光 | `lsmod \| grep jd9853`；`dmesg \| grep -i jd9853`；确认 `lcd_panel.ko` 已加载 |
| 屏完全不亮 | 检查 `patch/` 是否生效（`sys_config.ko` 内的 `spi_lcd_pin_mux`）、`/sys/class/backlight/*/brightness` |
| `[mp] AO 失败` / 无声 | `ps \| grep sdk_server`，MPP 依赖 UDS 守护进程 |
| 声音持续爆音 | 音频线程优先级未生效（`[a] 实时优先级失败`），或环形缓冲被抽干 |
| 画面卡顿 | 看 `[v]` 行的「转换」「刷屏」耗时，确认视频源分辨率是否过高 |
| 链接报 `symbol not found` | MPP 的 .so 为隐式依赖，需整组链接（见 `source/CMakeLists.txt` 的 `_MPP_LIBS`） |

## 九、清理

```bash
# 同一案例增量重编前的清理
bash scripts/build.sh clean

# 切换不同案例前的彻底清理
bash scripts/build.sh distclean
```

---

## 十、第三方组件

| 组件 | 位置 | 说明 |
|------|------|------|
| FFmpeg 7.1 静态库 | `prebuilt/lib/` | LGPL-2.1-or-later。以静态库形式随仓库分发，如需商用再分发请注意 LGPL 对"允许重新链接"的要求，或改为目标机动态链接 `libav*` |
| SDK 补丁 | `patch/` | 取自 HiSpark `hi_aiot_solution_cv610_ev_board`（`references/factory_demo/ws73_lcd/patch`），沿用原仓库许可 |
| `badapple.mp4` | `media/` | 第三方作品，**仅作演示用途**，请勿商用或再分发；建议替换为自己拍摄的素材 |

本仓库自身代码采用 MIT 许可，见 [LICENSE](LICENSE)。

## 参考

- 方案总览：<https://docs.hisilicon.com/projects/hi_aiot_solution/master/overview/index.html>
- 案例目录：<https://docs.hisilicon.com/projects/hi_aiot_solution/master/samples/cv610_dev_board/index.html>
- 源码仓库：<https://gitcode.com/HiSpark/hi_aiot_solution>
