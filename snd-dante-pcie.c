// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/debugfs.h>
#include <linux/miscdevice.h>
#include <linux/seq_file.h>
#include <linux/msi.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/initval.h>

#define DRIVER_NAME    "snd-dante-pcie"
#define CARD_NAME      "DantePCIe"
#define CARD_LONGNAME  "Dante PCIe"

#define PCI_VENDOR_GENNUM   0x1a39
#define PCI_DEVICE_GN4124   0x0004

#define FPGA_REG_CTRL       0x0004
#define FPGA_EVENT_SET      0x0008
#define FPGA_EVENT_CLR      0x000c
#define FPGA_EVENT          0x0010
#define FPGA_EVENT_EN       0x0014
#define FPGA_DPTR           0x0020
#define FPGA_CSR            0x0030
#define FPGA_FW_VERSION     0x0070
#define FPGA_SAMPLE_CNT_LO  0x0074
#define FPGA_SAMPLE_CNT_HI  0x0078

#define DRAM_PRIMARY        0x4000
#define DRAM_SHADOW         0x6000

#define GN4124_INT_CTRL     0x0810
#define GN4124_INT_STAT     0x0814
#define GN4124_INT_CFG_BASE 0x0820
#define GN4124_GPIO_BYPASS  0x0a00

#define INT_STAT_INT1       BIT(5)
#define EVENT_TX_EN         BIT(2)
#define EVENT_RX_EN         BIT(3)

#define CHUNK_FRAMES        16
#define CHUNK_BYTES         (CHUNK_FRAMES * 4)

#define MAX_BUF_BYTES       (512 * 128 * CHUNK_BYTES)

#define DANTE_IOC_MAGIC     'D'
#define DANTE_GET_INFO      _IOR(DANTE_IOC_MAGIC, 0, struct dante_card_info)

struct dante_card_info {
	u32 firmware_version;
	u32 sample_rate;
	u32 rx_channels;
	u32 tx_channels;
	u32 period_size;
	u32 chunks_per_channel;
	u32 channel_step;
	u32 dma_running;
	u64 sample_count;
	u64 rx_buffer_phys;
	u64 tx_buffer_phys;
	u32 rx_buffer_size;
	u32 tx_buffer_size;
};

static int index = SNDRV_DEFAULT_IDX1;
static char *id = SNDRV_DEFAULT_STR1;
module_param(index, int, 0444);
module_param(id, charp, 0444);

struct dante_pcie {
	struct pci_dev *pdev;
	struct snd_card *card;
	void __iomem *bar0;
	void __iomem *bar4;
	unsigned long bar0_len;
	unsigned long bar4_len;
	unsigned int sample_rate;
	unsigned int max_channels;
	u32 fw_version;
	int msi_vector_idx;
	struct snd_pcm_substream *pb_sub;
	struct snd_pcm_substream *cap_sub;
	spinlock_t lock;
	bool pb_running;
	bool cap_running;
	struct dentry *dbgfs;
	struct miscdevice miscdev;
	char miscdev_name[32];

	void *rx_area;
	dma_addr_t rx_addr;
	unsigned int rx_channels;
	unsigned int rx_stride;
	unsigned int rx_total;

	void *tx_area;
	dma_addr_t tx_addr;
	unsigned int tx_channels;
	unsigned int tx_stride;
	unsigned int tx_total;

	unsigned int cur_period_size;
	unsigned int cur_chunks;
};

static const struct {
	unsigned int rate;
	unsigned int max_ch;
} rate_table[] = {
	[0] = { 192000, 64 },
	[1] = { 96000, 128 },
	[2] = { 48000, 128 },
	[3] = { 0, 0 },
	[4] = { 176400, 64 },
	[5] = { 88200, 128 },
	[6] = { 44100, 128 },
};

static inline void dram_write(struct dante_pcie *d, u32 idx, u32 val)
{
	iowrite32(val, d->bar0 + DRAM_PRIMARY + idx * 4);
	if (idx < 0x30)
		iowrite32(val, d->bar0 + DRAM_SHADOW + idx * 4);
}

static inline u32 dram_read(struct dante_pcie *d, u32 idx)
{
	return ioread32(d->bar0 + DRAM_PRIMARY + idx * 4);
}

static const u32 vdma_microcode[] = {
	/* ENTRY: reload period countdown from word 0x08 */
	0x20000008, 0xa2000009,
	/* Buffer wrap check runs here, before WAIT — app note 53715 s4.1.1.2 */
	0x20000018, 0x10000038,
	0x20000010, 0xa2000018,
	0x40000014, 0x5000001a,
	0x20000019, 0x1000003e,
	0x20000011, 0xa2000019,
	0x40000016, 0x5000001c,
	/* VDMA_WAIT_EVENT(0x001, 0x001): vdma_seqcode.h lines 139-143, EVENT_EN=bit0 EVENT_STATE=bit0 */
	0x90001001, 0x80000001, 0x80000002,
	0x20000006,
	0x1800004e, 0x4000001a,
	0xf0000003, 0xe0000012,
	0x21000002, 0x10000044,
	0x20000018, 0x21000002, 0xa2000018,
	0x4000001a, 0xe0000005, 0x5000001a,
	0x20000007,
	0x1800005b, 0x4000001c,
	0xf0000004, 0xe0000013,
	0x21000002, 0x10000051,
	0x20000019, 0x21000002, 0xa2000019,
	0x4000001c, 0xe0000005, 0x5000001c,
	0x20000009, 0x21000002, 0xa2000009,
	0x10000032,
	0x84000002,
	0x1a000030,
};

static void vdma_upload_code(struct dante_pcie *d)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(vdma_microcode); i++)
		iowrite32(vdma_microcode[i],
			  d->bar0 + DRAM_PRIMARY + (0x30 + i) * 4);
}

static void vdma_init_data(struct dante_pcie *d)
{
	unsigned int i;

	dram_write(d, 0x00, 0x1a000030);
	dram_write(d, 0x01, 0x00000001);
	dram_write(d, 0x02, 0xffffffff);
	dram_write(d, 0x03, 0x00000000);
	dram_write(d, 0x04, 0x00000000);
	dram_write(d, 0x05, CHUNK_BYTES);
	dram_write(d, 0x06, 0);
	dram_write(d, 0x07, 0);
	dram_write(d, 0x08, 32);
	dram_write(d, 0x09, 0);
	for (i = 0x0a; i <= 0x0f; i++)
		dram_write(d, i, 0);
	for (i = 0x10; i <= 0x2f; i++)
		dram_write(d, i, 0);
}

static int setup_msi_quirk(struct dante_pcie *d)
{
	struct pci_dev *pdev = d->pdev;
	int msi_cap;
	u16 msi_flags, msi_data;
	int data_off;

	msi_cap = pci_find_capability(pdev, PCI_CAP_ID_MSI);
	if (!msi_cap)
		return -ENODEV;

	pci_read_config_word(pdev, msi_cap + PCI_MSI_FLAGS, &msi_flags);
	msi_flags = (msi_flags & ~PCI_MSI_FLAGS_QSIZE) | (2 << 4);
	pci_write_config_word(pdev, msi_cap + PCI_MSI_FLAGS, msi_flags);

	__msi_lock_descs(&pdev->dev);
	{
		struct msi_desc *desc;

		desc = msi_first_desc(&pdev->dev, MSI_DESC_ASSOCIATED);
		if (desc)
			desc->pci.msi_attrib.multiple = 2;
	}
	__msi_unlock_descs(&pdev->dev);

	data_off = (msi_flags & PCI_MSI_FLAGS_64BIT) ?
		   PCI_MSI_DATA_64 : PCI_MSI_DATA_32;
	pci_read_config_word(pdev, msi_cap + data_off, &msi_data);
	d->msi_vector_idx = msi_data & 0x3;

	return 0;
}

static void bridge_init(struct dante_pcie *d)
{
	iowrite32(0x00000002, d->bar4 + GN4124_INT_CTRL);
	iowrite32(0x00000002, d->bar4 + GN4124_GPIO_BYPASS);
	iowrite32(INT_STAT_INT1,
		  d->bar4 + GN4124_INT_CFG_BASE + d->msi_vector_idx * 4);

	/* REF 52624 p131: bits 0-1 RO, bits 2-3 write-1-to-assert SWI, bits 4-14 write-1-to-clear */
	iowrite32(0x7FF0, d->bar4 + GN4124_INT_STAT);
}

static irqreturn_t dante_irq(int irq, void *dev_id)
{
	struct dante_pcie *d = dev_id;
	u32 stat;

	stat = ioread32(d->bar4 + GN4124_INT_STAT);
	if (!(stat & INT_STAT_INT1))
		return IRQ_NONE;

	iowrite32(INT_STAT_INT1, d->bar4 + GN4124_INT_STAT);

	if (READ_ONCE(d->pb_running) && d->pb_sub)
		snd_pcm_period_elapsed(d->pb_sub);
	if (READ_ONCE(d->cap_running) && d->cap_sub)
		snd_pcm_period_elapsed(d->cap_sub);

	return IRQ_HANDLED;
}

static const struct snd_pcm_hardware dante_hw = {
	.info = SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_NONINTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S32_LE,
	.channels_min = 1,
	.channels_max = 128,
	.period_bytes_min = CHUNK_BYTES,
	.period_bytes_max = MAX_BUF_BYTES / 2,
	.periods_min = 2,
	.periods_max = 512,
	.buffer_bytes_max = MAX_BUF_BYTES,
};

static int dante_open(struct snd_pcm_substream *sub)
{
	struct dante_pcie *d = snd_pcm_substream_chip(sub);
	struct snd_pcm_runtime *rt = sub->runtime;

	rt->hw = dante_hw;
	rt->hw.channels_max = d->max_channels;
	rt->hw.rates = snd_pcm_rate_to_rate_bit(d->sample_rate);
	if (!rt->hw.rates)
		rt->hw.rates = SNDRV_PCM_RATE_KNOT;
	rt->hw.rate_min = d->sample_rate;
	rt->hw.rate_max = d->sample_rate;

	snd_pcm_hw_constraint_step(rt, 0, SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
				   CHUNK_FRAMES);
	snd_pcm_hw_constraint_step(rt, 0, SNDRV_PCM_HW_PARAM_BUFFER_SIZE,
				   CHUNK_FRAMES);

	return 0;
}

static int dante_close(struct snd_pcm_substream *sub)
{
	struct dante_pcie *d = snd_pcm_substream_chip(sub);
	unsigned long flags;

	spin_lock_irqsave(&d->lock, flags);
	if (sub->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		d->pb_sub = NULL;
		d->pb_running = false;
		d->tx_area = NULL;
		d->tx_addr = 0;
		d->tx_channels = 0;
		d->tx_stride = 0;
		d->tx_total = 0;
	} else {
		d->cap_sub = NULL;
		d->cap_running = false;
		d->rx_area = NULL;
		d->rx_addr = 0;
		d->rx_channels = 0;
		d->rx_stride = 0;
		d->rx_total = 0;
	}
	spin_unlock_irqrestore(&d->lock, flags);
	return 0;
}

static int dante_prepare(struct snd_pcm_substream *sub)
{
	struct dante_pcie *d = snd_pcm_substream_chip(sub);
	struct snd_pcm_runtime *rt = sub->runtime;
	unsigned int channels = rt->channels;
	unsigned int buf_frames = rt->buffer_size;
	unsigned int period_frames = rt->period_size;
	unsigned int chunks = buf_frames / CHUNK_FRAMES;
	unsigned int stride = chunks * CHUNK_BYTES;
	unsigned int periods_per_irq = period_frames / CHUNK_FRAMES;
	dma_addr_t addr = rt->dma_addr;
	unsigned long flags;

	spin_lock_irqsave(&d->lock, flags);

	iowrite32(0, d->bar0 + FPGA_CSR);

	/* XFER_CTL: app note 53715 s4.1.1, CNT=0x040 (64 bytes), bit 13=direction, bit 16=enable */
	dram_write(d, 0x03, 0x00012040);
	dram_write(d, 0x04, 0x00010040);

	if (sub->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		iowrite32(EVENT_TX_EN, d->bar0 + FPGA_EVENT_CLR);
		d->pb_sub = sub;

		d->tx_area = rt->dma_area;
		d->tx_addr = addr;
		d->tx_channels = channels;
		d->tx_stride = stride;
		d->tx_total = channels * stride;

		dram_write(d, 0x06, channels);
		dram_write(d, 0x10, chunks);
		dram_write(d, 0x12, stride);
		dram_write(d, 0x14, lower_32_bits(addr));
		dram_write(d, 0x15, upper_32_bits(addr));
		dram_write(d, 0x18, chunks);
		dram_write(d, 0x1a, lower_32_bits(addr));
		dram_write(d, 0x1b, upper_32_bits(addr));
	} else {
		iowrite32(EVENT_RX_EN, d->bar0 + FPGA_EVENT_CLR);
		d->cap_sub = sub;

		d->rx_area = rt->dma_area;
		d->rx_addr = addr;
		d->rx_channels = channels;
		d->rx_stride = stride;
		d->rx_total = channels * stride;

		dram_write(d, 0x07, channels);
		dram_write(d, 0x11, chunks);
		dram_write(d, 0x13, stride);
		dram_write(d, 0x16, lower_32_bits(addr));
		dram_write(d, 0x17, upper_32_bits(addr));
		dram_write(d, 0x19, chunks);
		dram_write(d, 0x1c, lower_32_bits(addr));
		dram_write(d, 0x1d, upper_32_bits(addr));
	}

	d->cur_period_size = period_frames;
	d->cur_chunks = chunks;
	dram_write(d, 0x08, periods_per_irq);
	/* countdown must equal periods_per_irq, not 0; 0 wraps to 0xFFFFFFFF on first decrement */
	dram_write(d, 0x09, periods_per_irq);

	iowrite32(0x30, d->bar0 + FPGA_DPTR);
	iowrite32(1, d->bar0 + FPGA_CSR);

	spin_unlock_irqrestore(&d->lock, flags);
	return 0;
}

static int dante_trigger(struct snd_pcm_substream *sub, int cmd)
{
	struct dante_pcie *d = snd_pcm_substream_chip(sub);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		if (sub->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			WRITE_ONCE(d->pb_running, true);
			iowrite32(EVENT_TX_EN, d->bar0 + FPGA_EVENT_SET);
		} else {
			WRITE_ONCE(d->cap_running, true);
			iowrite32(EVENT_RX_EN, d->bar0 + FPGA_EVENT_SET);
		}
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		if (sub->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			iowrite32(EVENT_TX_EN, d->bar0 + FPGA_EVENT_CLR);
			WRITE_ONCE(d->pb_running, false);
		} else {
			iowrite32(EVENT_RX_EN, d->bar0 + FPGA_EVENT_CLR);
			WRITE_ONCE(d->cap_running, false);
		}
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static snd_pcm_uframes_t dante_pointer(struct snd_pcm_substream *sub)
{
	struct dante_pcie *d = snd_pcm_substream_chip(sub);
	u32 cur_lo, start_lo;
	unsigned int byte_off;

	if (sub->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		cur_lo = dram_read(d, 0x1a);
		start_lo = dram_read(d, 0x14);
	} else {
		cur_lo = dram_read(d, 0x1c);
		start_lo = dram_read(d, 0x16);
	}

	byte_off = cur_lo - start_lo;
	return (byte_off / 4) % sub->runtime->buffer_size;
}

static const struct snd_pcm_ops dante_pcm_ops = {
	.open      = dante_open,
	.close     = dante_close,
	.prepare   = dante_prepare,
	.trigger   = dante_trigger,
	.pointer   = dante_pointer,
};

struct dante_dbgfs_bar {
	void __iomem *base;
	unsigned long len;
};

static ssize_t dbgfs_bar_read(struct file *f, char __user *buf,
			      size_t count, loff_t *ppos)
{
	struct dante_dbgfs_bar *bar = f->private_data;
	u32 val;

	if (*ppos >= bar->len || (*ppos & 3) || count < 4)
		return 0;

	val = ioread32(bar->base + *ppos);
	if (copy_to_user(buf, &val, 4))
		return -EFAULT;

	*ppos += 4;
	return 4;
}

static int dbgfs_bar_open(struct inode *inode, struct file *f)
{
	f->private_data = inode->i_private;
	return 0;
}

static const struct file_operations dbgfs_bar_fops = {
	.owner  = THIS_MODULE,
	.open   = dbgfs_bar_open,
	.read   = dbgfs_bar_read,
	.llseek = default_llseek,
};

struct dante_dbgfs_audio {
	struct dante_pcie *d;
	int is_tx;
};

static ssize_t dbgfs_audio_read(struct file *f, char __user *buf,
				size_t count, loff_t *ppos)
{
	struct dante_dbgfs_audio *a = f->private_data;
	struct dante_pcie *d = a->d;
	void *area;
	unsigned int total;
	ssize_t avail, to_copy;

	if (a->is_tx) {
		area = READ_ONCE(d->tx_area);
		total = READ_ONCE(d->tx_total);
	} else {
		area = READ_ONCE(d->rx_area);
		total = READ_ONCE(d->rx_total);
	}

	if (!area || !total || *ppos >= total)
		return 0;

	avail = total - *ppos;
	to_copy = min_t(ssize_t, count, avail);

	if (copy_to_user(buf, area + *ppos, to_copy))
		return -EFAULT;

	*ppos += to_copy;
	return to_copy;
}

static int dbgfs_audio_open(struct inode *inode, struct file *f)
{
	f->private_data = inode->i_private;
	return 0;
}

static const struct file_operations dbgfs_audio_fops = {
	.owner  = THIS_MODULE,
	.open   = dbgfs_audio_open,
	.read   = dbgfs_audio_read,
	.llseek = default_llseek,
};

static u32 compute_peak(const s32 *samples, unsigned int count)
{
	u32 peak = 0;
	unsigned int i;

	for (i = 0; i < count; i++) {
		s32 s = samples[i];
		u32 a = (s < 0) ? (u32)(-s) : (u32)s;
		if (a > peak)
			peak = a;
	}
	return peak;
}

static void format_bar(char *out, u32 peak)
{
	int bits = fls(peak);
	int filled = (bits + 3) / 4;
	int i;

	if (filled > 8)
		filled = 8;
	for (i = 0; i < 8; i++)
		out[i] = (i < filled) ? '#' : '.';
	out[8] = '\0';
}

static int meters_show(struct seq_file *s, void *v)
{
	struct dante_pcie *d = s->private;
	void *area;
	unsigned int channels, stride, ch;
	char bar[9];

	area = READ_ONCE(d->rx_area);
	channels = READ_ONCE(d->rx_channels);
	stride = READ_ONCE(d->rx_stride);

	seq_puts(s, "RX (capture):\n");
	if (area && channels && stride) {
		unsigned int samples_per_ch = stride / 4;

		for (ch = 0; ch < channels; ch++) {
			const s32 *base = (const s32 *)(area + ch * stride);
			u32 peak = compute_peak(base, samples_per_ch);

			format_bar(bar, peak);
			seq_printf(s, "%4u: %s %08x\n", ch, bar, peak);
		}
	} else {
		seq_puts(s, "  (no active stream)\n");
	}

	seq_puts(s, "TX (playback):\n");
	area = READ_ONCE(d->tx_area);
	channels = READ_ONCE(d->tx_channels);
	stride = READ_ONCE(d->tx_stride);

	if (area && channels && stride) {
		unsigned int samples_per_ch = stride / 4;

		for (ch = 0; ch < channels; ch++) {
			const s32 *base = (const s32 *)(area + ch * stride);
			u32 peak = compute_peak(base, samples_per_ch);

			format_bar(bar, peak);
			seq_printf(s, "%4u: %s %08x\n", ch, bar, peak);
		}
	} else {
		seq_puts(s, "  (no active stream)\n");
	}

	return 0;
}

static int meters_open(struct inode *inode, struct file *f)
{
	return single_open(f, meters_show, inode->i_private);
}

static const struct file_operations dbgfs_meters_fops = {
	.owner   = THIS_MODULE,
	.open    = meters_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

static int dante_cdev_open(struct inode *inode, struct file *f)
{
	struct miscdevice *misc = f->private_data;
	struct dante_pcie *d = container_of(misc, struct dante_pcie, miscdev);

	f->private_data = d;
	return 0;
}

static long dante_cdev_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct dante_pcie *d = f->private_data;
	struct dante_card_info info;
	u32 event;

	if (cmd != DANTE_GET_INFO)
		return -ENOTTY;

	memset(&info, 0, sizeof(info));
	info.firmware_version = d->fw_version;
	info.sample_rate = d->sample_rate;
	info.rx_channels = READ_ONCE(d->rx_channels);
	info.tx_channels = READ_ONCE(d->tx_channels);
	info.period_size = READ_ONCE(d->cur_period_size);
	info.chunks_per_channel = READ_ONCE(d->cur_chunks);
	info.channel_step = info.chunks_per_channel * CHUNK_BYTES;

	event = ioread32(d->bar0 + FPGA_EVENT);
	info.dma_running = 0;
	if (event & EVENT_TX_EN)
		info.dma_running |= 1;
	if (event & EVENT_RX_EN)
		info.dma_running |= 2;

	info.sample_count = (u64)ioread32(d->bar0 + FPGA_SAMPLE_CNT_LO) |
			    ((u64)ioread32(d->bar0 + FPGA_SAMPLE_CNT_HI) << 32);

	info.rx_buffer_phys = READ_ONCE(d->rx_addr);
	info.tx_buffer_phys = READ_ONCE(d->tx_addr);
	info.rx_buffer_size = READ_ONCE(d->rx_total);
	info.tx_buffer_size = READ_ONCE(d->tx_total);

	if (copy_to_user((void __user *)arg, &info, sizeof(info)))
		return -EFAULT;

	return 0;
}

static int dante_cdev_mmap(struct file *f, struct vm_area_struct *vma)
{
	struct dante_pcie *d = f->private_data;
	unsigned long offset = vma->vm_pgoff;
	unsigned long size = vma->vm_end - vma->vm_start;
	dma_addr_t phys;
	unsigned int buf_size;

	if (offset == 0) {
		phys = READ_ONCE(d->rx_addr);
		buf_size = READ_ONCE(d->rx_total);
	} else if (offset == 1) {
		phys = READ_ONCE(d->tx_addr);
		buf_size = READ_ONCE(d->tx_total);
	} else {
		return -EINVAL;
	}

	if (!phys || !buf_size || size > buf_size)
		return -EINVAL;

	vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
	return remap_pfn_range(vma, vma->vm_start,
			       phys >> PAGE_SHIFT, size,
			       vma->vm_page_prot);
}

static const struct file_operations dante_cdev_fops = {
	.owner          = THIS_MODULE,
	.open           = dante_cdev_open,
	.unlocked_ioctl = dante_cdev_ioctl,
	.compat_ioctl   = dante_cdev_ioctl,
	.mmap           = dante_cdev_mmap,
};

static void dante_debugfs_init(struct dante_pcie *d)
{
	struct dante_dbgfs_bar *b0, *b4;
	struct dante_dbgfs_audio *arx, *atx;

	d->dbgfs = debugfs_create_dir("dante-pcie", NULL);

	b0 = devm_kzalloc(&d->pdev->dev, sizeof(*b0), GFP_KERNEL);
	b4 = devm_kzalloc(&d->pdev->dev, sizeof(*b4), GFP_KERNEL);
	if (!b0 || !b4)
		return;

	b0->base = d->bar0;
	b0->len  = d->bar0_len;
	b4->base = d->bar4;
	b4->len  = d->bar4_len;

	debugfs_create_file_size("bar0", 0444, d->dbgfs, b0,
				 &dbgfs_bar_fops, d->bar0_len);
	debugfs_create_file_size("bar4", 0444, d->dbgfs, b4,
				 &dbgfs_bar_fops, d->bar4_len);

	arx = devm_kzalloc(&d->pdev->dev, sizeof(*arx), GFP_KERNEL);
	atx = devm_kzalloc(&d->pdev->dev, sizeof(*atx), GFP_KERNEL);
	if (!arx || !atx)
		return;

	arx->d = d;
	arx->is_tx = 0;
	atx->d = d;
	atx->is_tx = 1;

	debugfs_create_file("audio_rx", 0444, d->dbgfs, arx,
			    &dbgfs_audio_fops);
	debugfs_create_file("audio_tx", 0444, d->dbgfs, atx,
			    &dbgfs_audio_fops);

	debugfs_create_file("meters", 0444, d->dbgfs, d,
			    &dbgfs_meters_fops);
}

static int dante_cdev_register(struct dante_pcie *d)
{
	snprintf(d->miscdev_name, sizeof(d->miscdev_name),
		 "dante-pcie");
	d->miscdev.minor = MISC_DYNAMIC_MINOR;
	d->miscdev.name  = d->miscdev_name;
	d->miscdev.fops  = &dante_cdev_fops;
	d->miscdev.mode  = 0666;

	return misc_register(&d->miscdev);
}

static int dante_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct dante_pcie *d;
	struct snd_card *card;
	struct snd_pcm *pcm;
	unsigned int rate_idx;
	int err;

	err = pcim_enable_device(pdev);
	if (err)
		return err;

	pci_set_master(pdev);

	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (err) {
		err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		if (err)
			return err;
	}

	err = pcim_iomap_regions(pdev, BIT(0) | BIT(4), DRIVER_NAME);
	if (err)
		return err;

	err = snd_devm_card_new(&pdev->dev, index, CARD_NAME,
				THIS_MODULE, sizeof(*d), &card);
	if (err)
		return err;

	d = card->private_data;
	d->pdev = pdev;
	d->card = card;
	spin_lock_init(&d->lock);
	pci_set_drvdata(pdev, d);

	d->bar0 = pcim_iomap_table(pdev)[0];
	d->bar4 = pcim_iomap_table(pdev)[4];
	if (!d->bar0 || !d->bar4)
		return -ENOMEM;

	d->bar0_len = pci_resource_len(pdev, 0);
	d->bar4_len = pci_resource_len(pdev, 4);

	err = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI | PCI_IRQ_INTX);
	if (err < 0)
		return err;

	err = setup_msi_quirk(d);
	if (err)
		dev_warn(&pdev->dev, "MSI quirk setup failed, using default routing\n");

	bridge_init(d);

	d->fw_version = ioread32(d->bar0 + FPGA_FW_VERSION);
	rate_idx = (d->fw_version >> 28) & 0x7;
	if (rate_idx >= ARRAY_SIZE(rate_table) || !rate_table[rate_idx].rate) {
		dev_err(&pdev->dev, "unknown rate index %u in firmware 0x%08x\n",
			rate_idx, d->fw_version);
		return -ENODEV;
	}
	d->sample_rate = rate_table[rate_idx].rate;
	d->max_channels = rate_table[rate_idx].max_ch;
	dev_info(&pdev->dev, "firmware 0x%08x, rate %u Hz, max %u channels\n",
		 d->fw_version, d->sample_rate, d->max_channels);

	iowrite32(0, d->bar0 + FPGA_CSR);
	msleep(1);
	iowrite32(0x0000ffff, d->bar0 + FPGA_EVENT_CLR);
	iowrite32(0x00000007, d->bar0 + FPGA_REG_CTRL);
	iowrite32(0x0000ffff, d->bar0 + FPGA_EVENT_EN);

	vdma_upload_code(d);
	vdma_init_data(d);
	iowrite32(0, d->bar0 + FPGA_CSR);

	err = devm_request_irq(&pdev->dev, pci_irq_vector(pdev, 0),
			       dante_irq, IRQF_SHARED, CARD_NAME, d);
	if (err)
		return err;

	strscpy(card->driver, CARD_NAME, sizeof(card->driver));
	strscpy(card->shortname, CARD_NAME, sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname),
		 "%s at %s", CARD_LONGNAME, pci_name(pdev));

	err = snd_pcm_new(card, CARD_NAME " PCM", 0, 1, 1, &pcm);
	if (err)
		return err;

	pcm->private_data = d;
	strscpy(pcm->name, CARD_NAME " PCM", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &dante_pcm_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &dante_pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_DEV,
				       &pdev->dev, MAX_BUF_BYTES, MAX_BUF_BYTES);

	dante_debugfs_init(d);

	err = dante_cdev_register(d);
	if (err)
		dev_warn(&pdev->dev, "failed to register chardev: %d\n", err);

	err = snd_card_register(card);
	if (err)
		return err;

	return 0;
}

static void dante_remove(struct pci_dev *pdev)
{
	struct dante_pcie *d = pci_get_drvdata(pdev);

	if (d) {
		misc_deregister(&d->miscdev);

		iowrite32(EVENT_TX_EN | EVENT_RX_EN, d->bar0 + FPGA_EVENT_CLR);
		iowrite32(0, d->bar0 + FPGA_CSR);

		iowrite32(0, d->bar4 + GN4124_INT_CFG_BASE +
			  d->msi_vector_idx * 4);
		iowrite32(0x7FF0, d->bar4 + GN4124_INT_STAT);

		debugfs_remove_recursive(d->dbgfs);
	}
}

static const struct pci_device_id dante_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_GENNUM, PCI_DEVICE_GN4124) },
	{ }
};
MODULE_DEVICE_TABLE(pci, dante_ids);

static struct pci_driver dante_driver = {
	.name    = DRIVER_NAME,
	.id_table = dante_ids,
	.probe   = dante_probe,
	.remove  = dante_remove,
};
module_pci_driver(dante_driver);

MODULE_DESCRIPTION("ALSA driver for Dante PCIe audio cards");
MODULE_AUTHOR("Christopher Ritsen");
MODULE_LICENSE("GPL");
