/* vp6.c — M5.2: 四线程解耦的音视频同步播放器
 *
 *  解封装线程(main) : av_read_frame → 视频包队列 / 音频包队列（不做解码，很快）
 *  视频线程         : 取视频包 → 解码 → 按音频时钟节流 → 转 RGB565 → DRM 提交
 *  音频线程         : 取音频包 → 解码 → 重采样 → 塞环形缓冲（SCHED_FIFO，不被抢）
 *  AO 线程          : 环形缓冲 → MMZ → ss_mpi_ao_send_frame（阻塞，即音频时钟）
 *
 * 关键点：刷屏 37ms 完全不影响喂音频，因为音频有独立线程 + 4 秒缓冲。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>

#include "ss_mpi_audio.h"
#include "ss_mpi_sys_mem.h"

#define PANEL_W 240
#define PANEL_H 320
#define N_BUF   1   /* 局部刷新要求帧缓冲不变，否则内核强制整帧 */

#define AO_DEV        0
#define AO_CHN        0
#define CH            2
#define RATE          48000
#define CHUNK_FRAMES  1024
#define RING_FRAMES   (RATE * 4)
#define VIDEO_W       240
#define VIDEO_H       180

#define VQ_CAP 24
#define AQ_CAP 192

static long long now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000000LL + tv.tv_usec;
}

/* ==================== DRM ==================== */
static int      fd = -1;
static uint32_t crtc_id, conn_id, plane_id, blob_id;
static int      bpp = 16;
static uint32_t fourcc;
static struct { uint32_t fb; uint8_t *map; uint32_t pitch; uint64_t size; } bufs[N_BUF];
static uint32_t fc_conn, fc_mode, fc_act, fp_crtc, fp_fb, fp_sx, fp_sy, fp_sw, fp_sh,
                fp_cx, fp_cy, fp_cw, fp_ch;
static uint32_t fp_dmg;          /* DAMAGE_CLIPS 属性 */
static uint32_t damage_blob;     /* 只覆盖视频区域的 damage blob */

static uint32_t prop_id(uint32_t obj, uint32_t type, const char *name)
{
    drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, obj, type);
    uint32_t i, ret = 0;
    if (!props) return 0;
    for (i = 0; i < props->count_props; i++) {
        drmModePropertyPtr p = drmModeGetProperty(fd, props->props[i]);
        if (p) {
            if (strcmp(p->name, name) == 0) { ret = p->prop_id; drmModeFreeProperty(p); break; }
            drmModeFreeProperty(p);
        }
    }
    drmModeFreeObjectProperties(props);
    return ret;
}

static uint32_t find_plane(uint32_t crtc_idx, uint32_t fmt)
{
    drmModePlaneRes *pres = drmModeGetPlaneResources(fd);
    uint32_t i, j, found = 0;
    if (!pres) return 0;
    for (i = 0; i < pres->count_planes && !found; i++) {
        drmModePlane *p = drmModeGetPlane(fd, pres->planes[i]);
        if (!p) continue;
        if (!(p->possible_crtcs & (1u << crtc_idx))) { drmModeFreePlane(p); continue; }
        for (j = 0; j < p->count_formats; j++)
            if (p->formats[j] == fmt) { found = p->plane_id; break; }
        drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(pres);
    return found;
}

static int create_buf(int i)
{
    struct drm_mode_create_dumb creq;
    struct drm_mode_map_dumb mreq;
    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};

    memset(&creq, 0, sizeof(creq));
    creq.width = PANEL_W; creq.height = PANEL_H; creq.bpp = (uint32_t)bpp;
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) != 0) return -1;
    bufs[i].pitch = creq.pitch; bufs[i].size = creq.size;
    handles[0] = creq.handle; pitches[0] = creq.pitch;
    if (drmModeAddFB2(fd, PANEL_W, PANEL_H, fourcc, handles, pitches, offsets, &bufs[i].fb, 0) != 0)
        return -1;
    memset(&mreq, 0, sizeof(mreq));
    mreq.handle = creq.handle;
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) != 0) return -1;
    bufs[i].map = mmap(NULL, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset);
    return (bufs[i].map == MAP_FAILED) ? -1 : 0;
}

static int commit(int idx, int modeset)
{
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    uint32_t flags = 0;
    int ret;
    if (!req) return -1;
    if (modeset) {
        drmModeAtomicAddProperty(req, conn_id, fc_conn, crtc_id);
        drmModeAtomicAddProperty(req, crtc_id, fc_mode, blob_id);
        drmModeAtomicAddProperty(req, crtc_id, fc_act, 1);
        flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
    }
    drmModeAtomicAddProperty(req, plane_id, fp_fb, bufs[idx].fb);
    drmModeAtomicAddProperty(req, plane_id, fp_crtc, crtc_id);
    drmModeAtomicAddProperty(req, plane_id, fp_sx, 0);
    drmModeAtomicAddProperty(req, plane_id, fp_sy, 0);
    drmModeAtomicAddProperty(req, plane_id, fp_sw, (uint64_t)PANEL_W << 16);
    drmModeAtomicAddProperty(req, plane_id, fp_sh, (uint64_t)PANEL_H << 16);
    drmModeAtomicAddProperty(req, plane_id, fp_cx, 0);
    drmModeAtomicAddProperty(req, plane_id, fp_cy, 0);
    drmModeAtomicAddProperty(req, plane_id, fp_cw, PANEL_W);
    drmModeAtomicAddProperty(req, plane_id, fp_ch, PANEL_H);
    /* 非首次提交时只刷新视频矩形，其余（黑边）不传 */
    if (!modeset && fp_dmg && damage_blob)
        drmModeAtomicAddProperty(req, plane_id, fp_dmg, damage_blob);
    ret = drmModeAtomicCommit(fd, req, flags, NULL);
    drmModeAtomicFree(req);
    return ret;
}

static void set_backlight(void)
{
    DIR *d = opendir("/sys/class/backlight");
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        char path[256]; FILE *f; int max = 0, cur = 0;
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "/sys/class/backlight/%s/max_brightness", e->d_name);
        f = fopen(path, "r"); if (f) { if (fscanf(f, "%d", &max) != 1) max = 100; fclose(f); }
        if (max <= 0) max = 100;
        snprintf(path, sizeof(path), "/sys/class/backlight/%s/brightness", e->d_name);
        f = fopen(path, "r"); if (f) { if (fscanf(f, "%d", &cur) != 1) cur = 0; fclose(f); }
        if (cur < max * 7 / 10) {
            f = fopen(path, "w");
            if (f) { fprintf(f, "%d\n", max * 8 / 10); fclose(f); }
        }
    }
    closedir(d);
}

static int drm_init(void)
{
    drmModeRes *res; drmModeConnector *conn = NULL; drmModeModeInfo mode;
    uint32_t blob; int i;

    fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) { printf("[drm] open 失败: %s\n", strerror(errno)); return -1; }
    drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
    drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmSetMaster(fd);

    res = drmModeGetResources(fd);
    if (!res) return -1;
    for (i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c) continue;
        if (!conn && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) conn = c;
        else drmModeFreeConnector(c);
    }
    if (!conn) { printf("[drm] 无连接器\n"); return -1; }
    conn_id = conn->connector_id;
    memcpy(&mode, &conn->modes[0], sizeof(mode));
    if (drmModeCreatePropertyBlob(fd, &mode, sizeof(mode), &blob) != 0) return -1;
    blob_id = blob;
    crtc_id = res->crtcs[0];

    fourcc = DRM_FORMAT_RGB565; bpp = 16;
    plane_id = find_plane(0, fourcc);
    if (!plane_id) { fourcc = DRM_FORMAT_XRGB8888; bpp = 32; plane_id = find_plane(0, fourcc); }
    if (!plane_id) { printf("[drm] 无 plane\n"); return -1; }

    fc_conn = prop_id(conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    fc_mode = prop_id(crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID");
    fc_act  = prop_id(crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE");
    fp_crtc = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    fp_fb   = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");
    fp_sx   = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X");
    fp_sy   = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    fp_sw   = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W");
    fp_sh   = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H");
    fp_cx   = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    fp_cy   = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    fp_cw   = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    fp_ch   = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H");
    fp_dmg  = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "DAMAGE_CLIPS");
    if (!fc_conn || !fc_mode || !fp_fb || !fp_sw || !fp_cw) { printf("[drm] 属性缺失\n"); return -1; }
    printf("[drm] DAMAGE_CLIPS 属性: %s\n", fp_dmg ? "支持局部刷新" : "不支持（会退回整帧）");
    if (fp_dmg) {
        struct { int32_t x1, y1, x2, y2; } r;
        r.x1 = 0; r.y1 = (PANEL_H - VIDEO_H) / 2;
        r.x2 = VIDEO_W; r.y2 = (PANEL_H - VIDEO_H) / 2 + VIDEO_H;
        if (drmModeCreatePropertyBlob(fd, &r, sizeof(r), &damage_blob) != 0) {
            printf("[drm] damage blob 创建失败，退回整帧\n");
            fp_dmg = 0;
        } else {
            printf("[drm] 局部刷新区域 x=%d..%d y=%d..%d\n", r.x1, r.x2, r.y1, r.y2);
        }
    }

    for (i = 0; i < N_BUF; i++) if (create_buf(i) != 0) return -1;
    printf("[drm] %dx%d plane=%u bpp=%d\n", PANEL_W, PANEL_H, plane_id, bpp);
    return 0;
}

/* ==================== 帧转换 ==================== */
static void fill_black(int idx)
{
    for (uint32_t y = 0; y < PANEL_H; y++)
        memset(bufs[idx].map + (size_t)y * bufs[idx].pitch, 0,
               (size_t)PANEL_W * (bpp == 16 ? 2 : 4));
}

static int mx0[VIDEO_W], mux[VIDEO_W];
static int my0[VIDEO_H], muy[VIDEO_H];
static int g_map_w = -1, g_map_h = -1;

static void init_maps(int sw, int sh)
{
    if (sw == g_map_w && sh == g_map_h) return;
    g_map_w = sw; g_map_h = sh;
    for (int x = 0; x < VIDEO_W; x++) {
        mx0[x] = x * sw / VIDEO_W;
        mux[x] = x * (sw / 2) / VIDEO_W;
    }
    for (int y = 0; y < VIDEO_H; y++) {
        my0[y] = y * sh / VIDEO_H;
        muy[y] = y * (sh / 2) / VIDEO_H;
    }
}

static void yuv_to_rgb565(int idx, const AVFrame *f, int first)
{
    const uint8_t *Y = f->data[0], *U = f->data[1], *V = f->data[2];
    const int ys = f->linesize[0], us = f->linesize[1], vs = f->linesize[2];
    const int top = (PANEL_H - VIDEO_H) / 2;

    init_maps(f->width, f->height);
    if (first) fill_black(idx);   /* 黑边只写一次，之后靠局部刷新跳过 */

    for (int y = 0; y < VIDEO_H; y++) {
        uint16_t *row = (uint16_t *)(bufs[idx].map + (size_t)(y + top) * bufs[idx].pitch);
        const uint8_t *Yrow = Y + my0[y] * ys;
        const uint8_t *Urow = U + muy[y] * us;
        const uint8_t *Vrow = V + muy[y] * vs;
        for (int x = 0; x < VIDEO_W; x++) {
            int c = (int)Yrow[mx0[x]] - 16;
            int d = (int)Urow[mux[x]] - 128;
            int e = (int)Vrow[mux[x]] - 128;
            int r = (298 * c + 409 * e + 128) >> 8;
            int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
            int b = (298 * c + 516 * d + 128) >> 8;
            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;
            row[x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
}

/* ==================== 包队列 ==================== */
typedef struct {
    AVPacket **pkt;
    int cap, n;
    pthread_mutex_t m;
    pthread_cond_t notfull, notempty;
    int eof, stop;
} pktq_t;

static pktq_t vq, aq;

static void q_init(pktq_t *q, int cap)
{
    memset(q, 0, sizeof(*q));
    q->cap = cap;
    q->pkt = calloc((size_t)cap, sizeof(AVPacket *));
    pthread_mutex_init(&q->m, NULL);
    pthread_cond_init(&q->notfull, NULL);
    pthread_cond_init(&q->notempty, NULL);
}

static int q_push(pktq_t *q, AVPacket *p)
{
    pthread_mutex_lock(&q->m);
    while (q->n == q->cap && !q->stop) pthread_cond_wait(&q->notfull, &q->m);
    if (q->stop) { pthread_mutex_unlock(&q->m); return -1; }
    q->pkt[q->n++] = p;
    pthread_cond_signal(&q->notempty);
    pthread_mutex_unlock(&q->m);
    return 0;
}

static AVPacket *q_pop(pktq_t *q)
{
    AVPacket *p;
    pthread_mutex_lock(&q->m);
    while (q->n == 0 && !q->eof && !q->stop) pthread_cond_wait(&q->notempty, &q->m);
    if (q->n == 0) { pthread_mutex_unlock(&q->m); return NULL; }
    p = q->pkt[0];
    memmove(&q->pkt[0], &q->pkt[1], (size_t)(q->n - 1) * sizeof(AVPacket *));
    q->n--;
    pthread_cond_signal(&q->notfull);
    pthread_mutex_unlock(&q->m);
    return p;
}

static void q_flush(pktq_t *q)
{
    pthread_mutex_lock(&q->m);
    for (int i = 0; i < q->n; i++) av_packet_free(&q->pkt[i]);
    q->n = 0; q->eof = 0;
    pthread_cond_broadcast(&q->notfull);
    pthread_mutex_unlock(&q->m);
}

/* ==================== 环形缓冲（音频 PCM） ==================== */
static int16_t         *g_ring;
static int              g_head, g_cnt;
static pthread_mutex_t  g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   g_cv   = PTHREAD_COND_INITIALIZER;
static volatile int     g_run  = 1;
static volatile long long g_played_frames = 0;

static void ring_push(const int16_t *pcm, int frames)
{
    pthread_mutex_lock(&g_lock);
    while (RING_FRAMES - g_cnt < frames && g_run)
        pthread_cond_wait(&g_cv, &g_lock);
    for (int i = 0; i < frames; i++) {
        int pos = (g_head + g_cnt) % RING_FRAMES;
        g_ring[pos * CH + 0] = pcm[i * CH + 0];
        g_ring[pos * CH + 1] = pcm[i * CH + 1];
        g_cnt++;
    }
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_lock);
}

static int ring_pop(int16_t *out, int frames)
{
    pthread_mutex_lock(&g_lock);
    while (g_cnt < frames && g_run)
        pthread_cond_wait(&g_cv, &g_lock);
    if (!g_run && g_cnt < frames) { pthread_mutex_unlock(&g_lock); return 0; }
    for (int i = 0; i < frames; i++) {
        out[i * CH + 0] = g_ring[g_head * CH + 0];
        out[i * CH + 1] = g_ring[g_head * CH + 1];
        g_head = (g_head + 1) % RING_FRAMES;
        g_cnt--;
    }
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_lock);
    return 1;
}

/* ==================== AO ==================== */
static td_phys_addr_t g_mmz_phys;
static void          *g_mmz_virt;

static int ao_init(void)
{
    ot_aio_attr attr;
    int ret, chn;

    ss_mpi_audio_exit();
    if (ss_mpi_audio_init() != 0) { printf("[ao] init 失败\n"); return -1; }

    memset(&attr, 0, sizeof(attr));
    attr.sample_rate = OT_AUDIO_SAMPLE_RATE_48000;
    attr.bit_width   = OT_AUDIO_BIT_WIDTH_16;
    attr.work_mode   = OT_AIO_MODE_I2S_MASTER;
    attr.snd_mode    = OT_AUDIO_SOUND_MODE_STEREO;
    attr.frame_num   = 10;
    attr.point_num_per_frame = CHUNK_FRAMES;
    attr.chn_cnt     = 2;
    attr.clk_share   = 1;
    attr.i2s_type    = OT_AIO_I2STYPE_INNERCODEC;

    if (ss_mpi_ao_set_pub_attr(AO_DEV, &attr) != 0) { printf("[ao] set_pub_attr 失败\n"); return -1; }
    if (ss_mpi_ao_enable(AO_DEV) != 0) { printf("[ao] enable 失败\n"); return -1; }
    for (chn = 0; chn < (int)(attr.chn_cnt >> attr.snd_mode); chn++)
        if (ss_mpi_ao_enable_chn(AO_DEV, chn) != 0) { printf("[ao] enable_chn 失败\n"); return -1; }
    if (ss_mpi_ao_enable_chn(AO_DEV, OT_AO_SYS_CHN_ID) != 0) { printf("[ao] sys chn 失败\n"); return -1; }

    ret = ss_mpi_sys_mmz_alloc(&g_mmz_phys, (td_void **)&g_mmz_virt, "audio", TD_NULL,
                              (td_u32)(CHUNK_FRAMES * sizeof(int16_t) * CH * 2));
    if (ret != 0) { printf("[ao] mmz_alloc 失败: %#x\n", ret); return -1; }
    printf("[ao] 就绪 48k/16bit/立体声\n");
    return 0;
}

static void *ao_thread(void *arg)
{
    int16_t *tmp = malloc((size_t)CHUNK_FRAMES * CH * sizeof(int16_t));
    (void)arg;

    while (ring_pop(tmp, CHUNK_FRAMES)) {
        ot_audio_frame frame;
        int16_t *L = (int16_t *)g_mmz_virt;
        int16_t *R = (int16_t *)((char *)g_mmz_virt + CHUNK_FRAMES * (int)sizeof(int16_t));

        for (int k = 0; k < CHUNK_FRAMES; k++) { L[k] = tmp[k * CH]; R[k] = tmp[k * CH + 1]; }

        memset(&frame, 0, sizeof(frame));
        frame.bit_width    = OT_AUDIO_BIT_WIDTH_16;
        frame.snd_mode     = OT_AUDIO_SOUND_MODE_STEREO;
        frame.virt_addr[0] = (td_u8 *)g_mmz_virt;
        frame.virt_addr[1] = (td_u8 *)g_mmz_virt + CHUNK_FRAMES * (int)sizeof(int16_t);
        frame.phys_addr[0] = g_mmz_phys;
        frame.phys_addr[1] = g_mmz_phys + (td_phys_addr_t)(CHUNK_FRAMES * sizeof(int16_t));
        frame.len          = CHUNK_FRAMES * 2;

        if (ss_mpi_ao_send_frame(AO_DEV, AO_CHN, &frame, 1000) != 0) break;
        g_played_frames += CHUNK_FRAMES;
    }
    free(tmp);
    printf("[ao] 线程退出\n");
    return NULL;
}

static double audio_clock(void)
{
    long long f = g_played_frames - (long long)CHUNK_FRAMES * 3;
    if (f < 0) f = 0;
    return (double)f / RATE;
}

/* ==================== 线程上下文 ==================== */
static AVFormatContext *g_fmt;
static AVCodecContext  *g_vctx;
static AVCodecContext  *g_actx;
static int  g_vst = -1, g_ast = -1;
static double g_tb_v = 0.0, g_tb_a = 0.0;
static volatile int g_stop = 0;
static volatile int g_vpts_ready = 0, g_apts_ready = 0;
static double g_vpts0 = 0.0, g_apts0 = 0.0;
static volatile long long g_vframes = 0, g_dropped = 0;

static void *video_thread(void *arg)
{
    AVFrame *f = av_frame_alloc();
    int cur = 0, first = 1;
    long long stat_t0 = now_us(), stat_n = 0, conv_sum = 0, cmt_sum = 0;
    (void)arg;

    while (!g_stop) {
        AVPacket *p = q_pop(&vq);
        if (!p) break;

        if (avcodec_send_packet(g_vctx, p) == 0) {
            while (avcodec_receive_frame(g_vctx, f) == 0) {
                double vpts;
                if (!g_vpts_ready) { g_vpts0 = f->pts * g_tb_v; g_vpts_ready = 1; }
                vpts = f->pts * g_tb_v - g_vpts0;

                /* 同步：以音频时钟为准 */
                if (g_ast >= 0 && g_played_frames > (long long)CHUNK_FRAMES * 3) {
                    double delay = vpts - audio_clock();
                    if (delay < -0.200) {          /* 落后 >200ms，丢帧 */
                        g_dropped++;
                        av_frame_unref(f);
                        continue;
                    }
                    if (delay > 0.005) usleep((useconds_t)(delay * 1e6));
                }

                {
                    long long t0 = now_us();
                    yuv_to_rgb565(cur, f, first);
                    long long t1 = now_us();
                    commit(cur, first);
                    conv_sum += t1 - t0;
                    cmt_sum  += now_us() - t1;
                    first = 0;
                }
                cur = (cur + 1) % N_BUF;
                g_vframes++;
                stat_n++;
                av_frame_unref(f);

                if (now_us() - stat_t0 >= 2000000LL) {
                    double sec = (now_us() - stat_t0) / 1e6;
                    printf("[v] %.1f fps | 转换 %.1f | 刷屏 %.1f | 音频 %.2f s | 丢 %lld\n",
                           stat_n / sec,
                           stat_n ? conv_sum / 1000.0 / stat_n : 0.0,
                           stat_n ? cmt_sum / 1000.0 / stat_n : 0.0,
                           audio_clock(), g_dropped);
                    stat_t0 = now_us(); stat_n = 0; conv_sum = cmt_sum = 0;
                }
            }
        }
        av_packet_free(&p);
    }
    av_frame_free(&f);
    printf("[v] 线程退出\n");
    return NULL;
}

static void *audio_thread(void *arg)
{
    AVFrame *f = av_frame_alloc();
    SwrContext *swr = NULL;
    int16_t *acc = malloc((size_t)CHUNK_FRAMES * 4 * CH * sizeof(int16_t));
    int acc_frames = 0;
    struct sched_param sp;
    (void)arg;

    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = 60;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
        printf("[a] 实时优先级失败（可能出爆音）\n");
    else
        printf("[a] 已设 SCHED_FIFO/60\n");

    while (!g_stop) {
        AVPacket *p = q_pop(&aq);
        if (!p) break;

        if (avcodec_send_packet(g_actx, p) == 0) {
            while (avcodec_receive_frame(g_actx, f) == 0) {
                int max_out, got;
                uint8_t *out;

                if (!swr) {
                    AVChannelLayout lay = AV_CHANNEL_LAYOUT_STEREO;
                    if (swr_alloc_set_opts2(&swr, &lay, AV_SAMPLE_FMT_S16, RATE,
                                            &f->ch_layout, f->format, f->sample_rate, 0, NULL) < 0
                        || !swr || swr_init(swr) < 0) {
                        printf("[swr] 初始化失败\n");
                        av_frame_unref(f);
                        av_packet_free(&p);
                        return NULL;
                    }
                    if (!g_apts_ready) { g_apts0 = 0; g_apts_ready = 1; }
                }

                max_out = (int)av_rescale_rnd(
                    swr_get_delay(swr, f->sample_rate) + f->nb_samples,
                    RATE, f->sample_rate, AV_ROUND_UP);
                out = (uint8_t *)acc + (size_t)acc_frames * CH * sizeof(int16_t);
                got = swr_convert(swr, &out, max_out, (const uint8_t **)f->extended_data, f->nb_samples);
                if (got > 0) acc_frames += got;

                while (acc_frames >= CHUNK_FRAMES) {
                    ring_push(acc, CHUNK_FRAMES);
                    memmove(acc, acc + CHUNK_FRAMES * CH,
                            (size_t)(acc_frames - CHUNK_FRAMES) * CH * sizeof(int16_t));
                    acc_frames -= CHUNK_FRAMES;
                }
                av_frame_unref(f);
            }
        }
        av_packet_free(&p);
    }
    if (swr) swr_free(&swr);
    free(acc);
    av_frame_free(&f);
    printf("[a] 线程退出\n");
    return NULL;
}

/* ==================== main ==================== */
int main(int argc, char **argv)
{
    const char *file = (argc > 1) ? argv[1] : "/root/badapple.mp4";
    const AVCodec *vdec, *adec;
    pthread_t vt, at, aot;
    int i;

    printf("\n[vp6] 四线程音视频播放器\n[file] %s\n", file);

    if (drm_init() != 0) { printf("[vp6] DRM 失败\n"); return 1; }
    if (ao_init() != 0) { printf("[vp6] AO 失败\n"); return 1; }
    set_backlight();

    g_ring = malloc((size_t)RING_FRAMES * CH * sizeof(int16_t));
    if (!g_ring) return 1;

    if (avformat_open_input(&g_fmt, file, NULL, NULL) != 0) { printf("[ffmpeg] 打不开\n"); return 1; }
    if (avformat_find_stream_info(g_fmt, NULL) < 0) { printf("[ffmpeg] 无流信息\n"); return 1; }

    for (i = 0; i < (int)g_fmt->nb_streams; i++) {
        if (g_fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && g_vst < 0) g_vst = i;
        if (g_fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && g_ast < 0) g_ast = i;
    }
    if (g_vst < 0) { printf("[ffmpeg] 无视频流\n"); return 1; }
    g_tb_v = av_q2d(g_fmt->streams[g_vst]->time_base);

    vdec = avcodec_find_decoder(g_fmt->streams[g_vst]->codecpar->codec_id);
    g_vctx = avcodec_alloc_context3(vdec);
    avcodec_parameters_to_context(g_vctx, g_fmt->streams[g_vst]->codecpar);
    g_vctx->skip_loop_filter = AVDISCARD_ALL;
    if (avcodec_open2(g_vctx, vdec, NULL) < 0) return 1;

    if (g_ast >= 0) {
        g_tb_a = av_q2d(g_fmt->streams[g_ast]->time_base);
        adec = avcodec_find_decoder(g_fmt->streams[g_ast]->codecpar->codec_id);
        g_actx = avcodec_alloc_context3(adec);
        avcodec_parameters_to_context(g_actx, g_fmt->streams[g_ast]->codecpar);
        if (avcodec_open2(g_actx, adec, NULL) < 0) return 1;
        printf("[ffmpeg] 视频 %dx%d / 音频 %d Hz %d ch\n",
               g_vctx->width, g_vctx->height, g_actx->sample_rate, g_actx->ch_layout.nb_channels);
    } else {
        printf("[ffmpeg] 视频 %dx%d（无音频）\n", g_vctx->width, g_vctx->height);
    }

    q_init(&vq, VQ_CAP);
    q_init(&aq, AQ_CAP);

    pthread_create(&vt, NULL, video_thread, NULL);
    if (g_ast >= 0) {
        pthread_create(&aot, NULL, ao_thread, NULL);
        pthread_create(&at, NULL, audio_thread, NULL);
    }

    {
        AVPacket *pkt = av_packet_alloc();
        while (1) {
            if (av_read_frame(g_fmt, pkt) < 0) break;
            if (pkt->stream_index == g_vst) {
                if (q_push(&vq, pkt) != 0) { av_packet_free(&pkt); break; }
            } else if (g_ast >= 0 && pkt->stream_index == g_ast) {
                if (q_push(&aq, pkt) != 0) { av_packet_free(&pkt); break; }
            } else {
                av_packet_unref(pkt);
                continue;
            }
            pkt = av_packet_alloc();
        }
        av_packet_free(&pkt);
    }

    /* 收尾 */
    vq.eof = 1; aq.eof = 1;
    pthread_cond_broadcast(&vq.notempty);
    pthread_cond_broadcast(&aq.notempty);

    pthread_join(vt, NULL);
    if (g_ast >= 0) {
        pthread_join(at, NULL);
        g_run = 0;
        pthread_cond_broadcast(&g_cv);
        pthread_join(aot, NULL);
    }

    printf("\n[vp6] 播放结束：显示 %lld 帧，丢 %lld 帧\n", g_vframes, g_dropped);
    return 0;
}
