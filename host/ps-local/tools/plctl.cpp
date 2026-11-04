#include "pl_host.h"
#include "dma_frames.h"   /* CH=3/4/5 speed record layout (§4.4/§14) */
#include "../driver/pl_ctrl_uapi.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <climits>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <algorithm>
#include <cmath>
#include <vector>

static bool number(const char *text, uint32_t &value)
{
    char *end;
    errno=0;
    const unsigned long long n=std::strtoull(text, &end, 0);
    if(errno || !*text || *end || n>UINT32_MAX) return false;
    value=uint32_t(n);return true;
}

static bool read32(int fd, uint32_t offset, uint32_t &value)
{
    const ssize_t n = ::pread(fd, &value, sizeof(value), offset);
    if (n == ssize_t(sizeof(value)))
        return true;
    if (n >= 0)
        errno = EIO;
    return false;
}

static bool write32(int fd, uint32_t offset, uint32_t value)
{
    const ssize_t n = ::pwrite(fd, &value, sizeof(value), offset);
    if (n == ssize_t(sizeof(value)))
        return true;
    if (n >= 0)
        errno = EIO;
    return false;
}

static bool read64(int fd, uint32_t offset, uint64_t &value)
{
    uint32_t lo = 0;
    uint32_t hi = 0;
    if (!read32(fd, offset, lo) || !read32(fd, offset + 4, hi))
        return false;
    value = (uint64_t(hi) << 32) | lo;
    return true;
}

static int features(int fd)
{
    uint32_t available = 0;
    if (!read32(fd, 0x18, available)) {
        std::perror("features: FEATURE_AVAILABLE");
        return -1;
    }
    const uint32_t one = 1;
    if (::pwrite(fd, &one, 4, 0x14) != 4) {
        std::perror("features: FEATURE_SNAPSHOT");
        return -1;
    }
    std::printf("FEATURE_AVAILABLE=0x%08x\n", available);

    struct Channel { const char *name; uint32_t base; };
    const Channel channels[] = { { "X", 0x100 }, { "Y", 0x140 }, { "Z", 0x180 } };
    for (const Channel &ch : channels) {
        uint64_t dc = 0, rms = 0, var = 0, pp = 0, crest = 0, zcr = 0, kurt = 0;
        uint32_t seq = 0, flags = 0;
        if (!read64(fd, ch.base + 0x00, dc) || !read64(fd, ch.base + 0x08, rms)
            || !read64(fd, ch.base + 0x10, var) || !read64(fd, ch.base + 0x18, pp)
            || !read64(fd, ch.base + 0x20, crest) || !read64(fd, ch.base + 0x28, zcr)
            || !read64(fd, ch.base + 0x30, kurt) || !read32(fd, ch.base + 0x38, seq)
            || !read32(fd, ch.base + 0x3C, flags)) {
            std::perror("features: channel");
            return -1;
        }
        std::printf("%s DC=%.4f RMS=%.4f VAR=%.4f PP=%llu CREST=%.4f ZCR=%.4f KURT=%.4f seq=%u flags=0x%08x\n",
                    ch.name, double(int64_t(dc)) / 65536.0, double(rms) / 65536.0,
                    double(var) / 65536.0, (unsigned long long)pp, double(crest) / 65536.0,
                    double(zcr) / 65536.0, double(kurt) / 65536.0, seq, flags);
    }

    uint32_t fftSeq = 0, peakBin = 0, fftFlags = 0;
    uint64_t peakPower = 0;
    if (!read32(fd, 0x78, fftSeq) || !read32(fd, 0x7C, peakBin)
        || !read64(fd, 0x80, peakPower) || !read32(fd, 0x88, fftFlags)) {
        std::perror("features: FFT");
        return -1;
    }
    std::printf("FFT seq=%u peak_bin=%u peak_power=%.4f flags=0x%08x\n",
                fftSeq, peakBin, double(peakPower) / 65536.0, fftFlags);
    return 0;
}

struct dma_state { uint32_t ctrl, status, written; };

static bool dma_state_get(int fd, dma_state &st)
{
    return read32(fd, PL_CTRL_DMA_CTRL, st.ctrl)
        && read32(fd, PL_CTRL_DMA_STATUS, st.status)
        && read32(fd, PL_CTRL_DMA_WRITTEN, st.written);
}

static uint32_t dma_errcode(const dma_state &st)
{
    return (st.status & PL_CTRL_DMA_STATUS_ERRCODE_MASK) >> PL_CTRL_DMA_STATUS_ERRCODE_SHIFT;
}

static const char *dma_errcode_text(uint32_t code)
{
    switch (code) {
    case PL_CTRL_DMA_ERRCODE_PARAM: return "invalid length or alignment";
    case PL_CTRL_DMA_ERRCODE_BRESP: return "write response not OKAY";
    case PL_CTRL_DMA_ERRCODE_ABORT: return "aborted";
    default: return "unknown";
    }
}

static void dma_state_print(const dma_state &st)
{
    std::printf("DMA_CTRL=0x%08x BUSY=%u DONE=%u ERROR=%u TEST=%u\n", st.ctrl,
                (st.ctrl & PL_CTRL_DMA_CTRL_BUSY) ? 1U : 0U,
                (st.ctrl & PL_CTRL_DMA_CTRL_DONE) ? 1U : 0U,
                (st.ctrl & PL_CTRL_DMA_CTRL_ERROR) ? 1U : 0U,
                (st.ctrl & PL_CTRL_DMA_CTRL_TEST) ? 1U : 0U);
    std::printf("DMA_STATUS=0x%08x BURSTS=%u OVF=%u ERRCODE=%u\n", st.status,
                st.status & PL_CTRL_DMA_STATUS_BURSTS_MASK,
                (st.status & PL_CTRL_DMA_STATUS_OVF) ? 1U : 0U, dma_errcode(st));
    std::printf("DMA_WRITTEN=%u\n", st.written);
}

static double usec_since(const struct timespec &start)
{
    struct timespec now;
    ::clock_gettime(CLOCK_MONOTONIC, &now);
    return double(now.tv_sec - start.tv_sec) * 1e6
         + double(now.tv_nsec - start.tv_nsec) / 1e3;
}

/* /dev/mem window used to read the DMA buffer. O_SYNC selects the Normal-NC
 * mapping on arm64, so the CPU sees what the PL wrote without stale cache
 * lines. A ring maps the whole ring once and walks it block by block. */
struct mem_window {
    int fd = -1;
    void *map = MAP_FAILED;
    size_t span = 0;
    uint32_t offset = 0;
    const uint64_t *words = nullptr;
};

static bool window_open(mem_window &w, uint32_t base, uint32_t bytes, const char *hint)
{
    const long page = ::sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        std::fprintf(stderr, "dma: sysconf(_SC_PAGESIZE): %s\n", std::strerror(errno));
        errno = 0;
        return false;
    }
    const uint32_t pageMask = uint32_t(page) - 1U;
    w.offset = base & pageMask;
    w.span = size_t(((uint64_t(w.offset) + bytes + uint64_t(pageMask))
                     / uint64_t(page)) * uint64_t(page));
    w.fd = ::open("/dev/mem", O_RDONLY | O_SYNC | O_CLOEXEC);
    if (w.fd < 0) {
        std::fprintf(stderr, "dma: /dev/mem: %s (root and CONFIG_DEVMEM=y are required)\n",
                     std::strerror(errno));
        errno = 0;
        return false;
    }
    w.map = ::mmap(nullptr, w.span, PROT_READ, MAP_SHARED, w.fd, off_t(base - w.offset));
    if (w.map == MAP_FAILED) {
        std::fprintf(stderr, "dma: mmap 0x%08x+%u: %s (%s)\n",
                     base, bytes, std::strerror(errno), hint);
        ::close(w.fd);
        w.fd = -1;
        errno = 0;
        return false;
    }
    w.words = reinterpret_cast<const uint64_t *>(
        static_cast<const char *>(w.map) + w.offset);
    return true;
}

static void window_close(mem_window &w)
{
    if (w.map != MAP_FAILED)
        ::munmap(w.map, w.span);
    if (w.fd >= 0)
        ::close(w.fd);
    w.map = MAP_FAILED;
    w.fd = -1;
    w.words = nullptr;
}

struct sample_word { uint32_t ts; uint16_t ch; int16_t sample; };

static sample_word sample_decode(uint64_t word)
{
    sample_word w;
    w.ts = uint32_t(word >> 32);
    w.ch = uint16_t(word >> 16);
    w.sample = int16_t(word & 0xffffU);
    return w;
}

/* §4.4: PL_TS_NOW and the sample TS share the 10 ns/LSB timebase and both wrap
 * modulo 2^32, i.e. one full period per 2^32 * 10 ns = 42.94967296 s. The age is
 * therefore a wrapping (modular) subtraction of two 32-bit counters. The result
 * is ambiguous only when the true age exceeds half a period (2^31 ticks =
 * 21.47 s), e.g. a block left over from before the last wrap; that case is
 * rejected instead of being reported as a real delay. The TS of a whole 1 kHz
 * frame is identical, so a single measurement is quantized to +-1 ms. */
#define PL_TS_HALF_PERIOD 0x80000000U

/* Age of one sample TS in microseconds, wrap included. False when the wrap
 * makes the difference ambiguous. */
static bool pl_ts_age_us(uint32_t nowTicks, uint32_t sampleTs, double &us)
{
    const uint32_t delta = nowTicks - sampleTs;   /* wraps at 2^32 */
    if (delta >= PL_TS_HALF_PERIOD)
        return false;
    us = double(delta) * double(PL_CTRL_PL_TS_TICK_NS) / 1000.0;
    return true;
}

static bool latency_measure(int fd, uint32_t sampleTs, double &us)
{
    uint32_t now = 0;
    if (!read32(fd, PL_CTRL_PL_TS_NOW, now))
        return false;
    if (!pl_ts_age_us(now, sampleTs, us)) {
        us = -1.0;   /* stale across a wrap, not a real delay */
        return true;
    }
    return true;
}

/* 0 = read, -2 = this pl_ctrl.ko predates PL_CTRL_DMA_IRQ_COUNT, -1 = error. */
static int dma_irq_count_read(int fd, uint32_t &count)
{
    if (::ioctl(fd, PL_CTRL_DMA_IRQ_COUNT, &count) == 0)
        return 0;
    if (errno == ENOTTY || errno == EINVAL) {
        errno = 0;
        return -2;
    }
    std::fprintf(stderr, "dma: ioctl PL_CTRL_DMA_IRQ_COUNT: %s\n", std::strerror(errno));
    errno = 0;
    return -1;
}

enum { WAIT_PROGRESS = 0, WAIT_TIMEOUT = 1, WAIT_NO_IRQ = 2, WAIT_ERROR = -1 };

/* Monotonic microseconds; absolute values are only compared with each other, so
 * the arbitrary epoch is irrelevant. */
static uint64_t usec_now(void)
{
    struct timespec t;
    ::clock_gettime(CLOCK_MONOTONIC, &t);
    return uint64_t(t.tv_sec) * 1000000U + uint64_t(t.tv_nsec) / 1000U;
}

/* Poll pacing for the path without a block IRQ. The sleep between two
 * DMA_BLK_SEQ reads follows the measured producer period (EWMA), sleep =
 * period/16 clamped into [250 us, 1 ms]: a 4 KiB block every 54 ms is seen
 * within 1 ms of becoming visible, a fast producer near 1 ms keeps the 250 us
 * granularity, and the syscall cost stays far below one block. The measured
 * mean is printed, so the actual interval on the board is not an assumption. */
#define PL_RING_POLL_MIN_US 250U
#define PL_RING_POLL_MAX_US 1000U
#define PL_RING_POLL_DIVISOR 16.0
/* poll() window of the IRQ wait. The same window bounds a safety-net
 * DMA_BLK_SEQ check, so an undelivered line costs 8 ms instead of --timeout. */
#define PL_RING_IRQ_WINDOW_MS 8

struct poll_pacer {
    uint32_t intervalUs = PL_RING_POLL_MIN_US;
    double periodUs = 0.0;      /* EWMA of the producer period; 0 = not measured */
    uint64_t sleeps = 0;
    uint64_t sleptUs = 0;

    void observe(double blockPeriodUs)
    {
        if (!(blockPeriodUs > 0.0))
            return;
        periodUs = periodUs > 0.0 ? periodUs + 0.25 * (blockPeriodUs - periodUs)
                                  : blockPeriodUs;
        uint32_t want = uint32_t(periodUs / PL_RING_POLL_DIVISOR);
        if (want < PL_RING_POLL_MIN_US)
            want = PL_RING_POLL_MIN_US;
        if (want > PL_RING_POLL_MAX_US)
            want = PL_RING_POLL_MAX_US;
        intervalUs = want;
    }

    void sleep_once(void)
    {
        struct timespec pause = {0, long(intervalUs) * 1000L};
        const uint64_t t0 = usec_now();
        ::nanosleep(&pause, nullptr);
        ++sleeps;
        sleptUs += usec_now() - t0;
    }

    double mean_sleep_us(void) const
    {
        return sleeps ? double(sleptUs) / double(sleeps) : 0.0;
    }
};

static int read_seq(int fd, uint32_t &seq)
{
    if (read32(fd, PL_CTRL_DMA_BLK_SEQ, seq))
        return 0;
    std::fprintf(stderr, "dma: read DMA_BLK_SEQ: %s\n", std::strerror(errno));
    errno = 0;
    return -1;
}

/* Wait for the next §4.4 block while the block IRQ is armed. poll() blocks
 * until an interrupt of this device is serviced, and the DMA counter separates
 * block IRQs from the sensor IRQ that shares the node. Every poll window also
 * reads DMA_BLK_SEQ: a declared line that delivers no GIC event (trigger type
 * mismatch, or a pulse shorter than the synchroniser) must not stall the reader
 * for the whole --timeout while the PL keeps overwriting the ring. Such a
 * completion returns viaSeq=true so the caller reports the diagnosis once and
 * keeps reading. */
static int wait_block_irq(int fd, uint32_t &seen, uint32_t lastSeq, uint32_t timeoutMs,
                          uint32_t &seq, bool &viaSeq)
{
    struct timespec t0;
    ::clock_gettime(CLOCK_MONOTONIC, &t0);
    viaSeq = false;
    bool noCounter = false;     /* device/driver has no usable IRQ line */

    for (;;) {
        const double remainMs = double(timeoutMs) - usec_since(t0) / 1000.0;
        if (remainMs <= 0.0)
            return WAIT_TIMEOUT;
        const double windowMs = remainMs < double(PL_RING_IRQ_WINDOW_MS)
                              ? remainMs : double(PL_RING_IRQ_WINDOW_MS);
        struct pollfd pfd = {fd, POLLIN, 0};
        const int n = ::poll(&pfd, 1, int(windowMs));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            std::fprintf(stderr, "dma: poll: %s\n", std::strerror(errno));
            errno = 0;
            return WAIT_ERROR;
        }
        if (n > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)))
            noCounter = true;
        if (n > 0 && !noCounter) {
            uint32_t count = 0;
            const int rc = dma_irq_count_read(fd, count);
            if (rc == -2) {
                noCounter = true;
            } else if (rc < 0) {
                return WAIT_ERROR;
            } else if (count != seen) {
                seen = count;
                return read_seq(fd, seq) == 0 ? WAIT_PROGRESS : WAIT_ERROR;
            }
            /* A wake without a new block IRQ is spurious (shared sensor IRQ);
             * the seq check below decides. */
        }
        if (read_seq(fd, seq) != 0)
            return WAIT_ERROR;
        if (seq != lastSeq) {
            viaSeq = true;
            return WAIT_PROGRESS;
        }
        if (noCounter)
            return WAIT_NO_IRQ;
    }
}

/* Polling fallback: a new completed block is visible as a DMA_BLK_SEQ change. */
static int wait_block_seq(int fd, uint32_t lastSeq, uint32_t timeoutMs, uint32_t &seq,
                          poll_pacer &pacer)
{
    struct timespec t0;
    ::clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        if (read_seq(fd, seq) != 0)
            return WAIT_ERROR;
        if (seq != lastSeq)
            return WAIT_PROGRESS;
        if (usec_since(t0) >= double(timeoutMs) * 1000.0)
            return WAIT_TIMEOUT;
        pacer.sleep_once();
    }
}

/* True when the block holds the §4.4 internal ramp: word i == word 0 + i. The
 * counter is only cleared by PL reset, so word 0 need not be 0. */
static bool block_ramp_ok(const uint64_t *blk, uint32_t words)
{
    const uint64_t first = blk[0];
    for (uint32_t i = 1; i < words; ++i)
        if (blk[i] != first + uint64_t(i))
            return false;
    return true;
}

/* Verify the DDR buffer through /dev/mem. O_SYNC selects the Normal-NC mapping
 * on arm64, so the CPU sees what the PL wrote without stale cache lines. */
static int dma_check_buffer(uint32_t base, uint32_t bytes, bool test)
{
    mem_window win;
    if (!window_open(win, base, bytes, "base must be in reserved-memory"))
        return -1;
    const uint64_t *words = win.words;
    const uint32_t count = bytes / 8U;
    int ret = 0;
    if (test) {
        /* The PL ramp counter is cleared only by PL reset, so a repeated run
         * starts at a non-zero word; check consecutive 64-bit increments and
         * report the start value. ramp_start=0 is the contract's word i == i. */
        const uint64_t first = words[0];
        uint32_t bad = 0;
        bool mismatch = false;
        for (uint32_t i = 1; i < count; ++i) {
            if (words[i] != first + uint64_t(i)) { bad = i; mismatch = true; break; }
        }
        if (mismatch) {
            std::printf("verify=FAIL word=%u byte_offset=0x%x expected=0x%016llx actual=0x%016llx\n",
                        bad, bad * 8U, (unsigned long long)(first + bad),
                        (unsigned long long)words[bad]);
            ret = -1;
        } else if (first == 0) {
            std::printf("verify=PASS words=%u ramp_start=0\n", count);
        } else {
            std::printf("verify=PASS words=%u ramp_start=%llu (counter runs until PL reset)\n",
                        count, (unsigned long long)first);
        }
    } else {
        std::printf("verify=SKIP (capture stream; pass --test for the ramp check)\n");
        const uint32_t shown = count < 4U ? count : 4U;
        for (uint32_t i = 0; i < shown; ++i)
            std::printf("word[%u]=0x%016llx\n", i, (unsigned long long)words[i]);
    }
    window_close(win);
    if (ret < 0)
        errno = 0;
    return ret;
}

static int dma_abort(int fd)
{
    dma_state st = {};
    if (!dma_state_get(fd, st)) {
        std::fprintf(stderr, "dma: read registers before ABORT: %s\n", std::strerror(errno));
        errno = 0;
        return -1;
    }
    const bool wasBusy = (st.ctrl & PL_CTRL_DMA_CTRL_BUSY) != 0;
    std::printf("dma abort busy_before=%u\n", wasBusy ? 1U : 0U);
    if (!write32(fd, PL_CTRL_DMA_CTRL, PL_CTRL_DMA_CTRL_ABORT)) {
        std::fprintf(stderr, "dma: write DMA_CTRL.ABORT: %s\n", std::strerror(errno));
        errno = 0;
        return -1;
    }
    struct timespec t0;
    ::clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        if (!read32(fd, PL_CTRL_DMA_CTRL, st.ctrl)) {
            std::fprintf(stderr, "dma: poll DMA_CTRL after ABORT: %s\n", std::strerror(errno));
            errno = 0;
            return -1;
        }
        if (!(st.ctrl & PL_CTRL_DMA_CTRL_BUSY))
            break;
        if (usec_since(t0) >= 100000.0) {
            std::fprintf(stderr, "dma: BUSY did not clear within 100 ms after ABORT\n");
            errno = 0;
            return -1;
        }
        struct timespec pause = {0, 100000};
        ::nanosleep(&pause, nullptr);
    }
    if (!dma_state_get(fd, st)) {
        std::fprintf(stderr, "dma: read registers after ABORT: %s\n", std::strerror(errno));
        errno = 0;
        return -1;
    }
    dma_state_print(st);
    if (!wasBusy)
        std::fprintf(stderr, "dma: ABORT was issued while BUSY=0; ERRCODE=3 is only raised for a running transfer\n");
    if (dma_errcode(st) == PL_CTRL_DMA_ERRCODE_ABORT)
        std::printf("abort=ERRCODE 3 (stopped; DMA_WRITTEN is the completed prefix)\n");
    else
        std::printf("abort=issued (ERRCODE=%u)\n", dma_errcode(st));
    return 0;
}

/* §4.4 graceful stop of a ring transfer: DMA_CTRL[4]=1 lets the PL finish the
 * burst it is in and then release BUSY without touching ERRCODE, so a finished
 * run leaves DMA_STATUS clean and the next run needs no PL reload. ABORT (the
 * --abort path) instead raises the sticky ERRCODE=3 that only a PL reset
 * clears. TEST and RING are levels latched from the same write, so the STOP
 * write repeats them: a STOP that dropped RING would also reconfigure the mode.
 * The STOP level is dropped again afterwards so the register returns to idle. */
#define DMA_STOP_TIMEOUT_US 500000.0

static int dma_stop(int fd, const char *why)
{
    dma_state st = {};
    if (!dma_state_get(fd, st)) {
        std::fprintf(stderr, "dma: read registers before STOP: %s\n", std::strerror(errno));
        errno = 0;
        return -1;
    }
    const bool wasBusy = (st.ctrl & PL_CTRL_DMA_CTRL_BUSY) != 0;
    const uint32_t keep = st.ctrl & (PL_CTRL_DMA_CTRL_TEST | PL_CTRL_DMA_CTRL_RING);
    struct timespec t0;
    ::clock_gettime(CLOCK_MONOTONIC, &t0);

    if (!write32(fd, PL_CTRL_DMA_CTRL, keep | PL_CTRL_DMA_CTRL_STOP)) {
        std::fprintf(stderr, "dma: write DMA_CTRL.STOP: %s\n", std::strerror(errno));
        errno = 0;
        return -1;
    }
    for (;;) {
        if (!read32(fd, PL_CTRL_DMA_CTRL, st.ctrl)) {
            std::fprintf(stderr, "dma: poll DMA_CTRL after STOP: %s\n", std::strerror(errno));
            errno = 0;
            return -1;
        }
        if (!(st.ctrl & PL_CTRL_DMA_CTRL_BUSY))
            break;
        if (usec_since(t0) >= DMA_STOP_TIMEOUT_US) {
            std::fprintf(stderr, "dma: BUSY did not clear within %.0f ms after STOP (DMA_CTRL=0x%08x); "
                         "a bitstream without the §4.4 DMA_CTRL[4] stop bit ignores the write, so "
                         "reload a PL image that decodes it, or stop a stuck transfer with "
                         "\"plctl dma --abort\" (which sets ERRCODE=3)\n",
                         DMA_STOP_TIMEOUT_US / 1000.0, st.ctrl);
            errno = 0;
            return -1;
        }
        struct timespec pause = {0, 100000};
        ::nanosleep(&pause, nullptr);
    }
    const double releaseUs = wasBusy ? usec_since(t0) : 0.0;

    /* Drop the level again so the next START does not find a latched STOP. */
    if (!write32(fd, PL_CTRL_DMA_CTRL, keep)) {
        std::fprintf(stderr, "dma: clear DMA_CTRL.STOP: %s\n", std::strerror(errno));
        errno = 0;
        return -1;
    }
    if (!dma_state_get(fd, st)) {
        std::fprintf(stderr, "dma: read registers after STOP: %s\n", std::strerror(errno));
        errno = 0;
        return -1;
    }
    const uint32_t code = dma_errcode(st);
    std::printf("dma stop why=%s busy_before=%u release_us=%.1f DMA_CTRL=0x%08x errcode=%u %s\n",
                why, wasBusy ? 1U : 0U, releaseUs, st.ctrl, code,
                code ? dma_errcode_text(code) : "(clean: no sticky code, unlike ABORT)");
    return 0;
}

/* Idle a DMA ring that a killed application left started. S99qtapp calls this
 * instead of poking DMA_CTRL through read/write, so offsets and bits come from
 * pl_ctrl_uapi.h and an interface version change touches one place. STOP is
 * graceful (no sticky ERRCODE=3); DMA_CTRL is then written back as 0. */
static int ring_stop(int fd)
{
    dma_state st = {};
    if (!dma_state_get(fd, st)) {
        std::fprintf(stderr, "ring-stop: read registers: %s\n", std::strerror(errno));
        errno = 0;
        return -1;
    }
    if (!st.ctrl) {
        std::printf("ring-stop: DMA_CTRL=0x00000000 (already idle)\n");
        return 0;
    }
    if (st.ctrl & PL_CTRL_DMA_CTRL_BUSY) {
        if (dma_stop(fd, "ring-stop") != 0)
            return -1;
    }
    if (!write32(fd, PL_CTRL_DMA_CTRL, 0)) {
        std::fprintf(stderr, "ring-stop: clear DMA_CTRL: %s\n", std::strerror(errno));
        errno = 0;
        return -1;
    }
    if (!dma_state_get(fd, st)) {
        std::fprintf(stderr, "ring-stop: read registers after stop: %s\n", std::strerror(errno));
        errno = 0;
        return -1;
    }
    std::printf("ring-stop: DMA_CTRL=0x%08x (idle, ERRCODE=%u)\n", st.ctrl, dma_errcode(st));
    return 0;
}

/* Defined below the ring dispatcher, which falls back to it for a one-shot
 * transfer. */
static int dma_single(int fd, uint32_t base, uint32_t bytes, bool test, bool latency,
                      uint32_t timeoutMs);

/* End-to-end delay of a one-shot transfer: the last word of the buffer carries
 * the newest sample TS, which is compared against PL_TS_NOW. */
static int latency_single(int fd, uint32_t base, uint32_t bytes)
{
    mem_window win;
    if (!window_open(win, base, bytes, "base must be in reserved-memory"))
        return -1;
    const sample_word last = sample_decode(win.words[bytes / 8U - 1U]);
    double us = 0.0;
    const bool measured = last.ts != 0 && latency_measure(fd, last.ts, us);
    window_close(win);
    if (last.ts == 0) {
        std::fprintf(stderr, "latency: last sample TS=0 (empty buffer or ramp data); no measurement\n");
        errno = 0;
        return -1;
    }
    if (!measured) {
        std::fprintf(stderr, "latency: read PL_TS_NOW: %s\n", std::strerror(errno));
        errno = 0;
        return -1;
    }
    if (us < 0.0) {
        std::fprintf(stderr, "latency: sample TS=%u is ahead of PL_TS_NOW; the block is stale "
                     "(from a timebase wrap), rerun the transfer\n", last.ts);
        errno = 0;
        return -1;
    }
    std::printf("latency_us single=%.1f ts=%u (quantized to one 1 kHz frame: +-1000 us)\n",
                us, last.ts);
    return 0;
}

/* Ring mode (§4.4): DMA_CTRL[3]=1 writes 4 KiB blocks at
 * BASE + (BLK_SEQ mod N) * 4096 until DMA_CTRL[4]=STOP. Each completed block
 * increments DMA_BLK_SEQ, and one interrupt is raised when DMA_IRQ[0]=1.
 * Reading block (BLK_SEQ-1) mod N is therefore always a finished block:
 * BLK_SEQ points at the block the PL is filling now. A BLK_SEQ delta > N means
 * blocks were overwritten before they could be read; the excess is counted as
 * lost and classified at the end of the run.
 * The reader takes a block out of the ring as soon as it is visible (IRQ, or
 * DMA_BLK_SEQ as the safety net) and prints after the copy, so one printf does
 * not delay the next read. */

/* Distribution of a per-block number: a single maximum says little about the
 * steady state, so the summary reports min/median/p95/max as well. */
struct latency_stats {
    std::vector<double> values;

    void add(double v)
    {
        if (v >= 0.0)
            values.push_back(v);
    }
    bool empty() const { return values.empty(); }
    unsigned size() const { return unsigned(values.size()); }

    double mean() const
    {
        if (values.empty())
            return 0.0;
        double sum = 0.0;
        for (double v : values)
            sum += v;
        return sum / double(values.size());
    }
    double min() const
    {
        return values.empty() ? 0.0 : *std::min_element(values.begin(), values.end());
    }
    double max() const
    {
        return values.empty() ? 0.0 : *std::max_element(values.begin(), values.end());
    }
    /* Nearest-rank percentile: the smallest sample with at least p of the
     * samples at or below it. */
    double percentile(double p) const
    {
        if (values.empty())
            return 0.0;
        std::vector<double> sorted(values);
        std::sort(sorted.begin(), sorted.end());
        if (p >= 1.0)
            return sorted.back();
        const size_t rank = size_t(std::ceil(p * double(sorted.size())));
        const size_t idx = rank ? rank - 1U : 0U;
        return sorted[idx];
    }
    double median() const
    {
        if (values.empty())
            return 0.0;
        std::vector<double> sorted(values);
        std::sort(sorted.begin(), sorted.end());
        const size_t n = sorted.size();
        return (n % 2U) ? sorted[n / 2U] : 0.5 * (sorted[n / 2U - 1U] + sorted[n / 2U]);
    }
};
static int dma_ring(int fd, uint32_t base, uint32_t blocks, uint32_t count,
                    bool test, bool waitIrq, bool latency, uint32_t timeoutMs)
{
    const uint32_t bytes = PL_CTRL_DMA_BLOCK_SIZE;
    const uint32_t words = bytes / 8U;
    const char *waitMode = waitIrq ? "irq" : "poll";
    dma_state st = {};

    if (!dma_state_get(fd, st)) {
        std::fprintf(stderr, "dma: read DMA registers: %s\n", std::strerror(errno));
        errno = 0;
        return -1;
    }
    if (st.ctrl & PL_CTRL_DMA_CTRL_BUSY) {
        std::fprintf(stderr, "dma: BUSY is set (a transfer is running); run \"plctl dma --abort\" first\n");
        errno = 0;
        return -1;
    }
    if (st.ctrl & PL_CTRL_DMA_CTRL_ERROR) {
        const uint32_t code = dma_errcode(st);
        std::fprintf(stderr, "dma: ERROR is already set (ERRCODE=%u: %s); ERROR only clears on PL reset, "
                     "so reload the bitstream first\n", code, dma_errcode_text(code));
        errno = 0;
        return -1;
    }
    std::printf("dma ring base=0x%08x blocks=%u block_bytes=%u read=%u mode=%s wait=%s timeout_ms=%u\n",
                base, blocks, bytes, count, test ? "ramp" : "capture", waitMode, timeoutMs);
    if (blocks == 1U)
        std::fprintf(stderr, "dma: a one-block ring has no read margin: the PL rewrites block 0 "
                     "as soon as it is read; use --blocks 4 or more\n");
    if (!write32(fd, PL_CTRL_DMA_BASE_LO, base)
        || !write32(fd, PL_CTRL_DMA_BASE_HI, 0)
        || !write32(fd, PL_CTRL_DMA_BYTES, bytes)
        || !write32(fd, PL_CTRL_DMA_BLOCKS, blocks)) {
        std::fprintf(stderr, "dma: write DMA_BASE/DMA_BYTES/DMA_BLOCKS: %s\n", std::strerror(errno));
        errno = 0;
        return -1;
    }
    uint32_t baseLo = 0, bytesRb = 0, blocksRb = 0;
    if (!read32(fd, PL_CTRL_DMA_BASE_LO, baseLo) || !read32(fd, PL_CTRL_DMA_BYTES, bytesRb)
        || !read32(fd, PL_CTRL_DMA_BLOCKS, blocksRb)) {
        std::fprintf(stderr, "dma: read back DMA_BASE/DMA_BYTES/DMA_BLOCKS: %s\n", std::strerror(errno));
        errno = 0;
        return -1;
    }
    if (baseLo != base || bytesRb != bytes || blocksRb != blocks) {
        std::fprintf(stderr, "dma: ring readback BASE_LO=0x%08x BYTES=%u BLOCKS=%u, expected "
                     "0x%08x/%u/%u; the PL bitstream does not decode the §4.4 ring window "
                     "(0xa8..0xb4) or answered with SLVERR\n",
                     baseLo, bytesRb, blocksRb, base, bytes, blocks);
        errno = 0;
        return -1;
    }

    bool irq = waitIrq;
    bool armed = false;
    uint32_t dmaIrqSeen = 0;
    if (irq) {
        const int crc = dma_irq_count_read(fd, dmaIrqSeen);
        if (crc == -2) {
            std::fprintf(stderr, "dma: pl_ctrl.ko has no PL_CTRL_DMA_IRQ_COUNT; rebuild the module. "
                         "Falling back to polling DMA_BLK_SEQ\n");
            irq = false;
        } else if (crc < 0) {
            return -1;
        }
    }
    if (irq) {
        if (!write32(fd, PL_CTRL_DMA_IRQ, PL_CTRL_DMA_IRQ_ENABLE | PL_CTRL_DMA_IRQ_CLEAR)) {
            std::fprintf(stderr, "dma: write DMA_IRQ enable: %s\n", std::strerror(errno));
            errno = 0;
            return -1;
        }
        uint32_t irqRb = 0;
        if (!read32(fd, PL_CTRL_DMA_IRQ, irqRb)) {
            std::fprintf(stderr, "dma: read DMA_IRQ: %s\n", std::strerror(errno));
            errno = 0;
            return -1;
        }
        armed = true;
        if (!(irqRb & PL_CTRL_DMA_IRQ_ENABLE)) {
            std::fprintf(stderr, "dma: DMA_IRQ[0] reads back 0, so the PL does not enable the block IRQ; "
                         "falling back to polling DMA_BLK_SEQ\n");
            irq = false;
        }
    }
    /* Baseline of the serviced-interrupt counter, reported at the end: it is
     * the number /proc/interrupts should have grown by. */
    const uint32_t irqBase = dmaIrqSeen;

    mem_window win;
    if (!window_open(win, base, blocks * bytes, "base must be in reserved-memory"))
        return -1;
    const uint32_t start = PL_CTRL_DMA_CTRL_START | PL_CTRL_DMA_CTRL_RING
                         | (test ? PL_CTRL_DMA_CTRL_TEST : 0U);
    struct timespec t0;
    ::clock_gettime(CLOCK_MONOTONIC, &t0);
    if (!write32(fd, PL_CTRL_DMA_CTRL, start)) {
        std::fprintf(stderr, "dma: write DMA_CTRL.START|RING: %s\n", std::strerror(errno));
        window_close(win);
        errno = 0;
        return -1;
    }
    /* Baseline after START: DMA_BLK_SEQ counts from START, so a stale value
     * from an earlier run must not be mistaken for a huge jump. */
    uint32_t lastSeq = 0;
    if (!read32(fd, PL_CTRL_DMA_BLK_SEQ, lastSeq)) {
        std::fprintf(stderr, "dma: read DMA_BLK_SEQ: %s\n", std::strerror(errno));
        (void)dma_stop(fd, "ring start failed");
        window_close(win);
        errno = 0;
        return -1;
    }
    const uint32_t seqStart = lastSeq;
    uint32_t readBlocks = 0, jumps = 0, lost = 0, speedWords = 0;
    uint32_t maxDelta = 0, cycles = 0, lappedCycles = 0, seqOnlyWakes = 0;
    double cycleSumUs = 0.0, cycleMaxUs = 0.0;
    uint64_t lastWakeUs = usec_now();   /* START time: cycle 1 includes the first block */
    latency_stats lat, readLat, printLat;
    poll_pacer pacer;
    uint64_t snapshot[PL_CTRL_DMA_BLOCK_SIZE / 8U];
    int ret = 0;

    bool failed = false;
    for (uint32_t got = 0; got < count && !failed; ) {
        uint32_t seq = 0;
        bool viaSeq = false;
        const int rc = irq ? wait_block_irq(fd, dmaIrqSeen, lastSeq, timeoutMs, seq, viaSeq)
                           : wait_block_seq(fd, lastSeq, timeoutMs, seq, pacer);
        if (rc == WAIT_NO_IRQ) {
            std::fprintf(stderr, "dma: no block IRQ on this device (poll reports POLLERR); "
                         "falling back to polling DMA_BLK_SEQ\n");
            irq = false;
            continue;
        }
        if (rc == WAIT_TIMEOUT) {
            std::fprintf(stderr, "dma: timeout after %u ms waiting for block %u of %u "
                         "(DMA_BLK_SEQ=%u, wait=%s)\n", timeoutMs, readBlocks + 1U, count,
                         lastSeq, irq ? "irq+seq" : "poll");
            failed = true;
            break;
        }
        if (rc == WAIT_ERROR) {
            failed = true;
            break;
        }

        const uint64_t wakeUs = usec_now();
        const uint32_t delta = seq - lastSeq;
        if (delta == 0)
            continue;
        lastSeq = seq;
        ++cycles;
        const double thisCycleUs = double(wakeUs - lastWakeUs);
        cycleSumUs += thisCycleUs;
        if (thisCycleUs > cycleMaxUs)
            cycleMaxUs = thisCycleUs;
        /* Producer period of this cycle: the poll interval follows it. */
        pacer.observe(thisCycleUs / double(delta));
        if (viaSeq && irq) {
            ++seqOnlyWakes;
            if (seqOnlyWakes == 1U)
                std::fprintf(stderr, "dma: DMA_BLK_SEQ advanced without a serviced block IRQ %.1f ms "
                             "after the previous read; the line is declared but the GIC collects "
                             "nothing (level line declared as an edge?), check /proc/interrupts. "
                             "Continuing on the DMA_BLK_SEQ safety net\n", thisCycleUs / 1000.0);
        }
        /* Every completed block is still in the ring unless the PL lapped the
         * reader, so walk the whole delta instead of only the newest block. */
        uint32_t first = seq - delta + 1U;
        if (delta > blocks) {
            ++jumps;
            ++lappedCycles;
            lost += delta - blocks;
            first = seq - blocks + 1U;
        }
        if (delta > maxDelta)
            maxDelta = delta;
        for (uint32_t blkSeq = first; blkSeq != seq + 1U && got < count; ++blkSeq) {
            const uint32_t index = (blkSeq - 1U) % blocks;
            const uint64_t *blk = win.words + size_t(index) * words;

            /* Measure the delay before the copy: PL_TS_NOW is read while the
             * block has just become complete, so the copy and the printf below
             * cannot inflate the end-to-end number. */
            double latUs = 0.0;
            uint32_t latTs = 0;
            if (latency) {
                const sample_word newest = sample_decode(blk[words - 1U]);
                latTs = newest.ts;
                if (newest.ts != 0) {
                    if (!latency_measure(fd, newest.ts, latUs)) {
                        std::fprintf(stderr, "dma: read PL_TS_NOW: %s\n", std::strerror(errno));
                        errno = 0;
                        failed = true;
                        break;
                    }
                    if (latUs >= 0.0)
                        lat.add(latUs);
                }
            }

            /* Take the block out of the uncached DDR window while the PL is
             * already filling the next one; everything after this works on
             * local memory. */
            const uint64_t copyUs = usec_now();
            std::memcpy(snapshot, blk, bytes);
            readLat.add(double(usec_now() - copyUs));

            const uint64_t printUs = usec_now();
            std::printf("blk seq=%u idx=%u addr=0x%08x words[0..3]=", blkSeq, index,
                        base + index * bytes);
            for (uint32_t i = 0; i < 4U; ++i)
                std::printf("%s0x%016llx", i ? " " : "", (unsigned long long)snapshot[i]);
            const sample_word firstWord = sample_decode(snapshot[0]);
            std::printf("\n  word[0] ts=%u ch=0x%04x sample=%d", firstWord.ts,
                        unsigned(firstWord.ch), int(firstWord.sample));
            /* The speed record is its own three-word record, not a vibration
             * frame: CH=3/4/5 carry the status word, rpm_milli and raw_speed.
             * Only count them here; parsing stays in the ps-local app. */
            uint32_t blockSpeed = 0;
            for (uint32_t i = 0; i < words; ++i) {
                const uint16_t ch = sample_decode(snapshot[i]).ch;
                if (ch >= mlk::DmaFrameAssembler::kChSpeedTs
                    && ch <= mlk::DmaFrameAssembler::kChSpeedRaw)
                    ++blockSpeed;
            }
            speedWords += blockSpeed;
            std::printf(" speed_words=%u", blockSpeed);
            if (firstWord.ch >= mlk::DmaFrameAssembler::kChSpeedTs
                && firstWord.ch <= mlk::DmaFrameAssembler::kChSpeedRaw)
                std::printf(" speed_record=word%u", unsigned(firstWord.ch) - 2U);
            if (latency) {
                if (latTs == 0)
                    std::printf(" latency=undefined (TS=0)");
                else if (latUs < 0.0)
                    std::printf(" latency=undefined (sample TS ahead of PL_TS_NOW)");
                else
                    std::printf(" latency_us=%.1f", latUs);
            }
            std::printf("\n");
            if (test) {
                if (block_ramp_ok(snapshot, words)) {
                    std::printf("  verify=PASS words=%u ramp_start=%llu\n", words,
                                (unsigned long long)snapshot[0]);
                } else {
                    uint32_t bad = 0;
                    for (uint32_t i = 1; i < words; ++i)
                        if (snapshot[i] != snapshot[0] + uint64_t(i)) { bad = i; break; }
                    std::printf("  verify=FAIL word=%u byte_offset=0x%x expected=0x%016llx "
                                "actual=0x%016llx\n", bad, bad * 8U,
                                (unsigned long long)(snapshot[0] + uint64_t(bad)),
                                (unsigned long long)snapshot[bad]);
                }
            }
            printLat.add(double(usec_now() - printUs));
            ++readBlocks;
            ++got;
        }
        lastWakeUs = wakeUs;
    }
    ret = failed ? -1 : 0;

    /* §4.4 STOP releases BUSY without the sticky ERRCODE=3 of ABORT, so a run
     * that ends here can be repeated without reloading the PL bitstream. */
    const int stopRc = dma_stop(fd, ret == 0 ? "ring" : "ring after failure");
    if (ret == 0 && stopRc != 0)
        ret = -1;
    if (armed) {
        /* Drop the enable and the pending flag so a run that never got a
         * handler installed leaves the PL quiet instead of holding a level
         * line nobody services. */
        uint32_t ignored = 0;
        if (write32(fd, PL_CTRL_DMA_IRQ, PL_CTRL_DMA_IRQ_CLEAR))
            (void)read32(fd, PL_CTRL_DMA_IRQ, ignored);
    }
    window_close(win);

    const double elapsedMs = usec_since(t0) / 1000.0;
    const uint32_t produced = lastSeq - seqStart;
    const double blockPeriodUs = produced ? elapsedMs * 1000.0 / double(produced) : 0.0;
    const double meanCycleUs = cycles ? cycleSumUs / double(cycles) : 0.0;

    std::printf("ring done blocks_read=%u seq_start=%u seq_end=%u jumps=%u lost=%u wait=%s "
                "elapsed_ms=%.1f speed_words=%u\n", readBlocks, seqStart, lastSeq, jumps, lost,
                irq ? "irq" : "poll", elapsedMs, speedWords);
    /* What one block costs the reader: the copy out of the ring is the read
     * path itself, the printf is measured apart because a serial console often
     * dominates and then the ring laps the reader no matter how fast it reads. */
    std::printf("ring block_read_us mean=%.1f min=%.1f p95=%.1f max=%.1f n=%u "
                "(snapshot of %u B); print_us mean=%.1f; cycle_us mean=%.1f max=%.1f n=%u "
                "(one block every %.1f us); per_block_us mean=%.1f "
                "(elapsed_ms*1000/blocks_read: wait+copy+print per block)\n",
                readLat.mean(), readLat.min(), readLat.percentile(0.95), readLat.max(),
                readLat.size(), bytes, printLat.mean(), meanCycleUs, cycleMaxUs, cycles,
                blockPeriodUs, readBlocks ? elapsedMs * 1000.0 / double(readBlocks) : 0.0);

    const char *lostCause = "none";
    uint32_t suggestBlocks = blocks;
    if (lost) {
        /* One read cycle covers meanCycleUs. If that exceeds one block period
         * the reader is slower than the producer; otherwise the ring is simply
         * too shallow for the burst that was read. */
        lostCause = (blockPeriodUs > 0.0 && meanCycleUs > blockPeriodUs)
                  ? "ps_slower_than_block" : "ring_too_small";
        const double want = blockPeriodUs > 0.0 ? meanCycleUs / blockPeriodUs + 2.0 : 4.0;
        if (want > double(blocks))
            suggestBlocks = want >= double(PL_CTRL_DMA_BLOCKS_MAX)
                          ? PL_CTRL_DMA_BLOCKS_MAX : uint32_t(want);
        if (std::strcmp(lostCause, "ps_slower_than_block") == 0)
            std::printf("  hint: one read cycle (%.1f ms) spans %.1f block periods, so a %u-block "
                        "ring loses every block completed while the line was printed: keep the "
                        "block IRQ path (--wait-irq) and send stdout to a file; --blocks %u covers "
                        "this cycle\n", meanCycleUs / 1000.0,
                        blockPeriodUs > 0.0 ? meanCycleUs / blockPeriodUs : 0.0,
                        blocks, suggestBlocks);
        else
            std::printf("  hint: the ring holds %u blocks but the largest completed burst was %u; "
                        "raise --blocks to %u\n", blocks, maxDelta, suggestBlocks);
    }
    std::printf("ring lost_cause=%s delta_max=%u lapped_cycles=%u cycles=%u seq_only_wakes=%u "
                "avg_cycle_us=%.1f avg_block_us=%.1f ring_blocks=%u\n",
                lostCause, maxDelta, lappedCycles, cycles, seqOnlyWakes, meanCycleUs,
                blockPeriodUs, blocks);

    if (!irq) {
        std::printf("ring poll interval_us set=%u measured_mean=%.1f (block period %.1f us / %.0f, "
                    "clamped %u..%u) sleeps=%llu\n", pacer.intervalUs, pacer.mean_sleep_us(),
                    pacer.periodUs, PL_RING_POLL_DIVISOR, PL_RING_POLL_MIN_US,
                    PL_RING_POLL_MAX_US, (unsigned long long)pacer.sleeps);
    } else {
        uint32_t irqCount = 0;
        if (dma_irq_count_read(fd, irqCount) == 0)
            std::printf("ring irq_delivered=%u blocks_read=%u seq_only_wakes=%u\n",
                        irqCount - irqBase, readBlocks, seqOnlyWakes);
    }

    if (latency) {
        if (!lat.empty())
            std::printf("latency_us min=%.1f median=%.1f p95=%.1f max=%.1f mean=%.1f n=%u "
                        "(+-1000 us: one 1 kHz frame shares one TS; the age wraps modulo "
                        "2^32 = 42.94967296 s and is taken as a wrapping difference)\n",
                        lat.min(), lat.median(), lat.percentile(0.95), lat.max(), lat.mean(),
                        lat.size());
        else
            std::printf("latency=no nonzero sample TS in %u blocks (ramp data has TS=0)\n",
                        readBlocks);
    }
    if (ret < 0)
        errno = 0;
    return ret;
}

struct dma_opts {
    const char *baseText = nullptr;
    const char *bytesText = nullptr;
    const char *timeoutText = nullptr;
    const char *blocksText = nullptr;
    const char *countText = nullptr;
    bool test = false;
    bool abortOnly = false;
    bool ring = false;
    bool waitIrq = false;
    bool latency = false;
};

static int dma(int fd, const dma_opts &opt)
{
    uint32_t timeoutMs = 2000U;
    if (opt.timeoutText && (!number(opt.timeoutText, timeoutMs) || timeoutMs == 0U)) {
        std::fprintf(stderr, "dma: --timeout expects a positive millisecond count\n");
        errno = 0;
        return -1;
    }
    if (opt.abortOnly) {
        if (opt.baseText || opt.bytesText || opt.test || opt.ring || opt.blocksText
            || opt.countText || opt.waitIrq || opt.latency) {
            std::fprintf(stderr, "dma: --abort cannot be combined with other transfer options\n");
            errno = 0;
            return -1;
        }
        return dma_abort(fd);
    }
    if (!opt.baseText) {
        std::fprintf(stderr, "dma: --base is required\n");
        errno = 0;
        return -1;
    }
    if (opt.test && opt.latency)
        std::fprintf(stderr, "dma: --test fills the buffer with a ramp instead of samples, "
                     "so --latency compares against a TS that is not a timestamp\n");
    uint32_t base = 0;
    if (!number(opt.baseText, base)) {
        std::fprintf(stderr, "dma: --base expects a decimal or 0x-prefixed physical address\n");
        errno = 0;
        return -1;
    }

    if (opt.ring) {
        if (!opt.blocksText) {
            std::fprintf(stderr, "dma: --ring requires --blocks N (%u..%u)\n",
                         PL_CTRL_DMA_BLOCKS_MIN, PL_CTRL_DMA_BLOCKS_MAX);
            errno = 0;
            return -1;
        }
        uint32_t blocks = 0;
        if (!number(opt.blocksText, blocks)) {
            std::fprintf(stderr, "dma: --blocks expects a count\n");
            errno = 0;
            return -1;
        }
        if (blocks < PL_CTRL_DMA_BLOCKS_MIN || blocks > PL_CTRL_DMA_BLOCKS_MAX) {
            std::fprintf(stderr, "dma: --blocks must be in %u..%u (4 KiB blocks)\n",
                         PL_CTRL_DMA_BLOCKS_MIN, PL_CTRL_DMA_BLOCKS_MAX);
            errno = 0;
            return -1;
        }
        if (opt.bytesText) {
            uint32_t bytesText = 0;
            if (!number(opt.bytesText, bytesText)
                || bytesText != PL_CTRL_DMA_BLOCK_SIZE) {
                std::fprintf(stderr, "dma: ring blocks are fixed at %u bytes; "
                             "drop --bytes or pass --bytes %u\n",
                             PL_CTRL_DMA_BLOCK_SIZE, PL_CTRL_DMA_BLOCK_SIZE);
                errno = 0;
                return -1;
            }
        }
        if (base % PL_CTRL_DMA_BLOCK_SIZE) {
            std::fprintf(stderr, "dma: --ring --base must be %u-byte aligned\n",
                         PL_CTRL_DMA_BLOCK_SIZE);
            errno = 0;
            return -1;
        }
        uint32_t count = blocks;
        if (opt.countText && (!number(opt.countText, count) || count == 0U)) {
            std::fprintf(stderr, "dma: --count expects a positive block count\n");
            errno = 0;
            return -1;
        }
        if (count > 4096U) {
            std::fprintf(stderr, "dma: --count is capped at 4096 blocks per run\n");
            errno = 0;
            return -1;
        }
        if (uint64_t(base) + uint64_t(blocks) * PL_CTRL_DMA_BLOCK_SIZE > 0x40000000ULL) {
            std::fprintf(stderr, "dma: ring 0x%08x + %u x %u exceeds the 1 GiB DDR window\n",
                         base, blocks, PL_CTRL_DMA_BLOCK_SIZE);
            errno = 0;
            return -1;
        }
        return dma_ring(fd, base, blocks, count, opt.test, opt.waitIrq, opt.latency,
                        timeoutMs);
    }

    if (opt.blocksText || opt.countText) {
        std::fprintf(stderr, "dma: --blocks/--count are ring options; add --ring\n");
        errno = 0;
        return -1;
    }
    if (opt.waitIrq) {
        std::fprintf(stderr, "dma: --wait-irq needs --ring (the §4.4 block IRQ fires only per "
                     "completed ring block)\n");
        errno = 0;
        return -1;
    }
    uint32_t bytes = PL_CTRL_DMA_BYTES_MAX;
    if (opt.bytesText && !number(opt.bytesText, bytes)) {
        std::fprintf(stderr, "dma: --bytes expects a decimal or 0x-prefixed count\n");
        errno = 0;
        return -1;
    }
    if (bytes < PL_CTRL_DMA_BYTES_MIN || bytes > PL_CTRL_DMA_BYTES_MAX
        || (bytes % PL_CTRL_DMA_ALIGN)) {
        std::fprintf(stderr, "dma: --bytes must be a multiple of %u in %u..%u\n",
                     PL_CTRL_DMA_ALIGN, PL_CTRL_DMA_BYTES_MIN, PL_CTRL_DMA_BYTES_MAX);
        errno = 0;
        return -1;
    }
    if (base % PL_CTRL_DMA_ALIGN) {
        std::fprintf(stderr, "dma: --base must be %u-byte aligned\n", PL_CTRL_DMA_ALIGN);
        errno = 0;
        return -1;
    }
    /* Vendor DT: 1 GiB DDR at 0x00000000; DMA_BASE_HI stays 0 (§4.4). */
    if (uint64_t(base) + bytes > 0x40000000ULL) {
        std::fprintf(stderr, "dma: 0x%08x+%u exceeds the 1 GiB DDR window (0x00000000..0x3fffffff)\n",
                     base, bytes);
        errno = 0;
        return -1;
    }
    return dma_single(fd, base, bytes, opt.test, opt.latency, timeoutMs);
}

/* Single 4 KiB (or shorter) transfer: START, poll DONE, verify the buffer. */
static int dma_single(int fd, uint32_t base, uint32_t bytes, bool test, bool latency,
                      uint32_t timeoutMs)
{
    dma_state st = {};
    if (!dma_state_get(fd, st)) {
        std::fprintf(stderr, "dma: read DMA registers: %s\n", std::strerror(errno));
        errno = 0;
        return -1;
    }
    if (st.ctrl & PL_CTRL_DMA_CTRL_BUSY) {
        std::fprintf(stderr, "dma: BUSY is set (transfer running); wait for DONE or run \"plctl dma --abort\"\n");
        errno = 0;
        return -1;
    }
    if (st.ctrl & PL_CTRL_DMA_CTRL_ERROR) {
        const uint32_t code = dma_errcode(st);
        std::fprintf(stderr, "dma: ERROR is already set (ERRCODE=%u: %s); ERROR only clears on PL reset, "
                     "so START cannot yield a valid result - reload the bitstream first\n",
                     code, dma_errcode_text(code));
        errno = 0;
        return -1;
    }
    std::printf("dma base=0x%08x bytes=%u mode=%s timeout_ms=%u\n", base, bytes,
                test ? "ramp" : "capture", timeoutMs);
    if (!write32(fd, PL_CTRL_DMA_BASE_LO, base)
        || !write32(fd, PL_CTRL_DMA_BASE_HI, 0)
        || !write32(fd, PL_CTRL_DMA_BYTES, bytes)) {
        std::fprintf(stderr, "dma: write DMA_BASE/DMA_BYTES: %s\n", std::strerror(errno));
        errno = 0;
        return -1;
    }
    uint32_t baseLo = 0, bytesRb = 0;
    if (!read32(fd, PL_CTRL_DMA_BASE_LO, baseLo) || !read32(fd, PL_CTRL_DMA_BYTES, bytesRb)
        || baseLo != base || bytesRb != bytes) {
        std::fprintf(stderr, "dma: readback mismatch (BASE_LO=0x%08x, BYTES=%u); "
                     "the PL bitstream or pl_ctrl.ko lacks the §4.4 DMA window\n",
                     baseLo, bytesRb);
        errno = 0;
        return -1;
    }
    const uint32_t start = PL_CTRL_DMA_CTRL_START | (test ? PL_CTRL_DMA_CTRL_TEST : 0U);
    struct timespec t0;
    ::clock_gettime(CLOCK_MONOTONIC, &t0);
    if (!write32(fd, PL_CTRL_DMA_CTRL, start)) {
        std::fprintf(stderr, "dma: write DMA_CTRL.START: %s\n", std::strerror(errno));
        errno = 0;
        return -1;
    }
    bool error = false;
    double elapsedUs = 0.0;
    for (;;) {
        if (!read32(fd, PL_CTRL_DMA_CTRL, st.ctrl)) {
            std::fprintf(stderr, "dma: poll DMA_CTRL: %s\n", std::strerror(errno));
            errno = 0;
            return -1;
        }
        error = (st.ctrl & PL_CTRL_DMA_CTRL_ERROR) != 0;
        elapsedUs = usec_since(t0);
        if ((st.ctrl & PL_CTRL_DMA_CTRL_DONE) || error)
            break;
        if (elapsedUs >= double(timeoutMs) * 1000.0) {
            std::fprintf(stderr, "dma: timeout after %u ms (DONE not set, BUSY=%u); "
                         "check the PL bitstream and the HP1 port\n", timeoutMs,
                         (st.ctrl & PL_CTRL_DMA_CTRL_BUSY) ? 1U : 0U);
            errno = 0;
            return -1;
        }
        /* Poll tightly for the first 2 ms so short transfers are timed without
         * sleep granularity, then back off to a 50 us period. */
        if (elapsedUs < 2000.0)
            continue;
        struct timespec pause = {0, 50000};
        ::nanosleep(&pause, nullptr);
    }
    if (!dma_state_get(fd, st)) {
        std::fprintf(stderr, "dma: read DMA_STATUS/DMA_WRITTEN: %s\n", std::strerror(errno));
        errno = 0;
        return -1;
    }
    dma_state_print(st);
    if (error) {
        std::fprintf(stderr, "dma: ERROR set: ERRCODE=%u (%s), DMA_WRITTEN=%u of %u bytes; "
                     "discard the buffer\n", dma_errcode(st),
                     dma_errcode_text(dma_errcode(st)), st.written, bytes);
        errno = 0;
        return -1;
    }
    if (st.written != bytes) {
        std::fprintf(stderr, "dma: DMA_WRITTEN=%u, expected %u bytes\n", st.written, bytes);
        errno = 0;
        return -1;
    }
    const double mbps = elapsedUs > 0.0 ? double(bytes) * 8.0 / elapsedUs : 0.0;
    const double mbs = elapsedUs > 0.0 ? double(bytes) / elapsedUs : 0.0;
    std::printf("elapsed_us=%.1f bandwidth_mbps=%.1f bandwidth_mbs=%.2f\n",
                elapsedUs, mbps, mbs);
    if (st.status & PL_CTRL_DMA_STATUS_OVF)
        std::fprintf(stderr, "dma: DMA_STATUS.OVF is set; §4.4 leaves post-overflow behavior TBD\n");
    const int checked = dma_check_buffer(base, bytes, test);
    if (latency && latency_single(fd, base, bytes) < 0)
        return -1;
    return checked;
}

/* §4.3 configures the vibration channel at 3200 SPS and
 * fpga/uisrc/06_doc/时域与频域特征.md gives f[k] = k * Fs / N. The measured
 * in-board interval is about 3123 Hz (fpga/uisrc/06_doc/验证结果.md), so the
 * 3200 Hz axis is nominal and --rate is the override. */
static const double PL_SPECTRUM_NOMINAL_RATE_HZ = 3200.0;

struct spectrum_opts {
    const char *binsText = nullptr;
    const char *topText = nullptr;
    const char *rateText = nullptr;
    const char *baseText = nullptr;
    bool raw = false;
};

static void spectrum_usage(std::FILE *out)
{
    std::fprintf(out,
        "usage: plctl spectrum [--bins N] [--top M] [--raw] [--rate HZ] [--base PHYS]\n"
        "       plctl spectrum --help\n"
        "  Read the §4.6 BIN_POWER window through /dev/pl_ctrl pread:\n"
        "  read 0x78 FFT_SEQUENCE -> one pread of the requested bins at 0x400\n"
        "  -> read 0x78; accepted only when both sequence reads match (3 tries).\n"
        "  BIN_POWER[k] = (|X_k|^2) >> 8, unsigned32, k = 0..N-1 in natural order.\n"
        "  |X_k|^2 peaks near 2^39 (ADXL345 +-16 g), so >>8 keeps it inside uint32.\n"
        "  --bins N    leading bins to read, 1..%u; default %u (current PL build N=%u).\n"
        "              A shorter dump keeps the Fs/N axis of the full build N.\n"
        "  --top M     print the M strongest bins, 1..N; default 8, or the bin\n"
        "              count when --bins asks for fewer.\n"
        "  --raw       also print every bin, one row each.\n"
        "  --rate HZ   sample rate for the kHz column; default %.0f Hz, the §4.3\n"
        "              vibration configuration (ADXL345 3200 SPS). f[k]=k*Fs/N, so\n"
        "              the bin spacing is Fs/N (12.5 Hz at N=256). The measured ODR\n"
        "              is about 3123 Hz, so the axis is nominal; pass --rate for a\n"
        "              measured value. Bins above N/2 mirror bins N-k (real FFT).\n"
        "  --base PHYS PL_CTRL physical base shown in the header only (default\n"
        "              0x80000000); the read path uses /dev/pl_ctrl offsets.\n",
        PL_CTRL_SPECTRUM_WORDS, PL_CTRL_SPECTRUM_WORDS, PL_CTRL_SPECTRUM_WORDS,
        PL_SPECTRUM_NOMINAL_RATE_HZ);
}

static int spectrum(int fd, const spectrum_opts &opt)
{
    uint32_t binsWanted = PL_CTRL_SPECTRUM_WORDS;
    if (opt.binsText && !number(opt.binsText, binsWanted)) {
        std::fprintf(stderr, "spectrum: --bins expects a bin count\n");
        errno = 0;
        return -1;
    }
    if (binsWanted == 0U || binsWanted > PL_CTRL_SPECTRUM_WORDS) {
        std::fprintf(stderr, "spectrum: --bins must be 1..%u (N of this PL build)\n",
                     PL_CTRL_SPECTRUM_WORDS);
        errno = 0;
        return -1;
    }
    /* Default M is 8, or the whole window when --bins asked for fewer bins. */
    uint32_t top = binsWanted < 8U ? binsWanted : 8U;
    if (opt.topText && (!number(opt.topText, top) || top == 0U || top > binsWanted)) {
        std::fprintf(stderr, "spectrum: --top must be 1..%u (the requested bin count)\n",
                     binsWanted);
        errno = 0;
        return -1;
    }
    double rate = PL_SPECTRUM_NOMINAL_RATE_HZ;
    if (opt.rateText) {
        char *end = nullptr;
        errno = 0;
        rate = std::strtod(opt.rateText, &end);
        if (errno || !*opt.rateText || !end || *end || !(rate > 0.0) || !(rate <= 1e9)) {
            std::fprintf(stderr, "spectrum: --rate expects a positive sample rate in Hz\n");
            errno = 0;
            return -1;
        }
    }
    uint32_t base = 0x80000000U;
    if (opt.baseText && (!number(opt.baseText, base) || (base & 3U))) {
        std::fprintf(stderr,
                     "spectrum: --base expects a 4-byte-aligned decimal or 0x-prefixed address\n");
        errno = 0;
        return -1;
    }

    std::vector<unsigned> bins(binsWanted);
    unsigned count = 0, sequence = 0;
    if (pl_read_spectrum(fd, bins.data(), binsWanted, &count, &sequence) < 0) {
        if (errno == EAGAIN) {
            std::fprintf(stderr, "spectrum: FFT_SEQUENCE changed on 3 consecutive reads; "
                         "the window is being overwritten faster than it can be read\n");
            errno = 0;
        } else {
            std::fprintf(stderr, "spectrum: read BIN_POWER: %s (window 0x%03x; the PL "
                         "bitstream and pl_ctrl.ko must decode §4.6)\n",
                         std::strerror(errno), PL_CTRL_SPECTRUM_OFFSET);
            errno = 0;
        }
        return -1;
    }

    /* §4.6: FFT_FLAGS belongs to the same window as the bins. It is read after
     * the accepted burst, so a window retiring in between would leave the flags
     * one window newer; the extra sequence read only warns, it never rejects
     * otherwise valid bins. */
    uint32_t flags = 0;
    if (!read32(fd, 0x88, flags)) {
        std::fprintf(stderr, "spectrum: read FFT_FLAGS: %s\n", std::strerror(errno));
        errno = 0;
        return -1;
    }
    uint32_t sequenceAfterFlags = 0;
    if (read32(fd, 0x78, sequenceAfterFlags) && sequenceAfterFlags != sequence)
        std::fprintf(stderr, "spectrum: FFT_SEQUENCE advanced while reading FFT_FLAGS; "
                     "flags may describe the next window\n");

    unsigned peak = 0;
    for (unsigned i = 0; i < count; ++i)
        if (bins[i] > peak)
            peak = bins[i];
    /* --bins only limits how many leading bins are fetched; the FFT size of the
     * PL build still sets the axis, so a partial dump keeps the full-N scale. */
    const double binHz = rate / double(PL_CTRL_SPECTRUM_WORDS);
    const double binKhz = binHz / 1000.0;

    std::printf("spectrum: base=0x%08x offset=0x%03x bins=%u fft_n=%u sequence=%u "
                "flags=0x%08x%s%s rate_hz=%.4g bin_hz=%.6g\n",
                base, PL_CTRL_SPECTRUM_OFFSET, count, PL_CTRL_SPECTRUM_WORDS, sequence,
                flags, (flags & 1U) ? " dc_saturation" : "",
                (flags & 2U) ? " window_lost" : "", rate, binHz);

    struct bin_row { unsigned index, raw; };
    std::vector<bin_row> rows;
    rows.reserve(count);
    for (unsigned i = 0; i < count; ++i)
        rows.push_back({i, bins[i]});
    std::sort(rows.begin(), rows.end(), [](const bin_row &a, const bin_row &b) {
        if (a.raw != b.raw)
            return a.raw > b.raw;
        return a.index < b.index;   /* equal power: the contract picks the smaller bin */
    });

    const auto print_row = [&](const bin_row &row) {
        std::printf("  %5u  %12u", row.index, row.raw);
        if (row.raw && peak)
            std::printf("  %8.2f", 10.0 * std::log10(double(row.raw) / double(peak)));
        else
            std::printf("  %8s", "-inf");
        std::printf("  %9.4f\n", double(row.index) * binKhz);
    };

    std::printf("top %u of %u bins by raw power:\n", top, count);
    std::printf("  index          raw  dB(peak)       kHz\n");
    for (unsigned i = 0; i < top; ++i)
        print_row(rows[i]);
    if (opt.raw) {
        std::printf("all %u bins:\n", count);
        for (unsigned i = 0; i < count; ++i)
            print_row({i, bins[i]});
    }
    return 0;
}

/* §14: one speed event every 100 ms. SPEED_RPM_MILLI is a signed 0.001 rpm/LSB
 * integer and SPEED_RAW the encoder count delta over the same period, so a
 * reverse rotation keeps a negative sign. online=0 means the latest Modbus
 * transaction failed; the window then still holds the last successful sample,
 * which must not be printed as a current reading. */
static const char *speed_error_text(unsigned code)
{
    switch (code) {
    case PL_CTRL_SPEED_ERR_NONE:      return "none";
    case PL_CTRL_SPEED_ERR_TIMEOUT:   return "timeout";
    case PL_CTRL_SPEED_ERR_CRC:       return "crc";
    case PL_CTRL_SPEED_ERR_FRAME:     return "frame";
    case PL_CTRL_SPEED_ERR_UART:      return "uart";
    case PL_CTRL_SPEED_ERR_EXCEPTION: return "modbus_exception";
    case PL_CTRL_SPEED_ERR_GAP:       return "inter_frame_gap";
    case PL_CTRL_SPEED_ERR_RX_OVF:    return "rx_overflow";
    }
    return "reserved";
}

static void speed_usage(std::FILE *out)
{
    std::fprintf(out,
        "usage: plctl speed [--json]\n"
        "  Reads 0xb8..0xd0 (SPEED_RAW .. SPEED_ERR_COUNT) with seq -> fields -> seq\n"
        "  consistency and prints raw, rpm, count, err_count, ts, seq and status.\n"
        "  rpm_milli is 0.001 rpm/LSB, signed: forward is positive, reverse negative.\n"
        "  online=0 prints state=offline and rpm='-' instead of a 0 rpm reading; the\n"
        "  window then keeps the last successful sample, not a current one.\n"
        "  --json  one-line JSON for on-board scripts; rpm_milli and rpm are null\n"
        "          while offline.\n"
        "  The same event is also written to the DMA stream as three words:\n"
        "    W0={ts, CH=3, SPEED_STATUS[15:0]}: bit0 online, bit1 saturated, bit2 this\n"
        "       event is an error (no new sample), bit3 configured, [7:4] error code,\n"
        "       [15:8] Modbus exception code;\n"
        "    W1={rpm_milli[31:0], CH=4, 0}; W2={raw_speed[31:0], CH=5, 0}.\n"
        "  See \"plctl dma --ring\" output, which counts the CH=3/4/5 words per block.\n");
}

static int speed(int fd, bool json)
{
    pl_speed_t s = {};
    if (pl_read_speed(fd, &s) < 0) {
        if (json)
            std::printf("{\"ok\":false,\"error\":\"%s\"}\n", std::strerror(errno));
        std::fprintf(stderr, "speed: read 0xb8..0xd0: %s\n", std::strerror(errno));
        errno = 0;
        return -1;
    }
    const bool online = (s.status & PL_CTRL_SPEED_STATUS_ONLINE) != 0;
    const bool configured = (s.status & PL_CTRL_SPEED_STATUS_CONFIGURED) != 0;
    const bool saturated = (s.status & PL_CTRL_SPEED_STATUS_SATURATED) != 0;
    const bool error = (s.status & PL_CTRL_SPEED_STATUS_ERROR) != 0;
    const unsigned errorCode = (s.status & PL_CTRL_SPEED_STATUS_ERROR_CODE_MASK)
                               >> PL_CTRL_SPEED_STATUS_ERROR_CODE_SHIFT;
    const unsigned exceptionCode = (s.status & PL_CTRL_SPEED_STATUS_EXCEPTION_CODE_MASK)
                                   >> PL_CTRL_SPEED_STATUS_EXCEPTION_CODE_SHIFT;
    const double rpm = double(s.rpm_milli) / 1000.0;
    const double tsMs = double(s.ts) * double(PL_CTRL_PL_TS_TICK_NS) / 1.0e6;
    if (json) {
        if (online)
            std::printf("{\"ok\":true,\"raw\":%d,\"rpm_milli\":%d,\"rpm\":%.3f,",
                        int(s.raw), int(s.rpm_milli), rpm);
        else
            std::printf("{\"ok\":true,\"raw\":%d,\"rpm_milli\":null,\"rpm\":null,",
                        int(s.raw));
        std::printf("\"count\":%u,\"err_count\":%u,\"ts_ticks\":%u,\"ts_ms\":%.3f,"
                    "\"seq\":%u,\"status\":%u,\"online\":%s,\"configured\":%s,"
                    "\"saturated\":%s,\"error\":%s,\"error_code\":%u,"
                    "\"exception_code\":%u}\n",
                    s.count, s.err_count, s.ts, tsMs, s.seq, s.status,
                    online ? "true" : "false", configured ? "true" : "false",
                    saturated ? "true" : "false", error ? "true" : "false",
                    errorCode, exceptionCode);
        return 0;
    }
    std::printf("SPEED state=%s", online ? "online" : "offline");
    if (online)
        std::printf(" raw=%d rpm_milli=%d rpm=%.3f", int(s.raw), int(s.rpm_milli), rpm);
    else
        std::printf(" raw=%d rpm_milli=- rpm=-", int(s.raw));
    std::printf(" count=%u err_count=%u ts_ticks=%u ts_ms=%.3f seq=%u status=0x%08x"
                " online=%u configured=%u saturated=%u error=%u"
                " error_code=%u(%s) exception_code=%u\n",
                s.count, s.err_count, s.ts, tsMs, s.seq, s.status,
                online ? 1U : 0U, configured ? 1U : 0U, saturated ? 1U : 0U,
                error ? 1U : 0U, errorCode, speed_error_text(errorCode), exceptionCode);
    return 0;
}

int main(int argc,char **argv)
{
    if(argc<2) {
        std::fprintf(stderr,"usage: plctl probe | read OFFSET | write OFFSET VALUE | sensors | speed [--json]\n"
                            "       plctl features | irq TIMEOUT_MS\n"
                            "       plctl spectrum [--bins N] [--top M] [--raw] [--rate HZ] [--base PHYS]\n"
                            "       plctl dma --base PHYS [--bytes N] [--test] [--latency] [--timeout MS]\n"
                            "       plctl dma --ring --blocks N --base PHYS [--count K] [--wait-irq] [--latency] [--test] [--timeout MS]\n"
                            "       plctl dma --abort\n"
                            "       plctl ring-stop\n");
        return 2;
    }
    /* Help must work without a device node (and without root). */
    if(!std::strcmp(argv[1],"spectrum")) {
        for(int i=2;i<argc;++i)
            if(!std::strcmp(argv[i],"--help")) { spectrum_usage(stdout); return 0; }
    }
    if(!std::strcmp(argv[1],"speed")) {
        for(int i=2;i<argc;++i)
            if(!std::strcmp(argv[i],"--help")) { speed_usage(stdout); return 0; }
    }
    const int fd=pl_open(nullptr);
    if(fd<0) { std::perror("/dev/pl_ctrl");return 1; }
    int ret=-1;
    uint32_t value=0,offset=0;
    if(!std::strcmp(argv[1],"probe") && argc==2) {
        ret=pl_probe(fd,&value);
        if(!ret) std::printf("ID=0x%08x\n",value);
    } else if(!std::strcmp(argv[1],"read") && argc==3 && number(argv[2],offset)) {
        ret=::pread(fd,&value,4,offset)==4 ? 0 : -1;
        if(!ret) std::printf("0x%04x=0x%08x\n",offset,value);
    } else if(!std::strcmp(argv[1],"write") && argc==4 && number(argv[2],offset) && number(argv[3],value)) {
        ret=::pwrite(fd,&value,4,offset)==4 ? 0 : -1;
    } else if(!std::strcmp(argv[1],"sensors") && argc==2) {
        pl_sensors_t v={};ret=pl_read_sensors(fd,&v);
        if(!ret) std::printf("frame_ms=%u temp_x10=%d acc_raw=%d,%d,%d counts=%u,%u faults=0x%02x\n",
            v.frame_ms,v.temp_x10,v.acc_x,v.acc_y,v.acc_z,v.temp_count,v.acc_count,v.faults);
    } else if(!std::strcmp(argv[1],"speed") && argc==2) {
        ret=speed(fd,false);
    } else if(!std::strcmp(argv[1],"speed") && argc==3 && !std::strcmp(argv[2],"--json")) {
        ret=speed(fd,true);
    } else if(!std::strcmp(argv[1],"speed")) {
        speed_usage(stderr);
        errno=0;
        ret=-1;
    } else if(!std::strcmp(argv[1],"features") && argc==2) {
        ret=features(fd);
    } else if(!std::strcmp(argv[1],"irq") && argc==3 && number(argv[2],value) && value<=INT_MAX) {
        ret=pl_wait_irq(fd,int(value));
        if(ret>=0) {
            std::printf("IRQ=%s\n",ret ? "received" : "timeout");
            ret=ret ? 0 : 1;
        } else if (errno == ENODEV) {
            std::fprintf(stderr, "irq: sensor IRQ not wired; this build supports polling only\n");
            errno = 0;
            ret = -2;
        }
    } else if(!std::strcmp(argv[1],"ring-stop") && argc==2) {
        ret=ring_stop(fd);
    } else if(!std::strcmp(argv[1],"dma")) {
        dma_opts opt;
        bool bad=false;
        for(int i=2;i<argc && !bad;++i) {
            if(!std::strcmp(argv[i],"--test")) opt.test=true;
            else if(!std::strcmp(argv[i],"--abort")) opt.abortOnly=true;
            else if(!std::strcmp(argv[i],"--ring")) opt.ring=true;
            else if(!std::strcmp(argv[i],"--wait-irq")) opt.waitIrq=true;
            else if(!std::strcmp(argv[i],"--latency")) opt.latency=true;
            else if(!std::strcmp(argv[i],"--base") && i+1<argc && !opt.baseText) opt.baseText=argv[++i];
            else if(!std::strcmp(argv[i],"--bytes") && i+1<argc && !opt.bytesText) opt.bytesText=argv[++i];
            else if(!std::strcmp(argv[i],"--timeout") && i+1<argc && !opt.timeoutText) opt.timeoutText=argv[++i];
            else if(!std::strcmp(argv[i],"--blocks") && i+1<argc && !opt.blocksText) opt.blocksText=argv[++i];
            else if(!std::strcmp(argv[i],"--count") && i+1<argc && !opt.countText) opt.countText=argv[++i];
            else bad=true;
        }
        if(bad) {
            std::fprintf(stderr,"usage: plctl dma --base PHYS [--bytes N] [--test] [--latency] [--timeout MS]\n"
                                "       plctl dma --ring --blocks N --base PHYS [--count K] [--wait-irq]\n"
                                "              [--latency] [--test] [--timeout MS]\n"
                                "       plctl dma --abort\n");
            errno=0;
            ret=-1;
        } else ret=dma(fd,opt);
    } else if(!std::strcmp(argv[1],"spectrum")) {
        spectrum_opts opt;
        bool bad=false;
        for(int i=2;i<argc && !bad;++i) {
            if(!std::strcmp(argv[i],"--raw")) opt.raw=true;
            else if(!std::strcmp(argv[i],"--bins") && i+1<argc && !opt.binsText) opt.binsText=argv[++i];
            else if(!std::strcmp(argv[i],"--top") && i+1<argc && !opt.topText) opt.topText=argv[++i];
            else if(!std::strcmp(argv[i],"--rate") && i+1<argc && !opt.rateText) opt.rateText=argv[++i];
            else if(!std::strcmp(argv[i],"--base") && i+1<argc && !opt.baseText) opt.baseText=argv[++i];
            else bad=true;
        }
        if(bad) {
            spectrum_usage(stderr);
            errno=0;
            ret=-1;
        } else ret=spectrum(fd,opt);
    } else { errno=EINVAL; }
    if(ret<0 && errno) std::perror("plctl");
    pl_close(fd);return ret<0 ? 1 : ret;
}
