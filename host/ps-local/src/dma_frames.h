#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mlk {

/* 契约 §4.4 的 DMA 数据字：{TS[63:32], CH[31:16], SAMPLE[15:0]}。
 * 一个振动事件连续写 CH=0(X) / 1(Y) / 2(Z) 三个字，三字共享同一 TS；
 * 每块 512 字而 512 不是 3 的倍数，完整帧必然跨块。
 *
 * 一个转速事件同样连续写三个字（接口 v1.2，契约 §14）：
 *   W0 = {ts,              CH=3, SPEED_STATUS[15:0]}
 *   W1 = {rpm_milli(s32),  CH=4, 0}
 *   W2 = {raw_speed(s32),  CH=5, 0}
 * W0 的 SAMPLE 字段就是寄存器 SPEED_STATUS 的低 16 位，两者共用一套位图：
 *   bit0 在线 · bit1 换算饱和 · bit2 本次事件为错误（不携带新样本）
 *   bit3 FC06 采样时间已配置 · [7:4] error_code · [15:8] Modbus exception_code
 * 错误事件同样写一条记录，只是 bit2=1，此时 rpm/raw 仍是最近一次成功值，
 * 使用方只把 bit2=0 的记录计为新样本。
 * 转速记录的 W1/W2 高 32 位是数值而不是时间戳，只有 W0 的 TS 有效，
 * 与振动帧「TS 取首字」的规则一致。
 *
 * DmaFrameAssembler 按 CH 序列配齐记录：0,1,2 产出 DmaFrame，3,4,5 产出
 * DmaSpeedRecord；未配齐的字跨 push() 保留（至多两个字，即跨块的尾部余字），
 * 因此块边界处的半帧/半条记录不会丢失。
 *
 * 重同步规则（失配只丢已收到的部分，不丢后续数据）：
 *  - CH=pad(0xFFFF)：按填充字跳过；若正处在一个未配齐的记录里，该记录作废
 *    （计入 dropped），从下一个首字（CH=0 或 3）重新对齐。
 *  - 越界 CH（>5 且非 pad）：说明流已失配（计入 resync）；若正处在一个未配齐
 *    的记录里，该记录作废（计入 dropped），继续等下一个首字。
 *  - 处于空闲时收到非首字（振动 CH=1/2、转速 CH=4/5）：该字丢弃（计入 resync），
 *    继续等下一个首字。
 *  - 等后续字时收到不匹配的字：已收到的部分作废（计入 dropped 与 resync）；
 *    若该字本身是某个记录的首字（CH=0 或 3），则直接作为下一条记录的起点，
 *    不再额外丢一个字。
 *  - 帧/记录的 TS 取首字的 TS：契约规定同一条记录内各字共享同一 TS。
 *
 * 该类不依赖 Qt / OpenSSL，可单独编译和测试。
 */
struct DmaFrame
{
    uint32_t ts;
    int16_t x;
    int16_t y;
    int16_t z;
};

/* 流内转速记录（接口 v1.2）。ts 为 W0 的 PL 时间戳（10 ns/LSB），
 * rpm_milli 为 0.001 rpm 的千分整数，raw_speed 为每 100 ms 采样周期的计数差，
 * flags 为 W0 的状态字（位图见文件头注释，与寄存器 SPEED_STATUS 低 16 位一致）。 */
struct DmaSpeedRecord
{
    uint32_t ts;
    int32_t rpm_milli;
    int32_t raw_speed;
    uint16_t flags;
};

class DmaFrameAssembler
{
public:
    enum : uint16_t {
        kChX = 0,
        kChY = 1,
        kChZ = 2,
        kChSpeedTs = 3,
        kChSpeedRpm = 4,
        kChSpeedRaw = 5,
        kChPad = 0xFFFF,
    };
    enum : unsigned { kWordsPerFrame = 3 };
    /* 转速记录 W0 的状态位：SPEED_STATUS 的低 16 位（接口 v1.2）。 */
    enum : uint16_t {
        kSpeedFlagOnline = 1u << 0,
        kSpeedFlagSaturated = 1u << 1,
        kSpeedFlagError = 1u << 2,
        kSpeedFlagConfigured = 1u << 3,
    };
    enum : unsigned { kSpeedErrorCodeShift = 4, kSpeedExceptionCodeShift = 8 };

    /* 追加 count 个字（count 为本块有效字数），完整帧按出现顺序追加到 out，
     * 完整转速记录追加到 speeds（可为 nullptr）；返回本次追加的完整帧数。 */
    std::size_t push(const uint64_t *words, unsigned count, std::vector<DmaFrame> &out)
    {
        return push(words, count, out, nullptr);
    }

    std::size_t push(const uint64_t *words, unsigned count, std::vector<DmaFrame> &out,
                     std::vector<DmaSpeedRecord> *speeds)
    {
        if (!words)
            return 0;
        const std::size_t before = out.size();
        for (unsigned i = 0; i < count; ++i) {
            const uint64_t word = words[i];
            const uint32_t ts = uint32_t(word >> 32);
            const uint16_t raw = uint16_t(word & 0xFFFFu);
            const uint16_t ch = uint16_t((word >> 16) & 0xFFFFu);
            const int16_t sample = signed16(raw);

            if (ch == kChPad) {
                ++m_pad;
                discardPending();
                continue;
            }
            if (ch > kChSpeedRaw) {
                ++m_resync;   /* 越界 CH 说明流已失配 */
                discardPending();
                continue;
            }

            if (m_stage == 0) {
                if (ch == kChX)
                    startVibration(ts, sample);
                else if (ch == kChSpeedTs)
                    startSpeed(ts, raw);
                else
                    ++m_resync;   /* 缺首字的尾字：等下一个首字 */
                continue;
            }

            const uint16_t expect = (m_kind == kKindVibration)
                                        ? (m_stage == 1 ? kChY : kChZ)
                                        : (m_stage == 1 ? kChSpeedRpm : kChSpeedRaw);
            if (ch == expect) {
                if (m_kind == kKindVibration) {
                    if (m_stage == 1) {
                        m_y = sample;
                        m_stage = 2;
                    } else {
                        out.push_back(DmaFrame{m_ts, m_x, m_y, sample});
                        ++m_frames;
                        m_stage = 0;
                        m_kind = kKindNone;
                    }
                } else if (m_stage == 1) {
                    /* 转速记录的 W1：高 32 位是 rpm_milli，低 16 位保留为 0。 */
                    m_speed.rpm_milli = signed32(ts);
                    m_stage = 2;
                } else {
                    /* 转速记录的 W2：高 32 位是原始计数差，低 16 位保留为 0。 */
                    m_speed.raw_speed = signed32(ts);
                    if (speeds)
                        speeds->push_back(m_speed);
                    ++m_speedRecords;
                    m_stage = 0;
                    m_kind = kKindNone;
                }
                continue;
            }

            /* 失配：已收到的部分作废；本字若是首字则直接作为下一条记录起点。 */
            ++m_dropped;
            ++m_resync;
            m_stage = 0;
            m_kind = kKindNone;
            if (ch == kChX)
                startVibration(ts, sample);
            else if (ch == kChSpeedTs)
                startSpeed(ts, raw);
        }
        return out.size() - before;
    }

    /* 清空跨块余字与解析状态；未配齐的半帧/半条记录计入 dropped。统计计数保留。 */
    void reset()
    {
        discardPending();
        m_kind = kKindNone;
    }

    unsigned long long resyncCount() const { return m_resync; }
    unsigned long long droppedCount() const { return m_dropped; }
    unsigned long long padCount() const { return m_pad; }
    /* 已配齐的振动帧 / 转速记录总数（与是否传入输出向量无关）。 */
    unsigned long long frameCount() const { return m_frames; }
    unsigned long long speedRecordCount() const { return m_speedRecords; }
    /* 当前保留的跨块字数，0～2。 */
    unsigned pending() const { return m_stage; }

private:
    enum Kind { kKindNone = 0, kKindVibration = 1, kKindSpeed = 2 };

    static int16_t signed16(uint16_t raw)
    {
        return raw >= 0x8000u ? int16_t(int32_t(raw) - 0x10000) : int16_t(raw);
    }

    static int32_t signed32(uint32_t raw)
    {
        return raw >= 0x80000000u ? int32_t(int64_t(raw) - 0x100000000LL) : int32_t(raw);
    }

    void startVibration(uint32_t ts, int16_t x)
    {
        m_ts = ts;
        m_x = x;
        m_stage = 1;
        m_kind = kKindVibration;
    }

    void startSpeed(uint32_t ts, uint16_t flags)
    {
        m_speed = DmaSpeedRecord{ts, 0, 0, flags};
        m_stage = 1;
        m_kind = kKindSpeed;
    }

    void discardPending()
    {
        if (m_stage != 0) {
            ++m_dropped;
            m_stage = 0;
            m_kind = kKindNone;
        }
    }

    uint32_t m_ts = 0;
    int16_t m_x = 0;
    int16_t m_y = 0;
    DmaSpeedRecord m_speed = DmaSpeedRecord{0, 0, 0, 0};
    unsigned m_stage = 0;   /* 0=等首字，1=等第 2 字，2=等第 3 字 */
    unsigned m_kind = kKindNone;
    unsigned long long m_frames = 0;
    unsigned long long m_speedRecords = 0;
    unsigned long long m_resync = 0;
    unsigned long long m_dropped = 0;
    unsigned long long m_pad = 0;
};

} // namespace mlk
