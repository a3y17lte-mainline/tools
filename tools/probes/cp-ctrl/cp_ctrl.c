// SPDX-License-Identifier: GPL-2.0
/*
 * Read the Exynos 7870 modem control registers (PMU CP_CTRL_NS/S and friends,
 * secure-only) through the vendor EL3 monitor, two ways: the CP boot service
 * the vendor modem driver uses (SMC 0x82000700, READ_CTRL), from CPU 0 and
 * from another CPU, and the generic SFR read (SMC_CMD_REG).
 * Read-only probe: prints the answers and fails to load, so nothing stays behind.
 */
#include <linux/arm-smccc.h>
#include <linux/cpumask.h>
#include <linux/module.h>
#include <linux/workqueue.h>

#define SMC_CP_BOOT		0x82000700UL
#define SMC_CP_BOOT_READ_CTRL	3
#define SMC_CMD_REG		((unsigned long)(u32)-101)
#define SMC_REG_CLASS_SFR_R	(0x3UL << 30)

static long cp_ctrl_read(void *unused)
{
	static const unsigned long sfr[] = {
		0x10480030, 0x10480034, 0x10480038, 0x10480050, 0x10480070,
	};
	struct arm_smccc_res res;
	int i;

	for (i = 0; i < 2; i++) {
		arm_smccc_smc(SMC_CP_BOOT, SMC_CP_BOOT_READ_CTRL, 0, i, 0, 0, 0, 0,
			      &res);
		pr_info("cp_ctrl: cpu %d READ_CTRL %d: a0 %#lx a1 %#lx a2 %#lx a3 %#lx\n",
			smp_processor_id(), i, res.a0, res.a1, res.a2, res.a3);
	}

	for (i = 0; i < ARRAY_SIZE(sfr); i++) {
		arm_smccc_smc(SMC_CMD_REG, SMC_REG_CLASS_SFR_R | (sfr[i] >> 2),
			      0, 0, 0, 0, 0, 0, &res);
		pr_info("cp_ctrl: cpu %d SFR %#lx: ret %ld val %#010lx\n",
			smp_processor_id(), sfr[i], (long)res.a0,
			res.a2 & 0xffffffff);
	}

	return 0;
}

static int __init cp_ctrl_init(void)
{
	work_on_cpu(0, cp_ctrl_read, NULL);
	if (cpu_online(4))
		work_on_cpu(4, cp_ctrl_read, NULL);

	return -EAGAIN;
}
module_init(cp_ctrl_init);

MODULE_DESCRIPTION("Exynos 7870 modem control register read probe");
MODULE_LICENSE("GPL");
