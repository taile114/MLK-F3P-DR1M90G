/* Offline check for pl_read_speed() on a plain file: the helper only uses
 * pread(), so a regular file stands in for /dev/pl_ctrl and the §14 window
 * layout plus the signed decoding can be verified without hardware.
 *
 * Build/run (no device, no root):
 *   g++ -std=c++17 -O2 -Wall -Wextra -Werror -I ../src test_pl_speed.cpp \
 *       ../src/pl_host.cpp -o /tmp/test_pl_speed && /tmp/test_pl_speed
 */
#include "../driver/pl_ctrl_uapi.h"
#include "pl_host.h"

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++checks;                                                              \
        if (!(cond)) {                                                         \
            ++failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                      \
    } while (0)

static void put(int fd, uint32_t offset, uint32_t value)
{
    if (::pwrite(fd, &value, sizeof(value), off_t(offset)) != ssize_t(sizeof(value)))
        std::perror("pwrite");
}

/* 0x0009 = bit0 在线 + bit3 FC06 采样时间已配置（§14）。 */
static int make_sample(void)
{
    const int fd = ::open("/tmp/pl_speed_sample.bin", O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || ::ftruncate(fd, 0xd8) != 0) {
        std::perror("open/ftruncate");
        return -1;
    }
    put(fd, PL_CTRL_SPEED_RAW, uint32_t(-1000));
    put(fd, PL_CTRL_SPEED_RPM_MILLI, uint32_t(-146484));
    put(fd, PL_CTRL_SPEED_STATUS, 0x0009U);
    put(fd, PL_CTRL_SPEED_COUNT, 42U);
    put(fd, PL_CTRL_SPEED_TS, 123456U);
    put(fd, PL_CTRL_SPEED_SEQ, 7U);
    put(fd, PL_CTRL_SPEED_ERR_COUNT, 1U);
    return fd;
}

static void test_layout(void)
{
    const int fd = make_sample();
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    pl_speed_t s = {};
    CHECK(pl_read_speed(fd, &s) == 0);
    CHECK(s.raw == -1000);          /* signed: reverse rotation stays negative */
    CHECK(s.rpm_milli == -146484);  /* 0.001 rpm/LSB */
    CHECK(s.status == 0x0009U);
    CHECK(s.count == 42U);
    CHECK(s.ts == 123456U);
    CHECK(s.seq == 7U);
    CHECK(s.err_count == 1U);
    CHECK((s.status & PL_CTRL_SPEED_STATUS_ONLINE) != 0);
    CHECK((s.status & PL_CTRL_SPEED_STATUS_CONFIGURED) != 0);
    CHECK((s.status & PL_CTRL_SPEED_STATUS_ERROR) == 0);
    ::close(fd);
}

static void test_bad_args(void)
{
    const int fd = make_sample();
    CHECK(fd >= 0);
    if (fd >= 0) {
        errno = 0;
        CHECK(pl_read_speed(fd, nullptr) != 0);
        CHECK(errno == EINVAL);
        ::close(fd);
    }
    errno = 0;
    CHECK(pl_read_speed(-1, nullptr) != 0);
}

/* A file shorter than SPEED_ERR_COUNT makes the 7-word pread short, so the
 * burst cannot be accepted. */
static void test_short_window(void)
{
    const int fd = ::open("/tmp/pl_speed_short.bin", O_RDWR | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    CHECK(::ftruncate(fd, 0xc8) == 0);   /* ends right after SPEED_TS */
    pl_speed_t s = {};
    errno = 0;
    CHECK(pl_read_speed(fd, &s) != 0);
    CHECK(errno == EAGAIN || errno == EIO);
    ::close(fd);
}

int main()
{
    test_layout();
    test_bad_args();
    test_short_window();
    if (failures) {
        std::fprintf(stderr, "test_pl_speed: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    std::printf("test_pl_speed: all %d checks passed\n", checks);
    return 0;
}
