// SPDX-License-Identifier: GPL-2.0
/*
 * Ask the Exynos 7870 EL3 monitor to program the display content firewall
 * (CFW), the way the vendor DECON driver does after every DISPAUD power-on:
 * exynos_smc(MC_FC_SET_CFW_PROT, MC_FC_DRM_SET_CFW_PROT, DECON_CFW_OFFSET, 0),
 * expected return SMC_TZPC_OK (2). One-shot probe: prints the result and fails
 * to load, so nothing stays behind. Call it with the DECON clocks running
 * (display enabled), as the vendor does.
 */
#include <linux/arm-smccc.h>
#include <linux/module.h>

#define MC_FC_SET_CFW_PROT	0x82002040UL
#define MC_FC_DRM_SET_CFW_PROT	0x10000000UL
#define DECON_CFW_OFFSET	3

static int __init cfw_disp_init(void)
{
	struct arm_smccc_res res;

	arm_smccc_smc(MC_FC_SET_CFW_PROT, MC_FC_DRM_SET_CFW_PROT,
		      DECON_CFW_OFFSET, 0, 0, 0, 0, 0, &res);
	pr_info("cfw_disp: SET_CFW_PROT ret %ld (%#lx)\n", (long)res.a0, res.a0);
	return -EAGAIN;
}
module_init(cfw_disp_init);
MODULE_DESCRIPTION("Exynos 7870 display content firewall SMC probe");
MODULE_LICENSE("GPL");
