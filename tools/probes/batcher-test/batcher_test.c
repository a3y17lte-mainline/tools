// SPDX-License-Identifier: GPL-2.0
/*
 * Read-only check of the HSI2C batcher sequence of i2c-exynos5 (batcher mode) on hsi2c0 while
 * the normal driver owns the controller: holds the adapter lock and masks the controller IRQ,
 * reads PMIC registers through the batcher with the driver's own code (copied verbatim by
 * tools/probes/batcher-test/gen), restores the controller registers, prints, fails to load.
 * Only reads: the batcher's write path is never used here.
 */
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/irq.h>
#include <linux/module.h>

static int bus = 5;
module_param(bus, int, 0);
static int irq = -1;
module_param(irq, int, 0);
static int addr = 0x66;
module_param(addr, int, 0);
static int first;
module_param(first, int, 0);
static int count = 16;
module_param(count, int, 0);
static int len = 1;
module_param(len, int, 0);

/* Register Map */
#define HSI2C_CTL		0x00
#define HSI2C_FIFO_CTL		0x04
#define HSI2C_TRAILIG_CTL	0x08
#define HSI2C_CLK_CTL		0x0C
#define HSI2C_CLK_SLOT		0x10
#define HSI2C_INT_ENABLE	0x20
#define HSI2C_INT_STATUS	0x24
#define HSI2C_ERR_STATUS	0x2C
#define HSI2C_FIFO_STATUS	0x30
#define HSI2C_TX_DATA		0x34
#define HSI2C_RX_DATA		0x38
#define HSI2C_CONF		0x40
#define HSI2C_AUTO_CONF		0x44
#define HSI2C_TIMEOUT		0x48
#define HSI2C_MANUAL_CMD	0x4C
#define HSI2C_TRANS_STATUS	0x50
#define HSI2C_TIMING_HS1	0x54
#define HSI2C_TIMING_HS2	0x58
#define HSI2C_TIMING_HS3	0x5C
#define HSI2C_TIMING_FS1	0x60
#define HSI2C_TIMING_FS2	0x64
#define HSI2C_TIMING_FS3	0x68
#define HSI2C_TIMING_SLA	0x6C
#define HSI2C_ADDR		0x70
#define HSI2C_SM_RELEASE	0x200

/* Batcher Register Map */
#define HSI2C_BATCHER_CON		0x500
#define HSI2C_BATCHER_STATE		0x504
#define HSI2C_BATCHER_INT_EN		0x508
#define HSI2C_BATCHER_FIFO_STATUS	0x50C
#define HSI2C_BATCHER_INT_STATUS	0x510
#define HSI2C_BATCHER_OPCODE		0x600
#define HSI2C_BATCHER_PAYLOAD		0x1000

/* I2C_CTL Register bits */
#define HSI2C_FUNC_MODE_I2C			(1u << 0)
#define HSI2C_MASTER				(1u << 3)
#define HSI2C_RXCHON				(1u << 6)
#define HSI2C_TXCHON				(1u << 7)
#define HSI2C_EXT_MSB				(1u << 29)
#define HSI2C_EXT_ADDR				(1u << 30)
#define HSI2C_SW_RST				(1u << 31)

/* I2C_FIFO_CTL Register bits */
#define HSI2C_RXFIFO_EN				(1u << 0)
#define HSI2C_TXFIFO_EN				(1u << 1)
#define HSI2C_RXFIFO_TRIGGER_LEVEL(x)		((x) << 4)
#define HSI2C_TXFIFO_TRIGGER_LEVEL(x)		((x) << 16)

/* I2C_TRAILING_CTL Register bits */
#define HSI2C_TRAILING_COUNT			(0xf)

/* I2C_INT_EN Register bits */
#define HSI2C_INT_TX_ALMOSTEMPTY_EN		(1u << 0)
#define HSI2C_INT_RX_ALMOSTFULL_EN		(1u << 1)
#define HSI2C_INT_TRAILING_EN			(1u << 6)

/* I2C_INT_STAT Register bits */
#define HSI2C_INT_TX_ALMOSTEMPTY		(1u << 0)
#define HSI2C_INT_RX_ALMOSTFULL			(1u << 1)
#define HSI2C_INT_TX_UNDERRUN			(1u << 2)
#define HSI2C_INT_TX_OVERRUN			(1u << 3)
#define HSI2C_INT_RX_UNDERRUN			(1u << 4)
#define HSI2C_INT_RX_OVERRUN			(1u << 5)
#define HSI2C_INT_TRAILING			(1u << 6)
#define HSI2C_INT_I2C				(1u << 9)

#define HSI2C_INT_TRANS_DONE			(1u << 7)
#define HSI2C_INT_TRANS_ABORT			(1u << 8)
#define HSI2C_INT_NO_DEV_ACK			(1u << 9)
#define HSI2C_INT_NO_DEV			(1u << 10)
#define HSI2C_INT_TIMEOUT			(1u << 11)
#define HSI2C_INT_I2C_TRANS			(HSI2C_INT_TRANS_DONE |	\
						HSI2C_INT_TRANS_ABORT |	\
						HSI2C_INT_NO_DEV_ACK |	\
						HSI2C_INT_NO_DEV |	\
						HSI2C_INT_TIMEOUT)

/* I2C_FIFO_STAT Register bits */
#define HSI2C_RX_FIFO_EMPTY			(1u << 24)
#define HSI2C_RX_FIFO_FULL			(1u << 23)
#define HSI2C_RX_FIFO_LVL(x)			((x >> 16) & 0x7f)
#define HSI2C_TX_FIFO_EMPTY			(1u << 8)
#define HSI2C_TX_FIFO_FULL			(1u << 7)
#define HSI2C_TX_FIFO_LVL(x)			((x >> 0) & 0x7f)

/* I2C_CONF Register bits */
#define HSI2C_AUTO_MODE				(1u << 31)
#define HSI2C_10BIT_ADDR_MODE			(1u << 30)
#define HSI2C_HS_MODE				(1u << 29)
#define HSI2C_FILTER_EN_SCL			(1u << 28)
#define HSI2C_FILTER_EN_SDA			(1u << 27)
#define HSI2C_FTL_CYCLE_SCL(x)			((x) << 16)
#define HSI2C_FTL_CYCLE_SDA(x)			((x) << 13)

/* I2C_AUTO_CONF Register bits */
#define HSI2C_READ_WRITE			(1u << 16)
#define HSI2C_STOP_AFTER_TRANS			(1u << 17)
#define HSI2C_MASTER_RUN			(1u << 31)

/* I2C_TIMEOUT Register bits */
#define HSI2C_TIMEOUT_EN			(1u << 31)
#define HSI2C_TIMEOUT_MASK			0xff

/* I2C_MANUAL_CMD register bits */
#define HSI2C_CMD_READ_DATA			(1u << 4)
#define HSI2C_CMD_SEND_STOP			(1u << 2)

/* I2C_TRANS_STATUS register bits */
#define HSI2C_MASTER_BUSY			(1u << 17)
#define HSI2C_SLAVE_BUSY			(1u << 16)

/* I2C_TRANS_STATUS register bits for Exynos5 variant */
#define HSI2C_TIMEOUT_AUTO			(1u << 4)
#define HSI2C_NO_DEV				(1u << 3)
#define HSI2C_NO_DEV_ACK			(1u << 2)
#define HSI2C_TRANS_ABORT			(1u << 1)
#define HSI2C_TRANS_DONE			(1u << 0)

/* I2C_TRANS_STATUS register bits for Exynos7 variant */
#define HSI2C_MASTER_ST_MASK			0xf
#define HSI2C_MASTER_ST_IDLE			0x0
#define HSI2C_MASTER_ST_START			0x1
#define HSI2C_MASTER_ST_RESTART			0x2
#define HSI2C_MASTER_ST_STOP			0x3
#define HSI2C_MASTER_ST_MASTER_ID		0x4
#define HSI2C_MASTER_ST_ADDR0			0x5
#define HSI2C_MASTER_ST_ADDR1			0x6
#define HSI2C_MASTER_ST_ADDR2			0x7
#define HSI2C_MASTER_ST_ADDR_SR			0x8
#define HSI2C_MASTER_ST_READ			0x9
#define HSI2C_MASTER_ST_WRITE			0xa
#define HSI2C_MASTER_ST_NO_ACK			0xb
#define HSI2C_MASTER_ST_LOSE			0xc
#define HSI2C_MASTER_ST_WAIT			0xd
#define HSI2C_MASTER_ST_WAIT_CMD		0xe

/* I2C_ADDR register bits */
#define HSI2C_SLV_ADDR_SLV(x)			((x & 0x3ff) << 0)
#define HSI2C_SLV_ADDR_MAS(x)			((x & 0x3ff) << 10)
#define HSI2C_MASTER_ID(x)			((x & 0xff) << 24)
#define MASTER_ID(x)				((x & 0x7) + 0x08)

/* I2C_BATCHER_CON Register bits */
#define HSI2C_BATCHER_RESET			(1u << 31)
#define HSI2C_BATCHER_START			(1u << 4)
#define HSI2C_BATCHER_DEDICATE			(1u << 1)
#define HSI2C_BATCHER_ENABLE			(1u << 0)

/* I2C_BATCHER_STATE Register bits */
#define HSI2C_BATCHER_OPERATION_COMPLETE	(1u << 0)
#define HSI2C_BATCHER_UNEXPECTED_INTR		(1u << 1)
#define HSI2C_BATCHER_CONFIG_STATE		(1u << 6)
#define HSI2C_BATCHER_CLEAN_INTR_STATE		(1u << 13)

/* I2C_BATCHER_INT_EN Register bits */
#define HSI2C_BATCHER_INT_ENABLE		(1u << 0)

/*
 * The batcher takes a list of register writes for the controller, one opcode
 * byte (the register offset) and one payload word (the value) each, and the
 * received bytes come back in the payload words of the RX_DATA opcodes.
 */
#define HSI2C_BATCHER_OP_END			0xff
#define HSI2C_BATCHER_OP_UNUSED			0xff
#define HSI2C_BATCHER_RX_FILL			0x77
#define HSI2C_BATCHER_MAX_OPS			25
#define HSI2C_BATCHER_SETUP_OPS			15
#define HSI2C_BATCHER_MAX_LEN			\
	(HSI2C_BATCHER_MAX_OPS - HSI2C_BATCHER_SETUP_OPS - 1)
#define HSI2C_BATCHER_TRAILING_COUNT		0x2ff

#define EXYNOS5_I2C_TIMEOUT (msecs_to_jiffies(100))
#define EXYNOS5_BATCHER_RX_TIMEOUT_MS		500
#define EXYNOS5_BATCHER_TX_TIMEOUT_MS		1000


/* One register write of a batcher sequence */
struct exynos5_i2c_batcher_op {
	u8 reg;
	u32 val;
};


static unsigned int exynos5_i2c_batcher_op(struct exynos5_i2c_batcher_op *ops,
					   unsigned int n, u8 reg, u32 val)
{
	ops[n].reg = reg;
	ops[n].val = val;

	return n + 1;
}

/**
 * exynos5_i2c_batcher_build() - make the register list of one transfer
 * @ops: the list, HSI2C_BATCHER_MAX_OPS entries
 * @addr: 7-bit address of the device
 * @reg: for a read, the device register to read from
 * @buf: for a write, the bytes to send
 * @len: number of bytes to send or to receive, at most HSI2C_BATCHER_MAX_LEN
 * @read: read @len bytes from @reg instead of writing @buf
 * @rx_op: for a read, set to the index of the first entry whose payload
 *	   word receives a byte
 *
 * The timing values are those of the vendor kernel for a 2.5 MHz high speed
 * bus, which is what the other user of the controller works with.
 *
 * Return: the number of entries.
 */
static unsigned int
exynos5_i2c_batcher_build(struct exynos5_i2c_batcher_op *ops, u16 addr, u8 reg,
			  const u8 *buf, unsigned int len, bool read,
			  unsigned int *rx_op)
{
	u32 conf, ctl, i2c_addr, auto_conf, int_en;
	unsigned int n = 0;
	unsigned int i;

	n = exynos5_i2c_batcher_op(ops, n, HSI2C_TIMING_HS1, 0x0a0a0a0a);
	n = exynos5_i2c_batcher_op(ops, n, HSI2C_TIMING_HS2, 0x050f0a02);
	n = exynos5_i2c_batcher_op(ops, n, HSI2C_TIMING_HS3, 0x0000000c);
	n = exynos5_i2c_batcher_op(ops, n, HSI2C_TIMING_SLA, 0x00000005);
	n = exynos5_i2c_batcher_op(ops, n, HSI2C_TIMING_FS1, 0x26262600);
	n = exynos5_i2c_batcher_op(ops, n, HSI2C_TIMING_FS2, 0x130f2626);
	n = exynos5_i2c_batcher_op(ops, n, HSI2C_TIMING_FS3, 0x0001004c);
	n = exynos5_i2c_batcher_op(ops, n, HSI2C_TRAILIG_CTL,
				   HSI2C_BATCHER_TRAILING_COUNT);

	conf = HSI2C_AUTO_MODE | HSI2C_HS_MODE | HSI2C_FILTER_EN_SCL |
	       HSI2C_FILTER_EN_SDA | HSI2C_FTL_CYCLE_SCL(3) |
	       HSI2C_FTL_CYCLE_SDA(3) | 0xff;
	ctl = HSI2C_MASTER;
	auto_conf = HSI2C_MASTER_RUN | HSI2C_STOP_AFTER_TRANS | len;

	if (read) {
		/* The register number travels as the low address byte */
		u32 ext_addr = (addr & 0x7f) << 8 | reg;

		conf |= HSI2C_10BIT_ADDR_MODE;
		ctl |= HSI2C_RXCHON | HSI2C_EXT_ADDR;
		if (ext_addr & BIT(14))
			ctl |= HSI2C_EXT_MSB;
		i2c_addr = (ext_addr & 0x3fff) << 10;
		auto_conf |= HSI2C_READ_WRITE;
		int_en = HSI2C_INT_RX_ALMOSTFULL_EN;
	} else {
		ctl |= HSI2C_TXCHON;
		i2c_addr = (addr & 0x7f) << 10;
		int_en = HSI2C_INT_TX_ALMOSTEMPTY_EN | HSI2C_INT_TRANS_DONE;
	}

	n = exynos5_i2c_batcher_op(ops, n, HSI2C_CONF, conf);
	n = exynos5_i2c_batcher_op(ops, n, HSI2C_TIMEOUT, 0);
	n = exynos5_i2c_batcher_op(ops, n, HSI2C_FIFO_CTL,
				   HSI2C_RXFIFO_EN | HSI2C_TXFIFO_EN |
				   HSI2C_TXFIFO_TRIGGER_LEVEL(8) |
				   HSI2C_RXFIFO_TRIGGER_LEVEL(len));
	n = exynos5_i2c_batcher_op(ops, n, HSI2C_CTL, ctl);
	n = exynos5_i2c_batcher_op(ops, n, HSI2C_ADDR, i2c_addr);
	n = exynos5_i2c_batcher_op(ops, n, HSI2C_AUTO_CONF, auto_conf);
	n = exynos5_i2c_batcher_op(ops, n, HSI2C_INT_ENABLE, int_en);

	if (read)
		*rx_op = n;

	for (i = 0; i < len; i++) {
		if (read)
			n = exynos5_i2c_batcher_op(ops, n, HSI2C_RX_DATA,
						   HSI2C_BATCHER_RX_FILL);
		else
			n = exynos5_i2c_batcher_op(ops, n, HSI2C_TX_DATA,
						   buf[i]);
	}

	return exynos5_i2c_batcher_op(ops, n, HSI2C_BATCHER_OP_END, 0);
}

/* Hand the list to the batcher: four opcodes per word, one payload word each */
static void exynos5_i2c_batcher_load(void __iomem *regs,
				     const struct exynos5_i2c_batcher_op *ops,
				     unsigned int n)
{
	u32 opcodes = 0;
	unsigned int i;

	for (i = 0; i < n; i++) {
		unsigned int shift = 8 * (i % 4);

		if (!shift)
			opcodes = ~0u;

		opcodes &= ~(0xffu << shift);
		opcodes |= (u32)ops[i].reg << shift;
		writel(opcodes, regs + HSI2C_BATCHER_OPCODE + (i / 4) * 4);
		writel(ops[i].val, regs + HSI2C_BATCHER_PAYLOAD + i * 4);
	}
}


static const u32 saved_regs[] = {
	HSI2C_CTL, HSI2C_FIFO_CTL, HSI2C_TRAILIG_CTL, HSI2C_CLK_CTL, HSI2C_CLK_SLOT,
	HSI2C_INT_ENABLE, HSI2C_CONF, HSI2C_AUTO_CONF, HSI2C_TIMEOUT, HSI2C_TIMING_HS1,
	HSI2C_TIMING_HS2, HSI2C_TIMING_HS3, HSI2C_TIMING_FS1, HSI2C_TIMING_FS2,
	HSI2C_TIMING_FS3, HSI2C_TIMING_SLA, HSI2C_ADDR,
};

static int batcher_read(void __iomem *regs, u8 reg, u8 *buf, unsigned int n)
{
	struct exynos5_i2c_batcher_op ops[HSI2C_BATCHER_MAX_OPS];
	unsigned int rx_op = 0, cnt, i;
	u32 state;
	int ret;

	cnt = exynos5_i2c_batcher_build(ops, addr, reg, NULL, n, true, &rx_op);
	writel(0, regs + HSI2C_BATCHER_CON);
	writel(HSI2C_BATCHER_ENABLE | HSI2C_BATCHER_DEDICATE, regs + HSI2C_BATCHER_CON);
	writel(readl(regs + HSI2C_BATCHER_INT_STATUS), regs + HSI2C_BATCHER_INT_STATUS);
	exynos5_i2c_batcher_load(regs, ops, cnt);
	writel(HSI2C_BATCHER_INT_ENABLE, regs + HSI2C_BATCHER_INT_EN);
	writel(HSI2C_BATCHER_ENABLE | HSI2C_BATCHER_DEDICATE | HSI2C_BATCHER_START,
	       regs + HSI2C_BATCHER_CON);
	ret = readl_poll_timeout(regs + HSI2C_BATCHER_STATE, state,
				 state & (HSI2C_BATCHER_OPERATION_COMPLETE |
					  HSI2C_BATCHER_UNEXPECTED_INTR), 20, 500000);
	writel(0, regs + HSI2C_BATCHER_INT_EN);
	if (ret || (state & HSI2C_BATCHER_UNEXPECTED_INTR)) {
		pr_info("batcher_test: reg %#x: state %#x fifo %#x int %#x (%s)\n", reg, state,
			readl(regs + HSI2C_BATCHER_FIFO_STATUS),
			readl(regs + HSI2C_BATCHER_INT_STATUS), ret ? "timeout" : "error");
		ret = ret ?: -ENXIO;
	} else {
		for (i = 0; i < n; i++)
			buf[i] = readl(regs + HSI2C_BATCHER_PAYLOAD + (rx_op + i) * 4);
	}
	writel(state | HSI2C_BATCHER_OPERATION_COMPLETE, regs + HSI2C_BATCHER_STATE);
	writel(readl(regs + HSI2C_BATCHER_INT_STATUS), regs + HSI2C_BATCHER_INT_STATUS);
	writel(0, regs + HSI2C_BATCHER_CON);
	return ret;
}

static int __init batcher_test_init(void)
{
	u32 saved[ARRAY_SIZE(saved_regs)];
	struct i2c_adapter *adap;
	struct irq_data *d;
	void __iomem *regs;
	u8 buf[HSI2C_BATCHER_MAX_LEN];
	int i, r, ret, fails = 0;

	if (len < 1 || len > HSI2C_BATCHER_MAX_LEN || count < 1 || first < 0 ||
	    first + count > 256)
		return -EINVAL;
	adap = i2c_get_adapter(bus);
	if (!adap)
		return -ENODEV;
	d = irq_get_irq_data(irq);
	if (!adap->dev.parent || strcmp(dev_name(adap->dev.parent), "10510000.i2c") ||
	    !d || irqd_to_hwirq(d) != 32 + 371) {
		pr_info("batcher_test: bus %d / irq %d is not hsi2c0\n", bus, irq);
		i2c_put_adapter(adap);
		return -EINVAL;
	}
	regs = ioremap(0x10510000, 0x2000);
	if (!regs) {
		i2c_put_adapter(adap);
		return -ENOMEM;
	}

	i2c_lock_bus(adap, I2C_LOCK_ROOT_ADAPTER);
	disable_irq(irq);
	for (i = 0; i < ARRAY_SIZE(saved_regs); i++)
		saved[i] = readl(regs + saved_regs[i]);
	pr_info("batcher_test: before: batcher state %#x con %#x trans %#x\n",
		readl(regs + HSI2C_BATCHER_STATE), readl(regs + HSI2C_BATCHER_CON),
		readl(regs + HSI2C_TRANS_STATUS));

	for (r = first; r < first + count; r += len) {
		unsigned int n = min(len, first + count - r);

		memset(buf, 0xee, sizeof(buf));
		ret = batcher_read(regs, r, buf, n);
		if (ret)
			fails++;
		for (i = 0; i < n && !ret; i++)
			pr_info("batcher_test: %02x:%02x\n", r + i, buf[i]);
		if (fails > 3)
			break;
	}

	/* Hand the controller back the way the driver set it up */
	writel(readl(regs + HSI2C_CTL) | HSI2C_SW_RST, regs + HSI2C_CTL);
	writel(readl(regs + HSI2C_CTL) & ~HSI2C_SW_RST, regs + HSI2C_CTL);
	for (i = 0; i < ARRAY_SIZE(saved_regs); i++)
		writel(saved[i], regs + saved_regs[i]);
	writel(readl(regs + HSI2C_INT_STATUS), regs + HSI2C_INT_STATUS);
	pr_info("batcher_test: after: batcher state %#x con %#x trans %#x, %d failed\n",
		readl(regs + HSI2C_BATCHER_STATE), readl(regs + HSI2C_BATCHER_CON),
		readl(regs + HSI2C_TRANS_STATUS), fails);
	enable_irq(irq);
	i2c_unlock_bus(adap, I2C_LOCK_ROOT_ADAPTER);

	iounmap(regs);
	i2c_put_adapter(adap);
	return -EAGAIN;
}
module_init(batcher_test_init);

MODULE_DESCRIPTION("HSI2C batcher read check");
MODULE_LICENSE("GPL");
