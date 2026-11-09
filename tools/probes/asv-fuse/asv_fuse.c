// SPDX-License-Identifier: GPL-2.0
/*
 * Read the Exynos 7870 ASV fuse words (0x100d9000, secure-only) through the
 * vendor EL3 monitor's SMC_CMD_REG SFR read, the way the vendor pwrcal does.
 * Read-only probe: prints the words and fails to load, so nothing stays behind.
 */
#include <linux/arm-smccc.h>
#include <linux/module.h>

#define SMC_CMD_REG		((unsigned long)(u32)-101)
#define SMC_REG_CLASS_SFR_R	(0x3UL << 30)
#define ASV_INFO_BASE		0x100d9000UL

static int __init asv_fuse_init(void)
{
	struct arm_smccc_res res;
	int i;

	for (i = 0; i < 8; i++) {
		unsigned long addr = ASV_INFO_BASE + 4 * i;

		arm_smccc_smc(SMC_CMD_REG, SMC_REG_CLASS_SFR_R | (addr >> 2),
			      0, 0, 0, 0, 0, 0, &res);
		pr_info("asv_fuse: %#lx ret %ld val %#010lx\n", addr,
			(long)res.a0, res.a2 & 0xffffffff);
	}
	return -EAGAIN;
}
module_init(asv_fuse_init);
MODULE_DESCRIPTION("Exynos 7870 ASV fuse dump probe");
MODULE_LICENSE("GPL");
