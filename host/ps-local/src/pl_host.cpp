#include "pl_host.h"
#include "../driver/pl_ctrl_uapi.h"

#include <cerrno>
#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int read_word(int fd, unsigned offset, uint32_t *value)
{
    if (!value) { errno = EINVAL; return -1; }
    ssize_t n;
    do { n = ::pread(fd, value, sizeof(*value), offset); } while (n < 0 && errno == EINTR);
    if (n == sizeof(*value)) return 0;
    if (n >= 0) errno = EIO;
    return -1;
}

/* One pread for a whole 4-byte-word run (§13). The §4.6 read must be a single
 * transfer so the FFT_SEQUENCE reads around it bracket the complete array. */
static_assert(sizeof(unsigned) == 4, "BIN_POWER words are unsigned32");

static int read_words(int fd, unsigned offset, unsigned *values, unsigned count)
{
    const size_t bytes = size_t(count) * sizeof(*values);
    ssize_t n;
    do { n = ::pread(fd, values, bytes, offset); } while (n < 0 && errno == EINTR);
    if (n == ssize_t(bytes)) return 0;
    if (n >= 0) errno = EIO;
    return -1;
}

static int read_dword(int fd, unsigned offset, uint64_t *value)
{
    if (!value) { errno = EINVAL; return -1; }
    uint32_t lo = 0, hi = 0;
    if (read_word(fd, offset, &lo) || read_word(fd, offset + 4, &hi)) return -1;
    *value = (uint64_t(hi) << 32) | lo;
    return 0;
}

static int write_word(int fd, unsigned offset, uint32_t value)
{
    ssize_t n;
    do { n = ::pwrite(fd, &value, sizeof(value), offset); } while (n < 0 && errno == EINTR);
    if (n == sizeof(value)) return 0;
    if (n >= 0) errno = EIO;
    return -1;
}

int pl_open(const char *dev) { return ::open(dev ? dev : "/dev/pl_ctrl", O_RDWR | O_CLOEXEC); }
void pl_close(int fd) { if (fd >= 0) ::close(fd); }
/* v1.2 acquisition remains compatible; alarm access requires v1.3. */
int pl_probe(int fd, uint32_t *id)
{
    if (read_word(fd, 0, id)) return -1;
    if (*id != PL_CTRL_ID_VALUE && *id != PL_CTRL_ID_COMPAT) { errno = ENODEV; return -1; }
    return 0;
}
int pl_configure(int fd, uint32_t ctrl) { return write_word(fd, 4, ctrl); }
int pl_status(int fd, uint32_t *stat) { return read_word(fd, 8, stat); }
int pl_latest(int fd, uint16_t *sample, uint16_t *cnt)
{
    if (!sample || !cnt) { errno = EINVAL; return -1; }
    uint32_t value;
    if (read_word(fd, 12, &value)) return -1;
    *sample = uint16_t(value); *cnt = uint16_t(value >> 16);
    return 0;
}
int pl_clear_irq(int fd) { return write_word(fd, 16, 1); }
int pl_wait_irq(int fd, int timeout_ms)
{
    struct pollfd p = {fd, POLLIN, 0};
    const int ret = ::poll(&p, 1, timeout_ms);
    if (ret <= 0) return ret;
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) { errno = ENODEV; return -1; }
    uint32_t count;
    if (!(p.revents & POLLIN)) { errno = EIO; return -1; }
    return ::ioctl(fd, PL_CTRL_IRQ_COUNT, &count) == 0 ? 1 : -1;
}

int pl_read_dma_ctrl(int fd, uint32_t *ctrl)
{
    return read_word(fd, PL_CTRL_DMA_CTRL, ctrl);
}

int pl_read_dma_status(int fd, pl_dma_status_t *out)
{
    if (!out) { errno = EINVAL; return -1; }
    uint32_t value = 0;
    if (read_word(fd, PL_CTRL_DMA_STATUS, &value)) return -1;
    out->bursts = value & PL_CTRL_DMA_STATUS_BURSTS_MASK;
    out->overflow = (value & PL_CTRL_DMA_STATUS_OVF) ? 1u : 0u;
    out->errcode = (value & PL_CTRL_DMA_STATUS_ERRCODE_MASK) >> PL_CTRL_DMA_STATUS_ERRCODE_SHIFT;
    return 0;
}

int pl_read_pl_ts(int fd, uint32_t *ts)
{
    return read_word(fd, PL_CTRL_PL_TS_NOW, ts);
}

int pl_read_sensors(int fd, pl_sensors_t *out)
{
    if (!out) { errno = EINVAL; return -1; }
    for (int attempt = 0; attempt < 8; ++attempt) {
        uint32_t first, last, counts, before_counts, words[8], faults, current_count;
        if (read_word(fd, 0x34, &before_counts) || read_word(fd, 0x54, &first)) return -1;
        for (unsigned i = 0; i < 8; ++i)
            if (read_word(fd, 0x58 + i * 4, &words[i])) return -1;
        if (read_word(fd, 0x54, &last) || read_word(fd, 0x34, &counts) ||
            read_word(fd, 0x3c, &faults) || read_word(fd, 0x38, &current_count)) return -1;
        if (first != last || counts != before_counts) continue;
        out->frame_ms = first;
        out->temp_x10 = int16_t(words[0]);
        out->acc_x = int16_t(words[1]); out->acc_y = int16_t(words[2]); out->acc_z = int16_t(words[3]);
        out->current_raw = int16_t(words[4]);
        out->acc_ts = words[5]; out->current_ts = words[6]; out->temp_ts = words[7];
        out->temp_count = uint16_t(counts); out->acc_count = uint16_t(counts >> 16);
        out->current_count = uint16_t(current_count);
        out->faults = faults;
        return 0;
    }
    errno = EAGAIN;
    return -1;
}

int pl_read_features(int fd, pl_features_t *out)
{
    if (!out) { errno = EINVAL; return -1; }
    std::memset(out, 0, sizeof(*out));
    if (read_word(fd, 0x18, &out->available)) return -1;
    if (write_word(fd, 0x14, 1)) return -1;
    for (int i = 0; i < 3; ++i) {
        const unsigned base = 0x100u + unsigned(i) * 0x40u;
        pl_feature_channel_t &ch = out->ch[i];
        if (read_dword(fd, base + 0x00, &ch.dc)
            || read_dword(fd, base + 0x08, &ch.rms)
            || read_dword(fd, base + 0x10, &ch.variance)
            || read_dword(fd, base + 0x18, &ch.peak_to_peak)
            || read_dword(fd, base + 0x20, &ch.crest)
            || read_dword(fd, base + 0x28, &ch.zero_cross)
            || read_dword(fd, base + 0x30, &ch.kurtosis)
            || read_word(fd, base + 0x38, &ch.sequence)
            || read_word(fd, base + 0x3C, &ch.flags))
            return -1;
    }
    if (read_word(fd, 0x78, &out->fft_sequence)
        || read_word(fd, 0x7C, &out->fft_peak_bin)
        || read_dword(fd, 0x80, &out->fft_peak_power)
        || read_word(fd, 0x88, &out->fft_flags))
        return -1;
    return 0;
}

int pl_read_spectrum(int fd, unsigned *bins, unsigned max_bins, unsigned *count,
                     unsigned *sequence)
{
    if (!bins || !max_bins || !count || !sequence) { errno = EINVAL; return -1; }
    const unsigned words = max_bins < PL_CTRL_SPECTRUM_WORDS
                         ? max_bins : unsigned(PL_CTRL_SPECTRUM_WORDS);
    /* §4.6: SEQUENCE -> BIN_POWER[0..N-1] -> SEQUENCE, accept only when both
     * reads agree; a difference means a new FFT window overwrote (part of) the
     * array while it was being fetched. The bins across one window are written
     * by the core in consecutive cycles and SEQUENCE increments after the last
     * write, so the two sequence reads are the only ordering evidence the
     * hardware offers. */
    for (int attempt = 0; attempt < 3; ++attempt) {
        uint32_t seq_before = 0, seq_after = 0;
        if (read_word(fd, 0x78, &seq_before)) return -1;
        if (read_words(fd, PL_CTRL_SPECTRUM_OFFSET, bins, words)) return -1;
        if (read_word(fd, 0x78, &seq_after)) return -1;
        if (seq_before == seq_after) {
            *count = words;
            *sequence = unsigned(seq_after);
            return 0;
        }
    }
    /* N=256 at the configured 3200 SPS retires a window every 80 ms, so three
     * consecutive collisions mean the reader is slower than the producer (or a
     * reset bumped SEQUENCE); report it instead of returning mixed bins. */
    errno = EAGAIN;
    return -EAGAIN;
}

int pl_set_current_calibration(int fd, int32_t zero, int32_t gain)
{
    return write_word(fd, 0x4c, uint32_t(zero)) || write_word(fd, 0x50, uint32_t(gain)) ? -1 : 0;
}

int pl_zero_current(int fd, int32_t zero)
{
    return write_word(fd, 0x4c, uint32_t(zero));
}

int pl_read_current_calibration(int fd, int32_t *zero, int32_t *gain)
{
    if (!zero || !gain) { errno = EINVAL; return -1; }
    uint32_t z = 0, g = 0;
    if (read_word(fd, 0x4c, &z) || read_word(fd, 0x50, &g)) return -1;
    *zero = int32_t(z);
    *gain = int32_t(g);
    return 0;
}

int pl_read_speed(int fd, pl_speed_t *out)
{
    if (fd < 0 || !out) { errno = EINVAL; return -1; }
    for (int attempt = 0; attempt < 3; ++attempt) {
        unsigned seq_before = 0, values[7] = {};
        if (read_word(fd, PL_CTRL_SPEED_SEQ, &seq_before)) return -1;
        /* 0xb8..0xd0 一次读完，避免逐字读期间被 PL 更新。 */
        if (read_words(fd, PL_CTRL_SPEED_RAW, values, 7)) return -1;
        unsigned seq_after = 0;
        if (read_word(fd, PL_CTRL_SPEED_SEQ, &seq_after)) return -1;
        if (seq_before == seq_after && seq_before == values[5]) {
            out->raw = int32_t(values[0]);
            out->rpm_milli = int32_t(values[1]);
            out->status = values[2];
            out->count = values[3];
            out->ts = values[4];
            out->seq = values[5];
            out->err_count = values[6];
            return 0;
        }
    }
    /* 100 ms 一个样本、每轮读数只要几微秒，连续 3 次撞上发布说明读数比
     * 生产者慢（或 PL 未就绪）：报告不可用，而不是返回拼凑的字段。 */
    errno = EAGAIN;
    return -EAGAIN;
}

int pl_dma_ring_open(pl_dma_ring_t *ring, int fd, unsigned base, unsigned blocks)
{
    if (!ring || fd < 0) { errno = EINVAL; return -1; }
    std::memset(ring, 0, sizeof(*ring));
    ring->mem_fd = -1;
    if (blocks < 2 || blocks > 256 || (base & 0xfffu)) { errno = EINVAL; return -1; }

    uint32_t ctrl = 0;
    if (read_word(fd, PL_CTRL_DMA_CTRL, &ctrl)) return -1;
    if (ctrl & PL_CTRL_DMA_CTRL_BUSY) {
        if (write_word(fd, PL_CTRL_DMA_CTRL, PL_CTRL_DMA_CTRL_ABORT)) return -1;
        for (int i = 0; i < 100; ++i) {
            if (read_word(fd, PL_CTRL_DMA_CTRL, &ctrl)) return -1;
            if (!(ctrl & PL_CTRL_DMA_CTRL_BUSY)) break;
            ::usleep(1000);
        }
        if (ctrl & PL_CTRL_DMA_CTRL_BUSY) { errno = EBUSY; return -1; }
    }

    if (write_word(fd, PL_CTRL_DMA_BASE_LO, base)
        || write_word(fd, PL_CTRL_DMA_BASE_HI, 0)
        || write_word(fd, PL_CTRL_DMA_BYTES, PL_DMA_BLOCK_BYTES)
        || write_word(fd, PL_CTRL_DMA_BLOCKS, blocks))
        return -1;

    ring->mem_fd = ::open("/dev/mem", O_RDONLY | O_SYNC | O_CLOEXEC);
    if (ring->mem_fd < 0) return -1;
    const size_t span = size_t(blocks) * PL_DMA_BLOCK_BYTES;
    void *map = ::mmap(nullptr, span, PROT_READ, MAP_SHARED, ring->mem_fd, off_t(base));
    if (map == MAP_FAILED) {
        const int saved = errno;
        ::close(ring->mem_fd);
        ring->mem_fd = -1;
        errno = saved;
        return -1;
    }
    ring->map = map;
    ring->base = base;
    ring->blocks = blocks;
    ring->last_seq = 0;
    if (write_word(fd, PL_CTRL_DMA_CTRL, PL_CTRL_DMA_CTRL_RING | PL_CTRL_DMA_CTRL_START)) {
        const int saved = errno;
        pl_dma_ring_stop(ring, fd);
        errno = saved;
        return -1;
    }
    return 0;
}

int pl_dma_ring_next(pl_dma_ring_t *ring, int fd, const uint64_t **words, uint32_t *seq)
{
    if (!ring || !ring->map || ring->blocks < 2 || fd < 0) { errno = EINVAL; return -1; }
    uint32_t done = 0;
    if (read_word(fd, PL_CTRL_DMA_BLK_SEQ, &done)) return -1;
    if (done == ring->last_seq) { errno = EAGAIN; return -1; }
    const uint32_t pending = done - ring->last_seq;
    if (pending >= (1u << 20) && done == 0) {
        // START has reset the producer; no block has completed in this epoch.
        ring->last_seq = 0;
        errno = EAGAIN;
        return -1;
    }
    // One slot is being written by PL. Recover the oldest unread completed slot
    // still retained, rather than dropping all but the newest after a slow poll.
    // A backwards sequence denotes START/reset: resume at the latest complete slot.
    const uint32_t available = pending >= (1u << 20) ? 1u
        : (pending < ring->blocks ? pending : ring->blocks - 1u);
    const uint32_t next = done - available + 1u;
    const unsigned index = (next - 1u) % ring->blocks;
    const auto *source = static_cast<const volatile uint64_t *>(ring->map)
        + size_t(index) * PL_DMA_BLOCK_WORDS;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    for (unsigned i = 0; i < PL_DMA_BLOCK_WORDS; ++i) ring->snapshot[i] = source[i];
    std::atomic_thread_fence(std::memory_order_seq_cst);
    uint32_t after = 0;
    if (read_word(fd, PL_CTRL_DMA_BLK_SEQ, &after)) return -1;
    // Reject a snapshot whose slot could have been overwritten during the copy.
    // Do not advance last_seq: the next poll will recover from the new producer head.
    if (after - next >= ring->blocks - 1u) { errno = EAGAIN; return -1; }
    ring->last_seq = next;
    if (words) *words = ring->snapshot;
    if (seq) *seq = next;
    return 0;
}

void pl_dma_ring_stop(pl_dma_ring_t *ring, int fd)
{
    if (!ring) return;
    if (fd >= 0 && ring->map) {
        uint32_t ctrl = 0;
        if (read_word(fd, PL_CTRL_DMA_CTRL, &ctrl) == 0 && (ctrl & PL_CTRL_DMA_CTRL_BUSY)) {
            /* STOP 是电平位，写时必须重复 RING，否则会退回单次模式。 */
            write_word(fd, PL_CTRL_DMA_CTRL, PL_CTRL_DMA_CTRL_RING | PL_CTRL_DMA_CTRL_STOP);
            for (int i = 0; i < 500; ++i) {
                if (read_word(fd, PL_CTRL_DMA_CTRL, &ctrl) || !(ctrl & PL_CTRL_DMA_CTRL_BUSY)) break;
                ::usleep(1000);
            }
            write_word(fd, PL_CTRL_DMA_CTRL, PL_CTRL_DMA_CTRL_RING);
        }
    }
    if (ring->map) {
        ::munmap(ring->map, size_t(ring->blocks) * PL_DMA_BLOCK_BYTES);
        ring->map = nullptr;
    }
    if (ring->mem_fd >= 0) {
        ::close(ring->mem_fd);
        ring->mem_fd = -1;
    }
}

int pl_set_alarm(int fd, uint32_t state) {
    uint32_t id=0;
    if (state & ~0x3fu) { errno=EINVAL; return -1; }
    if (pl_probe(fd,&id)) return -1;
    if (id!=PL_CTRL_ID_VALUE) { errno=ENOTSUP; return -1; }
    return write_word(fd,PL_CTRL_ALARM_STATE,state);
}
int pl_read_alarm(int fd, uint32_t *state, uint32_t *status) {
    uint32_t id=0;
    if (pl_probe(fd,&id)) return -1;
    if (id!=PL_CTRL_ID_VALUE) { errno=ENOTSUP; return -1; }
    return read_word(fd,PL_CTRL_ALARM_STATE,state) || read_word(fd,PL_CTRL_ALARM_STATUS,status) ? -1 : 0;
}
