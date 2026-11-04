#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t frame_ms, acc_ts, current_ts, temp_ts, faults;
    int16_t temp_x10, acc_x, acc_y, acc_z, current_raw;
    uint16_t temp_count, acc_count, current_count;
} pl_sensors_t;

typedef struct {
    uint64_t dc, rms, variance, peak_to_peak, crest, zero_cross, kurtosis;
    uint32_t sequence, flags;
} pl_feature_channel_t;

typedef struct {
    uint32_t available;
    pl_feature_channel_t ch[3];
    uint32_t fft_sequence, fft_peak_bin, fft_flags;
    uint64_t fft_peak_power;
} pl_features_t;

int pl_read_sensors(int fd, pl_sensors_t *out);
int pl_read_features(int fd, pl_features_t *out);
/* §4.6 full spectrum window: BIN_POWER words are (|X_k|^2) >> 8. Read 0x78
 * FFT_SEQUENCE, one pread of
 * count = min(max_bins, PL_CTRL_SPECTRUM_WORDS) words at 0x400, read 0x78
 * again; the bins are only accepted when both sequence reads match (the
 * returned *sequence is the second read). A mismatch redoes the whole burst,
 * up to 3 attempts; a persistent mismatch returns -EAGAIN with errno=EAGAIN,
 * and the bins are then unusable. On success *count and *sequence are set. */
int pl_read_spectrum(int fd, unsigned *bins, unsigned max_bins, unsigned *count,
                     unsigned *sequence);
int pl_set_current_calibration(int fd, int32_t zero, int32_t gain);
/* 只写 CURRENT_ZERO(0x4C)，保留已标定的 CURRENT_GAIN(0x50)。 */
int pl_zero_current(int fd, int32_t zero);
int pl_read_current_calibration(int fd, int32_t *zero, int32_t *gain);
int pl_open(const char *dev);
void pl_close(int fd);
int pl_probe(int fd, uint32_t *id);
int pl_configure(int fd, uint32_t ctrl);
int pl_status(int fd, uint32_t *stat);
int pl_latest(int fd, uint16_t *sample, uint16_t *cnt);
int pl_clear_irq(int fd);
int pl_wait_irq(int fd, int timeout_ms);
/* §4.4 DMA 状态：环形消费者用它统计溢出与错误，并测「采集到读取」延迟。 */
typedef struct {
    uint32_t bursts;    /* DMA_STATUS[15:0]，本次传输已完成突发数 */
    uint32_t overflow;  /* DMA_STATUS[16]，1 = 内部缓冲溢出 */
    uint32_t errcode;   /* DMA_STATUS[19:17]：0 无错 · 1 参数非法 · 2 写响应非 OKAY · 3 中止 */
} pl_dma_status_t;

/* DMA_CTRL[8]：1 = 传输中。 */
#define PL_DMA_CTRL_BUSY (1u << 8)

int pl_read_dma_ctrl(int fd, uint32_t *ctrl);
int pl_read_dma_status(int fd, pl_dma_status_t *out);
/* PL_TS_NOW：与数据字 TS 同一时基，10 ns/LSB，32 位回绕。 */
int pl_read_pl_ts(int fd, uint32_t *ts);
/* 4 KiB 块的环形搬运（契约 §4.4）。块内为 8 字节字：
 * {ts[63:32], ch[31:16], sample[15:0]}，ch 0/1/2 对应 X/Y/Z。 */
#define PL_DMA_BLOCK_BYTES 4096u
#define PL_DMA_BLOCK_WORDS (PL_DMA_BLOCK_BYTES / 8u)

typedef struct {
    int mem_fd;      /* /dev/mem，-1 表示未打开 */
    void *map;       /* 环形缓冲只读映射 */
    unsigned base;
    unsigned blocks;
    unsigned last_seq;
    uint64_t snapshot[PL_DMA_BLOCK_WORDS]; /* 读取后归 PS 所有，下一次 next 前有效 */
} pl_dma_ring_t;

int pl_dma_ring_open(pl_dma_ring_t *ring, int fd, unsigned base, unsigned blocks);
/* 至少 2 个槽；按序读取仍保留的完成块，排除 PL 正在写入的槽。
 * 返回 PS 快照，不把可被 PL 覆写的映射地址交给解析器。 */
int pl_dma_ring_next(pl_dma_ring_t *ring, int fd, const uint64_t **words, uint32_t *seq);
void pl_dma_ring_stop(pl_dma_ring_t *ring, int fd);

/* 接口 v1.2 的 RS485 转速通道（0xb8..0xd0，只读）。每 100 ms 一个样本：
 * raw 是这段时间的编码器计数差，rpm_milli = raw*60000000/(4096*100) 的
 * 千分整数（0.001 rpm/LSB，反转时为负）。status 各位见
 * pl_ctrl_uapi.h 的 PL_CTRL_SPEED_STATUS_*：在线/配置完成/换算饱和/
 * 最近事件为错误/error_code/exception_code。DMA 记录丢弃并入
 * DMA_STATUS.overflow，不在本状态字内。 */
typedef struct {
    int32_t raw;
    int32_t rpm_milli;
    uint32_t status;
    uint32_t count;     /* 成功样本数，只在正确帧上递增 */
    uint32_t ts;        /* 事件时间戳，10 ns/LSB，32 位回绕 */
    uint32_t seq;       /* 发布事件计数，含不产生新样本的错误事件 */
    uint32_t err_count; /* 错误事件数 */
} pl_speed_t;

/* 一致性读取：0xcc(seq) -> 一次 pread 0xb8..0xd0 -> 0xcc(seq)。两次 seq 相同
 * 且与突发内的 seq 一致才接受，否则整轮重读（至多 3 次）；
 * 持续不一致返回 -EAGAIN（errno=EAGAIN），此时各字段不可用。 */
int pl_read_speed(int fd, pl_speed_t *out);
/* v1.3: heartbeat <=250 ms; hardware lease 2 s; [3:0] channels, [5:4] grade. */
int pl_set_alarm(int fd, uint32_t state);
int pl_read_alarm(int fd, uint32_t *state, uint32_t *status);

#ifdef __cplusplus
}
#endif
