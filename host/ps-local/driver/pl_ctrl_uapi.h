#ifndef MLK_PL_CTRL_UAPI_H
#define MLK_PL_CTRL_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* PL_CTRL_IRQ_COUNT counts every serviced interrupt (sensor and DMA);
 * PL_CTRL_DMA_IRQ_COUNT counts only the §4.4 block-complete interrupts.
 * Reading either count also consumes all events queued so far, so a following
 * poll() blocks until the next interrupt instead of returning at once. */
#define PL_CTRL_IRQ_COUNT _IOR('P', 0, __u32)
#define PL_CTRL_DMA_IRQ_COUNT _IOR('P', 1, __u32)
#define PL_CTRL_ID_VALUE 0x504c0103U
#define PL_CTRL_ID_COMPAT 0x504c0102U
#define PL_CTRL_ALARM_STATE 0xd4U
#define PL_CTRL_ALARM_STATUS 0xd8U
#define PL_CTRL_ALARM_PIN (1U << 0)
#define PL_CTRL_ALARM_BUSY (1U << 1)
#define PL_CTRL_ALARM_MODEL (1U << 2)
#define PL_CTRL_ALARM_EXPIRED (1U << 3)
#define PL_CTRL_WINDOW 0x200U

/* Interface v1.2 (ID=0x504c0102): §4.6 full spectrum window, BIN_POWER[k]
 * sits at 0x400 + 4k and holds (|X_k|^2) >> 8 as unsigned32, one word per bin.
 * |X|^2 peaks near 2^39 on the ADXL345 +-16 g range, so >>8 keeps the word
 * below 2^32. PL_CTRL_SPECTRUM_WORDS is
 * the bin count N baked into the current PL build (N=256, so 0x400..0x7ff; an
 * N=512 build needs 512 words, 0x400..0xbff). PL_CTRL_SPECTRUM_MAX_WORDS only
 * bounds the pread window decoded by the driver, so replaying a larger
 * build does not need another driver change. */
#define PL_CTRL_SPECTRUM_OFFSET 0x400U
#define PL_CTRL_SPECTRUM_WORDS 256U
#define PL_CTRL_SPECTRUM_MAX_WORDS 768U

/* Sensor DMA channel (docs/PL-PS接口约定.md §4.4), offsets inside the window. */
#define PL_CTRL_DMA_CTRL 0x90U
#define PL_CTRL_DMA_BASE_LO 0x94U
#define PL_CTRL_DMA_BASE_HI 0x98U
#define PL_CTRL_DMA_BYTES 0x9cU
#define PL_CTRL_DMA_STATUS 0xa0U
#define PL_CTRL_DMA_WRITTEN 0xa4U
#define PL_CTRL_DMA_BLK_SEQ 0xa8U
#define PL_CTRL_DMA_IRQ 0xacU
#define PL_CTRL_PL_TS_NOW 0xb0U
#define PL_CTRL_DMA_BLOCKS 0xb4U

/* DMA_CTRL: START / ABORT self-clear after one cycle; TEST, RING and STOP are
 * levels. In ring mode STOP releases BUSY after the current burst and leaves
 * ERRCODE untouched, so a ring run ends without the sticky ERRCODE=3 of ABORT.
 * The PL latches TEST/RING/STOP from the same write, so a STOP write must carry
 * the live RING/TEST bits or it also reconfigures the mode. */
#define PL_CTRL_DMA_CTRL_START (1U << 0)
#define PL_CTRL_DMA_CTRL_ABORT (1U << 1)
#define PL_CTRL_DMA_CTRL_TEST (1U << 2)
#define PL_CTRL_DMA_CTRL_RING (1U << 3)
#define PL_CTRL_DMA_CTRL_STOP (1U << 4)
#define PL_CTRL_DMA_CTRL_BUSY (1U << 8)
#define PL_CTRL_DMA_CTRL_DONE (1U << 9)
#define PL_CTRL_DMA_CTRL_ERROR (1U << 10)

/* DMA_IRQ: [0] enable, [8] W1C clear pending, [16] read-only pending. */
#define PL_CTRL_DMA_IRQ_ENABLE (1U << 0)
#define PL_CTRL_DMA_IRQ_CLEAR (1U << 8)
#define PL_CTRL_DMA_IRQ_PENDING (1U << 16)

/* Ring mode: 4 KiB blocks, DMA_BLOCKS = N in 1..256, address wraps modulo N. */
#define PL_CTRL_DMA_BLOCK_SIZE 4096U
#define PL_CTRL_DMA_BLOCKS_MIN 1U
#define PL_CTRL_DMA_BLOCKS_MAX 256U

/* PL_TS_NOW shares the §4.3 timebase: 10 ns per LSB, wraps modulo 2^32. */
#define PL_CTRL_PL_TS_TICK_NS 10U

/* §5: f2p_irq[n] is GIC SPI 82+n, i.e. GIC ID 114+n (PL_INTER0 = 114, from the
 * SDK PL_IRQ_NUM0/1 and the vendor VDMA node <GIC_SPI 82/83>). The §4.4
 * block-complete IRQ is f2p_irq[3] -> SPI 85 -> GIC ID 117 and is LEVEL_HIGH:
 * the line follows DMA_IRQ[16] PEND, which the PS drops by writing
 * DMA_IRQ[8]=1 (W1C), so the level and the clearing write cannot race away
 * from each other. An opted-in device tree therefore carries
 * `interrupts = <0x0 0x55 0x4>`; platform/flash/build_hdmi_dtb.sh writes that
 * cell for `pl_irq=auto` and `<gic>`, and omits the property for `pl_irq=off`. */
#define PL_CTRL_DMA_IRQ_GIC 117U
#define PL_CTRL_DMA_IRQ_SPI 85U

/* DMA_STATUS: completed bursts, sticky overflow and error code. */
#define PL_CTRL_DMA_STATUS_BURSTS_MASK 0x0000ffffU
#define PL_CTRL_DMA_STATUS_OVF (1U << 16)
#define PL_CTRL_DMA_STATUS_ERRCODE_SHIFT 17U
#define PL_CTRL_DMA_STATUS_ERRCODE_MASK (0x7U << PL_CTRL_DMA_STATUS_ERRCODE_SHIFT)

/* DMA_STATUS.ERRCODE values. */
#define PL_CTRL_DMA_ERRCODE_PARAM 1U
#define PL_CTRL_DMA_ERRCODE_BRESP 2U
#define PL_CTRL_DMA_ERRCODE_ABORT 3U

/* DMA_BYTES: multiple of 8, 8..4096; DMA_BASE must be 8-byte aligned. */
#define PL_CTRL_DMA_BYTES_MIN 8U
#define PL_CTRL_DMA_BYTES_MAX 4096U
#define PL_CTRL_DMA_ALIGN 8U

/* RS485 speed channel (interface v1.2), read-only, docs/PL-PS接口约定.md §14.
 * One sample per 100 ms:
 * SPEED_RAW is the encoder count delta over that period, SPEED_RPM_MILLI the
 * converted speed in 0.001 rpm/LSB (signed, negative on reverse rotation).
 * SPEED_TS uses the same 10 ns timebase as PL_TS_NOW; SPEED_SEQ counts every
 * published event, including error events that produce no new sample, while
 * SPEED_COUNT only counts successful samples. Read them seq -> fields -> seq:
 * a changed seq means the PL published an event while the burst was in flight. */
#define PL_CTRL_SPEED_RAW 0xb8U
#define PL_CTRL_SPEED_RPM_MILLI 0xbcU
#define PL_CTRL_SPEED_STATUS 0xc0U
#define PL_CTRL_SPEED_COUNT 0xc4U
#define PL_CTRL_SPEED_TS 0xc8U
#define PL_CTRL_SPEED_SEQ 0xccU
#define PL_CTRL_SPEED_ERR_COUNT 0xd0U

/* SPEED_STATUS (32 bit; the upper 16 bits read 0). The DMA speed record's
 * first word carries the low 16 bits of this same register in its SAMPLE
 * field, so one bitmap covers both widths:
 *   bit0 online, bit1 conversion saturated, bit2 this event is an error and
 *   carries no new sample, bit3 FC06 sample time configured,
 *   [7:4] last error code, [15:8] last Modbus exception code, rest 0.
 * The [7:4] / [15:8] fields are only valid while bit2 is set. A write from the
 * PS side is never accepted; the whole window 0xb8..0xd0 stays read-only. */
#define PL_CTRL_SPEED_STATUS_ONLINE (1U << 0)
#define PL_CTRL_SPEED_STATUS_SATURATED (1U << 1)
#define PL_CTRL_SPEED_STATUS_ERROR (1U << 2)
#define PL_CTRL_SPEED_STATUS_CONFIGURED (1U << 3)
#define PL_CTRL_SPEED_STATUS_ERROR_CODE_SHIFT 4U
#define PL_CTRL_SPEED_STATUS_ERROR_CODE_MASK (0xfU << PL_CTRL_SPEED_STATUS_ERROR_CODE_SHIFT)
#define PL_CTRL_SPEED_STATUS_EXCEPTION_CODE_SHIFT 8U
#define PL_CTRL_SPEED_STATUS_EXCEPTION_CODE_MASK (0xffU << PL_CTRL_SPEED_STATUS_EXCEPTION_CODE_SHIFT)

/* SPEED_STATUS.ERROR_CODE values (docs/RS485转速通道接入.md). */
#define PL_CTRL_SPEED_ERR_NONE 0U
#define PL_CTRL_SPEED_ERR_TIMEOUT 1U
#define PL_CTRL_SPEED_ERR_CRC 2U
#define PL_CTRL_SPEED_ERR_FRAME 3U
#define PL_CTRL_SPEED_ERR_UART 4U
#define PL_CTRL_SPEED_ERR_EXCEPTION 5U
#define PL_CTRL_SPEED_ERR_GAP 6U
#define PL_CTRL_SPEED_ERR_RX_OVF 7U

#endif
