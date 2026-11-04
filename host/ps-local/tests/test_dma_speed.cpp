/* dma_frames.h 的离线单元测试：振动帧（CH=0/1/2）与转速记录（CH=3/4/5）。
 *
 * 只用标准库，不依赖 Qt / 设备 / 交叉工具链：
 *   g++ -std=c++17 -O2 -Wall -Wextra -Werror -I ../src test_dma_speed.cpp -o test_dma_speed
 * 退出码 0 表示全部通过。覆盖：完整记录、跨块半条记录、混流、未知 CH、
 * 半条记录被后续首字打断、振动帧既有跨块与重同步行为。
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "dma_frames.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char *what, int line, const char *file)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, what);
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__, __FILE__)

/* 契约数据字：{TS[63:32], CH[31:16], SAMPLE[15:0]}。 */
uint64_t word(uint32_t ts, uint16_t ch, uint16_t sample)
{
    return (uint64_t(ts) << 32) | (uint64_t(ch) << 16) | uint64_t(sample);
}

/* 转速记录 W0/W1/W2：W0 的 SAMPLE 是 SPEED_STATUS 低 16 位，W1/W2 的高
 * 32 位是数值而不是时间戳，低 16 位保留为 0（契约 §14）。 */
void appendSpeed(std::vector<uint64_t> &w, uint32_t ts, int32_t rpmMilli, int32_t raw,
                 uint16_t flags)
{
    w.push_back(word(ts, mlk::DmaFrameAssembler::kChSpeedTs, flags));
    w.push_back(word(uint32_t(rpmMilli), mlk::DmaFrameAssembler::kChSpeedRpm, 0));
    w.push_back(word(uint32_t(raw), mlk::DmaFrameAssembler::kChSpeedRaw, 0));
}

void appendVibration(std::vector<uint64_t> &w, uint32_t ts, int16_t x, int16_t y, int16_t z)
{
    w.push_back(word(ts, mlk::DmaFrameAssembler::kChX, uint16_t(x)));
    w.push_back(word(ts, mlk::DmaFrameAssembler::kChY, uint16_t(y)));
    w.push_back(word(ts, mlk::DmaFrameAssembler::kChZ, uint16_t(z)));
}

/* 1. 振动帧跨块：块 1 只有 X/Y，块 2 补 Z。行为必须与扩展前一致。 */
void testVibrationAcrossBlocks()
{
    mlk::DmaFrameAssembler asm_;
    std::vector<uint64_t> a{word(100, mlk::DmaFrameAssembler::kChX, 11),
                            word(100, mlk::DmaFrameAssembler::kChY, 12)};
    std::vector<mlk::DmaFrame> frames;
    CHECK(asm_.push(a.data(), unsigned(a.size()), frames) == 0);
    CHECK(asm_.pending() == 2);

    std::vector<uint64_t> b{word(100, mlk::DmaFrameAssembler::kChZ, 13)};
    CHECK(asm_.push(b.data(), unsigned(b.size()), frames) == 1);
    CHECK(frames.size() == 1 && frames[0].ts == 100 && frames[0].x == 11 && frames[0].y == 12
          && frames[0].z == 13);
    CHECK(asm_.resyncCount() == 0 && asm_.droppedCount() == 0 && asm_.pending() == 0);
}

/* 2. 完整转速记录：字段拆解正确（rpm 有符号、seq 取 W1 低 16 位、raw 有符号）。 */
void testCompleteSpeedRecord()
{
    mlk::DmaFrameAssembler asm_;
    std::vector<uint64_t> w;
    appendSpeed(w, 0x12345678u, -146484, -1000, 0x0009u);
    std::vector<mlk::DmaFrame> frames;
    std::vector<mlk::DmaSpeedRecord> speeds;
    CHECK(asm_.push(w.data(), unsigned(w.size()), frames, &speeds) == 0);
    CHECK(frames.empty());
    CHECK(speeds.size() == 1);
    CHECK(speeds[0].ts == 0x12345678u);
    CHECK(speeds[0].rpm_milli == -146484);
    CHECK(speeds[0].raw_speed == -1000);
    /* 0x0009 = bit0 在线 + bit3 已配置。 */
    CHECK(speeds[0].flags == 0x0009u);
    CHECK(asm_.speedRecordCount() == 1);
    CHECK(asm_.resyncCount() == 0 && asm_.droppedCount() == 0 && asm_.pending() == 0);
}

/* 3. 半条转速记录跨块：W0/W1 在块 1，W2 在块 2，必须补成一条完整记录。 */
void testSpeedAcrossBlocks()
{
    mlk::DmaFrameAssembler asm_;
    std::vector<mlk::DmaFrame> frames;
    std::vector<mlk::DmaSpeedRecord> speeds;
    std::vector<uint64_t> a{word(777, mlk::DmaFrameAssembler::kChSpeedTs, 0x0005u),
                            word(uint32_t(3000), mlk::DmaFrameAssembler::kChSpeedRpm, 0)};
    CHECK(asm_.push(a.data(), unsigned(a.size()), frames, &speeds) == 0);
    CHECK(speeds.empty());
    CHECK(asm_.pending() == 2);

    std::vector<uint64_t> b{word(uint32_t(60), mlk::DmaFrameAssembler::kChSpeedRaw, 0)};
    CHECK(asm_.push(b.data(), unsigned(b.size()), frames, &speeds) == 0);
    CHECK(speeds.size() == 1);
    CHECK(speeds[0].ts == 777 && speeds[0].rpm_milli == 3000 && speeds[0].raw_speed == 60);
    CHECK(speeds[0].flags == 0x0005u);
    CHECK(asm_.pending() == 0 && asm_.droppedCount() == 0);
}

/* 4. 混流：振动帧与转速记录交错，两条解析互不干扰，顺序保持。 */
void testMixedStream()
{
    mlk::DmaFrameAssembler asm_;
    std::vector<uint64_t> w;
    appendVibration(w, 10, 1, 2, 3);
    appendSpeed(w, 20, 1000, 5, 0x0001u);
    appendVibration(w, 30, 4, 5, 6);
    appendSpeed(w, 40, -1000, -5, 0x0001u);

    std::vector<mlk::DmaFrame> frames;
    std::vector<mlk::DmaSpeedRecord> speeds;
    asm_.push(w.data(), unsigned(w.size()), frames, &speeds);
    CHECK(frames.size() == 2 && speeds.size() == 2);
    CHECK(frames[0].ts == 10 && frames[0].x == 1 && frames[0].z == 3);
    CHECK(frames[1].ts == 30 && frames[1].y == 5 && frames[1].z == 6);
    CHECK(speeds[0].ts == 20 && speeds[0].rpm_milli == 1000);
    CHECK(speeds[1].ts == 40 && speeds[1].rpm_milli == -1000);
    CHECK(asm_.resyncCount() == 0 && asm_.droppedCount() == 0 && asm_.padCount() == 0);
}

/* 5. 半条转速记录被振动帧首字打断：只丢这半条，振动帧照常产出。 */
void testSpeedAbortedByVibration()
{
    mlk::DmaFrameAssembler asm_;
    std::vector<mlk::DmaFrame> frames;
    std::vector<mlk::DmaSpeedRecord> speeds;
    std::vector<uint64_t> w{word(50, mlk::DmaFrameAssembler::kChSpeedTs, 1),
                            word(1000, mlk::DmaFrameAssembler::kChSpeedRpm, 0)};
    appendVibration(w, 60, 7, 8, 9);
    asm_.push(w.data(), unsigned(w.size()), frames, &speeds);
    CHECK(speeds.empty());
    CHECK(frames.size() == 1 && frames[0].ts == 60 && frames[0].x == 7 && frames[0].z == 9);
    CHECK(asm_.droppedCount() == 1);
    CHECK(asm_.resyncCount() == 1);
    CHECK(asm_.pending() == 0);
}

/* 6. 未知 CH：计入 resync 并作废半条记录，但后续记录不丢。 */
void testUnknownChannel()
{
    mlk::DmaFrameAssembler asm_;
    std::vector<mlk::DmaFrame> frames;
    std::vector<mlk::DmaSpeedRecord> speeds;
    std::vector<uint64_t> w{word(1, mlk::DmaFrameAssembler::kChSpeedTs, 1),
                            word(2, 9, 0),   /* 未知 CH=9 */
                            };
    appendSpeed(w, 70, 500, 3, 0x0001u);
    asm_.push(w.data(), unsigned(w.size()), frames, &speeds);
    CHECK(speeds.size() == 1 && speeds[0].ts == 70 && speeds[0].rpm_milli == 500);
    CHECK(asm_.droppedCount() == 1);
    CHECK(asm_.resyncCount() == 1);
    CHECK(asm_.pending() == 0);
}

/* 7. 半条转速记录被新的 W0 打断：第一条作废，第二条完整产出。 */
void testSpeedRestartOnNewW0()
{
    mlk::DmaFrameAssembler asm_;
    std::vector<mlk::DmaFrame> frames;
    std::vector<mlk::DmaSpeedRecord> speeds;
    std::vector<uint64_t> w{word(10, mlk::DmaFrameAssembler::kChSpeedTs, 0x0001u)};
    appendSpeed(w, 20, 2000, 4, 0x0001u);
    asm_.push(w.data(), unsigned(w.size()), frames, &speeds);
    CHECK(speeds.size() == 1 && speeds[0].ts == 20 && speeds[0].rpm_milli == 2000);
    CHECK(asm_.droppedCount() == 1 && asm_.resyncCount() == 1);
}

/* 8. 既有振动重同步规则：等 X 时收到 Y 只计 resync，不计 dropped。 */
void testVibrationOrphanTail()
{
    mlk::DmaFrameAssembler asm_;
    std::vector<mlk::DmaFrame> frames;
    std::vector<uint64_t> w{word(1, mlk::DmaFrameAssembler::kChY, 3),
                            word(2, mlk::DmaFrameAssembler::kChZ, 4)};
    appendVibration(w, 5, 1, 2, 3);
    asm_.push(w.data(), unsigned(w.size()), frames);
    CHECK(frames.size() == 1 && frames[0].ts == 5);
    CHECK(asm_.resyncCount() == 2 && asm_.droppedCount() == 0);
}

/* 9. pad 填充：计 pad，作废未配齐的转速记录，不增加 resync。 */
void testPadDropsPending()
{
    mlk::DmaFrameAssembler asm_;
    std::vector<mlk::DmaFrame> frames;
    std::vector<mlk::DmaSpeedRecord> speeds;
    std::vector<uint64_t> w{word(1, mlk::DmaFrameAssembler::kChSpeedTs, 1),
                            word(0, mlk::DmaFrameAssembler::kChPad, 0)};
    appendSpeed(w, 90, 1234, 1, 0x0003u);
    asm_.push(w.data(), unsigned(w.size()), frames, &speeds);
    CHECK(speeds.size() == 1 && speeds[0].ts == 90);
    CHECK(asm_.padCount() == 1);
    CHECK(asm_.droppedCount() == 1);
    CHECK(asm_.resyncCount() == 0);
}

/* 10. reset() 丢弃跨块余字并计入 dropped，统计计数保留。 */
void testReset()
{
    mlk::DmaFrameAssembler asm_;
    std::vector<mlk::DmaFrame> frames;
    std::vector<mlk::DmaSpeedRecord> speeds;
    std::vector<uint64_t> w{word(1, mlk::DmaFrameAssembler::kChSpeedTs, 1),
                            word(2, mlk::DmaFrameAssembler::kChSpeedRpm, 0)};
    asm_.push(w.data(), unsigned(w.size()), frames, &speeds);
    CHECK(asm_.pending() == 2);
    asm_.reset();
    CHECK(asm_.pending() == 0 && asm_.droppedCount() == 1);
    std::vector<uint64_t> v;   /* reset 之后仍能正常解析新记录 */
    appendSpeed(v, 3, 77, 1, 0x0001u);
    asm_.push(v.data(), unsigned(v.size()), frames, &speeds);
    CHECK(speeds.size() == 1 && speeds[0].rpm_milli == 77);
}

} // namespace

int main()
{
    testVibrationAcrossBlocks();
    testCompleteSpeedRecord();
    testSpeedAcrossBlocks();
    testMixedStream();
    testSpeedAbortedByVibration();
    testUnknownChannel();
    testSpeedRestartOnNewW0();
    testVibrationOrphanTail();
    testPadDropsPending();
    testReset();
    if (g_failures != 0) {
        std::fprintf(stderr, "test_dma_speed: %d/%d checks failed\n", g_failures, g_checks);
        return EXIT_FAILURE;
    }
    std::printf("test_dma_speed: all %d checks passed\n", g_checks);
    return EXIT_SUCCESS;
}
