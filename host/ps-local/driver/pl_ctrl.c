// SPDX-License-Identifier: GPL-2.0
// Register semantics: docs/PL-PS接口约定.md §§4,5,13 (interface v1.2,
// ID=0x504c0103 (v1.2 acquisition compatible)).
// 0x00-0x10 keep their frozen semantics. 0x90-0x9c are the §4.4 DMA control and
// configuration words and are writable; 0xa0/0xa4 are DMA status (read-only).
// 0xa8/0xac/0xb0/0xb4 are the §4.4 ring window: 0xac (DMA_IRQ) and 0xb4
// (DMA_BLOCKS) are writable, 0xa8 (DMA_BLK_SEQ) and 0xb0 (PL_TS_NOW) are not.
// 0xb8-0xd0 is the read-only RS485 speed channel: reading it as one pread is how
// the speed registers are sampled consistently (seq -> fields -> seq). None of
// these offsets enter the write whitelist.
// 0x400-0xfff is the §4.6 read-only BIN_POWER window holding (|X|^2)>>8; only
// pread is allowed there, and the BIN_POWER words never enter the write
// whitelist.
// The block-complete interrupt is optional: without an `interrupts` property the
// driver stays in polling-only mode and every probe still succeeds.
#include <linux/atomic.h>
#include <linux/compat.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "pl_ctrl_uapi.h"

/* §5 puts the sensor/frame pulse and §4.4 the block-complete pulse on separate
 * GIC lines, and a device tree may declare one or both. One handler services
 * both registers, so the lines only differ in how many times it is entered. */
#define PL_CTRL_IRQ_MAX 2

struct pl_ctrl {
	struct miscdevice misc;
	struct kref ref;
	struct mutex lock;
	wait_queue_head_t wait;
	atomic_t irq_count;	/* every serviced interrupt (poll, IRQ_COUNT) */
	atomic_t dma_irq_count;	/* §4.4 block-complete interrupts only */
	void __iomem *regs;
	resource_size_t start, size;
	int irq[PL_CTRL_IRQ_MAX];	/* requested lines only */
	int nirq;		/* 0 when the device tree declares no line */
	bool dead;
};

struct pl_file {
	struct pl_ctrl *pl;
	u32 seen;
};

static void pl_release_ref(struct kref *ref)
{
	kfree(container_of(ref, struct pl_ctrl, ref));
}

static bool pl_readable(unsigned int offset)
{
	return offset <= 0x18 || (offset >= 0x20 && offset <= 0x88) ||
	       /* §4.4 ring window, then the interface v1.2 speed channel
		* through SPEED_ERR_COUNT (0xd0), followed by the v1.3 alarm window. */
	       (offset >= 0x90 && offset <= 0xd8) ||
	       (offset >= 0x100 && offset < 0x200) ||
	       /* §4.6 BIN_POWER[k], one unsigned32 per bin. The platform
		* resource is the 64 KiB §3 aperture, so the reserved window
		* (ending at 0xfff with PL_CTRL_SPECTRUM_MAX_WORDS) is always
		* inside the ioremap. */
	       (offset >= PL_CTRL_SPECTRUM_OFFSET &&
		offset < PL_CTRL_SPECTRUM_OFFSET + 4U * PL_CTRL_SPECTRUM_MAX_WORDS);
}

/* §13: a single read must stay inside one decoded window. The §4.6 spectrum
 * window is not contiguous with 0x000..0x1ff, so a read cannot span both. */
static bool pl_read_window(loff_t pos, size_t count)
{
	const loff_t spectrum_end = (loff_t)PL_CTRL_SPECTRUM_OFFSET +
				    4 * (loff_t)PL_CTRL_SPECTRUM_MAX_WORDS;
	loff_t limit;

	if (pos < PL_CTRL_WINDOW)
		limit = PL_CTRL_WINDOW;
	else if (pos >= (loff_t)PL_CTRL_SPECTRUM_OFFSET && pos < spectrum_end)
		limit = spectrum_end;
	else
		return false;
	return count <= (size_t)(limit - pos);
}

static bool pl_writable(unsigned int offset)
{
	switch (offset) {
	case 0x04: case 0x08: case 0x10: case 0x14:
	case 0x40: case 0x44: case 0x48: case 0x4c: case 0x50:
	/* §4.4 DMA_CTRL / DMA_BASE_LO / DMA_BASE_HI / DMA_BYTES. */
	case 0x90: case 0x94: case 0x98: case 0x9c:
	/* §4.4 DMA_IRQ (enable, W1C clear) and DMA_BLOCKS (ring size); the
	 * neighbouring DMA_BLK_SEQ (0xa8) and PL_TS_NOW (0xb0) stay read-only. */
	case 0xac: case 0xb4: case PL_CTRL_ALARM_STATE:
		return true;
	default:
		return false;
	}
}

static int pl_open_file(struct inode *inode, struct file *file)
{
	struct pl_ctrl *pl = container_of(file->private_data, struct pl_ctrl, misc);
	struct pl_file *ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);

	if (!ctx)
		return -ENOMEM;
	mutex_lock(&pl->lock);
	if (pl->dead) {
		mutex_unlock(&pl->lock);
		kfree(ctx);
		return -ENODEV;
	}
	kref_get(&pl->ref);
	ctx->pl = pl;
	ctx->seen = atomic_read(&pl->irq_count);
	file->private_data = ctx;
	mutex_unlock(&pl->lock);
	return 0;
}

static int pl_close_file(struct inode *inode, struct file *file)
{
	struct pl_file *ctx = file->private_data;

	kref_put(&ctx->pl->ref, pl_release_ref);
	kfree(ctx);
	return 0;
}

static ssize_t pl_read_file(struct file *file, char __user *buf,
			    size_t count, loff_t *pos)
{
	struct pl_ctrl *pl = ((struct pl_file *)file->private_data)->pl;
	u32 window[PL_CTRL_WINDOW / sizeof(u32)];
	u32 *values = window;
	unsigned int offset;
	size_t i;
	int ret = 0;

	if (*pos < 0 || (*pos & 3) || (count & 3) ||
	    !pl_read_window(*pos, count))
		return -EINVAL;
	offset = (unsigned int)*pos;
	for (i = 0; i < count; i += 4)
		if (!pl_readable(offset + (unsigned int)i))
			return -EINVAL;
	/* A §4.6 spectrum read can reach 4 * PL_CTRL_SPECTRUM_MAX_WORDS bytes;
	 * stage only that larger transfer on the heap instead of growing the
	 * stack frame every read() caller has to reserve. */
	if (count > sizeof(window)) {
		values = kmalloc(count, GFP_KERNEL);
		if (!values)
			return -ENOMEM;
	}
	mutex_lock(&pl->lock);
	if (pl->dead) {
		ret = -ENODEV;
		goto out;
	}
	if (offset <= PL_CTRL_ALARM_STATUS && offset + count > PL_CTRL_ALARM_STATE &&
	    readl(pl->regs) != PL_CTRL_ID_VALUE) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	for (i = 0; i < count; i += 4)
		values[i / 4] = readl(pl->regs + offset + i);
	if (copy_to_user(buf, values, count))
		ret = -EFAULT;
	else
		*pos += count;
out:
	mutex_unlock(&pl->lock);
	if (values != window)
		kfree(values);
	return ret ? ret : count;
}

static ssize_t pl_write_file(struct file *file, const char __user *buf,
			     size_t count, loff_t *pos)
{
	struct pl_ctrl *pl = ((struct pl_file *)file->private_data)->pl;
	u32 value;
	int ret = 0;

	if (*pos < 0 || *pos >= PL_CTRL_WINDOW || (*pos & 3) || count != 4 ||
	    !pl_writable(*pos))
		return -EINVAL;
	if (copy_from_user(&value, buf, sizeof(value)))
		return -EFAULT;
	mutex_lock(&pl->lock);
	if (pl->dead)
		ret = -ENODEV;
	else if (*pos == PL_CTRL_ALARM_STATE && readl(pl->regs) != PL_CTRL_ID_VALUE)
		ret = -EOPNOTSUPP;
	else {
		writel(value, pl->regs + *pos);
		readl(pl->regs + *pos);
		*pos += count;
	}
	mutex_unlock(&pl->lock);
	return ret ? ret : count;
}

static __poll_t pl_poll(struct file *file, poll_table *wait)
{
	struct pl_file *ctx = file->private_data;
	struct pl_ctrl *pl = ctx->pl;

	poll_wait(file, &pl->wait, wait);
	if (READ_ONCE(pl->dead))
		return EPOLLHUP;
	/* No wired interrupt: report the wait as unavailable instead of hanging. */
	if (!pl->nirq)
		return EPOLLERR;
	return (u32)atomic_read(&pl->irq_count) != READ_ONCE(ctx->seen) ?
		EPOLLIN | EPOLLRDNORM : 0;
}

static long pl_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct pl_file *ctx = file->private_data;
	u32 count;

	if (cmd != PL_CTRL_IRQ_COUNT && cmd != PL_CTRL_DMA_IRQ_COUNT)
		return -ENOTTY;
	if (READ_ONCE(ctx->pl->dead))
		return -ENODEV;
	if (cmd == PL_CTRL_DMA_IRQ_COUNT)
		count = atomic_read(&ctx->pl->dma_irq_count);
	else
		count = atomic_read(&ctx->pl->irq_count);
	if (copy_to_user((void __user *)arg, &count, sizeof(count)))
		return -EFAULT;
	/* Both counters consume the event queue: mark every event up to now as
	 * seen, so the next poll() blocks instead of returning immediately. */
	WRITE_ONCE(ctx->seen, (u32)atomic_read(&ctx->pl->irq_count));
	return 0;
}

static const struct file_operations pl_fops = {
	.owner = THIS_MODULE,
	.open = pl_open_file,
	.release = pl_close_file,
	.read = pl_read_file,
	.write = pl_write_file,
	.llseek = default_llseek,
	.poll = pl_poll,
	.unlocked_ioctl = pl_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = compat_ptr_ioctl,
#endif
};

static irqreturn_t pl_irq(int irq, void *data)
{
	struct pl_ctrl *pl = data;
	bool handled = false;
	u32 dma_irq;

	/* §5 sensor / frame pulse: IRQ[0] is cleared by writing its own bit. */
	if (readl(pl->regs + 0x10) & 1) {
		writel(1, pl->regs + 0x10);
		readl(pl->regs + 0x10);
		handled = true;
	}
	/* §4.4 block-complete: DMA_IRQ[16] is the pending flag and DMA_IRQ[8] is
	 * its W1C clear. Acknowledge it whenever it is set, even when DMA_IRQ[0]
	 * is clear: the enable bit only gates PL-side generation, and a pending
	 * flag left set would keep a level-triggered line asserted and re-enter
	 * this handler forever. */
	dma_irq = readl(pl->regs + PL_CTRL_DMA_IRQ);
	if (dma_irq & PL_CTRL_DMA_IRQ_PENDING) {
		writel(PL_CTRL_DMA_IRQ_CLEAR, pl->regs + PL_CTRL_DMA_IRQ);
		readl(pl->regs + PL_CTRL_DMA_IRQ);
		atomic_inc(&pl->dma_irq_count);
		handled = true;
	}
	if (!handled)
		return IRQ_NONE;
	atomic_inc(&pl->irq_count);
	wake_up_interruptible(&pl->wait);
	return IRQ_HANDLED;
}

static int pl_probe_device(struct platform_device *pdev)
{
	struct resource *res;
	struct pl_ctrl *pl;
	int lines[PL_CTRL_IRQ_MAX];
	int nlines = 0;
	int i, ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || resource_size(res) < PL_CTRL_WINDOW)
		return -EINVAL;
	pl = kzalloc(sizeof(*pl), GFP_KERNEL);
	if (!pl)
		return -ENOMEM;
	kref_init(&pl->ref);
	mutex_init(&pl->lock);
	init_waitqueue_head(&pl->wait);
	atomic_set(&pl->irq_count, 0);
	atomic_set(&pl->dma_irq_count, 0);
	pl->start = res->start;
	pl->size = resource_size(res);
	for (i = 0; i < PL_CTRL_IRQ_MAX; i++) {
		int irq = platform_get_irq_optional(pdev, i);

		if (irq == -EPROBE_DEFER) {
			ret = irq;
			goto fail_ref;
		}
		if (irq < 0)
			break;
		lines[nlines++] = irq;
	}
	if (!nlines) {
		/* IRQ is optional: the sensor registers are usable through read/pread,
		 * the Qt dashboard polls them, and §4.4 block completion is readable
		 * through DMA_BLK_SEQ. Only plctl irq / dma --wait-irq need a line. */
		dev_info(&pdev->dev,
			 "no IRQ (polling-only); §4.4 block IRQ f2p_irq[3] would be GIC %u (SPI %u)\n",
			 PL_CTRL_DMA_IRQ_GIC, PL_CTRL_DMA_IRQ_SPI);
	}
	if (!request_mem_region(pl->start, pl->size, dev_name(&pdev->dev))) {
		ret = -EBUSY;
		goto fail_ref;
	}
	pl->regs = ioremap(pl->start, pl->size);
	if (!pl->regs) {
		ret = -ENOMEM;
		goto fail_region;
	}
	if (readl(pl->regs) != PL_CTRL_ID_VALUE && readl(pl->regs) != PL_CTRL_ID_COMPAT) {
		ret = -ENODEV;
		goto fail_map;
	}
	if (readl(pl->regs) == PL_CTRL_ID_VALUE)
		writel(0, pl->regs + PL_CTRL_ALARM_STATE);
	/* Mask and clear before installing a handler; userspace enables acquisition. */
	writel(0, pl->regs + 0x04);
	writel(1, pl->regs + 0x10);
	readl(pl->regs + 0x10);
	if (nlines) {
		/* Drop a stale §4.4 block-complete pending flag before the handler
		 * can run; the PL clears it through DMA_IRQ[8] (W1C). The write is
		 * only reached when the device tree opted in, because a PL build
		 * without the ring window does not decode 0xac at all. */
		writel(PL_CTRL_DMA_IRQ_CLEAR, pl->regs + PL_CTRL_DMA_IRQ);
		readl(pl->regs + PL_CTRL_DMA_IRQ);
		for (i = 0; i < nlines; i++) {
			ret = request_irq(lines[i], pl_irq, 0, dev_name(&pdev->dev), pl);
			if (ret) {
				/* §5 lists the IRQ numbers as REF: a missing or
				 * misnumbered line must not take the register window
				 * down with it. */
				dev_warn(&pdev->dev,
					 "request_irq(%d) failed: %d; polling this line only\n",
					 lines[i], ret);
				continue;
			}
			pl->irq[pl->nirq++] = lines[i];
		}
	}
	pl->misc.minor = MISC_DYNAMIC_MINOR;
	pl->misc.name = "pl_ctrl";
	pl->misc.fops = &pl_fops;
	pl->misc.parent = &pdev->dev;
	pl->misc.mode = 0600;
	ret = misc_register(&pl->misc);
	if (ret)
		goto fail_irq;
	platform_set_drvdata(pdev, pl);
	if (pl->nirq)
		dev_info(&pdev->dev,
			 "pl_ctrl v1.2 at %pa, %d Linux IRQ line(s), first %d (block IRQ f2p_irq[3] = GIC %u)\n",
			 &pl->start, pl->nirq, pl->irq[0], PL_CTRL_DMA_IRQ_GIC);
	else
		dev_info(&pdev->dev,
			 "pl_ctrl v1.2 at %pa, polling-only (block IRQ at GIC %u once the DT enables it)\n",
			 &pl->start, PL_CTRL_DMA_IRQ_GIC);
	return 0;
fail_irq:
	for (i = 0; i < pl->nirq; i++)
		free_irq(pl->irq[i], pl);
fail_map:
	iounmap(pl->regs);
fail_region:
	release_mem_region(pl->start, pl->size);
fail_ref:
	kref_put(&pl->ref, pl_release_ref);
	return ret;
}

static int pl_remove_device(struct platform_device *pdev)
{
	struct pl_ctrl *pl = platform_get_drvdata(pdev);
	int i;

	mutex_lock(&pl->lock);
	WRITE_ONCE(pl->dead, true);
	if (readl(pl->regs) == PL_CTRL_ID_VALUE)
		writel(0, pl->regs + PL_CTRL_ALARM_STATE);
	writel(0, pl->regs + 0x04);
	readl(pl->regs + 0x04);
	if (pl->nirq) {
		/* Disable and clear the §4.4 block IRQ before the handler goes away:
		 * with a level-triggered line a leftover pending flag would stay
		 * asserted with no handler left to clear it. */
		writel(PL_CTRL_DMA_IRQ_CLEAR, pl->regs + PL_CTRL_DMA_IRQ);
		readl(pl->regs + PL_CTRL_DMA_IRQ);
	}
	mutex_unlock(&pl->lock);
	misc_deregister(&pl->misc);
	for (i = 0; i < pl->nirq; i++)
		free_irq(pl->irq[i], pl);
	wake_up_interruptible(&pl->wait);
	iounmap(pl->regs);
	release_mem_region(pl->start, pl->size);
	kref_put(&pl->ref, pl_release_ref);
	return 0;
}

static const struct of_device_id pl_of_match[] = {
	{ .compatible = "mlk,pl-ctrl-1.0" },
	{ }
};
MODULE_DEVICE_TABLE(of, pl_of_match);

static struct platform_driver pl_driver = {
	.probe = pl_probe_device,
	.remove = pl_remove_device,
	.driver = { .name = "pl_ctrl", .of_match_table = pl_of_match },
};
module_platform_driver(pl_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MLK synchronized acquisition register and IRQ driver");
