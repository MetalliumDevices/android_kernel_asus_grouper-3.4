/*
 * arch/arm/mach-tegra/tegra11_clocks.c
 *
 * Copyright (C) 2011-2012 NVIDIA Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/cpufreq.h>

#include <asm/clkdev.h>

#include <mach/iomap.h>
#include <mach/edp.h>
#include <mach/hardware.h>

#include "clock.h"
#include "fuse.h"
#include "dvfs.h"
#include "pm.h"
#include "sleep.h"
#include "tegra11_emc.h"
#include "tegra_cl_dvfs.h"

#define RST_DEVICES_L			0x004
#define RST_DEVICES_H			0x008
#define RST_DEVICES_U			0x00C
#define RST_DEVICES_V			0x358
#define RST_DEVICES_W			0x35C
#define RST_DEVICES_X			0x28C
#define RST_DEVICES_SET_L		0x300
#define RST_DEVICES_CLR_L		0x304
#define RST_DEVICES_SET_V		0x430
#define RST_DEVICES_CLR_V		0x434
#define RST_DEVICES_SET_X		0x290
#define RST_DEVICES_CLR_X		0x294
#define RST_DEVICES_NUM			6

#define CLK_OUT_ENB_L			0x010
#define CLK_OUT_ENB_H			0x014
#define CLK_OUT_ENB_U			0x018
#define CLK_OUT_ENB_V			0x360
#define CLK_OUT_ENB_W			0x364
#define CLK_OUT_ENB_X			0x280
#define CLK_OUT_ENB_SET_L		0x320
#define CLK_OUT_ENB_CLR_L		0x324
#define CLK_OUT_ENB_SET_V		0x440
#define CLK_OUT_ENB_CLR_V		0x444
#define CLK_OUT_ENB_SET_X		0x284
#define CLK_OUT_ENB_CLR_X		0x288
#define CLK_OUT_ENB_NUM			6

#define RST_DEVICES_V_SWR_CPULP_RST_DIS	(0x1 << 1)
#define CLK_OUT_ENB_V_CLK_ENB_CPULP_EN	(0x1 << 1)

#define PERIPH_CLK_TO_BIT(c)		(1 << (c->u.periph.clk_num % 32))
#define PERIPH_CLK_TO_RST_REG(c)	\
	periph_clk_to_reg((c), RST_DEVICES_L, RST_DEVICES_V, RST_DEVICES_X, 4)
#define PERIPH_CLK_TO_RST_SET_REG(c)	\
	periph_clk_to_reg((c), RST_DEVICES_SET_L, RST_DEVICES_SET_V, \
		RST_DEVICES_SET_X, 8)
#define PERIPH_CLK_TO_RST_CLR_REG(c)	\
	periph_clk_to_reg((c), RST_DEVICES_CLR_L, RST_DEVICES_CLR_V, \
		RST_DEVICES_CLR_X, 8)

#define PERIPH_CLK_TO_ENB_REG(c)	\
	periph_clk_to_reg((c), CLK_OUT_ENB_L, CLK_OUT_ENB_V, CLK_OUT_ENB_X, 4)
#define PERIPH_CLK_TO_ENB_SET_REG(c)	\
	periph_clk_to_reg((c), CLK_OUT_ENB_SET_L, CLK_OUT_ENB_SET_V, \
		CLK_OUT_ENB_SET_X, 8)
#define PERIPH_CLK_TO_ENB_CLR_REG(c)	\
	periph_clk_to_reg((c), CLK_OUT_ENB_CLR_L, CLK_OUT_ENB_CLR_V, \
		CLK_OUT_ENB_CLR_X, 8)

#define CLK_MASK_ARM			0x44
#define MISC_CLK_ENB			0x48

#define OSC_CTRL			0x50
#define OSC_CTRL_OSC_FREQ_MASK		(0xF<<28)
#define OSC_CTRL_OSC_FREQ_13MHZ		(0x0<<28)
#define OSC_CTRL_OSC_FREQ_19_2MHZ	(0x4<<28)
#define OSC_CTRL_OSC_FREQ_12MHZ		(0x8<<28)
#define OSC_CTRL_OSC_FREQ_26MHZ		(0xC<<28)
#define OSC_CTRL_OSC_FREQ_16_8MHZ	(0x1<<28)
#define OSC_CTRL_OSC_FREQ_38_4MHZ	(0x5<<28)
#define OSC_CTRL_OSC_FREQ_48MHZ		(0x9<<28)
#define OSC_CTRL_MASK			(0x3f2 | OSC_CTRL_OSC_FREQ_MASK)

#define OSC_CTRL_PLL_REF_DIV_MASK	(3<<26)
#define OSC_CTRL_PLL_REF_DIV_1		(0<<26)
#define OSC_CTRL_PLL_REF_DIV_2		(1<<26)
#define OSC_CTRL_PLL_REF_DIV_4		(2<<26)

#define PERIPH_CLK_SOURCE_I2S1		0x100
#define PERIPH_CLK_SOURCE_EMC		0x19c
#define PERIPH_CLK_SOURCE_OSC		0x1fc
#define PERIPH_CLK_SOURCE_NUM1 \
	((PERIPH_CLK_SOURCE_OSC - PERIPH_CLK_SOURCE_I2S1) / 4)

#define PERIPH_CLK_SOURCE_G3D2		0x3b0
#define PERIPH_CLK_SOURCE_SE		0x42c
#define PERIPH_CLK_SOURCE_NUM2 \
	((PERIPH_CLK_SOURCE_SE - PERIPH_CLK_SOURCE_G3D2) / 4 + 1)

#define AUDIO_DLY_CLK			0x49c
#define AUDIO_SYNC_CLK_SPDIF		0x4b4
#define PERIPH_CLK_SOURCE_NUM3 \
	((AUDIO_SYNC_CLK_SPDIF - AUDIO_DLY_CLK) / 4 + 1)

#define PERIPH_CLK_SOURCE_XUSB		0x4cc
#define PERIPH_CLK_SOURCE_DDS_UART	0x4f8
#define PERIPH_CLK_SOURCE_NUM4 \
	((PERIPH_CLK_SOURCE_XUSB - PERIPH_CLK_SOURCE_DDS_UART) / 4 + 1)

#define PERIPH_CLK_SOURCE_NUM		(PERIPH_CLK_SOURCE_NUM1 + \
					 PERIPH_CLK_SOURCE_NUM2 + \
					 PERIPH_CLK_SOURCE_NUM3 + \
					 PERIPH_CLK_SOURCE_NUM4)

#define CPU_SOFTRST_CTRL		0x380

#define PERIPH_CLK_SOURCE_DIVU71_MASK	0xFF
#define PERIPH_CLK_SOURCE_DIVU16_MASK	0xFFFF
#define PERIPH_CLK_SOURCE_DIV_SHIFT	0
#define PERIPH_CLK_SOURCE_DIVIDLE_SHIFT	8
#define PERIPH_CLK_SOURCE_DIVIDLE_VAL	50
#define PERIPH_CLK_UART_DIV_ENB		(1<<24)
#define PERIPH_CLK_VI_SEL_EX_SHIFT	24
#define PERIPH_CLK_VI_SEL_EX_MASK	(0x3<<PERIPH_CLK_VI_SEL_EX_SHIFT)
#define PERIPH_CLK_NAND_DIV_EX_ENB	(1<<8)
#define PERIPH_CLK_DTV_POLARITY_INV	(1<<25)

#define AUDIO_SYNC_SOURCE_MASK		0x0F
#define AUDIO_SYNC_DISABLE_BIT		0x10
#define AUDIO_SYNC_TAP_NIBBLE_SHIFT(c)	((c->reg_shift - 24) * 4)

/* PLL common */
#define PLL_BASE			0x0
#define PLL_BASE_BYPASS			(1<<31)
#define PLL_BASE_ENABLE			(1<<30)
#define PLL_BASE_REF_ENABLE		(1<<29)
#define PLL_BASE_OVERRIDE		(1<<28)
#define PLL_BASE_LOCK			(1<<27)
#define PLL_BASE_DIVP_MASK		(0x7<<20)
#define PLL_BASE_DIVP_SHIFT		20
#define PLL_BASE_DIVN_MASK		(0x3FF<<8)
#define PLL_BASE_DIVN_SHIFT		8
#define PLL_BASE_DIVM_MASK		(0x1F)
#define PLL_BASE_DIVM_SHIFT		0

#define PLL_BASE_PARSE(pll, cfg, b)					       \
	do {								       \
		(cfg).m = ((b) & pll##_BASE_DIVM_MASK) >> PLL_BASE_DIVM_SHIFT; \
		(cfg).n = ((b) & pll##_BASE_DIVN_MASK) >> PLL_BASE_DIVN_SHIFT; \
		(cfg).p = ((b) & pll##_BASE_DIVP_MASK) >> PLL_BASE_DIVP_SHIFT; \
	} while (0)

#define PLL_OUT_RATIO_MASK		(0xFF<<8)
#define PLL_OUT_RATIO_SHIFT		8
#define PLL_OUT_OVERRIDE		(1<<2)
#define PLL_OUT_CLKEN			(1<<1)
#define PLL_OUT_RESET_DISABLE		(1<<0)

#define PLL_MISC(c)			\
	(((c)->flags & PLL_ALT_MISC_REG) ? 0x4 : 0xc)
#define PLL_MISCN(c, n)		\
	((c)->u.pll.misc1 + ((n) - 1) * PLL_MISC(c))
#define PLL_MISC_LOCK_ENABLE(c)	\
	(((c)->flags & (PLLU | PLLD)) ? (1<<22) : (1<<18))

#define PLL_MISC_DCCON_SHIFT		20
#define PLL_MISC_CPCON_SHIFT		8
#define PLL_MISC_CPCON_MASK		(0xF<<PLL_MISC_CPCON_SHIFT)
#define PLL_MISC_LFCON_SHIFT		4
#define PLL_MISC_LFCON_MASK		(0xF<<PLL_MISC_LFCON_SHIFT)
#define PLL_MISC_VCOCON_SHIFT		0
#define PLL_MISC_VCOCON_MASK		(0xF<<PLL_MISC_VCOCON_SHIFT)

#define PLL_FIXED_MDIV(c, ref)		((ref) > (c)->u.pll.cf_max ? 2 : 1)

/* PLLU */
#define PLLU_BASE_POST_DIV		(1<<20)

/* PLLD */
#define PLLD_BASE_CSI_CLKENABLE		(1<<26)
#define PLLD_BASE_DSI_MUX_SHIFT		25
#define PLLD_BASE_DSI_MUX_MASK		(1<<PLLD_BASE_DSI_MUX_SHIFT)
#define PLLD_BASE_CSI_CLKSOURCE		(1<<24)

#define PLLD_MISC_DSI_CLKENABLE		(1<<30)
#define PLLD_MISC_DIV_RST		(1<<23)
#define PLLD_MISC_DCCON_SHIFT		12

#define PLLDU_LFCON_SET_DIVN		600

/* PLLC2 and PLLC3 (PLLCX) */
#define PLLCX_USE_DYN_RAMP		0
#define PLLCX_BASE_PHASE_LOCK		(1<<26)
#define PLLCX_BASE_DIVP_MASK		(0x7<<PLL_BASE_DIVP_SHIFT)
#define PLLCX_BASE_DIVN_MASK		(0xFF<<PLL_BASE_DIVN_SHIFT)
#define PLLCX_BASE_DIVM_MASK		(0x3<<PLL_BASE_DIVM_SHIFT)
#define PLLCX_PDIV_MAX	((PLLCX_BASE_DIVP_MASK >> PLL_BASE_DIVP_SHIFT))
#define PLLCX_IS_DYN(new_p, old_p)	(((new_p) <= 8) && ((old_p) <= 8))

#define PLLCX_MISC_STROBE		(1<<31)
#define PLLCX_MISC_RESET		(1<<30)
#define PLLCX_MISC_SDM_DIV_SHIFT	28
#define PLLCX_MISC_SDM_DIV_MASK		(0x3 << PLLCX_MISC_SDM_DIV_SHIFT)
#define PLLCX_MISC_FILT_DIV_SHIFT	26
#define PLLCX_MISC_FILT_DIV_MASK	(0x3 << PLLCX_MISC_FILT_DIV_SHIFT)
#define PLLCX_MISC_ALPHA_SHIFT		18
#define PLLCX_MISC_ALPHA_MASK		(0xFF << PLLCX_MISC_ALPHA_SHIFT)
#define PLLCX_MISC_KB_SHIFT		9
#define PLLCX_MISC_KB_MASK		(0x1FF << PLLCX_MISC_KB_SHIFT)
#define PLLCX_MISC_KA_SHIFT		2
#define PLLCX_MISC_KA_MASK		(0x7F << PLLCX_MISC_KA_SHIFT)
#define PLLCX_MISC_VCO_GAIN_SHIFT	0
#define PLLCX_MISC_VCO_GAIN_MASK	(0x3 << PLLCX_MISC_VCO_GAIN_SHIFT)

#define PLLCX_MISC_KOEF_LOW_RANGE	\
	((0x14 << PLLCX_MISC_KA_SHIFT) | (0x38 << PLLCX_MISC_KB_SHIFT))
#define PLLCX_MISC_KOEF_HIGH_RANGE	\
	((0x10 << PLLCX_MISC_KA_SHIFT) | (0xA0 << PLLCX_MISC_KB_SHIFT))

#define PLLCX_MISC_DEFAULT_VALUE	((0x0 << PLLCX_MISC_VCO_GAIN_SHIFT) | \
					PLLCX_MISC_KOEF_LOW_RANGE | \
					(0x19 << PLLCX_MISC_ALPHA_SHIFT) | \
					(0x2 << PLLCX_MISC_SDM_DIV_SHIFT) | \
					(0x2 << PLLCX_MISC_FILT_DIV_SHIFT) | \
					PLLCX_MISC_RESET)
#define PLLCX_MISC1_DEFAULT_VALUE	0x00132308
#define PLLCX_MISC2_DEFAULT_VALUE	0x11111100
#define PLLCX_MISC3_DEFAULT_VALUE	0x0

/* PLLX and PLLC (PLLXC)*/
#define PLLXC_USE_DYN_RAMP		0
#define PLLXC_BASE_DIVP_MASK		(0xF<<PLL_BASE_DIVP_SHIFT)
#define PLLXC_BASE_DIVN_MASK		(0xFF<<PLL_BASE_DIVN_SHIFT)
#define PLLXC_BASE_DIVM_MASK		(0xFF<<PLL_BASE_DIVM_SHIFT)

/* PLLXC has 4-bit PDIV, but entry 15 is not allowed in h/w,
   and s/w usage is limited to 5 */
#define PLLXC_PDIV_MAX			14
#define PLLXC_SW_PDIV_MAX		5

/* PLLX */
#define PLLX_MISC2_DYNRAMP_STEPB_SHIFT	24
#define PLLX_MISC2_DYNRAMP_STEPB_MASK	(0xFF << PLLX_MISC2_DYNRAMP_STEPB_SHIFT)
#define PLLX_MISC2_DYNRAMP_STEPA_SHIFT	16
#define PLLX_MISC2_DYNRAMP_STEPA_MASK	(0xFF << PLLX_MISC2_DYNRAMP_STEPA_SHIFT)
#define PLLX_MISC2_NDIV_NEW_SHIFT	8
#define PLLX_MISC2_NDIV_NEW_MASK	(0xFF << PLLX_MISC2_NDIV_NEW_SHIFT)
#define PLLX_MISC2_LOCK_OVERRIDE	(0x1 << 4)
#define PLLX_MISC2_DYNRAMP_DONE		(0x1 << 2)
#define PLLX_MISC2_CLAMP_NDIV		(0x1 << 1)
#define PLLX_MISC2_EN_DYNRAMP		(0x1 << 0)

#define PLLX_MISC3_IDDQ			(0x1 << 3)

#define PLLX_HW_CTRL_CFG		0x548
#define PLLX_HW_CTRL_CFG_SWCTRL		(0x1 << 0)

/* PLLC */
#define PLLC_BASE_LOCK_OVERRIDE		(1<<28)

#define PLLC_MISC_IDDQ			(0x1 << 26)
#define PLLC_MISC_LOCK_ENABLE		(0x1 << 24)

#define PLLC_MISC1_CLAMP_NDIV		(0x1 << 26)
#define PLLC_MISC1_EN_DYNRAMP		(0x1 << 25)
#define PLLC_MISC1_DYNRAMP_STEPA_SHIFT	17
#define PLLC_MISC1_DYNRAMP_STEPA_MASK	(0xFF << PLLC_MISC1_DYNRAMP_STEPA_SHIFT)
#define PLLC_MISC1_DYNRAMP_STEPB_SHIFT	9
#define PLLC_MISC1_DYNRAMP_STEPB_MASK	(0xFF << PLLC_MISC1_DYNRAMP_STEPB_SHIFT)
#define PLLC_MISC1_NDIV_NEW_SHIFT	1
#define PLLC_MISC1_NDIV_NEW_MASK	(0xFF << PLLC_MISC1_NDIV_NEW_SHIFT)
#define PLLC_MISC1_DYNRAMP_DONE		(0x1 << 0)

/* PLLM */
#define PLLM_BASE_DIVP_MASK		(0x1 << PLL_BASE_DIVP_SHIFT)
#define PLLM_BASE_DIVN_MASK		(0xFF << PLL_BASE_DIVN_SHIFT)
#define PLLM_BASE_DIVM_MASK		(0xFF << PLL_BASE_DIVM_SHIFT)
#define PLLM_PDIV_MAX			1

#define PLLM_MISC_FSM_SW_OVERRIDE	(0x1 << 10)
#define PLLM_MISC_IDDQ			(0x1 << 5)
#define PLLM_MISC_LOCK_DISABLE		(0x1 << 4)
#define PLLM_MISC_LOCK_OVERRIDE		(0x1 << 3)

#define PMC_PLLP_WB0_OVERRIDE			0xf8
#define PMC_PLLP_WB0_OVERRIDE_PLLM_ENABLE	(1 << 12)
#define PMC_PLLP_WB0_OVERRIDE_PLLM_OVERRIDE	(1 << 11)

/* M, N layout for PLLM override and base registers are the same */
#define PMC_PLLM_WB0_OVERRIDE			0x1dc

#define PMC_PLLM_WB0_OVERRIDE_2			0x2b0
#define PMC_PLLM_WB0_OVERRIDE_2_DIVP_MASK	(0x1 << 27)

/* PLLRE */
#define PLLRE_BASE_DIVP_SHIFT		16
#define PLLRE_BASE_DIVP_MASK		(0xF << PLLRE_BASE_DIVP_SHIFT)
#define PLLRE_BASE_DIVN_MASK		(0xFF << PLL_BASE_DIVN_SHIFT)
#define PLLRE_BASE_DIVM_MASK		(0xFF << PLL_BASE_DIVM_SHIFT)

/* PLLRE has 4-bit PDIV, but entry 15 is not allowed in h/w,
   and s/w usage is limited to 5 */
#define PLLRE_PDIV_MAX			14
#define PLLRE_SW_PDIV_MAX		5

#define PLLRE_MISC_LOCK_ENABLE		(0x1 << 30)
#define PLLRE_MISC_LOCK_OVERRIDE	(0x1 << 29)
#define PLLRE_MISC_LOCK			(0x1 << 24)
#define PLLRE_MISC_IDDQ			(0x1 << 16)

/* FIXME: OUT_OF_TABLE_CPCON per pll */
#define OUT_OF_TABLE_CPCON		0x8

#define SUPER_CLK_MUX			0x00
#define SUPER_STATE_SHIFT		28
#define SUPER_STATE_MASK		(0xF << SUPER_STATE_SHIFT)
#define SUPER_STATE_STANDBY		(0x0 << SUPER_STATE_SHIFT)
#define SUPER_STATE_IDLE		(0x1 << SUPER_STATE_SHIFT)
#define SUPER_STATE_RUN			(0x2 << SUPER_STATE_SHIFT)
#define SUPER_STATE_IRQ			(0x3 << SUPER_STATE_SHIFT)
#define SUPER_STATE_FIQ			(0x4 << SUPER_STATE_SHIFT)
#define SUPER_LP_DIV2_BYPASS		(0x1 << 16)
#define SUPER_SOURCE_MASK		0xF
#define	SUPER_FIQ_SOURCE_SHIFT		12
#define	SUPER_IRQ_SOURCE_SHIFT		8
#define	SUPER_RUN_SOURCE_SHIFT		4
#define	SUPER_IDLE_SOURCE_SHIFT		0

#define SUPER_CLK_DIVIDER		0x04
#define SUPER_CLOCK_DIV_U71_SHIFT	16
#define SUPER_CLOCK_DIV_U71_MASK	(0xff << SUPER_CLOCK_DIV_U71_SHIFT)

#define BUS_CLK_DISABLE			(1<<3)
#define BUS_CLK_DIV_MASK		0x3

#define PMC_CTRL			0x0
 #define PMC_CTRL_BLINK_ENB		(1 << 7)

#define PMC_DPD_PADS_ORIDE		0x1c
 #define PMC_DPD_PADS_ORIDE_BLINK_ENB	(1 << 20)

#define PMC_BLINK_TIMER_DATA_ON_SHIFT	0
#define PMC_BLINK_TIMER_DATA_ON_MASK	0x7fff
#define PMC_BLINK_TIMER_ENB		(1 << 15)
#define PMC_BLINK_TIMER_DATA_OFF_SHIFT	16
#define PMC_BLINK_TIMER_DATA_OFF_MASK	0xffff

#define UTMIP_PLL_CFG2					0x488
#define UTMIP_PLL_CFG2_STABLE_COUNT(x)			(((x) & 0xfff) << 6)
#define UTMIP_PLL_CFG2_ACTIVE_DLY_COUNT(x)		(((x) & 0x3f) << 18)
#define UTMIP_PLL_CFG2_FORCE_PD_SAMP_A_POWERDOWN	(1 << 0)
#define UTMIP_PLL_CFG2_FORCE_PD_SAMP_B_POWERDOWN	(1 << 2)
#define UTMIP_PLL_CFG2_FORCE_PD_SAMP_C_POWERDOWN	(1 << 4)

#define UTMIP_PLL_CFG1					0x484
#define UTMIP_PLL_CFG1_ENABLE_DLY_COUNT(x)		(((x) & 0x1f) << 27)
#define UTMIP_PLL_CFG1_XTAL_FREQ_COUNT(x)		(((x) & 0xfff) << 0)
#define UTMIP_PLL_CFG1_FORCE_PLL_ENABLE_POWERDOWN	(1 << 14)
#define UTMIP_PLL_CFG1_FORCE_PLL_ACTIVE_POWERDOWN	(1 << 12)
#define UTMIP_PLL_CFG1_FORCE_PLLU_POWERDOWN		(1 << 16)

#define PLLE_BASE_CML_ENABLE		(1<<31)
#define PLLE_BASE_ENABLE		(1<<30)
#define PLLE_BASE_DIVCML_SHIFT		24
#define PLLE_BASE_DIVCML_MASK		(0xf<<PLLE_BASE_DIVCML_SHIFT)
#define PLLE_BASE_DIVP_SHIFT		16
#define PLLE_BASE_DIVP_MASK		(0x3f<<PLLE_BASE_DIVP_SHIFT)
#define PLLE_BASE_DIVN_SHIFT		8
#define PLLE_BASE_DIVN_MASK		(0xFF<<PLLE_BASE_DIVN_SHIFT)
#define PLLE_BASE_DIVM_SHIFT		0
#define PLLE_BASE_DIVM_MASK		(0xFF<<PLLE_BASE_DIVM_SHIFT)
#define PLLE_BASE_DIV_MASK		\
	(PLLE_BASE_DIVCML_MASK | PLLE_BASE_DIVP_MASK | \
	 PLLE_BASE_DIVN_MASK | PLLE_BASE_DIVM_MASK)
#define PLLE_BASE_DIV(m, n, p, cml)		\
	 (((cml)<<PLLE_BASE_DIVCML_SHIFT) | ((p)<<PLLE_BASE_DIVP_SHIFT) | \
	  ((n)<<PLLE_BASE_DIVN_SHIFT) | ((m)<<PLLE_BASE_DIVM_SHIFT))

#define PLLE_MISC_SETUP_BASE_SHIFT	16
#define PLLE_MISC_SETUP_BASE_MASK	(0xFFFF<<PLLE_MISC_SETUP_BASE_SHIFT)
#define PLLE_MISC_READY			(1<<15)
#define PLLE_MISC_LOCK			(1<<11)
#define PLLE_MISC_LOCK_ENABLE		(1<<9)
#define PLLE_MISC_SETUP_EX_SHIFT	2
#define PLLE_MISC_SETUP_EX_MASK		(0x3<<PLLE_MISC_SETUP_EX_SHIFT)
#define PLLE_MISC_SETUP_MASK		\
	  (PLLE_MISC_SETUP_BASE_MASK | PLLE_MISC_SETUP_EX_MASK)
#define PLLE_MISC_SETUP_VALUE		\
	  ((0x7<<PLLE_MISC_SETUP_BASE_SHIFT) | (0x0<<PLLE_MISC_SETUP_EX_SHIFT))

#define PLLE_SS_CTRL			0x68
#define	PLLE_SS_INCINTRV_SHIFT		24
#define	PLLE_SS_INCINTRV_MASK		(0x3f<<PLLE_SS_INCINTRV_SHIFT)
#define	PLLE_SS_INC_SHIFT		16
#define	PLLE_SS_INC_MASK		(0xff<<PLLE_SS_INC_SHIFT)
#define	PLLE_SS_MAX_SHIFT		0
#define	PLLE_SS_MAX_MASK		(0x1ff<<PLLE_SS_MAX_SHIFT)
#define PLLE_SS_COEFFICIENTS_MASK	\
	(PLLE_SS_INCINTRV_MASK | PLLE_SS_INC_MASK | PLLE_SS_MAX_MASK)
#define PLLE_SS_COEFFICIENTS_12MHZ	\
	((0x18<<PLLE_SS_INCINTRV_SHIFT) | (0x1<<PLLE_SS_INC_SHIFT) | \
	 (0x24<<PLLE_SS_MAX_SHIFT))
#define PLLE_SS_DISABLE			((1<<12) | (1<<11) | (1<<10))

#define PLLE_AUX			0x48c
#define PLLE_AUX_PLLP_SEL		(1<<2)
#define PLLE_AUX_CML_SATA_ENABLE	(1<<1)
#define PLLE_AUX_CML_PCIE_ENABLE	(1<<0)

#define	PMC_SATA_PWRGT			0x1ac
#define PMC_SATA_PWRGT_PLLE_IDDQ_VALUE	(1<<5)
#define PMC_SATA_PWRGT_PLLE_IDDQ_SWCTL	(1<<4)

#define ROUND_DIVIDER_UP	0
#define ROUND_DIVIDER_DOWN	1

/* PLLP default fixed rate in h/w controlled mode */
#define PLLP_DEFAULT_FIXED_RATE		216000000

static bool tegra11_is_dyn_ramp(struct clk *c,
				unsigned long rate, bool from_vco_min);
static void tegra11_pllp_init_dependencies(unsigned long pllp_rate);
static int tegra11_clk_shared_bus_update(struct clk *bus);

static bool detach_shared_bus;
module_param(detach_shared_bus, bool, 0644);

static bool use_dfll;
module_param(use_dfll, bool, 0644);

/**
* Structure defining the fields for USB UTMI clocks Parameters.
*/
struct utmi_clk_param
{
	/* Oscillator Frequency in KHz */
	u32 osc_frequency;
	/* UTMIP PLL Enable Delay Count  */
	u8 enable_delay_count;
	/* UTMIP PLL Stable count */
	u8 stable_count;
	/*  UTMIP PLL Active delay count */
	u8 active_delay_count;
	/* UTMIP PLL Xtal frequency count */
	u8 xtal_freq_count;
};

static const struct utmi_clk_param utmi_parameters[] =
{
/*	OSC_FREQUENCY,	ENABLE_DLY,	STABLE_CNT,	ACTIVE_DLY,	XTAL_FREQ_CNT */
	{13000000,	0x02,		0x33,		0x05,		0x7F},
	{19200000,	0x03,		0x4B,		0x06,		0xBB},
	{12000000,	0x02,		0x2F,		0x04,		0x76},
	{26000000,	0x04,		0x66,		0x09,		0xFE},
	{16800000,	0x03,		0x41,		0x0A,		0xA4},
};

static void __iomem *reg_clk_base = IO_ADDRESS(TEGRA_CLK_RESET_BASE);
static void __iomem *reg_pmc_base = IO_ADDRESS(TEGRA_PMC_BASE);
static void __iomem *misc_gp_hidrev_base = IO_ADDRESS(TEGRA_APB_MISC_BASE);

#define MISC_GP_HIDREV                  0x804

/*
 * Some peripheral clocks share an enable bit, so refcount the enable bits
 * in registers CLK_ENABLE_L, ... CLK_ENABLE_W, and protect refcount updates
 * with lock
 */
static DEFINE_SPINLOCK(periph_refcount_lock);
static int tegra_periph_clk_enable_refcount[CLK_OUT_ENB_NUM * 32];

#define clk_writel(value, reg) \
	__raw_writel(value, (u32)reg_clk_base + (reg))
#define clk_readl(reg) \
	__raw_readl((u32)reg_clk_base + (reg))
#define pmc_writel(value, reg) \
	__raw_writel(value, (u32)reg_pmc_base + (reg))
#define pmc_readl(reg) \
	__raw_readl((u32)reg_pmc_base + (reg))
#define chipid_readl() \
	__raw_readl((u32)misc_gp_hidrev_base + MISC_GP_HIDREV)

#define clk_writel_delay(value, reg) 					\
	do {								\
		__raw_writel((value), (u32)reg_clk_base + (reg));	\
		udelay(2);						\
	} while (0)

#define pll_writel_delay(value, reg)					\
	do {								\
		__raw_writel((value), (u32)reg_clk_base + (reg));	\
		udelay(1);						\
	} while (0)


static inline int clk_set_div(struct clk *c, u32 n)
{
	return clk_set_rate(c, (clk_get_rate(c->parent) + n-1) / n);
}

static inline u32 periph_clk_to_reg(
	struct clk *c, u32 reg_L, u32 reg_V, u32 reg_X, int offs)
{
	u32 reg = c->u.periph.clk_num / 32;
	BUG_ON(reg >= RST_DEVICES_NUM);
	if (reg < 3)
		reg = reg_L + (reg * offs);
	else if (reg < 5)
		reg = reg_V + ((reg - 3) * offs);
	else
		reg = reg_X;
	return reg;
}

static int clk_div_x1_get_divider(unsigned long parent_rate, unsigned long rate,
			u32 max_x,
				 u32 flags, u32 round_mode)
{
	s64 divider_ux1 = parent_rate;
	if (!rate)
		return -EINVAL;

	if (!(flags & DIV_U71_INT))
		divider_ux1 *= 2;
	if (round_mode == ROUND_DIVIDER_UP)
		divider_ux1 += rate - 1;
	do_div(divider_ux1, rate);
	if (flags & DIV_U71_INT)
		divider_ux1 *= 2;

	if (divider_ux1 - 2 < 0)
		return 0;

	if (divider_ux1 - 2 > max_x)
		return -EINVAL;

	return divider_ux1 - 2;
}

static int clk_div71_get_divider(unsigned long parent_rate, unsigned long rate,
				 u32 flags, u32 round_mode)
{
	return clk_div_x1_get_divider(parent_rate, rate, 0xFF,
			flags, round_mode);
}

static int clk_div151_get_divider(unsigned long parent_rate, unsigned long rate,
				 u32 flags, u32 round_mode)
{
	return clk_div_x1_get_divider(parent_rate, rate, 0xFFFF,
			flags, round_mode);
}

static int clk_div16_get_divider(unsigned long parent_rate, unsigned long rate)
{
	s64 divider_u16;

	divider_u16 = parent_rate;
	if (!rate)
		return -EINVAL;
	divider_u16 += rate - 1;
	do_div(divider_u16, rate);

	if (divider_u16 - 1 < 0)
		return 0;

	if (divider_u16 - 1 > 0xFFFF)
		return -EINVAL;

	return divider_u16 - 1;
}

/* clk_m functions */
static unsigned long tegra11_clk_m_autodetect_rate(struct clk *c)
{
	u32 osc_ctrl = clk_readl(OSC_CTRL);
	u32 auto_clock_control = osc_ctrl & ~OSC_CTRL_OSC_FREQ_MASK;
	u32 pll_ref_div = osc_ctrl & OSC_CTRL_PLL_REF_DIV_MASK;

	c->rate = tegra_clk_measure_input_freq();
	switch (c->rate) {
	case 12000000:
		auto_clock_control |= OSC_CTRL_OSC_FREQ_12MHZ;
		BUG_ON(pll_ref_div != OSC_CTRL_PLL_REF_DIV_1);
		break;
	case 13000000:
		auto_clock_control |= OSC_CTRL_OSC_FREQ_13MHZ;
		BUG_ON(pll_ref_div != OSC_CTRL_PLL_REF_DIV_1);
		break;
	case 19200000:
		auto_clock_control |= OSC_CTRL_OSC_FREQ_19_2MHZ;
		BUG_ON(pll_ref_div != OSC_CTRL_PLL_REF_DIV_1);
		break;
	case 26000000:
		auto_clock_control |= OSC_CTRL_OSC_FREQ_26MHZ;
		BUG_ON(pll_ref_div != OSC_CTRL_PLL_REF_DIV_1);
		break;
	case 16800000:
		auto_clock_control |= OSC_CTRL_OSC_FREQ_16_8MHZ;
		BUG_ON(pll_ref_div != OSC_CTRL_PLL_REF_DIV_1);
		break;
	case 38400000:
		auto_clock_control |= OSC_CTRL_OSC_FREQ_38_4MHZ;
		BUG_ON(pll_ref_div != OSC_CTRL_PLL_REF_DIV_2);
		break;
	case 48000000:
		auto_clock_control |= OSC_CTRL_OSC_FREQ_48MHZ;
		BUG_ON(pll_ref_div != OSC_CTRL_PLL_REF_DIV_4);
		break;
	case 115200:	/* fake 13M for QT */
	case 230400:	/* fake 13M for QT */
		auto_clock_control |= OSC_CTRL_OSC_FREQ_13MHZ;
		c->rate = 13000000;
		BUG_ON(pll_ref_div != OSC_CTRL_PLL_REF_DIV_1);
		break;
	default:
		pr_err("%s: Unexpected clock rate %ld", __func__, c->rate);
		BUG();
	}
	clk_writel(auto_clock_control, OSC_CTRL);
	return c->rate;
}

static void tegra11_clk_m_init(struct clk *c)
{
	pr_debug("%s on clock %s\n", __func__, c->name);
	tegra11_clk_m_autodetect_rate(c);
}

static int tegra11_clk_m_enable(struct clk *c)
{
	pr_debug("%s on clock %s\n", __func__, c->name);
	return 0;
}

static void tegra11_clk_m_disable(struct clk *c)
{
	pr_debug("%s on clock %s\n", __func__, c->name);
	WARN(1, "Attempting to disable main SoC clock\n");
}

static struct clk_ops tegra_clk_m_ops = {
	.init		= tegra11_clk_m_init,
	.enable		= tegra11_clk_m_enable,
	.disable	= tegra11_clk_m_disable,
};

static struct clk_ops tegra_clk_m_div_ops = {
	.enable		= tegra11_clk_m_enable,
};

/* PLL reference divider functions */
static void tegra11_pll_ref_init(struct clk *c)
{
	u32 pll_ref_div = clk_readl(OSC_CTRL) & OSC_CTRL_PLL_REF_DIV_MASK;
	pr_debug("%s on clock %s\n", __func__, c->name);

	switch (pll_ref_div) {
	case OSC_CTRL_PLL_REF_DIV_1:
		c->div = 1;
		break;
	case OSC_CTRL_PLL_REF_DIV_2:
		c->div = 2;
		break;
	case OSC_CTRL_PLL_REF_DIV_4:
		c->div = 4;
		break;
	default:
		pr_err("%s: Invalid pll ref divider %d", __func__, pll_ref_div);
		BUG();
	}
	c->mul = 1;
	c->state = ON;
}

static struct clk_ops tegra_pll_ref_ops = {
	.init		= tegra11_pll_ref_init,
	.enable		= tegra11_clk_m_enable,
	.disable	= tegra11_clk_m_disable,
};

/* super clock functions */
/* "super clocks" on tegra11x have two-stage muxes, fractional 7.1 divider and
 * clock skipping super divider.  We will ignore the clock skipping divider,
 * since we can't lower the voltage when using the clock skip, but we can if
 * we lower the PLL frequency. Note that skipping divider can and will be used
 * by thermal control h/w for automatic throttling. There is also a 7.1 divider
 * that most CPU super-clock inputs can be routed through. We will not use it
 * as well (keep default 1:1 state), to avoid high jitter on PLLX and DFLL path
 * and possible concurrency access issues with thermal h/w (7.1 divider setting
 * share register with clock skipping divider)
 */
static void tegra11_super_clk_init(struct clk *c)
{
	u32 val;
	int source;
	int shift;
	const struct clk_mux_sel *sel;
	val = clk_readl(c->reg + SUPER_CLK_MUX);
	c->state = ON;
	BUG_ON(((val & SUPER_STATE_MASK) != SUPER_STATE_RUN) &&
		((val & SUPER_STATE_MASK) != SUPER_STATE_IDLE));
	shift = ((val & SUPER_STATE_MASK) == SUPER_STATE_IDLE) ?
		SUPER_IDLE_SOURCE_SHIFT : SUPER_RUN_SOURCE_SHIFT;
	source = (val >> shift) & SUPER_SOURCE_MASK;
	if (c->flags & DIV_2)
		source |= val & SUPER_LP_DIV2_BYPASS;
	for (sel = c->inputs; sel->input != NULL; sel++) {
		if (sel->value == source)
			break;
	}
	BUG_ON(sel->input == NULL);
	c->parent = sel->input;

	if (c->flags & DIV_U71) {
		c->mul = 2;
		c->div = 2;

		/* Make sure 7.1 divider is 1:1, clear s/w skipper control */
		/* FIXME: set? preserve? thermal h/w skipper control */
		val = clk_readl(c->reg + SUPER_CLK_DIVIDER);
		BUG_ON(val & SUPER_CLOCK_DIV_U71_MASK);
		val = 0;
		clk_writel(val, c->reg + SUPER_CLK_DIVIDER);
	}
	else
		clk_writel(0, c->reg + SUPER_CLK_DIVIDER);
}

static int tegra11_super_clk_enable(struct clk *c)
{
	return 0;
}

static void tegra11_super_clk_disable(struct clk *c)
{
	/* since tegra 3 has 2 CPU super clocks - low power lp-mode clock and
	   geared up g-mode super clock - mode switch may request to disable
	   either of them; accept request with no affect on h/w */
}

static int tegra11_super_clk_set_parent(struct clk *c, struct clk *p)
{
	u32 val;
	const struct clk_mux_sel *sel;
	int shift;

	val = clk_readl(c->reg + SUPER_CLK_MUX);
	BUG_ON(((val & SUPER_STATE_MASK) != SUPER_STATE_RUN) &&
		((val & SUPER_STATE_MASK) != SUPER_STATE_IDLE));
	shift = ((val & SUPER_STATE_MASK) == SUPER_STATE_IDLE) ?
		SUPER_IDLE_SOURCE_SHIFT : SUPER_RUN_SOURCE_SHIFT;
	for (sel = c->inputs; sel->input != NULL; sel++) {
		if (sel->input == p) {
			/* For LP mode super-clock switch between PLLX direct
			   and divided-by-2 outputs is allowed only when other
			   than PLLX clock source is current parent */
			if ((c->flags & DIV_2) && (p->flags & PLLX) &&
			    ((sel->value ^ val) & SUPER_LP_DIV2_BYPASS)) {
				if (c->parent->flags & PLLX)
					return -EINVAL;
				val ^= SUPER_LP_DIV2_BYPASS;
				clk_writel_delay(val, c->reg);
			}
			val &= ~(SUPER_SOURCE_MASK << shift);
			val |= (sel->value & SUPER_SOURCE_MASK) << shift;

			if (c->flags & DIV_U71) {
				/* Make sure 7.1 divider is 1:1 */
				u32 div = clk_readl(c->reg + SUPER_CLK_DIVIDER);
				BUG_ON(div & SUPER_CLOCK_DIV_U71_MASK);
			}

			if (c->refcnt)
				clk_enable(p);

			clk_writel_delay(val, c->reg);

			if (c->refcnt && c->parent)
				clk_disable(c->parent);

			clk_reparent(c, p);
			return 0;
		}
	}
	return -EINVAL;
}

/*
 * Do not use super clocks "skippers", since dividing using a clock skipper
 * does not allow the voltage to be scaled down. Instead adjust the rate of
 * the parent clock. This requires that the parent of a super clock have no
 * other children, otherwise the rate will change underneath the other
 * children.
 */
static int tegra11_super_clk_set_rate(struct clk *c, unsigned long rate)
{
	/* In tegra11_cpu_clk_set_plls() and  tegra11_sbus_cmplx_set_rate()
	 * this call is skipped by directly setting rate of source plls. If we
	 * ever use 7.1 divider at other than 1:1 setting, or exercise s/w
	 * skipper control, not only this function, but cpu and sbus set_rate
	 * APIs should be changed accordingly.
	 */
	return clk_set_rate(c->parent, rate);
}

static struct clk_ops tegra_super_ops = {
	.init			= tegra11_super_clk_init,
	.enable			= tegra11_super_clk_enable,
	.disable		= tegra11_super_clk_disable,
	.set_parent		= tegra11_super_clk_set_parent,
	.set_rate		= tegra11_super_clk_set_rate,
};

/* virtual cpu clock functions */
/* some clocks can not be stopped (cpu, memory bus) while the SoC is running.
   To change the frequency of these clocks, the parent pll may need to be
   reprogrammed, so the clock must be moved off the pll, the pll reprogrammed,
   and then the clock moved back to the pll.  To hide this sequence, a virtual
   clock handles it.
 */
static void tegra11_cpu_clk_init(struct clk *c)
{
	c->state = (!is_lp_cluster() == (c->u.cpu.mode == MODE_G))? ON : OFF;
}

static int tegra11_cpu_clk_enable(struct clk *c)
{
	return 0;
}

static void tegra11_cpu_clk_disable(struct clk *c)
{
	/* since tegra 3 has 2 virtual CPU clocks - low power lp-mode clock
	   and geared up g-mode clock - mode switch may request to disable
	   either of them; accept request with no affect on h/w */
}

static int tegra11_cpu_clk_set_plls(struct clk *c, unsigned long rate,
				    unsigned long old_rate)
{
	int ret = 0;
	bool on_main = false;
	unsigned long backup_rate, main_rate;
	unsigned long vco_min = c->u.cpu.main->u.pll.vco_min;

	/*
	 * Take an extra reference to the main pll so it doesn't turn off when
	 * we move the cpu off of it. If possible, use main pll dynamic ramp
	 * to reach target rate in one shot. Otherwise, use dynamic ramp to
	 * lower current rate to pll VCO minimum level before switching to
	 * backup source.
	 */
	if (c->parent->parent == c->u.cpu.main) {
		bool dramp = (rate > c->u.cpu.backup_rate) &&
			tegra11_is_dyn_ramp(c->u.cpu.main, rate, false);
		clk_enable(c->u.cpu.main);
		on_main = true;

		if (dramp ||
		    ((old_rate > vco_min) &&
		     tegra11_is_dyn_ramp(c->u.cpu.main, vco_min, false))) {
			main_rate = dramp ? rate : vco_min;
			ret = clk_set_rate(c->u.cpu.main, main_rate);
			if (ret) {
				pr_err("Failed to set cpu rate %lu on source"
				       " %s\n", main_rate, c->u.cpu.main->name);
				goto out;
			}
			if (dramp)
				goto out;
		} else if (old_rate > vco_min) {
			pr_warn("No dynamic ramp down: %s: %lu to %lu\n",
				c->u.cpu.main->name, old_rate, vco_min);
		}
	}

	/* Switch to back-up source, and stay on it if target rate is below
	   backup rate */
	if (c->parent->parent != c->u.cpu.backup) {
		ret = clk_set_parent(c->parent, c->u.cpu.backup);
		if (ret) {
			pr_err("Failed to switch cpu to %s\n",
			       c->u.cpu.backup->name);
			goto out;
		}
	}

	backup_rate = min(rate, c->u.cpu.backup_rate);
	if (backup_rate != clk_get_rate_locked(c)) {
		ret = clk_set_rate(c->u.cpu.backup, backup_rate);
		if (ret) {
			pr_err("Failed to set cpu rate %lu on backup source\n",
			       backup_rate);
			goto out;
		}
	}
	if (rate == backup_rate)
		goto out;

	/* Switch from backup source to main at rate not exceeding pll VCO
	   minimum. Use dynamic ramp to reach target rate if it is above VCO
	   minimum. */
	main_rate = rate;
	if (rate > vco_min) {
		if (tegra11_is_dyn_ramp(c->u.cpu.main, rate, true))
			main_rate = vco_min;
		else
			pr_warn("No dynamic ramp up: %s: %lu to %lu\n",
				c->u.cpu.main->name, vco_min, rate);
	}

	ret = clk_set_rate(c->u.cpu.main, main_rate);
	if (ret) {
		pr_err("Failed to set cpu rate %lu on source"
		       " %s\n", main_rate, c->u.cpu.main->name);
		goto out;
	}
	ret = clk_set_parent(c->parent, c->u.cpu.main);
	if (ret) {
		pr_err("Failed to switch cpu to %s\n", c->u.cpu.main->name);
		goto out;
	}
	if (rate != main_rate) {
		ret = clk_set_rate(c->u.cpu.main, rate);
		if (ret) {
			pr_err("Failed to set cpu rate %lu on source"
			       " %s\n", rate, c->u.cpu.main->name);
			goto out;
		}
	}

out:
	if (on_main)
		clk_disable(c->u.cpu.main);

	return ret;
}

static int tegra11_cpu_clk_dfll_on(struct clk *c, unsigned long rate,
				   unsigned long old_rate)
{
	int ret;
	struct clk *dfll = c->u.cpu.dynamic;

	/* dfll rate request */
	ret = clk_set_rate(dfll, rate);
	if (ret) {
		pr_err("Failed to set cpu rate %lu on source"
		       " %s\n", rate, dfll->name);
		return ret;
	}

	/* 1st time - switch to dfll */
	if (c->parent->parent != dfll) {
		ret = clk_set_parent(c->parent, dfll);
		if (ret) {
			pr_err("Failed to switch cpu to %s\n", dfll->name);
			return ret;
		}
		ret = tegra_clk_cfg_ex(dfll, TEGRA_CLK_DFLL_LOCK, 1);
		if (ret) {
			pr_err("Failed to lock %s\n", dfll->name);
			return ret;
		}
		/* prevent legacy dvfs voltage scaling */
		if (c->dvfs && c->dvfs->dvfs_rail)
			c->dvfs->dvfs_rail->auto_control = true;
	}
	return 0;
}

static int tegra11_cpu_clk_dfll_off(struct clk *c, unsigned long rate,
				    unsigned long old_rate)
{
	int ret;
	struct clk *dfll = c->u.cpu.dynamic;
	struct clk *pll = (old_rate <= c->u.cpu.backup_rate) ?
		c->u.cpu.backup : c->u.cpu.main;

	ret = tegra_clk_cfg_ex(dfll, TEGRA_CLK_DFLL_LOCK, 0);
	if (ret) {
		pr_err("Failed to unlock %s\n", dfll->name);
		return ret;
	}

	/* restore legacy dvfs operations and set appropriate voltage */
	if (c->dvfs && c->dvfs->dvfs_rail)
		c->dvfs->dvfs_rail->auto_control = false;

	rate = max(rate, old_rate);
	ret = tegra_dvfs_set_rate(c, rate);
	if (ret) {
		pr_err("Failed to set cpu rail for rate %lu\n", rate);
		return ret;
	}

	/* set pll rate same as dfll old rate, and return to pll source */
	ret = clk_set_rate(pll, old_rate);
	if (ret) {
		pr_err("Failed to set cpu rate %lu on source"
		       " %s\n", old_rate, pll->name);
		return ret;
	}
	ret = clk_set_parent(c->parent, pll);
	if (ret) {
		pr_err("Failed to switch cpu to %s\n", pll->name);
		return ret;
	}
	return 0;
}

static int tegra11_cpu_clk_set_rate(struct clk *c, unsigned long rate)
{
	unsigned long old_rate = clk_get_rate_locked(c);
	bool has_dfll = c->u.cpu.dynamic &&
		(c->u.cpu.dynamic->state != UNINITIALIZED);
	bool is_dfll = c->parent->parent == c->u.cpu.dynamic;

	/* On SILICON allow CPU rate change only if cpu regulator is connected.
	   Ignore regulator connection on FPGA and SIMULATION platforms. */
#ifdef CONFIG_TEGRA_SILICON_PLATFORM
	if (c->dvfs) {
		if (!c->dvfs->dvfs_rail)
			return -ENOSYS;
		else if ((!c->dvfs->dvfs_rail->reg) && (old_rate != rate)) {
			WARN(1, "Changing CPU rate while regulator is not"
				" ready is not allowed\n");
			return -ENOSYS;
		}
	}
#endif
	if (has_dfll) {
		if (use_dfll)
			return tegra11_cpu_clk_dfll_on(c, rate, old_rate);
		else if (is_dfll) {
			int ret = tegra11_cpu_clk_dfll_off(c, rate, old_rate);
			if (ret)
				return ret;
		}
	}
	return tegra11_cpu_clk_set_plls(c, rate, old_rate);
}

static struct clk_ops tegra_cpu_ops = {
	.init     = tegra11_cpu_clk_init,
	.enable   = tegra11_cpu_clk_enable,
	.disable  = tegra11_cpu_clk_disable,
	.set_rate = tegra11_cpu_clk_set_rate,
};


static void tegra11_cpu_cmplx_clk_init(struct clk *c)
{
	int i = !!is_lp_cluster();

	BUG_ON(c->inputs[0].input->u.cpu.mode != MODE_G);
	BUG_ON(c->inputs[1].input->u.cpu.mode != MODE_LP);
	c->parent = c->inputs[i].input;
}

/* cpu complex clock provides second level vitualization (on top of
   cpu virtual cpu rate control) in order to hide the CPU mode switch
   sequence */
#if PARAMETERIZE_CLUSTER_SWITCH
static unsigned int switch_delay;
static unsigned int switch_flags;
static DEFINE_SPINLOCK(parameters_lock);

void tegra_cluster_switch_set_parameters(unsigned int us, unsigned int flags)
{
	spin_lock(&parameters_lock);
	switch_delay = us;
	switch_flags = flags;
	spin_unlock(&parameters_lock);
}
#endif

static int tegra11_cpu_cmplx_clk_enable(struct clk *c)
{
	return 0;
}

static void tegra11_cpu_cmplx_clk_disable(struct clk *c)
{
	pr_debug("%s on clock %s\n", __func__, c->name);

	/* oops - don't disable the CPU complex clock! */
	BUG();
}

static int tegra11_cpu_cmplx_clk_set_rate(struct clk *c, unsigned long rate)
{
	unsigned long flags;
	int ret;
	struct clk *parent = c->parent;

	if (!parent->ops || !parent->ops->set_rate)
		return -ENOSYS;

	clk_lock_save(parent, &flags);

	ret = clk_set_rate_locked(parent, rate);

	clk_unlock_restore(parent, &flags);

	return ret;
}

static int tegra11_cpu_cmplx_clk_set_parent(struct clk *c, struct clk *p)
{
	int ret;
	unsigned int flags, delay;
	const struct clk_mux_sel *sel;
	unsigned long rate = clk_get_rate(c->parent);
	struct clk *dfll = c->parent->u.cpu.dynamic ? : p->u.cpu.dynamic;
	struct clk *p_source;

	pr_debug("%s: %s %s\n", __func__, c->name, p->name);
	BUG_ON(c->parent->u.cpu.mode != (is_lp_cluster() ? MODE_LP : MODE_G));

	for (sel = c->inputs; sel->input != NULL; sel++) {
		if (sel->input == p)
			break;
	}
	if (!sel->input)
		return -EINVAL;

#if PARAMETERIZE_CLUSTER_SWITCH
	spin_lock(&parameters_lock);
	flags = switch_flags;
	delay = switch_delay;
	switch_flags = 0;
	spin_unlock(&parameters_lock);

	if (flags) {
		/* over-clocking after the switch - allow, but lower rate */
		if (rate > p->max_rate) {
			rate = p->max_rate;
			ret = clk_set_rate(c->parent, rate);
			if (ret) {
				pr_err("%s: Failed to set rate %lu for %s\n",
				        __func__, rate, p->name);
				return ret;
			}
		}
	} else
#endif
	{
		if (p == c->parent)		/* already switched - exit*/
			return 0;

		if (rate > p->max_rate) {	/* over-clocking - no switch */
			pr_warn("%s: No %s mode switch to %s at rate %lu\n",
				 __func__, c->name, p->name, rate);
			return -ECANCELED;
		}
		flags = TEGRA_POWER_CLUSTER_IMMEDIATE;
		flags |= TEGRA_POWER_CLUSTER_PART_DEFAULT;
		delay = 0;
	}
	flags |= (p->u.cpu.mode == MODE_LP) ? TEGRA_POWER_CLUSTER_LP :
		TEGRA_POWER_CLUSTER_G;

	if (c->parent->parent->parent == dfll) {
		/* G (DFLL selected as clock source) => LP switch:
		 * turn DFLL into open loop mode ("release" VDD_CPU rail)
		 * select target p_source for LP, and get its rate ready
		 */
		ret = tegra_clk_cfg_ex(dfll, TEGRA_CLK_DFLL_LOCK, 0);
		if (ret)
			return ret;

		p_source = rate <= p->u.cpu.backup_rate ?
			p->u.cpu.backup : p->u.cpu.main;
		ret = clk_set_rate(p_source, rate);
		if (ret)
			return ret;
	} else if (p->parent->parent == dfll) {
		/* LP => G (DFLL selected as clock source) switch:
		 * set DFLL rate ready (DFLL is still disabled)
		 * (set target p_source as dfll, G source is already selected)
		 */
		p_source = dfll;
		ret = clk_set_rate(p_source, rate);
		if (ret)
			return ret;
	} else
		/* DFLL is not selcted on either side of the switch:
		 * set target p_source equal to current clock source
		 */
		p_source = c->parent->parent->parent;

	/* Switch new parent to target clock source if necessary */
	if (p->parent->parent != p_source) {
		ret = clk_set_parent(p->parent, p_source);
		if (ret) {
			pr_err("%s: Failed to set parent %s for %s\n",
			       __func__, p_source->name, p->name);
			return ret;
		}
	}

	/* Enabling new parent scales new mode voltage rail in advanvce
	   before the switch happens (if p_source is DFLL: open loop mode) */
	if (c->refcnt)
		clk_enable(p);

	/* switch CPU mode */
	ret = tegra_cluster_control(delay, flags);
	if (ret) {
		if (c->refcnt)
			clk_disable(p);
		pr_err("%s: Failed to switch %s mode to %s\n",
		       __func__, c->name, p->name);
		return ret;
	}

	/* Disabling old parent scales old mode voltage rail */
	if (c->refcnt && c->parent)
		clk_disable(c->parent);

	clk_reparent(c, p);

	/* Lock DFLL now (resume closed loop VDD_CPU control) */
	if (p_source == dfll)
		tegra_clk_cfg_ex(dfll, TEGRA_CLK_DFLL_LOCK, 1);

	return 0;
}

static long tegra11_cpu_cmplx_round_rate(struct clk *c,
	unsigned long rate)
{
	if (rate > c->parent->max_rate)
		rate = c->parent->max_rate;
	else if (rate < c->parent->min_rate)
		rate = c->parent->min_rate;
	return rate;
}

static struct clk_ops tegra_cpu_cmplx_ops = {
	.init     = tegra11_cpu_cmplx_clk_init,
	.enable   = tegra11_cpu_cmplx_clk_enable,
	.disable  = tegra11_cpu_cmplx_clk_disable,
	.set_rate = tegra11_cpu_cmplx_clk_set_rate,
	.set_parent = tegra11_cpu_cmplx_clk_set_parent,
	.round_rate = tegra11_cpu_cmplx_round_rate,
};

/* virtual cop clock functions. Used to acquire the fake 'cop' clock to
 * reset the COP block (i.e. AVP) */
static void tegra11_cop_clk_reset(struct clk *c, bool assert)
{
	unsigned long reg = assert ? RST_DEVICES_SET_L : RST_DEVICES_CLR_L;

	pr_debug("%s %s\n", __func__, assert ? "assert" : "deassert");
	clk_writel(1 << 1, reg);
}

static struct clk_ops tegra_cop_ops = {
	.reset    = tegra11_cop_clk_reset,
};

/* bus clock functions */
static void tegra11_bus_clk_init(struct clk *c)
{
	u32 val = clk_readl(c->reg);
	c->state = ((val >> c->reg_shift) & BUS_CLK_DISABLE) ? OFF : ON;
	c->div = ((val >> c->reg_shift) & BUS_CLK_DIV_MASK) + 1;
	c->mul = 1;
}

static int tegra11_bus_clk_enable(struct clk *c)
{
	u32 val = clk_readl(c->reg);
	val &= ~(BUS_CLK_DISABLE << c->reg_shift);
	clk_writel(val, c->reg);
	return 0;
}

static void tegra11_bus_clk_disable(struct clk *c)
{
	u32 val = clk_readl(c->reg);
	val |= BUS_CLK_DISABLE << c->reg_shift;
	clk_writel(val, c->reg);
}

static int tegra11_bus_clk_set_rate(struct clk *c, unsigned long rate)
{
	u32 val = clk_readl(c->reg);
	unsigned long parent_rate = clk_get_rate(c->parent);
	int i;
	for (i = 1; i <= 4; i++) {
		if (rate >= parent_rate / i) {
			val &= ~(BUS_CLK_DIV_MASK << c->reg_shift);
			val |= (i - 1) << c->reg_shift;
			clk_writel(val, c->reg);
			c->div = i;
			c->mul = 1;
			return 0;
		}
	}
	return -EINVAL;
}

static struct clk_ops tegra_bus_ops = {
	.init			= tegra11_bus_clk_init,
	.enable			= tegra11_bus_clk_enable,
	.disable		= tegra11_bus_clk_disable,
	.set_rate		= tegra11_bus_clk_set_rate,
};

/* Virtual system bus complex clock is used to hide the sequence of
   changing sclk/hclk/pclk parents and dividers to configure requested
   sclk target rate. */
static void tegra11_sbus_cmplx_init(struct clk *c)
{
	unsigned long rate;

	c->max_rate = c->parent->max_rate;
	c->min_rate = c->parent->min_rate;

	/* Threshold must be an exact proper factor of low range parent,
	   and both low/high range parents have 7.1 fractional dividers */
	rate = clk_get_rate(c->u.system.sclk_low->parent);
	if (c->u.system.threshold) {
		BUG_ON(c->u.system.threshold > rate) ;
		BUG_ON((rate % c->u.system.threshold) != 0);
	}
	BUG_ON(!(c->u.system.sclk_low->flags & DIV_U71));
	BUG_ON(!(c->u.system.sclk_high->flags & DIV_U71));
}

/* This special sbus round function is implemented because:
 *
 * (a) fractional dividers can not be used to derive system bus clock with one
 * exception: 1 : 2.5 divider is allowed at 1.2V and above (and we do need this
 * divider to reach top sbus frequencies from high frequency source).
 *
 * (b) since sbus is a shared bus, and its frequency is set to the highest
 * enabled shared_bus_user clock, the target rate should be rounded up divider
 * ladder (if max limit allows it) - for pll_div and peripheral_div common is
 * rounding down - special case again.
 *
 * Note that final rate is trimmed (not rounded up) to avoid spiraling up in
 * recursive calls. Lost 1Hz is added in tegra11_sbus_cmplx_set_rate before
 * actually setting divider rate.
 */
static unsigned long sclk_high_2_5_rate;
static bool sclk_high_2_5_valid;

static long tegra11_sbus_cmplx_round_rate(struct clk *c, unsigned long rate)
{
	int i, divider;
	unsigned long source_rate, round_rate;
	struct clk *new_parent;

	rate = max(rate, c->min_rate);

	if (!sclk_high_2_5_rate) {
		source_rate = clk_get_rate(c->u.system.sclk_high->parent);
		sclk_high_2_5_rate = 2 * source_rate / 5;
		i = tegra_dvfs_predict_millivolts(c, sclk_high_2_5_rate);
		if (!IS_ERR_VALUE(i) && (i >= 1200) &&
		    (sclk_high_2_5_rate <= c->max_rate))
			sclk_high_2_5_valid = true;
	}

	new_parent = (rate <= c->u.system.threshold) ?
		c->u.system.sclk_low : c->u.system.sclk_high;
	source_rate = clk_get_rate(new_parent->parent);

	divider = clk_div71_get_divider(source_rate, rate,
		new_parent->flags | DIV_U71_INT, ROUND_DIVIDER_DOWN);
	if (divider < 0)
		return divider;

	round_rate = source_rate * 2 / (divider + 2);
	if (round_rate > c->max_rate) {
		divider += 2;
		round_rate = source_rate * 2 / (divider + 2);
	}

	if (new_parent == c->u.system.sclk_high) {
		/* Check if 1 : 2.5 ratio provides better approximation */
		if (sclk_high_2_5_valid) {
			if (((sclk_high_2_5_rate < round_rate) &&
			    (sclk_high_2_5_rate >= rate)) ||
			    ((round_rate < sclk_high_2_5_rate) &&
			     (round_rate < rate)))
				round_rate = sclk_high_2_5_rate;
		}

		if (round_rate <= c->u.system.threshold)
			round_rate = c->u.system.threshold;
	}
	return round_rate;
}

static int tegra11_sbus_cmplx_set_rate(struct clk *c, unsigned long rate)
{
	int ret;
	struct clk *new_parent;

	/* - select the appropriate sclk parent
	   - keep hclk at the same rate as sclk
	   - set pclk at 1:2 rate of hclk unless pclk minimum is violated,
	     in the latter case switch to 1:1 ratio */

	if (rate >= c->u.system.pclk->min_rate * 2) {
		ret = clk_set_div(c->u.system.pclk, 2);
		if (ret) {
			pr_err("Failed to set 1 : 2 pclk divider\n");
			return ret;
		}
	}

	new_parent = (rate <= c->u.system.threshold) ?
		c->u.system.sclk_low : c->u.system.sclk_high;

	ret = clk_set_rate(new_parent, rate + 1);
	if (ret) {
		pr_err("Failed to set sclk source %s to %lu\n",
		       new_parent->name, rate);
		return ret;
	}

	if (new_parent != clk_get_parent(c->parent)) {
		ret = clk_set_parent(c->parent, new_parent);
		if (ret) {
			pr_err("Failed to switch sclk source to %s\n",
			       new_parent->name);
			return ret;
		}
	}

	if (rate < c->u.system.pclk->min_rate * 2) {
		ret = clk_set_div(c->u.system.pclk, 1);
		if (ret) {
			pr_err("Failed to set 1 : 1 pclk divider\n");
			return ret;
		}
	}

	return 0;
}

static struct clk_ops tegra_sbus_cmplx_ops = {
	.init = tegra11_sbus_cmplx_init,
	.set_rate = tegra11_sbus_cmplx_set_rate,
	.round_rate = tegra11_sbus_cmplx_round_rate,
	.shared_bus_update = tegra11_clk_shared_bus_update,
};

/* Blink output functions */

static void tegra11_blink_clk_init(struct clk *c)
{
	u32 val;

	val = pmc_readl(PMC_CTRL);
	c->state = (val & PMC_CTRL_BLINK_ENB) ? ON : OFF;
	c->mul = 1;
	val = pmc_readl(c->reg);

	if (val & PMC_BLINK_TIMER_ENB) {
		unsigned int on_off;

		on_off = (val >> PMC_BLINK_TIMER_DATA_ON_SHIFT) &
			PMC_BLINK_TIMER_DATA_ON_MASK;
		val >>= PMC_BLINK_TIMER_DATA_OFF_SHIFT;
		val &= PMC_BLINK_TIMER_DATA_OFF_MASK;
		on_off += val;
		/* each tick in the blink timer is 4 32KHz clocks */
		c->div = on_off * 4;
	} else {
		c->div = 1;
	}
}

static int tegra11_blink_clk_enable(struct clk *c)
{
	u32 val;

	val = pmc_readl(PMC_DPD_PADS_ORIDE);
	pmc_writel(val | PMC_DPD_PADS_ORIDE_BLINK_ENB, PMC_DPD_PADS_ORIDE);

	val = pmc_readl(PMC_CTRL);
	pmc_writel(val | PMC_CTRL_BLINK_ENB, PMC_CTRL);

	return 0;
}

static void tegra11_blink_clk_disable(struct clk *c)
{
	u32 val;

	val = pmc_readl(PMC_CTRL);
	pmc_writel(val & ~PMC_CTRL_BLINK_ENB, PMC_CTRL);

	val = pmc_readl(PMC_DPD_PADS_ORIDE);
	pmc_writel(val & ~PMC_DPD_PADS_ORIDE_BLINK_ENB, PMC_DPD_PADS_ORIDE);
}

static int tegra11_blink_clk_set_rate(struct clk *c, unsigned long rate)
{
	unsigned long parent_rate = clk_get_rate(c->parent);
	if (rate >= parent_rate) {
		c->div = 1;
		pmc_writel(0, c->reg);
	} else {
		unsigned int on_off;
		u32 val;

		on_off = DIV_ROUND_UP(parent_rate / 8, rate);
		c->div = on_off * 8;

		val = (on_off & PMC_BLINK_TIMER_DATA_ON_MASK) <<
			PMC_BLINK_TIMER_DATA_ON_SHIFT;
		on_off &= PMC_BLINK_TIMER_DATA_OFF_MASK;
		on_off <<= PMC_BLINK_TIMER_DATA_OFF_SHIFT;
		val |= on_off;
		val |= PMC_BLINK_TIMER_ENB;
		pmc_writel(val, c->reg);
	}

	return 0;
}

static struct clk_ops tegra_blink_clk_ops = {
	.init			= &tegra11_blink_clk_init,
	.enable			= &tegra11_blink_clk_enable,
	.disable		= &tegra11_blink_clk_disable,
	.set_rate		= &tegra11_blink_clk_set_rate,
};

/* PLL Functions */
static int tegra11_pll_clk_wait_for_lock(
	struct clk *c, u32 lock_reg, u32 lock_bits)
{
#ifndef CONFIG_TEGRA_SIMULATION_PLATFORM
#if USE_PLL_LOCK_BITS
	int i;
	for (i = 0; i < (c->u.pll.lock_delay / PLL_PRE_LOCK_DELAY + 1); i++) {
		udelay(PLL_PRE_LOCK_DELAY);
		if ((clk_readl(lock_reg) & lock_bits) == lock_bits) {
			udelay(PLL_POST_LOCK_DELAY);
			return 0;
		}
	}
	pr_err("Timed out waiting for lock bit on pll %s\n", c->name);
	return -1;
#endif
	udelay(c->u.pll.lock_delay);
#endif
	return 0;
}


static void tegra11_utmi_param_configure(struct clk *c)
{
	u32 reg;
	int i;
	unsigned long main_rate =
		clk_get_rate(c->parent->parent);

	for (i = 0; i < ARRAY_SIZE(utmi_parameters); i++) {
		if (main_rate == utmi_parameters[i].osc_frequency) {
			break;
		}
	}

	if (i >= ARRAY_SIZE(utmi_parameters)) {
		pr_err("%s: Unexpected main rate %lu\n", __func__, main_rate);
		return;
	}

	reg = clk_readl(UTMIP_PLL_CFG2);

	/* Program UTMIP PLL stable and active counts */
	/* [FIXME] arclk_rst.h says WRONG! This should be 1ms -> 0x50 Check! */
	reg &= ~UTMIP_PLL_CFG2_STABLE_COUNT(~0);
	reg |= UTMIP_PLL_CFG2_STABLE_COUNT(
			utmi_parameters[i].stable_count);

	reg &= ~UTMIP_PLL_CFG2_ACTIVE_DLY_COUNT(~0);

	reg |= UTMIP_PLL_CFG2_ACTIVE_DLY_COUNT(
			utmi_parameters[i].active_delay_count);

	/* Remove power downs from UTMIP PLL control bits */
	reg &= ~UTMIP_PLL_CFG2_FORCE_PD_SAMP_A_POWERDOWN;
	reg &= ~UTMIP_PLL_CFG2_FORCE_PD_SAMP_B_POWERDOWN;
	reg &= ~UTMIP_PLL_CFG2_FORCE_PD_SAMP_C_POWERDOWN;

	clk_writel(reg, UTMIP_PLL_CFG2);

	/* Program UTMIP PLL delay and oscillator frequency counts */
	reg = clk_readl(UTMIP_PLL_CFG1);
	reg &= ~UTMIP_PLL_CFG1_ENABLE_DLY_COUNT(~0);

	reg |= UTMIP_PLL_CFG1_ENABLE_DLY_COUNT(
		utmi_parameters[i].enable_delay_count);

	reg &= ~UTMIP_PLL_CFG1_XTAL_FREQ_COUNT(~0);
	reg |= UTMIP_PLL_CFG1_XTAL_FREQ_COUNT(
		utmi_parameters[i].xtal_freq_count);

	/* Remove power downs from UTMIP PLL control bits */
	reg &= ~UTMIP_PLL_CFG1_FORCE_PLL_ENABLE_POWERDOWN;
	reg &= ~UTMIP_PLL_CFG1_FORCE_PLL_ACTIVE_POWERDOWN;
	reg &= ~UTMIP_PLL_CFG1_FORCE_PLLU_POWERDOWN;

	clk_writel(reg, UTMIP_PLL_CFG1);
}

static void tegra11_pll_clk_init(struct clk *c)
{
	u32 val = clk_readl(c->reg + PLL_BASE);

	c->state = (val & PLL_BASE_ENABLE) ? ON : OFF;

	if (c->flags & PLL_FIXED && !(val & PLL_BASE_OVERRIDE)) {
		const struct clk_pll_freq_table *sel;
		unsigned long input_rate = clk_get_rate(c->parent);
		c->u.pll.fixed_rate = PLLP_DEFAULT_FIXED_RATE;

		for (sel = c->u.pll.freq_table; sel->input_rate != 0; sel++) {
			if (sel->input_rate == input_rate &&
				sel->output_rate == c->u.pll.fixed_rate) {
				c->mul = sel->n;
				c->div = sel->m * sel->p;
				return;
			}
		}
		pr_err("Clock %s has unknown fixed frequency\n", c->name);
		BUG();
	} else if (val & PLL_BASE_BYPASS) {
		c->mul = 1;
		c->div = 1;
	} else {
		c->mul = (val & PLL_BASE_DIVN_MASK) >> PLL_BASE_DIVN_SHIFT;
		c->div = (val & PLL_BASE_DIVM_MASK) >> PLL_BASE_DIVM_SHIFT;
		if (c->flags & PLLU)
			c->div *= (val & PLLU_BASE_POST_DIV) ? 1 : 2;
		else
			c->div *= (0x1 << ((val & PLL_BASE_DIVP_MASK) >>
					PLL_BASE_DIVP_SHIFT));
	}

	if (c->flags & PLL_FIXED) {
		c->u.pll.fixed_rate = clk_get_rate_locked(c);
	}

	if (c->flags & PLLU) {
		tegra11_utmi_param_configure(c);
	}
}

static int tegra11_pll_clk_enable(struct clk *c)
{
	u32 val;
	pr_debug("%s on clock %s\n", __func__, c->name);

#if USE_PLL_LOCK_BITS
	val = clk_readl(c->reg + PLL_MISC(c));
	val |= PLL_MISC_LOCK_ENABLE(c);
	clk_writel(val, c->reg + PLL_MISC(c));
#endif
	val = clk_readl(c->reg + PLL_BASE);
	val &= ~PLL_BASE_BYPASS;
	val |= PLL_BASE_ENABLE;
	clk_writel(val, c->reg + PLL_BASE);

	tegra11_pll_clk_wait_for_lock(c, c->reg + PLL_BASE, PLL_BASE_LOCK);

	return 0;
}

static void tegra11_pll_clk_disable(struct clk *c)
{
	u32 val;
	pr_debug("%s on clock %s\n", __func__, c->name);

	val = clk_readl(c->reg);
	val &= ~(PLL_BASE_BYPASS | PLL_BASE_ENABLE);
	clk_writel(val, c->reg);
}

static int tegra11_pll_clk_set_rate(struct clk *c, unsigned long rate)
{
	u32 val, p_div, old_base;
	unsigned long input_rate;
	const struct clk_pll_freq_table *sel;
	struct clk_pll_freq_table cfg;

	pr_debug("%s: %s %lu\n", __func__, c->name, rate);

	if (c->flags & PLL_FIXED) {
		int ret = 0;
		if (rate != c->u.pll.fixed_rate) {
			pr_err("%s: Can not change %s fixed rate %lu to %lu\n",
			       __func__, c->name, c->u.pll.fixed_rate, rate);
			ret = -EINVAL;
		}
		return ret;
	}

	p_div = 0;
	input_rate = clk_get_rate(c->parent);

	/* Check if the target rate is tabulated */
	for (sel = c->u.pll.freq_table; sel->input_rate != 0; sel++) {
		if (sel->input_rate == input_rate && sel->output_rate == rate) {
			if (c->flags & PLLU) {
				BUG_ON(sel->p < 1 || sel->p > 2);
				if (sel->p == 1)
					p_div = PLLU_BASE_POST_DIV;
			} else {
				BUG_ON(sel->p < 1);
				for (val = sel->p; val > 1; val >>= 1, p_div++);
				p_div <<= PLL_BASE_DIVP_SHIFT;
			}
			break;
		}
	}

	/* Configure out-of-table rate */
	if (sel->input_rate == 0) {
		unsigned long cfreq;
		BUG_ON(c->flags & PLLU);
		sel = &cfg;

		switch (input_rate) {
		case 12000000:
		case 26000000:
			cfreq = (rate <= 1000000 * 1000) ? 1000000 : 2000000;
			break;
		case 13000000:
			cfreq = (rate <= 1000000 * 1000) ? 1000000 : 2600000;
			break;
		case 16800000:
		case 19200000:
			cfreq = (rate <= 1200000 * 1000) ? 1200000 : 2400000;
			break;
		default:
			if (c->parent->flags & DIV_U71_FIXED) {
				/* PLLP_OUT1 rate is not in PLLA table */
				pr_warn("%s: failed %s ref/out rates %lu/%lu\n",
					__func__, c->name, input_rate, rate);
				cfreq = input_rate/(input_rate/1000000);
				break;
			}
			pr_err("%s: Unexpected reference rate %lu\n",
			       __func__, input_rate);
			BUG();
		}

		/* Raise VCO to guarantee 0.5% accuracy */
		for (cfg.output_rate = rate; cfg.output_rate < 200 * cfreq;
		      cfg.output_rate <<= 1, p_div++);

		cfg.p = 0x1 << p_div;
		cfg.m = input_rate / cfreq;
		cfg.n = cfg.output_rate / cfreq;
		cfg.cpcon = OUT_OF_TABLE_CPCON;

		if ((cfg.m > (PLL_BASE_DIVM_MASK >> PLL_BASE_DIVM_SHIFT)) ||
		    (cfg.n > (PLL_BASE_DIVN_MASK >> PLL_BASE_DIVN_SHIFT)) ||
		    (p_div > (PLL_BASE_DIVP_MASK >> PLL_BASE_DIVP_SHIFT)) ||
		    (cfg.output_rate > c->u.pll.vco_max)) {
			pr_err("%s: Failed to set %s out-of-table rate %lu\n",
			       __func__, c->name, rate);
			return -EINVAL;
		}
		p_div <<= PLL_BASE_DIVP_SHIFT;
	}

	c->mul = sel->n;
	c->div = sel->m * sel->p;

	old_base = val = clk_readl(c->reg + PLL_BASE);
	val &= ~(PLL_BASE_DIVM_MASK | PLL_BASE_DIVN_MASK |
		 ((c->flags & PLLU) ? PLLU_BASE_POST_DIV : PLL_BASE_DIVP_MASK));
	val |= (sel->m << PLL_BASE_DIVM_SHIFT) |
		(sel->n << PLL_BASE_DIVN_SHIFT) | p_div;
	if (val == old_base)
		return 0;

	if (c->state == ON) {
		tegra11_pll_clk_disable(c);
		val &= ~(PLL_BASE_BYPASS | PLL_BASE_ENABLE);
	}
	clk_writel(val, c->reg + PLL_BASE);

	if (c->flags & PLL_HAS_CPCON) {
		val = clk_readl(c->reg + PLL_MISC(c));
		val &= ~PLL_MISC_CPCON_MASK;
		val |= sel->cpcon << PLL_MISC_CPCON_SHIFT;
		if (c->flags & (PLLU | PLLD)) {
			val &= ~PLL_MISC_LFCON_MASK;
			if (sel->n >= PLLDU_LFCON_SET_DIVN)
				val |= 0x1 << PLL_MISC_LFCON_SHIFT;
		}
		clk_writel(val, c->reg + PLL_MISC(c));
	}

	if (c->state == ON)
		tegra11_pll_clk_enable(c);

	return 0;
}

static struct clk_ops tegra_pll_ops = {
	.init			= tegra11_pll_clk_init,
	.enable			= tegra11_pll_clk_enable,
	.disable		= tegra11_pll_clk_disable,
	.set_rate		= tegra11_pll_clk_set_rate,
};

static void tegra11_pllp_clk_init(struct clk *c)
{
	tegra11_pll_clk_init(c);
	tegra11_pllp_init_dependencies(c->u.pll.fixed_rate);
}

static void tegra11_pllp_clk_resume(struct clk *c)
{
	unsigned long rate = c->u.pll.fixed_rate;
	tegra11_pll_clk_init(c);
	BUG_ON(rate != c->u.pll.fixed_rate);
}

static struct clk_ops tegra_pllp_ops = {
	.init			= tegra11_pllp_clk_init,
	.enable			= tegra11_pll_clk_enable,
	.disable		= tegra11_pll_clk_disable,
	.set_rate		= tegra11_pll_clk_set_rate,
};

static int
tegra11_plld_clk_cfg_ex(struct clk *c, enum tegra_clk_ex_param p, u32 setting)
{
	u32 val, mask, reg;

	switch (p) {
	case TEGRA_CLK_PLLD_CSI_OUT_ENB:
		mask = PLLD_BASE_CSI_CLKENABLE | PLLD_BASE_CSI_CLKSOURCE;
		reg = c->reg + PLL_BASE;
		break;
	case TEGRA_CLK_PLLD_DSI_OUT_ENB:
		mask = PLLD_MISC_DSI_CLKENABLE;
		reg = c->reg + PLL_MISC(c);
		break;
	case TEGRA_CLK_PLLD_MIPI_MUX_SEL:
		mask = PLLD_BASE_DSI_MUX_MASK;
		reg = c->reg + PLL_BASE;
		break;
	default:
		return -EINVAL;
	}

	val = clk_readl(reg);
	if (setting)
		val |= mask;
	else
		val &= ~mask;
	clk_writel(val, reg);
	return 0;
}

static struct clk_ops tegra_plld_ops = {
	.init			= tegra11_pll_clk_init,
	.enable			= tegra11_pll_clk_enable,
	.disable		= tegra11_pll_clk_disable,
	.set_rate		= tegra11_pll_clk_set_rate,
	.clk_cfg_ex		= tegra11_plld_clk_cfg_ex,
};

/*
 * Dynamic ramp PLLs:
 *  PLLC2 and PLLC3 (PLLCX)
 *  PLLX and PLLC (PLLXC)
 *
 * When scaling PLLC and PLLX, dynamic ramp is allowed for any transition that
 * changes NDIV only. As a matter of policy we will make sure that switching
 * between output rates above VCO minimum is always dynamic. The pre-requisite
 * for the above guarantee is the following configuration convention:
 * - pll configured with fixed MDIV
 * - when output rate is above VCO minimum PDIV = 0 (p-value = 1)
 * Switching between output rates below VCO minimum may or may not be dynamic,
 * and switching across VCO minimum is never dynamic.
 *
 * PLLC2 and PLLC3 in addition to dynamic ramp mechanism have also glitchless
 * output dividers. However dynamic ramp without overshoot is guaranteed only
 * when output divisor is less or equal 8.
 *
 * Of course, dynamic ramp is applied provided PLL is already enabled.
 */

/*
 * Common configuration policy for dynamic ramp PLLs:
 * - always set fixed M-value based on the reference rate
 * - always set P-value value 1:1 for output rates above VCO minimum, and
 *   choose minimum necessary P-value for output rates below VCO minimum
 * - calculate N-value based on selected M and P
 */
static int pll_dyn_ramp_cfg(struct clk *c, struct clk_pll_freq_table *cfg,
	unsigned long rate, unsigned long input_rate, u32 *pdiv)
{
	u32 p;

	if (!rate)
		return -EINVAL;

	p = DIV_ROUND_UP(c->u.pll.vco_min, rate);
	p = c->u.pll.round_p_to_pdiv(p, pdiv);
	if (IS_ERR_VALUE(p))
		return -EINVAL;

	cfg->m = PLL_FIXED_MDIV(c, input_rate);
	cfg->p = p;
	cfg->output_rate = rate * cfg->p;
	cfg->n = cfg->output_rate * cfg->m / input_rate;

	/* can use PLLCX N-divider field layout for all dynamic ramp PLLs */
	if ((cfg->n > (PLLCX_BASE_DIVN_MASK >> PLL_BASE_DIVN_SHIFT)) ||
	    (cfg->output_rate > c->u.pll.vco_max))
		return -EINVAL;

	return 0;
}

static int pll_dyn_ramp_find_cfg(struct clk *c, struct clk_pll_freq_table *cfg,
	unsigned long rate, unsigned long input_rate, u32 *pdiv)
{
	const struct clk_pll_freq_table *sel;

	/* Check if the target rate is tabulated */
	for (sel = c->u.pll.freq_table; sel->input_rate != 0; sel++) {
		if (sel->input_rate == input_rate && sel->output_rate == rate) {
			u32 p = c->u.pll.round_p_to_pdiv(sel->p, pdiv);
			BUG_ON(IS_ERR_VALUE(p));
			if (rate >= c->u.pll.vco_min)
				BUG_ON(sel->p != 1);
			BUG_ON(sel->m != PLL_FIXED_MDIV(c, input_rate));
			*cfg = *sel;
			return 0;
		}
	}

	/* Configure out-of-table rate */
	if (pll_dyn_ramp_cfg(c, cfg, rate, input_rate, pdiv)) {
		pr_err("%s: Failed to set %s out-of-table rate %lu\n",
		       __func__, c->name, rate);
		return -EINVAL;
	}
	return 0;
}

static inline void pll_do_iddq(struct clk *c, u32 offs, u32 iddq_bit, bool set)
{
	u32 val = clk_readl(c->reg + offs);
	if (set)
		val |= iddq_bit;
	else
		val &= ~iddq_bit;
	clk_writel_delay(val, c->reg + offs);
}


/* FIXME: pllcx suspend/resume */

static u8 pllcx_p[PLLCX_PDIV_MAX + 1] = {
/* PDIV: 0, 1, 2, 3, 4, 5,  6,  7 */
/* p: */ 1, 2, 3, 4, 6, 8, 12, 16 };

static u32 pllcx_round_p_to_pdiv(u32 p, u32 *pdiv)
{
	int i;

	if (p) {
		for (i = 0; i <= PLLCX_PDIV_MAX; i++) {
			if (p <= pllcx_p[i]) {
				if (pdiv)
					*pdiv = i;
				return pllcx_p[i];
			}
		}
	}
	return -EINVAL;
}

static void pllcx_update_dynamic_koef(struct clk *c, unsigned long input_rate,
					u32 n)
{
	u32 val, n_threshold;

	switch (input_rate) {
	case 12000000:
		n_threshold = 77;
		break;
	case 13000000:
	case 26000000:
		n_threshold = 71;
		break;
	case 16800000:
		n_threshold = 55;
		break;
	case 19200000:
		n_threshold = 48;
		break;
	default:
		pr_err("%s: Unexpected reference rate %lu\n",
			__func__, input_rate);
		BUG();
		return;
	}

	val = clk_readl(c->reg + PLL_MISC(c));
	val &= ~(PLLCX_MISC_KA_MASK | PLLCX_MISC_KB_MASK);
	val |= n <= n_threshold ?
		PLLCX_MISC_KOEF_LOW_RANGE : PLLCX_MISC_KOEF_HIGH_RANGE;
	clk_writel(val, c->reg + PLL_MISC(c));
}

static void pllcx_strobe(struct clk *c)
{
	u32 reg = c->reg + PLL_MISC(c);
	u32 val = clk_readl(reg);

	val |= PLLCX_MISC_STROBE;
	pll_writel_delay(val, reg);

	val &= ~PLLCX_MISC_STROBE;
	clk_writel(val, reg);
}

static void pllcx_set_defaults(struct clk *c, unsigned long input_rate, u32 n)
{
	clk_writel(PLLCX_MISC_DEFAULT_VALUE, c->reg + PLL_MISC(c));
	clk_writel(PLLCX_MISC1_DEFAULT_VALUE, c->reg + PLL_MISCN(c, 1));
	clk_writel(PLLCX_MISC2_DEFAULT_VALUE, c->reg + PLL_MISCN(c, 2));
	clk_writel(PLLCX_MISC3_DEFAULT_VALUE, c->reg + PLL_MISCN(c, 3));

	pllcx_update_dynamic_koef(c, input_rate, n);
}

static void tegra11_pllcx_clk_init(struct clk *c)
{
	unsigned long input_rate = clk_get_rate(c->parent);
	u32 m, n, p, val;

	/* clip vco_min to exact multiple of input rate to avoid crossover
	   by rounding */
	c->u.pll.vco_min =
		DIV_ROUND_UP(c->u.pll.vco_min, input_rate) * input_rate;
	c->min_rate = DIV_ROUND_UP(c->u.pll.vco_min, pllcx_p[PLLCX_PDIV_MAX]);

	val = clk_readl(c->reg + PLL_BASE);
	c->state = (val & PLL_BASE_ENABLE) ? ON : OFF;

	/*
	 * PLLCX is not a boot PLL, it should be left disabled by boot-loader,
	 * and no enabled module clocks should use it as a source during clock
	 * init.
	 */
#ifndef CONFIG_TEGRA_SIMULATION_PLATFORM
	BUG_ON(c->state == ON);
#endif
	/*
	 * Most of PLLCX register fields are shadowed, and can not be read
	 * directly from PLL h/w. Hence, actual PLLCX boot state is unknown.
	 * Initialize PLL to default state: disabled, reset; shadow registers
	 * loaded with default parameters; dividers are preset for half of
	 * minimum VCO rate (the latter assured that shadowed divider settings
	 * are within supported range).
	 */
	m = PLL_FIXED_MDIV(c, input_rate);
	n = m * c->u.pll.vco_min / input_rate;
	p = pllcx_p[1];
	val = (m << PLL_BASE_DIVM_SHIFT) | (n << PLL_BASE_DIVN_SHIFT) |
		(1 << PLL_BASE_DIVP_SHIFT);
	clk_writel(val, c->reg + PLL_BASE);	/* PLL disabled */

	pllcx_set_defaults(c, input_rate, n);

	c->mul = n;
	c->div = m * p;
}

static int tegra11_pllcx_clk_enable(struct clk *c)
{
	u32 val;
	pr_debug("%s on clock %s\n", __func__, c->name);

	val = clk_readl(c->reg + PLL_BASE);
	val &= ~PLL_BASE_BYPASS;
	val |= PLL_BASE_ENABLE;
	pll_writel_delay(val, c->reg + PLL_BASE);

	val = clk_readl(c->reg + PLL_MISC(c));
	val &= ~PLLCX_MISC_RESET;
	pll_writel_delay(val, c->reg + PLL_MISC(c));

	pllcx_strobe(c);
	tegra11_pll_clk_wait_for_lock(c, c->reg + PLL_BASE,
			PLL_BASE_LOCK | PLLCX_BASE_PHASE_LOCK);
	return 0;
}

static void tegra11_pllcx_clk_disable(struct clk *c)
{
	u32 val;
	pr_debug("%s on clock %s\n", __func__, c->name);

	val = clk_readl(c->reg);
	val &= ~(PLL_BASE_BYPASS | PLL_BASE_ENABLE);
	clk_writel(val, c->reg);

	val = clk_readl(c->reg + PLL_MISC(c));
	val |= PLLCX_MISC_RESET;
	pll_writel_delay(val, c->reg + PLL_MISC(c));
}

static int tegra11_pllcx_clk_set_rate(struct clk *c, unsigned long rate)
{
	u32 val, pdiv;
	unsigned long input_rate;
	struct clk_pll_freq_table cfg, old_cfg;
	const struct clk_pll_freq_table *sel = &cfg;

	pr_debug("%s: %s %lu\n", __func__, c->name, rate);

	input_rate = clk_get_rate(c->parent);

	if (pll_dyn_ramp_find_cfg(c, &cfg, rate, input_rate, &pdiv))
		return -EINVAL;

	c->mul = sel->n;
	c->div = sel->m * sel->p;

	val = clk_readl(c->reg + PLL_BASE);
	PLL_BASE_PARSE(PLLCX, old_cfg, val);
	old_cfg.p = pllcx_p[old_cfg.p];

	BUG_ON(old_cfg.m != sel->m);
	if ((sel->n == old_cfg.n) && (sel->p == old_cfg.p))
		return 0;

#if PLLCX_USE_DYN_RAMP
	if (c->state == ON && ((sel->n == old_cfg.n) ||
			       PLLCX_IS_DYN(sel->p, old_cfg.p))) {
		/*
		 * Dynamic ramp if PLL is enabled, and M divider is unchanged:
		 * - Change P divider 1st if intermediate rate is below either
		 *   old or new rate.
		 * - Change N divider with DFS strobe - target rate is either
		 *   final new rate or below old rate
		 * - If divider has been changed, exit without waiting for lock.
		 *   Otherwise, wait for lock and change divider.
		 */
		if (sel->p > old_cfg.p) {
			val &= ~PLLCX_BASE_DIVP_MASK;
			val |= pdiv << PLL_BASE_DIVP_SHIFT;
			clk_writel(val, c->reg + PLL_BASE);
		}

		if (sel->n != old_cfg.n) {
			pllcx_update_dynamic_koef(c, input_rate, sel->n);
			val &= ~PLLCX_BASE_DIVN_MASK;
			val |= sel->n << PLL_BASE_DIVN_SHIFT;
			pll_writel_delay(val, c->reg + PLL_BASE);

			pllcx_strobe(c);
			tegra11_pll_clk_wait_for_lock(c, c->reg + PLL_BASE,
					PLL_BASE_LOCK | PLLCX_BASE_PHASE_LOCK);
		}

		if (sel->p < old_cfg.p) {
			val &= ~PLLCX_BASE_DIVP_MASK;
			val |= pdiv << PLL_BASE_DIVP_SHIFT;
			clk_writel(val, c->reg + PLL_BASE);
		}
		return 0;
	}
#endif

	val &= ~(PLLCX_BASE_DIVN_MASK | PLLCX_BASE_DIVP_MASK);
	val |= (sel->n << PLL_BASE_DIVN_SHIFT) |
		(pdiv << PLL_BASE_DIVP_SHIFT);

	if (c->state == ON) {
		tegra11_pllcx_clk_disable(c);
		val &= ~(PLL_BASE_BYPASS | PLL_BASE_ENABLE);
	}
	pllcx_update_dynamic_koef(c, input_rate, sel->n);
	clk_writel(val, c->reg + PLL_BASE);
	if (c->state == ON)
		tegra11_pllcx_clk_enable(c);

	return 0;
}

static struct clk_ops tegra_pllcx_ops = {
	.init			= tegra11_pllcx_clk_init,
	.enable			= tegra11_pllcx_clk_enable,
	.disable		= tegra11_pllcx_clk_disable,
	.set_rate		= tegra11_pllcx_clk_set_rate,
};


/* FIXME: pllxc suspend/resume */

/* non-monotonic mapping below is not a typo */
static u8 pllxc_p[PLLXC_PDIV_MAX + 1] = {
/* PDIV: 0, 1, 2, 3, 4, 5, 6,  7,  8,  9, 10, 11, 12, 13, 14 */
/* p: */ 1, 2, 3, 4, 5, 6, 8, 10, 12, 16, 12, 16, 20, 24, 32 };

static u32 pllxc_round_p_to_pdiv(u32 p, u32 *pdiv)
{
	if (!p || (p > PLLXC_SW_PDIV_MAX + 1))
		return -EINVAL;

	if (pdiv)
		*pdiv = p - 1;
	return p;
}

static void pllxc_get_dyn_steps(struct clk *c, unsigned long input_rate,
				u32 *step_a, u32 *step_b)
{
	switch (input_rate) {
	case 12000000:
	case 13000000:
	case 26000000:
		*step_a = 0x2B;
		*step_b = 0x0B;
		return;
	case 16800000:
		*step_a = 0x1A;
		*step_b = 0x09;
		return;
	case 19200000:
		*step_a = 0x12;
		*step_b = 0x08;
		return;
	default:
		pr_err("%s: Unexpected reference rate %lu\n",
			__func__, input_rate);
		BUG();
	}
}

static void pllx_set_defaults(struct clk *c, unsigned long input_rate)
{
	u32 val;
	u32 step_a, step_b;

	/* Only s/w dyn ramp control is supported */
	val = clk_readl(PLLX_HW_CTRL_CFG);
#ifndef CONFIG_TEGRA_SIMULATION_PLATFORM
	BUG_ON(!(val & PLLX_HW_CTRL_CFG_SWCTRL));
#endif

	pllxc_get_dyn_steps(c, input_rate, &step_a, &step_b);
	val = step_a << PLLX_MISC2_DYNRAMP_STEPA_SHIFT;
	val |= step_b << PLLX_MISC2_DYNRAMP_STEPB_SHIFT;

	/* Get ready dyn ramp state machine, disable lock override */
	clk_writel(val, c->reg + PLL_MISCN(c, 2));

	/* Enable outputs to CPUs and configure lock */
	val = 0;
#if USE_PLL_LOCK_BITS
	val |= PLL_MISC_LOCK_ENABLE(c);
#endif
	clk_writel(val, c->reg + PLL_MISC(c));

	/* Check/set IDDQ */
	val = clk_readl(c->reg + PLL_MISCN(c, 3));
	if (c->state == ON) {
#ifndef CONFIG_TEGRA_SIMULATION_PLATFORM
		BUG_ON(val & PLLX_MISC3_IDDQ);
#endif
	} else {
		val |= PLLX_MISC3_IDDQ;
		clk_writel(val, c->reg + PLL_MISCN(c, 3));
	}
}

static void pllc_set_defaults(struct clk *c, unsigned long input_rate)
{
	u32 val;
	u32 step_a, step_b;

	/* Get ready dyn ramp state machine */
	pllxc_get_dyn_steps(c, input_rate, &step_a, &step_b);
	val = step_a << PLLC_MISC1_DYNRAMP_STEPA_SHIFT;
	val |= step_b << PLLC_MISC1_DYNRAMP_STEPB_SHIFT;
	clk_writel(val, c->reg + PLL_MISCN(c, 1));

	/* Configure lock and check/set IDDQ */
	val = clk_readl(c->reg + PLL_BASE);
	val &= ~PLLC_BASE_LOCK_OVERRIDE;
	clk_writel(val, c->reg + PLL_BASE);

	val = clk_readl(c->reg + PLL_MISC(c));
#if USE_PLL_LOCK_BITS
	val |= PLLC_MISC_LOCK_ENABLE;
#else
	val &= ~PLLC_MISC_LOCK_ENABLE;
#endif
	clk_writel(val, c->reg + PLL_MISC(c));

	if (c->state == ON) {
#ifndef CONFIG_TEGRA_SIMULATION_PLATFORM
		BUG_ON(val & PLLC_MISC_IDDQ);
#endif
	} else {
		val |= PLLC_MISC_IDDQ;
		clk_writel(val, c->reg + PLL_MISC(c));
	}
}

static void tegra11_pllxc_clk_init(struct clk *c)
{
	unsigned long input_rate = clk_get_rate(c->parent);
	u32 m, p, val;

	/* clip vco_min to exact multiple of input rate to avoid crossover
	   by rounding */
	c->u.pll.vco_min =
		DIV_ROUND_UP(c->u.pll.vco_min, input_rate) * input_rate;
	c->min_rate =
		DIV_ROUND_UP(c->u.pll.vco_min, pllxc_p[PLLXC_SW_PDIV_MAX]);

	val = clk_readl(c->reg + PLL_BASE);
	c->state = (val & PLL_BASE_ENABLE) ? ON : OFF;

	m = (val & PLLXC_BASE_DIVM_MASK) >> PLL_BASE_DIVM_SHIFT;
	p = (val & PLLXC_BASE_DIVP_MASK) >> PLL_BASE_DIVP_SHIFT;
	BUG_ON(p > PLLXC_PDIV_MAX);
	p = pllxc_p[p];

	c->div = m * p;
	c->mul = (val & PLLXC_BASE_DIVN_MASK) >> PLL_BASE_DIVN_SHIFT;

	if (c->flags & PLLX)
		pllx_set_defaults(c, input_rate);
	else
		pllc_set_defaults(c, input_rate);
}

static int tegra11_pllxc_clk_enable(struct clk *c)
{
	u32 val;
	pr_debug("%s on clock %s\n", __func__, c->name);

	if (c->flags & PLLX)
		pll_do_iddq(c, PLL_MISCN(c, 3), PLLX_MISC3_IDDQ, false);
	else
		pll_do_iddq(c, PLL_MISC(c), PLLC_MISC_IDDQ, false);

	val = clk_readl(c->reg + PLL_BASE);
	val |= PLL_BASE_ENABLE;
	clk_writel(val, c->reg + PLL_BASE);

	tegra11_pll_clk_wait_for_lock(c, c->reg + PLL_BASE, PLL_BASE_LOCK);

	return 0;
}

static void tegra11_pllxc_clk_disable(struct clk *c)
{
	u32 val;
	pr_debug("%s on clock %s\n", __func__, c->name);

	val = clk_readl(c->reg + PLL_BASE);
	val &= ~PLL_BASE_ENABLE;
	clk_writel(val, c->reg + PLL_BASE);

	if (c->flags & PLLX)
		pll_do_iddq(c, PLL_MISCN(c, 3), PLLX_MISC3_IDDQ, true);
	else
		pll_do_iddq(c, PLL_MISC(c), PLLC_MISC_IDDQ, true);

}

#define PLLXC_DYN_RAMP(pll_misc, reg)					\
	do {								\
		u32 misc = clk_readl((reg));				\
									\
		misc &= ~pll_misc##_NDIV_NEW_MASK;			\
		misc |= sel->n << pll_misc##_NDIV_NEW_SHIFT;		\
		pll_writel_delay(misc, (reg));				\
									\
		misc |= pll_misc##_EN_DYNRAMP;				\
		clk_writel(misc, (reg));				\
		tegra11_pll_clk_wait_for_lock(c, (reg),			\
					pll_misc##_DYNRAMP_DONE);	\
									\
		val &= ~PLLXC_BASE_DIVN_MASK;				\
		val |= sel->n << PLL_BASE_DIVN_SHIFT;			\
		pll_writel_delay(val, c->reg + PLL_BASE);		\
									\
		misc &= ~pll_misc##_EN_DYNRAMP;				\
		pll_writel_delay(misc, (reg));				\
	} while (0)

static int tegra11_pllxc_clk_set_rate(struct clk *c, unsigned long rate)
{
	u32 val, pdiv;
	unsigned long input_rate;
	struct clk_pll_freq_table cfg, old_cfg;
	const struct clk_pll_freq_table *sel = &cfg;

	pr_debug("%s: %s %lu\n", __func__, c->name, rate);

	input_rate = clk_get_rate(c->parent);

	if (pll_dyn_ramp_find_cfg(c, &cfg, rate, input_rate, &pdiv))
		return -EINVAL;

	c->mul = sel->n;
	c->div = sel->m * sel->p;

	val = clk_readl(c->reg + PLL_BASE);
	PLL_BASE_PARSE(PLLXC, old_cfg, val);
	old_cfg.p = pllxc_p[old_cfg.p];

	if ((sel->m == old_cfg.m) && (sel->n == old_cfg.n) &&
	    (sel->p == old_cfg.p))
		return 0;

#if PLLXC_USE_DYN_RAMP
	/*
	 * Dynamic ramp can be used if M, P dividers are unchanged
	 * (coveres superset of conventional dynamic ramps)
	 */
	if ((c->state == ON) && (sel->m == old_cfg.m) &&
	    (sel->p == old_cfg.p)) {

		if (c->flags & PLLX) {
			u32 reg = c->reg + PLL_MISCN(c, 2);
			PLLXC_DYN_RAMP(PLLX_MISC2, reg);
		} else {
			u32 reg = c->reg + PLL_MISCN(c, 1);
			PLLXC_DYN_RAMP(PLLC_MISC1, reg);
		}

		return 0;
	}
#endif
	if (c->state == ON) {
		/* Use "ENABLE" pulse without placing PLL into IDDQ */
		val &= ~PLL_BASE_ENABLE;
		pll_writel_delay(val, c->reg + PLL_BASE);
	}

	val &= ~(PLLXC_BASE_DIVM_MASK |
		 PLLXC_BASE_DIVN_MASK | PLLXC_BASE_DIVP_MASK);
	val |= (sel->m << PLL_BASE_DIVM_SHIFT) |
		(sel->n << PLL_BASE_DIVN_SHIFT) | (pdiv << PLL_BASE_DIVP_SHIFT);
	clk_writel(val, c->reg + PLL_BASE);

	if (c->state == ON) {
		val |= PLL_BASE_ENABLE;
		clk_writel(val, c->reg + PLL_BASE);
		tegra11_pll_clk_wait_for_lock(c, c->reg + PLL_BASE,
					      PLL_BASE_LOCK);
	}
	return 0;
}

static struct clk_ops tegra_pllxc_ops = {
	.init			= tegra11_pllxc_clk_init,
	.enable			= tegra11_pllxc_clk_enable,
	.disable		= tegra11_pllxc_clk_disable,
	.set_rate		= tegra11_pllxc_clk_set_rate,
};


/* FIXME: pllm suspend/resume */

static u32 pllm_round_p_to_pdiv(u32 p, u32 *pdiv)
{
	if (!p || (p > 2))
		return -EINVAL;

	if (pdiv)
		*pdiv = p - 1;
	return p;
}

static void pllm_set_defaults(struct clk *c, unsigned long input_rate)
{
	u32 val = clk_readl(c->reg + PLL_MISC(c));

	val &= ~PLLM_MISC_LOCK_OVERRIDE;
#if USE_PLL_LOCK_BITS
	val &= ~PLLM_MISC_LOCK_DISABLE;
#else
	val |= PLLM_MISC_LOCK_DISABLE;
#endif

	if (c->state != ON)
		val |= PLLM_MISC_IDDQ;
#ifndef CONFIG_TEGRA_SIMULATION_PLATFORM
	else
		BUG_ON(val & PLLM_MISC_IDDQ);
#endif

	clk_writel(val, c->reg + PLL_MISC(c));
}

static void tegra11_pllm_clk_init(struct clk *c)
{
	unsigned long input_rate = clk_get_rate(c->parent);
	u32 m, p, val;

	/* clip vco_min to exact multiple of input rate to avoid crossover
	   by rounding */
	c->u.pll.vco_min =
		DIV_ROUND_UP(c->u.pll.vco_min, input_rate) * input_rate;
	c->min_rate =
		DIV_ROUND_UP(c->u.pll.vco_min, 2);

	val = pmc_readl(PMC_PLLP_WB0_OVERRIDE);
	if (val & PMC_PLLP_WB0_OVERRIDE_PLLM_OVERRIDE) {
		c->state = (val & PMC_PLLP_WB0_OVERRIDE_PLLM_ENABLE) ? ON : OFF;
		val = pmc_readl(PMC_PLLM_WB0_OVERRIDE_2);
		p = (val & PMC_PLLM_WB0_OVERRIDE_2_DIVP_MASK) ? 2 : 1;

		val = pmc_readl(PMC_PLLM_WB0_OVERRIDE);
	} else {
		val = clk_readl(c->reg + PLL_BASE);
		c->state = (val & PLL_BASE_ENABLE) ? ON : OFF;
		p = (val & PLLM_BASE_DIVP_MASK) ? 2 : 1;
	}

	m = (val & PLLM_BASE_DIVM_MASK) >> PLL_BASE_DIVM_SHIFT;
	BUG_ON(m != PLL_FIXED_MDIV(c, input_rate));
	c->div = m * p;
	c->mul = (val & PLLM_BASE_DIVN_MASK) >> PLL_BASE_DIVN_SHIFT;

	pllm_set_defaults(c, input_rate);
}

static int tegra11_pllm_clk_enable(struct clk *c)
{
	u32 val;
	pr_debug("%s on clock %s\n", __func__, c->name);

	pll_do_iddq(c, PLL_MISC(c), PLLM_MISC_IDDQ, false);

	/* Just enable both base and override - one would work */
	val = clk_readl(c->reg + PLL_BASE);
	val |= PLL_BASE_ENABLE;
	clk_writel(val, c->reg + PLL_BASE);

	val = pmc_readl(PMC_PLLP_WB0_OVERRIDE);
	val |= PMC_PLLP_WB0_OVERRIDE_PLLM_ENABLE;
	pmc_writel(val, PMC_PLLP_WB0_OVERRIDE);
	val = pmc_readl(PMC_PLLP_WB0_OVERRIDE);

	tegra11_pll_clk_wait_for_lock(c, c->reg + PLL_BASE, PLL_BASE_LOCK);
	return 0;
}

static void tegra11_pllm_clk_disable(struct clk *c)
{
	u32 val;
	pr_debug("%s on clock %s\n", __func__, c->name);

	/* Just disable both base and override - one would work */
	val = pmc_readl(PMC_PLLP_WB0_OVERRIDE);
	val &= ~PMC_PLLP_WB0_OVERRIDE_PLLM_ENABLE;
	pmc_writel(val, PMC_PLLP_WB0_OVERRIDE);
	val = pmc_readl(PMC_PLLP_WB0_OVERRIDE);

	val = clk_readl(c->reg + PLL_BASE);
	val &= ~PLL_BASE_ENABLE;
	clk_writel(val, c->reg + PLL_BASE);

	pll_do_iddq(c, PLL_MISC(c), PLLM_MISC_IDDQ, true);
}

static int tegra11_pllm_clk_set_rate(struct clk *c, unsigned long rate)
{
	u32 val, pdiv;
	unsigned long input_rate;
	struct clk_pll_freq_table cfg;
	const struct clk_pll_freq_table *sel = &cfg;

	pr_debug("%s: %s %lu\n", __func__, c->name, rate);

	if (c->state == ON) {
		if (rate != clk_get_rate_locked(c)) {
			pr_err("%s: Can not change memory %s rate in flight\n",
			       __func__, c->name);
			return -EINVAL;
		}
		return 0;
	}

	input_rate = clk_get_rate(c->parent);

	if (pll_dyn_ramp_find_cfg(c, &cfg, rate, input_rate, &pdiv))
		return -EINVAL;

	c->mul = sel->n;
	c->div = sel->m * sel->p;

	val = pmc_readl(PMC_PLLP_WB0_OVERRIDE);
	if (val & PMC_PLLP_WB0_OVERRIDE_PLLM_OVERRIDE) {
		val = pmc_readl(PMC_PLLM_WB0_OVERRIDE_2);
		val = pdiv ? (val | PMC_PLLM_WB0_OVERRIDE_2_DIVP_MASK) :
			(val & ~PMC_PLLM_WB0_OVERRIDE_2_DIVP_MASK);
		pmc_writel(val, PMC_PLLM_WB0_OVERRIDE_2);

		val = pmc_readl(PMC_PLLM_WB0_OVERRIDE);
		val &= ~(PLLM_BASE_DIVM_MASK | PLLM_BASE_DIVN_MASK);
		val |= (sel->m << PLL_BASE_DIVM_SHIFT) |
			(sel->n << PLL_BASE_DIVN_SHIFT);
		pmc_writel(val, PMC_PLLM_WB0_OVERRIDE);
	} else {
		val = clk_readl(c->reg + PLL_BASE);
		val &= ~(PLLM_BASE_DIVM_MASK | PLLM_BASE_DIVN_MASK |
			 PLLM_BASE_DIVP_MASK);
		val |= (sel->m << PLL_BASE_DIVM_SHIFT) |
			(sel->n << PLL_BASE_DIVN_SHIFT) |
			(pdiv ? PLLM_BASE_DIVP_MASK : 0);
		clk_writel(val, c->reg + PLL_BASE);
	}

	return 0;
}

static struct clk_ops tegra_pllm_ops = {
	.init			= tegra11_pllm_clk_init,
	.enable			= tegra11_pllm_clk_enable,
	.disable		= tegra11_pllm_clk_disable,
	.set_rate		= tegra11_pllm_clk_set_rate,
};


/* FIXME: pllre suspend/resume */
/* non-monotonic mapping below is not a typo */
static u8 pllre_p[PLLRE_PDIV_MAX + 1] = {
/* PDIV: 0, 1, 2, 3, 4, 5, 6,  7,  8,  9, 10, 11, 12, 13, 14 */
/* p: */ 1, 2, 3, 4, 5, 6, 8, 10, 12, 16, 12, 16, 20, 24, 32 };

static u32 pllre_round_p_to_pdiv(u32 p, u32 *pdiv)
{
	if (!p || (p > PLLRE_SW_PDIV_MAX + 1))
		return -EINVAL;

	if (pdiv)
		*pdiv = p - 1;
	return p;
}

static void pllre_set_defaults(struct clk *c, unsigned long input_rate)
{
	u32 val = clk_readl(c->reg + PLL_MISC(c));

	val &= ~PLLRE_MISC_LOCK_OVERRIDE;
#if USE_PLL_LOCK_BITS
	val |= PLLRE_MISC_LOCK_ENABLE;
#else
	val &= ~PLLRE_MISC_LOCK_ENABLE;
#endif

	if (c->state != ON)
		val |= PLLRE_MISC_IDDQ;
#ifndef CONFIG_TEGRA_SIMULATION_PLATFORM
	else
		BUG_ON(val & PLLRE_MISC_IDDQ);
#endif

	clk_writel(val, c->reg + PLL_MISC(c));
}

static void tegra11_pllre_clk_init(struct clk *c)
{
	unsigned long input_rate = clk_get_rate(c->parent);
	u32 m, val;

	/* clip vco_min to exact multiple of input rate to avoid crossover
	   by rounding */
	c->u.pll.vco_min =
		DIV_ROUND_UP(c->u.pll.vco_min, input_rate) * input_rate;
	c->min_rate = c->u.pll.vco_min;

	val = clk_readl(c->reg + PLL_BASE);
	c->state = (val & PLL_BASE_ENABLE) ? ON : OFF;

	if (!val) {
		/* overwrite h/w por state with min setting */
		m = PLL_FIXED_MDIV(c, input_rate);
		val = (m << PLL_BASE_DIVM_SHIFT) |
			(c->min_rate / input_rate << PLL_BASE_DIVN_SHIFT);
		clk_writel(val, c->reg + PLL_BASE);
	}

	m = (val & PLLRE_BASE_DIVM_MASK) >> PLL_BASE_DIVM_SHIFT;
	BUG_ON(m != PLL_FIXED_MDIV(c, input_rate));

	c->div = m;
	c->mul = (val & PLLRE_BASE_DIVN_MASK) >> PLL_BASE_DIVN_SHIFT;

	pllre_set_defaults(c, input_rate);
}

static int tegra11_pllre_clk_enable(struct clk *c)
{
	u32 val;
	pr_debug("%s on clock %s\n", __func__, c->name);

	pll_do_iddq(c, PLL_MISC(c), PLLRE_MISC_IDDQ, false);

	val = clk_readl(c->reg + PLL_BASE);
	val |= PLL_BASE_ENABLE;
	clk_writel(val, c->reg + PLL_BASE);

	tegra11_pll_clk_wait_for_lock(c, c->reg + PLL_MISC(c), PLLRE_MISC_LOCK);
	return 0;
}

static void tegra11_pllre_clk_disable(struct clk *c)
{
	u32 val;
	pr_debug("%s on clock %s\n", __func__, c->name);

	val = clk_readl(c->reg + PLL_BASE);
	val &= ~PLL_BASE_ENABLE;
	clk_writel(val, c->reg + PLL_BASE);

	pll_do_iddq(c, PLL_MISC(c), PLLRE_MISC_IDDQ, true);
}

static int tegra11_pllre_clk_set_rate(struct clk *c, unsigned long rate)
{
	u32 val, old_base;
	unsigned long input_rate;
	struct clk_pll_freq_table cfg;

	pr_debug("%s: %s %lu\n", __func__, c->name, rate);

	if (rate < c->min_rate) {
		pr_err("%s: Failed to set %s rate %lu\n",
		       __func__, c->name, rate);
		return -EINVAL;
	}

	input_rate = clk_get_rate(c->parent);
	cfg.m = PLL_FIXED_MDIV(c, input_rate);
	cfg.n = rate * cfg.m / input_rate;

	c->mul = cfg.n;
	c->div = cfg.m;

	val = old_base = clk_readl(c->reg + PLL_BASE);
	val &= ~(PLLRE_BASE_DIVM_MASK | PLLRE_BASE_DIVN_MASK);
	val |= (cfg.m << PLL_BASE_DIVM_SHIFT) | (cfg.n << PLL_BASE_DIVN_SHIFT);
	if (val == old_base)
		return 0;

	if (c->state == ON) {
		/* Use "ENABLE" pulse without placing PLL into IDDQ */
		val &= ~PLL_BASE_ENABLE;
		old_base &= ~PLL_BASE_ENABLE;
		pll_writel_delay(old_base, c->reg + PLL_BASE);
	}

	clk_writel(val, c->reg + PLL_BASE);

	if (c->state == ON) {
		val |= PLL_BASE_ENABLE;
		clk_writel(val, c->reg + PLL_BASE);
		tegra11_pll_clk_wait_for_lock(
			c, c->reg + PLL_MISC(c), PLLRE_MISC_LOCK);
	}
	return 0;
}

static struct clk_ops tegra_pllre_ops = {
	.init			= tegra11_pllre_clk_init,
	.enable			= tegra11_pllre_clk_enable,
	.disable		= tegra11_pllre_clk_disable,
	.set_rate		= tegra11_pllre_clk_set_rate,
};

static void tegra11_pllre_out_clk_init(struct clk *c)
{
	u32 p, val;

	val = clk_readl(c->reg);
	p = (val & PLLRE_BASE_DIVP_MASK) >> PLLRE_BASE_DIVP_SHIFT;
	BUG_ON(p > PLLRE_PDIV_MAX);
	p = pllre_p[p];

	c->div = p;
	c->mul = 1;
	c->state = c->parent->state;
}

static int tegra11_pllre_out_clk_enable(struct clk *c)
{
	return 0;
}

static void tegra11_pllre_out_clk_disable(struct clk *c)
{
}

static int tegra11_pllre_out_clk_set_rate(struct clk *c, unsigned long rate)
{
	u32 val, p, pdiv;
	unsigned long input_rate, flags;

	pr_debug("%s: %s %lu\n", __func__, c->name, rate);

	clk_lock_save(c->parent, &flags);
	input_rate = clk_get_rate_locked(c->parent);

	p = DIV_ROUND_UP(input_rate, rate);
	p = c->parent->u.pll.round_p_to_pdiv(p, &pdiv);
	if (IS_ERR_VALUE(p)) {
		pr_err("%s: Failed to set %s rate %lu\n",
		       __func__, c->name, rate);
		clk_unlock_restore(c->parent, &flags);
		return -EINVAL;
	}
	c->div = p;

	val = clk_readl(c->reg);
	val &= ~PLLRE_BASE_DIVP_MASK;
	val |= pdiv << PLLRE_BASE_DIVP_SHIFT;
	clk_writel(val, c->reg);

	clk_unlock_restore(c->parent, &flags);
	return 0;
}

static struct clk_ops tegra_pllre_out_ops = {
	.init			= tegra11_pllre_out_clk_init,
	.enable			= tegra11_pllre_out_clk_enable,
	.disable		= tegra11_pllre_out_clk_disable,
	.set_rate		= tegra11_pllre_out_clk_set_rate,
};


static void tegra11_plle_clk_init(struct clk *c)
{
	u32 val;

	val = clk_readl(PLLE_AUX);
	c->parent = (val & PLLE_AUX_PLLP_SEL) ?
		tegra_get_clock_by_name("pll_p") :
		tegra_get_clock_by_name("pll_ref");

	val = clk_readl(c->reg + PLL_BASE);
	c->state = (val & PLLE_BASE_ENABLE) ? ON : OFF;
	c->mul = (val & PLLE_BASE_DIVN_MASK) >> PLLE_BASE_DIVN_SHIFT;
	c->div = (val & PLLE_BASE_DIVM_MASK) >> PLLE_BASE_DIVM_SHIFT;
	c->div *= (val & PLLE_BASE_DIVP_MASK) >> PLLE_BASE_DIVP_SHIFT;
}

static void tegra11_plle_clk_disable(struct clk *c)
{
	u32 val;
	pr_debug("%s on clock %s\n", __func__, c->name);

	val = clk_readl(c->reg + PLL_BASE);
	val &= ~(PLLE_BASE_CML_ENABLE | PLLE_BASE_ENABLE);
	clk_writel(val, c->reg + PLL_BASE);
}

static void tegra11_plle_training(struct clk *c)
{
	u32 val;

	/* PLLE is already disabled, and setup cleared;
	 * create falling edge on PLLE IDDQ input */
	val = pmc_readl(PMC_SATA_PWRGT);
	val |= PMC_SATA_PWRGT_PLLE_IDDQ_VALUE;
	pmc_writel(val, PMC_SATA_PWRGT);

	val = pmc_readl(PMC_SATA_PWRGT);
	val |= PMC_SATA_PWRGT_PLLE_IDDQ_SWCTL;
	pmc_writel(val, PMC_SATA_PWRGT);

	val = pmc_readl(PMC_SATA_PWRGT);
	val &= ~PMC_SATA_PWRGT_PLLE_IDDQ_VALUE;
	pmc_writel(val, PMC_SATA_PWRGT);

	do {
		val = clk_readl(c->reg + PLL_MISC(c));
	} while (!(val & PLLE_MISC_READY));
}

static int tegra11_plle_configure(struct clk *c, bool force_training)
{
	u32 val;
	const struct clk_pll_freq_table *sel;
	unsigned long rate = c->u.pll.fixed_rate;
	unsigned long input_rate = clk_get_rate(c->parent);

	for (sel = c->u.pll.freq_table; sel->input_rate != 0; sel++) {
		if (sel->input_rate == input_rate && sel->output_rate == rate)
			break;
	}

	if (sel->input_rate == 0)
		return -ENOSYS;

	/* disable PLLE, clear setup fiels */
	tegra11_plle_clk_disable(c);

	val = clk_readl(c->reg + PLL_MISC(c));
	val &= ~(PLLE_MISC_LOCK_ENABLE | PLLE_MISC_SETUP_MASK);
	clk_writel(val, c->reg + PLL_MISC(c));

	/* training */
	val = clk_readl(c->reg + PLL_MISC(c));
	if (force_training || (!(val & PLLE_MISC_READY)))
		tegra11_plle_training(c);

	/* configure dividers, setup, disable SS */
	val = clk_readl(c->reg + PLL_BASE);
	val &= ~PLLE_BASE_DIV_MASK;
	val |= PLLE_BASE_DIV(sel->m, sel->n, sel->p, sel->cpcon);
	clk_writel(val, c->reg + PLL_BASE);
	c->mul = sel->n;
	c->div = sel->m * sel->p;

	val = clk_readl(c->reg + PLL_MISC(c));
	val |= PLLE_MISC_SETUP_VALUE;
	val |= PLLE_MISC_LOCK_ENABLE;
	clk_writel(val, c->reg + PLL_MISC(c));

	val = clk_readl(PLLE_SS_CTRL);
	val |= PLLE_SS_DISABLE;
	clk_writel(val, PLLE_SS_CTRL);

	/* enable and lock PLLE*/
	val = clk_readl(c->reg + PLL_BASE);
	val |= (PLLE_BASE_CML_ENABLE | PLLE_BASE_ENABLE);
	clk_writel(val, c->reg + PLL_BASE);

	tegra11_pll_clk_wait_for_lock(c, c->reg + PLL_MISC(c), PLLE_MISC_LOCK);

#if USE_PLLE_SS
	/* configure spread spectrum coefficients */
	/* FIXME: coefficients for 216MHZ input? */
#ifdef CONFIG_TEGRA_SILICON_PLATFORM
	if (input_rate == 12000000)
#endif
	{
		val = clk_readl(PLLE_SS_CTRL);
		val &= ~(PLLE_SS_COEFFICIENTS_MASK | PLLE_SS_DISABLE);
		val |= PLLE_SS_COEFFICIENTS_12MHZ;
		clk_writel(val, PLLE_SS_CTRL);
	}
#endif
	return 0;
}

static int tegra11_plle_clk_enable(struct clk *c)
{
	pr_debug("%s on clock %s\n", __func__, c->name);
	return tegra11_plle_configure(c, !c->set);
}

static struct clk_ops tegra_plle_ops = {
	.init			= tegra11_plle_clk_init,
	.enable			= tegra11_plle_clk_enable,
	.disable		= tegra11_plle_clk_disable,
};

/* DFLL operations */
static int tegra11_dfll_clk_enable(struct clk *c)
{
	return tegra_cl_dvfs_enable(c->u.dfll.cl_dvfs);
}

static void tegra11_dfll_clk_disable(struct clk *c)
{
	tegra_cl_dvfs_disable(c->u.dfll.cl_dvfs);
}

static int tegra11_dfll_clk_set_rate(struct clk *c, unsigned long rate)
{
	int ret = tegra_cl_dvfs_request_rate(c->u.dfll.cl_dvfs, rate);

	if (!ret)
		c->rate = tegra_cl_dvfs_request_get(c->u.dfll.cl_dvfs);

	return ret;
}

static void tegra11_dfll_clk_reset(struct clk *c, bool assert)
{
	u32 val = assert ? 1 : 0;
	clk_writel_delay(val, c->reg);
}

static int
tegra11_dfll_clk_cfg_ex(struct clk *c, enum tegra_clk_ex_param p, u32 setting)
{
	if (p == TEGRA_CLK_DFLL_LOCK)
		return setting ? tegra_cl_dvfs_lock(c->u.dfll.cl_dvfs) :
				 tegra_cl_dvfs_unlock(c->u.dfll.cl_dvfs);
	return -EINVAL;
}

static struct clk_ops tegra_dfll_ops = {
	.enable			= tegra11_dfll_clk_enable,
	.disable		= tegra11_dfll_clk_disable,
	.set_rate		= tegra11_dfll_clk_set_rate,
	.reset			= tegra11_dfll_clk_reset,
	.clk_cfg_ex		= tegra11_dfll_clk_cfg_ex,
};

/* Clock divider ops (non-atomic shared register access) */
static DEFINE_SPINLOCK(pll_div_lock);

static int tegra11_pll_div_clk_set_rate(struct clk *c, unsigned long rate);
static void tegra11_pll_div_clk_init(struct clk *c)
{
	if (c->flags & DIV_U71) {
		u32 divu71;
		u32 val = clk_readl(c->reg);
		val >>= c->reg_shift;
		c->state = (val & PLL_OUT_CLKEN) ? ON : OFF;
		if (!(val & PLL_OUT_RESET_DISABLE))
			c->state = OFF;

		if (c->u.pll_div.default_rate) {
			int ret = tegra11_pll_div_clk_set_rate(
					c, c->u.pll_div.default_rate);
			if (!ret)
				return;
		}
		divu71 = (val & PLL_OUT_RATIO_MASK) >> PLL_OUT_RATIO_SHIFT;
		c->div = (divu71 + 2);
		c->mul = 2;
	} else if (c->flags & DIV_2) {
		c->state = ON;
		if (c->flags & (PLLD | PLLX)) {
			c->div = 2;
			c->mul = 1;
		}
		else
			BUG();
	} else {
		c->state = ON;
		c->div = 1;
		c->mul = 1;
	}
}

static int tegra11_pll_div_clk_enable(struct clk *c)
{
	u32 val;
	u32 new_val;
	unsigned long flags;

	pr_debug("%s: %s\n", __func__, c->name);
	if (c->flags & DIV_U71) {
		spin_lock_irqsave(&pll_div_lock, flags);
		val = clk_readl(c->reg);
		new_val = val >> c->reg_shift;
		new_val &= 0xFFFF;

		new_val |= PLL_OUT_CLKEN | PLL_OUT_RESET_DISABLE;

		val &= ~(0xFFFF << c->reg_shift);
		val |= new_val << c->reg_shift;
		clk_writel_delay(val, c->reg);
		spin_unlock_irqrestore(&pll_div_lock, flags);
		return 0;
	} else if (c->flags & DIV_2) {
		return 0;
	}
	return -EINVAL;
}

static void tegra11_pll_div_clk_disable(struct clk *c)
{
	u32 val;
	u32 new_val;
	unsigned long flags;

	pr_debug("%s: %s\n", __func__, c->name);
	if (c->flags & DIV_U71) {
		spin_lock_irqsave(&pll_div_lock, flags);
		val = clk_readl(c->reg);
		new_val = val >> c->reg_shift;
		new_val &= 0xFFFF;

		new_val &= ~(PLL_OUT_CLKEN | PLL_OUT_RESET_DISABLE);

		val &= ~(0xFFFF << c->reg_shift);
		val |= new_val << c->reg_shift;
		clk_writel_delay(val, c->reg);
		spin_unlock_irqrestore(&pll_div_lock, flags);
	}
}

static int tegra11_pll_div_clk_set_rate(struct clk *c, unsigned long rate)
{
	u32 val;
	u32 new_val;
	int divider_u71;
	unsigned long parent_rate = clk_get_rate(c->parent);
	unsigned long flags;

	pr_debug("%s: %s %lu\n", __func__, c->name, rate);
	if (c->flags & DIV_U71) {
		divider_u71 = clk_div71_get_divider(
			parent_rate, rate, c->flags, ROUND_DIVIDER_UP);
		if (divider_u71 >= 0) {
			spin_lock_irqsave(&pll_div_lock, flags);
			val = clk_readl(c->reg);
			new_val = val >> c->reg_shift;
			new_val &= 0xFFFF;
			if (c->flags & DIV_U71_FIXED)
				new_val |= PLL_OUT_OVERRIDE;
			new_val &= ~PLL_OUT_RATIO_MASK;
			new_val |= divider_u71 << PLL_OUT_RATIO_SHIFT;

			val &= ~(0xFFFF << c->reg_shift);
			val |= new_val << c->reg_shift;
			clk_writel_delay(val, c->reg);
			c->div = divider_u71 + 2;
			c->mul = 2;
			spin_unlock_irqrestore(&pll_div_lock, flags);
			return 0;
		}
	} else if (c->flags & DIV_2)
		return clk_set_rate(c->parent, rate * 2);

	return -EINVAL;
}

static long tegra11_pll_div_clk_round_rate(struct clk *c, unsigned long rate)
{
	int divider;
	unsigned long parent_rate = clk_get_rate(c->parent);
	pr_debug("%s: %s %lu\n", __func__, c->name, rate);

	if (c->flags & DIV_U71) {
		divider = clk_div71_get_divider(
			parent_rate, rate, c->flags, ROUND_DIVIDER_UP);
		if (divider < 0)
			return divider;
		return DIV_ROUND_UP(parent_rate * 2, divider + 2);
	} else if (c->flags & DIV_2)
		/* no rounding - fixed DIV_2 dividers pass rate to parent PLL */
		return rate;

	return -EINVAL;
}

static struct clk_ops tegra_pll_div_ops = {
	.init			= tegra11_pll_div_clk_init,
	.enable			= tegra11_pll_div_clk_enable,
	.disable		= tegra11_pll_div_clk_disable,
	.set_rate		= tegra11_pll_div_clk_set_rate,
	.round_rate		= tegra11_pll_div_clk_round_rate,
};

/* Periph clk ops */
static inline u32 periph_clk_source_mask(struct clk *c)
{
	if (c->u.periph.src_mask)
		return c->u.periph.src_mask;
	else if (c->flags & MUX8)
		return 7 << 29;
	else if (c->flags & MUX_PWM)
		return 3 << 28;
	else if (c->flags & MUX_CLK_OUT)
		return 3 << (c->u.periph.clk_num + 4);
	else if (c->flags & PLLD)
		return PLLD_BASE_DSI_MUX_MASK;
	else
		return 3 << 30;
}

static inline u32 periph_clk_source_shift(struct clk *c)
{
	if (c->u.periph.src_shift)
		return c->u.periph.src_shift;
	else if (c->flags & MUX8)
		return 29;
	else if (c->flags & MUX_PWM)
		return 28;
	else if (c->flags & MUX_CLK_OUT)
		return c->u.periph.clk_num + 4;
	else if (c->flags & PLLD)
		return PLLD_BASE_DSI_MUX_SHIFT;
	else
		return 30;
}

static void tegra11_periph_clk_init(struct clk *c)
{
	u32 val = clk_readl(c->reg);
	const struct clk_mux_sel *mux = 0;
	const struct clk_mux_sel *sel;
	if (c->flags & MUX) {
		for (sel = c->inputs; sel->input != NULL; sel++) {
			if (((val & periph_clk_source_mask(c)) >>
			    periph_clk_source_shift(c)) == sel->value)
				mux = sel;
		}
		BUG_ON(!mux);

		c->parent = mux->input;
	} else {
		c->parent = c->inputs[0].input;
	}

	if (c->flags & DIV_U71) {
		u32 divu71 = val & PERIPH_CLK_SOURCE_DIVU71_MASK;
		if (c->flags & DIV_U71_IDLE) {
			val &= ~(PERIPH_CLK_SOURCE_DIVU71_MASK <<
				PERIPH_CLK_SOURCE_DIVIDLE_SHIFT);
			val |= (PERIPH_CLK_SOURCE_DIVIDLE_VAL <<
				PERIPH_CLK_SOURCE_DIVIDLE_SHIFT);
			clk_writel(val, c->reg);
		}
		c->div = divu71 + 2;
		c->mul = 2;
	} else if (c->flags & DIV_U151) {
		u32 divu151 = val & PERIPH_CLK_SOURCE_DIVU16_MASK;
		if ((c->flags & DIV_U151_UART) &&
		    (!(val & PERIPH_CLK_UART_DIV_ENB))) {
			divu151 = 0;
		}
		c->div = divu151 + 2;
		c->mul = 2;
	} else if (c->flags & DIV_U16) {
		u32 divu16 = val & PERIPH_CLK_SOURCE_DIVU16_MASK;
		c->div = divu16 + 1;
		c->mul = 1;
	} else {
		c->div = 1;
		c->mul = 1;
	}

	c->state = ON;

	if (c->flags & PERIPH_NO_ENB)
		return;

	if (!(clk_readl(PERIPH_CLK_TO_ENB_REG(c)) & PERIPH_CLK_TO_BIT(c)))
		c->state = OFF;
	if (!(c->flags & PERIPH_NO_RESET))
		if (clk_readl(PERIPH_CLK_TO_RST_REG(c)) & PERIPH_CLK_TO_BIT(c))
			c->state = OFF;
}

static int tegra11_periph_clk_enable(struct clk *c)
{
	unsigned long flags;
	pr_debug("%s on clock %s\n", __func__, c->name);

	if (c->flags & PERIPH_NO_ENB)
		return 0;

	spin_lock_irqsave(&periph_refcount_lock, flags);

	tegra_periph_clk_enable_refcount[c->u.periph.clk_num]++;
	if (tegra_periph_clk_enable_refcount[c->u.periph.clk_num] > 1) {
		spin_unlock_irqrestore(&periph_refcount_lock, flags);
		return 0;
	}

	clk_writel_delay(PERIPH_CLK_TO_BIT(c), PERIPH_CLK_TO_ENB_SET_REG(c));
	if (!(c->flags & PERIPH_NO_RESET) && !(c->flags & PERIPH_MANUAL_RESET)) {
		if (clk_readl(PERIPH_CLK_TO_RST_REG(c)) & PERIPH_CLK_TO_BIT(c)) {
			udelay(5);	/* reset propagation delay */
			clk_writel(PERIPH_CLK_TO_BIT(c), PERIPH_CLK_TO_RST_CLR_REG(c));
		}
	}
	spin_unlock_irqrestore(&periph_refcount_lock, flags);
	return 0;
}

static void tegra11_periph_clk_disable(struct clk *c)
{
	unsigned long val, flags;
	pr_debug("%s on clock %s\n", __func__, c->name);

	if (c->flags & PERIPH_NO_ENB)
		return;

	spin_lock_irqsave(&periph_refcount_lock, flags);

	if (c->refcnt)
		tegra_periph_clk_enable_refcount[c->u.periph.clk_num]--;

	if (tegra_periph_clk_enable_refcount[c->u.periph.clk_num] == 0) {
		/* If peripheral is in the APB bus then read the APB bus to
		 * flush the write operation in apb bus. This will avoid the
		 * peripheral access after disabling clock*/
		if (c->flags & PERIPH_ON_APB)
			val = chipid_readl();

		clk_writel_delay(
			PERIPH_CLK_TO_BIT(c), PERIPH_CLK_TO_ENB_CLR_REG(c));
	}
	spin_unlock_irqrestore(&periph_refcount_lock, flags);
}

static void tegra11_periph_clk_reset(struct clk *c, bool assert)
{
	unsigned long val;
	pr_debug("%s %s on clock %s\n", __func__,
		 assert ? "assert" : "deassert", c->name);

	if (c->flags & PERIPH_NO_ENB)
		return;

	if (!(c->flags & PERIPH_NO_RESET)) {
		if (assert) {
			/* If peripheral is in the APB bus then read the APB
			 * bus to flush the write operation in apb bus. This
			 * will avoid the peripheral access after disabling
			 * clock */
			if (c->flags & PERIPH_ON_APB)
				val = chipid_readl();

			clk_writel(PERIPH_CLK_TO_BIT(c),
				   PERIPH_CLK_TO_RST_SET_REG(c));
		} else
			clk_writel(PERIPH_CLK_TO_BIT(c),
				   PERIPH_CLK_TO_RST_CLR_REG(c));
	}
}

static int tegra11_periph_clk_set_parent(struct clk *c, struct clk *p)
{
	u32 val;
	const struct clk_mux_sel *sel;
	pr_debug("%s: %s %s\n", __func__, c->name, p->name);

	if (!(c->flags & MUX))
		return (p == c->parent) ? 0 : (-EINVAL);

	for (sel = c->inputs; sel->input != NULL; sel++) {
		if (sel->input == p) {
			val = clk_readl(c->reg);
			val &= ~periph_clk_source_mask(c);
			val |= (sel->value << periph_clk_source_shift(c));

			if (c->refcnt)
				clk_enable(p);

			clk_writel_delay(val, c->reg);

			if (c->refcnt && c->parent)
				clk_disable(c->parent);

			clk_reparent(c, p);
			return 0;
		}
	}

	return -EINVAL;
}

static int tegra11_periph_clk_set_rate(struct clk *c, unsigned long rate)
{
	u32 val;
	int divider;
	unsigned long parent_rate = clk_get_rate(c->parent);

	if (c->flags & DIV_U71) {
		divider = clk_div71_get_divider(
			parent_rate, rate, c->flags, ROUND_DIVIDER_UP);
		if (divider >= 0) {
			val = clk_readl(c->reg);
			val &= ~PERIPH_CLK_SOURCE_DIVU71_MASK;
			val |= divider;
			clk_writel_delay(val, c->reg);
			c->div = divider + 2;
			c->mul = 2;
			return 0;
		}
	} else if (c->flags & DIV_U151) {
		divider = clk_div151_get_divider(
			parent_rate, rate, c->flags, ROUND_DIVIDER_UP);
		if (divider >= 0) {
			val = clk_readl(c->reg);
			val &= ~PERIPH_CLK_SOURCE_DIVU16_MASK;
			val |= divider;
			if (c->flags & DIV_U151_UART) {
				if (divider)
					val |= PERIPH_CLK_UART_DIV_ENB;
				else
					val &= ~PERIPH_CLK_UART_DIV_ENB;
			}
			clk_writel_delay(val, c->reg);
			c->div = divider + 2;
			c->mul = 2;
			return 0;
		}
	} else if (c->flags & DIV_U16) {
		divider = clk_div16_get_divider(parent_rate, rate);
		if (divider >= 0) {
			val = clk_readl(c->reg);
			val &= ~PERIPH_CLK_SOURCE_DIVU16_MASK;
			val |= divider;
			clk_writel_delay(val, c->reg);
			c->div = divider + 1;
			c->mul = 1;
			return 0;
		}
	} else if (parent_rate <= rate) {
		c->div = 1;
		c->mul = 1;
		return 0;
	}
	return -EINVAL;
}

static long tegra11_periph_clk_round_rate(struct clk *c,
	unsigned long rate)
{
	int divider;
	unsigned long parent_rate = clk_get_rate(c->parent);
	pr_debug("%s: %s %lu\n", __func__, c->name, rate);

	if (c->flags & DIV_U71) {
		divider = clk_div71_get_divider(
			parent_rate, rate, c->flags, ROUND_DIVIDER_UP);
		if (divider < 0)
			return divider;

		return DIV_ROUND_UP(parent_rate * 2, divider + 2);
	} else if (c->flags & DIV_U151) {
		divider = clk_div151_get_divider(
			parent_rate, rate, c->flags, ROUND_DIVIDER_UP);
		if (divider < 0)
			return divider;

		return DIV_ROUND_UP(parent_rate * 2, divider + 2);
	} else if (c->flags & DIV_U16) {
		divider = clk_div16_get_divider(parent_rate, rate);
		if (divider < 0)
			return divider;
		return DIV_ROUND_UP(parent_rate, divider + 1);
	}
	return -EINVAL;
}

static struct clk_ops tegra_periph_clk_ops = {
	.init			= &tegra11_periph_clk_init,
	.enable			= &tegra11_periph_clk_enable,
	.disable		= &tegra11_periph_clk_disable,
	.set_parent		= &tegra11_periph_clk_set_parent,
	.set_rate		= &tegra11_periph_clk_set_rate,
	.round_rate		= &tegra11_periph_clk_round_rate,
	.reset			= &tegra11_periph_clk_reset,
};


/* Periph extended clock configuration ops */
static int
tegra11_vi_clk_cfg_ex(struct clk *c, enum tegra_clk_ex_param p, u32 setting)
{
	if (p == TEGRA_CLK_VI_INP_SEL) {
		u32 val = clk_readl(c->reg);
		val &= ~PERIPH_CLK_VI_SEL_EX_MASK;
		val |= (setting << PERIPH_CLK_VI_SEL_EX_SHIFT) &
			PERIPH_CLK_VI_SEL_EX_MASK;
		clk_writel(val, c->reg);
		return 0;
	}
	return -EINVAL;
}

static struct clk_ops tegra_vi_clk_ops = {
	.init			= &tegra11_periph_clk_init,
	.enable			= &tegra11_periph_clk_enable,
	.disable		= &tegra11_periph_clk_disable,
	.set_parent		= &tegra11_periph_clk_set_parent,
	.set_rate		= &tegra11_periph_clk_set_rate,
	.round_rate		= &tegra11_periph_clk_round_rate,
	.clk_cfg_ex		= &tegra11_vi_clk_cfg_ex,
	.reset			= &tegra11_periph_clk_reset,
};

static int
tegra11_nand_clk_cfg_ex(struct clk *c, enum tegra_clk_ex_param p, u32 setting)
{
	if (p == TEGRA_CLK_NAND_PAD_DIV2_ENB) {
		u32 val = clk_readl(c->reg);
		if (setting)
			val |= PERIPH_CLK_NAND_DIV_EX_ENB;
		else
			val &= ~PERIPH_CLK_NAND_DIV_EX_ENB;
		clk_writel(val, c->reg);
		return 0;
	}
	return -EINVAL;
}

static struct clk_ops tegra_nand_clk_ops = {
	.init			= &tegra11_periph_clk_init,
	.enable			= &tegra11_periph_clk_enable,
	.disable		= &tegra11_periph_clk_disable,
	.set_parent		= &tegra11_periph_clk_set_parent,
	.set_rate		= &tegra11_periph_clk_set_rate,
	.round_rate		= &tegra11_periph_clk_round_rate,
	.clk_cfg_ex		= &tegra11_nand_clk_cfg_ex,
	.reset			= &tegra11_periph_clk_reset,
};


static int
tegra11_dtv_clk_cfg_ex(struct clk *c, enum tegra_clk_ex_param p, u32 setting)
{
	if (p == TEGRA_CLK_DTV_INVERT) {
		u32 val = clk_readl(c->reg);
		if (setting)
			val |= PERIPH_CLK_DTV_POLARITY_INV;
		else
			val &= ~PERIPH_CLK_DTV_POLARITY_INV;
		clk_writel(val, c->reg);
		return 0;
	}
	return -EINVAL;
}

static struct clk_ops tegra_dtv_clk_ops = {
	.init			= &tegra11_periph_clk_init,
	.enable			= &tegra11_periph_clk_enable,
	.disable		= &tegra11_periph_clk_disable,
	.set_parent		= &tegra11_periph_clk_set_parent,
	.set_rate		= &tegra11_periph_clk_set_rate,
	.round_rate		= &tegra11_periph_clk_round_rate,
	.clk_cfg_ex		= &tegra11_dtv_clk_cfg_ex,
	.reset			= &tegra11_periph_clk_reset,
};

static int tegra11_dsi_clk_set_parent(struct clk *c, struct clk *p)
{
	const struct clk_mux_sel *sel;
	struct clk *d = tegra_get_clock_by_name("pll_d");
	if (c->reg != d->reg)
		d = tegra_get_clock_by_name("pll_d2");

	pr_debug("%s: %s %s\n", __func__, c->name, p->name);

	for (sel = c->inputs; sel->input != NULL; sel++) {
		if (sel->input == p) {
			if (c->refcnt)
				clk_enable(p);

			/* The DSI parent selection bit is in PLLD base
			   register - can not do direct r-m-w, must be
			   protected by PLLD lock */
			tegra_clk_cfg_ex(
				d, TEGRA_CLK_PLLD_MIPI_MUX_SEL, sel->value);

			if (c->refcnt && c->parent)
				clk_disable(c->parent);

			clk_reparent(c, p);
			return 0;
		}
	}

	return -EINVAL;
}

static struct clk_ops tegra_dsi_clk_ops = {
	.init			= &tegra11_periph_clk_init,
	.enable			= &tegra11_periph_clk_enable,
	.disable		= &tegra11_periph_clk_disable,
	.set_parent		= &tegra11_dsi_clk_set_parent,
	.set_rate		= &tegra11_periph_clk_set_rate,
	.round_rate		= &tegra11_periph_clk_round_rate,
	.reset			= &tegra11_periph_clk_reset,
};

/* pciex clock support only reset function */
static struct clk_ops tegra_pciex_clk_ops = {
	.reset    = tegra11_periph_clk_reset,
};

/* Output clock ops */

static DEFINE_SPINLOCK(clk_out_lock);

static void tegra11_clk_out_init(struct clk *c)
{
	const struct clk_mux_sel *mux = 0;
	const struct clk_mux_sel *sel;
	u32 val = pmc_readl(c->reg);

	c->state = (val & (0x1 << c->u.periph.clk_num)) ? ON : OFF;
	c->mul = 1;
	c->div = 1;

	for (sel = c->inputs; sel->input != NULL; sel++) {
		if (((val & periph_clk_source_mask(c)) >>
		     periph_clk_source_shift(c)) == sel->value)
			mux = sel;
	}
	BUG_ON(!mux);
	c->parent = mux->input;
}

static int tegra11_clk_out_enable(struct clk *c)
{
	u32 val;
	unsigned long flags;

	pr_debug("%s on clock %s\n", __func__, c->name);

	spin_lock_irqsave(&clk_out_lock, flags);
	val = pmc_readl(c->reg);
	val |= (0x1 << c->u.periph.clk_num);
	pmc_writel(val, c->reg);
	spin_unlock_irqrestore(&clk_out_lock, flags);

	return 0;
}

static void tegra11_clk_out_disable(struct clk *c)
{
	u32 val;
	unsigned long flags;

	pr_debug("%s on clock %s\n", __func__, c->name);

	spin_lock_irqsave(&clk_out_lock, flags);
	val = pmc_readl(c->reg);
	val &= ~(0x1 << c->u.periph.clk_num);
	pmc_writel(val, c->reg);
	spin_unlock_irqrestore(&clk_out_lock, flags);
}

static int tegra11_clk_out_set_parent(struct clk *c, struct clk *p)
{
	u32 val;
	unsigned long flags;
	const struct clk_mux_sel *sel;

	pr_debug("%s: %s %s\n", __func__, c->name, p->name);

	for (sel = c->inputs; sel->input != NULL; sel++) {
		if (sel->input == p) {
			if (c->refcnt)
				clk_enable(p);

			spin_lock_irqsave(&clk_out_lock, flags);
			val = pmc_readl(c->reg);
			val &= ~periph_clk_source_mask(c);
			val |= (sel->value << periph_clk_source_shift(c));
			pmc_writel(val, c->reg);
			spin_unlock_irqrestore(&clk_out_lock, flags);

			if (c->refcnt && c->parent)
				clk_disable(c->parent);

			clk_reparent(c, p);
			return 0;
		}
	}
	return -EINVAL;
}

static struct clk_ops tegra_clk_out_ops = {
	.init			= &tegra11_clk_out_init,
	.enable			= &tegra11_clk_out_enable,
	.disable		= &tegra11_clk_out_disable,
	.set_parent		= &tegra11_clk_out_set_parent,
};


/* External memory controller clock ops */
static void tegra11_emc_clk_init(struct clk *c)
{
	tegra11_periph_clk_init(c);
	tegra_emc_dram_type_init(c);
	c->max_rate = clk_get_rate(c->parent);
}

static long tegra11_emc_clk_round_rate(struct clk *c, unsigned long rate)
{
	long new_rate = rate;

	new_rate = tegra_emc_round_rate(new_rate);
	if (new_rate < 0)
		new_rate = c->max_rate;

	return new_rate;
}

static int tegra11_emc_clk_set_rate(struct clk *c, unsigned long rate)
{
	int ret;
	u32 div_value;
	struct clk *p;

	/* The tegra11x memory controller has an interlock with the clock
	 * block that allows memory shadowed registers to be updated,
	 * and then transfer them to the main registers at the same
	 * time as the clock update without glitches. During clock change
	 * operation both clock parent and divider may change simultaneously
	 * to achieve requested rate. */
	p = tegra_emc_predict_parent(rate, &div_value);
	div_value += 2;		/* emc has fractional DIV_U71 divider */
	if (!p)
		return -EINVAL;

	if (p == c->parent) {
		if (div_value == c->div)
			return 0;
	} else if (c->refcnt)
		clk_enable(p);

	ret = tegra_emc_set_rate(rate);
	if (ret < 0)
		return ret;

	if (p != c->parent) {
		if(c->refcnt && c->parent)
			clk_disable(c->parent);
		clk_reparent(c, p);
	}
	c->div = div_value;
	c->mul = 2;
	return 0;
}

static struct clk_ops tegra_emc_clk_ops = {
	.init			= &tegra11_emc_clk_init,
	.enable			= &tegra11_periph_clk_enable,
	.disable		= &tegra11_periph_clk_disable,
	.set_rate		= &tegra11_emc_clk_set_rate,
	.round_rate		= &tegra11_emc_clk_round_rate,
	.reset			= &tegra11_periph_clk_reset,
	.shared_bus_update	= &tegra11_clk_shared_bus_update,
};

/* Clock doubler ops (non-atomic shared register access) */
static DEFINE_SPINLOCK(doubler_lock);

static void tegra11_clk_double_init(struct clk *c)
{
	u32 val = clk_readl(c->reg);
	c->mul = val & (0x1 << c->reg_shift) ? 1 : 2;
	c->div = 1;
	c->state = ON;
	if (!(clk_readl(PERIPH_CLK_TO_ENB_REG(c)) & PERIPH_CLK_TO_BIT(c)))
		c->state = OFF;
};

static int tegra11_clk_double_set_rate(struct clk *c, unsigned long rate)
{
	u32 val;
	unsigned long parent_rate = clk_get_rate(c->parent);
	unsigned long flags;

	if (rate == parent_rate) {
		spin_lock_irqsave(&doubler_lock, flags);
		val = clk_readl(c->reg) | (0x1 << c->reg_shift);
		clk_writel(val, c->reg);
		c->mul = 1;
		c->div = 1;
		spin_unlock_irqrestore(&doubler_lock, flags);
		return 0;
	} else if (rate == 2 * parent_rate) {
		spin_lock_irqsave(&doubler_lock, flags);
		val = clk_readl(c->reg) & (~(0x1 << c->reg_shift));
		clk_writel(val, c->reg);
		c->mul = 2;
		c->div = 1;
		spin_unlock_irqrestore(&doubler_lock, flags);
		return 0;
	}
	return -EINVAL;
}

static struct clk_ops tegra_clk_double_ops = {
	.init			= &tegra11_clk_double_init,
	.enable			= &tegra11_periph_clk_enable,
	.disable		= &tegra11_periph_clk_disable,
	.set_rate		= &tegra11_clk_double_set_rate,
};

/* Audio sync clock ops */
static int tegra11_sync_source_set_rate(struct clk *c, unsigned long rate)
{
	c->rate = rate;
	return 0;
}

static struct clk_ops tegra_sync_source_ops = {
	.set_rate		= &tegra11_sync_source_set_rate,
};

static void tegra11_audio_sync_clk_init(struct clk *c)
{
	int source;
	const struct clk_mux_sel *sel;
	u32 val = clk_readl(c->reg);
	c->state = (val & AUDIO_SYNC_DISABLE_BIT) ? OFF : ON;
	source = val & AUDIO_SYNC_SOURCE_MASK;
	for (sel = c->inputs; sel->input != NULL; sel++)
		if (sel->value == source)
			break;
	BUG_ON(sel->input == NULL);
	c->parent = sel->input;
}

static int tegra11_audio_sync_clk_enable(struct clk *c)
{
	u32 val = clk_readl(c->reg);
	clk_writel((val & (~AUDIO_SYNC_DISABLE_BIT)), c->reg);
	return 0;
}

static void tegra11_audio_sync_clk_disable(struct clk *c)
{
	u32 val = clk_readl(c->reg);
	clk_writel((val | AUDIO_SYNC_DISABLE_BIT), c->reg);
}

static int tegra11_audio_sync_clk_set_parent(struct clk *c, struct clk *p)
{
	u32 val;
	const struct clk_mux_sel *sel;
	for (sel = c->inputs; sel->input != NULL; sel++) {
		if (sel->input == p) {
			val = clk_readl(c->reg);
			val &= ~AUDIO_SYNC_SOURCE_MASK;
			val |= sel->value;

			if (c->refcnt)
				clk_enable(p);

			clk_writel(val, c->reg);

			if (c->refcnt && c->parent)
				clk_disable(c->parent);

			clk_reparent(c, p);
			return 0;
		}
	}

	return -EINVAL;
}

static struct clk_ops tegra_audio_sync_clk_ops = {
	.init       = tegra11_audio_sync_clk_init,
	.enable     = tegra11_audio_sync_clk_enable,
	.disable    = tegra11_audio_sync_clk_disable,
	.set_parent = tegra11_audio_sync_clk_set_parent,
};

/* cml0 (pcie), and cml1 (sata) clock ops */
static void tegra11_cml_clk_init(struct clk *c)
{
	u32 val = clk_readl(c->reg);
	c->state = val & (0x1 << c->u.periph.clk_num) ? ON : OFF;
}

static int tegra11_cml_clk_enable(struct clk *c)
{
	u32 val = clk_readl(c->reg);
	val |= (0x1 << c->u.periph.clk_num);
	clk_writel(val, c->reg);
	return 0;
}

static void tegra11_cml_clk_disable(struct clk *c)
{
	u32 val = clk_readl(c->reg);
	val &= ~(0x1 << c->u.periph.clk_num);
	clk_writel(val, c->reg);
}

static struct clk_ops tegra_cml_clk_ops = {
	.init			= &tegra11_cml_clk_init,
	.enable			= &tegra11_cml_clk_enable,
	.disable		= &tegra11_cml_clk_disable,
};


/* cbus ops */
/*
 * Some clocks require dynamic re-locking of source PLL in order to
 * achieve frequency scaling granularity that matches characterized
 * core voltage steps. The cbus clock creates a shared bus that
 * provides a virtual root for such clocks to hide and synchronize
 * parent PLL re-locking as well as backup operations.
*/

static void tegra11_clk_cbus_init(struct clk *c)
{
	c->state = OFF;
	c->set = true;
}

static int tegra11_clk_cbus_enable(struct clk *c)
{
	return 0;
}

static long tegra11_clk_cbus_round_rate(struct clk *c, unsigned long rate)
{
	int i;

	if (!c->dvfs)
		return rate;

	/* update min now, since no dvfs table was available during init
	   (skip placeholder entries set to 1 kHz) */
	if (!c->min_rate) {
		for (i = 0; i < (c->dvfs->num_freqs - 1); i++) {
			if (c->dvfs->freqs[i] > 1 * c->dvfs->freqs_mult) {
				c->min_rate = c->dvfs->freqs[i];
				break;
			}
		}
		BUG_ON(!c->min_rate);
	}
	rate = max(rate, c->min_rate);

	for (i = 0; i < (c->dvfs->num_freqs - 1); i++) {
		unsigned long f = c->dvfs->freqs[i];
		int mv = c->dvfs->millivolts[i];
		if ((f >= rate) || (mv >= c->dvfs->max_millivolts))
			break;
	}
	return c->dvfs->freqs[i];
}

static int cbus_switch_one(struct clk *c, struct clk *p, u32 div, bool abort)
{
	int ret = 0;

	/* set new divider if it is bigger than the current one */
	if (c->div < c->mul * div) {
		ret = clk_set_div(c, div);
		if (ret) {
			pr_err("%s: failed to set %s clock divider %u: %d\n",
			       __func__, c->name, div, ret);
			if (abort)
				return ret;
		}
	}

	if (c->parent != p) {
		ret = clk_set_parent(c, p);
		if (ret) {
			pr_err("%s: failed to set %s clock parent %s: %d\n",
			       __func__, c->name, p->name, ret);
			if (abort)
				return ret;
		}
	}

	/* set new divider if it is smaller than the current one */
	if (c->div > c->mul * div) {
		ret = clk_set_div(c, div);
		if (ret)
			pr_err("%s: failed to set %s clock divider %u: %d\n",
			       __func__, c->name, div, ret);
	}

	return ret;
}

static int cbus_backup(struct clk *c)
{
	int ret;
	struct clk *user;

	list_for_each_entry(user, &c->shared_bus_list,
			u.shared_bus_user.node) {
		struct clk *client = user->u.shared_bus_user.client;
		if (client && (client->state == ON) &&
		    (client->parent == c->parent)) {
			ret = cbus_switch_one(client,
					      c->shared_bus_backup.input,
					      c->shared_bus_backup.value *
					      user->div, true);
			if (ret)
				return ret;
		}
	}
	return 0;
}

static int cbus_dvfs_set_rate(struct clk *c, unsigned long rate)
{
	int ret;
	struct clk *user;

	list_for_each_entry(user, &c->shared_bus_list,
			u.shared_bus_user.node) {
		struct clk *client =  user->u.shared_bus_user.client;
		if (client && client->refcnt && (client->parent == c->parent)) {
			ret = tegra_dvfs_set_rate(c, rate);
			if (ret)
				return ret;
		}
	}
	return 0;
}

static void cbus_restore(struct clk *c)
{
	struct clk *user;

	list_for_each_entry(user, &c->shared_bus_list,
			u.shared_bus_user.node) {
		if (user->u.shared_bus_user.client)
			cbus_switch_one(user->u.shared_bus_user.client,
					c->parent, c->div * user->div, false);
	}
}

static int get_next_backup_div(struct clk *c, unsigned long rate)
{
	u32 div = c->div;
	unsigned long backup_rate = clk_get_rate(c->shared_bus_backup.input);

	rate = max(rate, clk_get_rate_locked(c));
	if ((u64)rate * div < backup_rate)
		div = DIV_ROUND_UP(backup_rate, rate);

	BUG_ON(!div);
	return div;
}

static int tegra11_clk_cbus_set_rate(struct clk *c, unsigned long rate)
{
	int ret;
	bool dramp;

	if (rate == 0)
		return 0;

	ret = clk_enable(c->parent);
	if (ret) {
		pr_err("%s: failed to enable %s clock: %d\n",
		       __func__, c->name, ret);
		return ret;
	}

	dramp = tegra11_is_dyn_ramp(c->parent, rate * c->div, false);
	if (!dramp) {
		c->shared_bus_backup.value = get_next_backup_div(c, rate);
		ret = cbus_backup(c);
		if (ret)
			goto out;
	}

	ret = clk_set_rate(c->parent, rate * c->div);
	if (ret) {
		pr_err("%s: failed to set %s clock rate %lu: %d\n",
		       __func__, c->name, rate, ret);
		goto out;
	}

	/* Safe voltage setting is taken care of by cbus clock dvfs; the call
	 * below only records requirements for each enabled client.
	 */
	if (dramp)
		ret = cbus_dvfs_set_rate(c, rate);

	cbus_restore(c);

out:
	clk_disable(c->parent);
	return ret;
}

static inline bool cbus_user_is_slower(struct clk *a, struct clk *b)
{
	return a->u.shared_bus_user.client->max_rate * a->div <
		b->u.shared_bus_user.client->max_rate * b->div;
}

static inline bool cbus_user_request_is_lower(struct clk *a, struct clk *b)
{
	return a->u.shared_bus_user.rate * a->div <
		b->u.shared_bus_user.rate * b->div;
}

static inline void cbus_move_enabled_user(
	struct clk *user, struct clk *dst, struct clk *src)
{
	clk_enable(dst);
	list_move_tail(&user->u.shared_bus_user.node, &dst->shared_bus_list);
	clk_disable(src);
	clk_reparent(user, dst);
}

#ifdef CONFIG_TEGRA_DYNAMIC_CBUS
static int tegra11_clk_cbus_update(struct clk *bus)
{
	int ret, mv;
	struct clk *c;
	struct clk *slow = NULL;
	struct clk *top = NULL;

	unsigned long rate = bus->min_rate;
	unsigned long ceiling = bus->max_rate;

	if (detach_shared_bus)
		return 0;

	list_for_each_entry(c, &bus->shared_bus_list,
			u.shared_bus_user.node) {
		/* Ignore requests from disabled users */
		if (c->u.shared_bus_user.enabled) {
			unsigned long request_rate = c->u.shared_bus_user.rate *
				(c->div ? : 1);

			switch (c->u.shared_bus_user.mode) {
			case SHARED_CEILING:
				ceiling = min(request_rate, ceiling);
				break;
			case SHARED_AUTO:
				break;
			case SHARED_FLOOR:
			default:
				if (rate < request_rate) {
					rate = request_rate;
					top = c;
				}
			}
			if (c->u.shared_bus_user.client &&
				(!slow || cbus_user_is_slower(c, slow)))
				slow = c;
		}
	}

	/* use dvfs table of the slowest enabled client as cbus dvfs table */
	if (bus->dvfs && slow && (slow != bus->u.cbus.slow_user)) {
		int i;
		unsigned long *dest = &bus->dvfs->freqs[0];
		unsigned long *src =
			&slow->u.shared_bus_user.client->dvfs->freqs[0];
		if (slow->div > 1)
			for (i = 0; i < bus->dvfs->num_freqs; i++)
				dest[i] = src[i] * slow->div;
		else
			memcpy(dest, src, sizeof(*dest) * bus->dvfs->num_freqs);
	}

	/* update bus state variables and rate */
	bus->u.cbus.slow_user = slow;
	bus->u.cbus.top_user = top;
	rate = min(rate, ceiling);

	rate = bus->ops->round_rate(bus, rate);
	mv = tegra_dvfs_predict_millivolts(bus, rate);
	if (IS_ERR_VALUE(mv))
		return -EINVAL;

	if (bus->dvfs) {
		mv -= bus->dvfs->cur_millivolts;
		if (bus->refcnt && (mv > 0)) {
			ret = tegra_dvfs_set_rate(bus, rate);
			if (ret)
				return ret;
		}
	}

	ret = bus->ops->set_rate(bus, rate);
	if (ret)
		return ret;

	if (bus->dvfs) {
		if (bus->refcnt && (mv <= 0)) {
			ret = tegra_dvfs_set_rate(bus, rate);
			if (ret)
				return ret;
		}
	}

	return 0;
};
#endif

static int tegra11_clk_cbus_migrate_users(struct clk *user)
{
#ifdef CONFIG_TEGRA_MIGRATE_CBUS_USERS
	struct clk *src_bus, *dst_bus, *top_user, *c;
	struct list_head *pos, *n;

	if (!user->u.shared_bus_user.client || !user->inputs)
		return 0;

	/* Dual cbus on Tegra11 */
	src_bus = user->inputs[0].input;
	dst_bus = user->inputs[1].input;

	if (!src_bus->u.cbus.top_user && !dst_bus->u.cbus.top_user)
		return 0;

	/* Make sure top user on the source bus is requesting highest rate */
	if (!src_bus->u.cbus.top_user || (dst_bus->u.cbus.top_user &&
		cbus_user_request_is_lower(src_bus->u.cbus.top_user,
					   dst_bus->u.cbus.top_user)))
		swap(src_bus, dst_bus);

	/* If top user is the slow one on its own (source) bus, do nothing */
	top_user = src_bus->u.cbus.top_user;
	BUG_ON(!top_user->u.shared_bus_user.client);
	if (top_user == src_bus->u.cbus.slow_user)
		return 0;

	/* If source bus top user is slower than all users on destination bus,
	   move top user; otherwise move all users slower than the top one */
	if (!dst_bus->u.cbus.slow_user ||
	    cbus_user_is_slower(top_user, dst_bus->u.cbus.slow_user)) {
		cbus_move_enabled_user(top_user, dst_bus, src_bus);
	} else {
		list_for_each_safe(pos, n, &src_bus->shared_bus_list) {
			c = list_entry(pos, struct clk, u.shared_bus_user.node);
			if (c->u.shared_bus_user.enabled &&
			    c->u.shared_bus_user.client &&
			    cbus_user_is_slower(c, top_user))
				cbus_move_enabled_user(c, dst_bus, src_bus);
		}
	}

	/* Update destination bus 1st (move clients), then source */
	tegra_clk_shared_bus_update(dst_bus);
	tegra_clk_shared_bus_update(src_bus);
#endif
	return 0;
}

static struct clk_ops tegra_clk_cbus_ops = {
	.init = tegra11_clk_cbus_init,
	.enable = tegra11_clk_cbus_enable,
	.set_rate = tegra11_clk_cbus_set_rate,
	.round_rate = tegra11_clk_cbus_round_rate,
#ifdef CONFIG_TEGRA_DYNAMIC_CBUS
	.shared_bus_update = tegra11_clk_cbus_update,
#else
	.shared_bus_update = tegra11_clk_shared_bus_update,
#endif
};

/* shared bus ops */
/*
 * Some clocks may have multiple downstream users that need to request a
 * higher clock rate.  Shared bus clocks provide a unique shared_bus_user
 * clock to each user.  The frequency of the bus is set to the highest
 * enabled shared_bus_user clock, with a minimum value set by the
 * shared bus.
 */

static int tegra11_clk_shared_bus_update(struct clk *bus)
{
	struct clk *c;
	unsigned long old_rate;
	unsigned long rate = bus->min_rate;
	unsigned long bw = 0;
	unsigned long ceiling = bus->max_rate;

	if (detach_shared_bus)
		return 0;

	list_for_each_entry(c, &bus->shared_bus_list,
			u.shared_bus_user.node) {
		/* Ignore requests from disabled users */
		if (c->u.shared_bus_user.enabled) {
			unsigned long request_rate = c->u.shared_bus_user.rate *
				(c->div ? : 1);

			switch (c->u.shared_bus_user.mode) {
			case SHARED_BW:
				bw += request_rate;
				break;
			case SHARED_CEILING:
				ceiling = min(request_rate, ceiling);
				break;
			case SHARED_AUTO:
				break;
			case SHARED_FLOOR:
			default:
				rate = max(request_rate, rate);
			}
		}
	}
	rate = min(max(rate, bw), ceiling);
	rate = clk_round_rate_locked(bus, rate);

	old_rate = clk_get_rate_locked(bus);
	if (rate == old_rate)
		return 0;

	return clk_set_rate_locked(bus, rate);
};

static int tegra_clk_shared_bus_migrate_users(struct clk *user)
{
	if (detach_shared_bus)
		return 0;

	/* Only cbus migration is supported */
	if (user->flags & PERIPH_ON_CBUS)
		return tegra11_clk_cbus_migrate_users(user);
	return -ENOSYS;
}

static void tegra_clk_shared_bus_init(struct clk *c)
{
	c->max_rate = c->parent->max_rate;
	c->u.shared_bus_user.rate = c->parent->max_rate;
	c->state = OFF;
	c->set = true;

	if (c->u.shared_bus_user.client_id) {
		c->u.shared_bus_user.client =
			tegra_get_clock_by_name(c->u.shared_bus_user.client_id);
		if (!c->u.shared_bus_user.client) {
			pr_err("%s: could not find clk %s\n", __func__,
			       c->u.shared_bus_user.client_id);
			return;
		}
		c->u.shared_bus_user.client->flags |=
			c->parent->flags & PERIPH_ON_CBUS;
		c->flags |= c->parent->flags & PERIPH_ON_CBUS;
		c->div = c->u.shared_bus_user.client_div ? : 1;
		c->mul = 1;
	}

	list_add_tail(&c->u.shared_bus_user.node,
		&c->parent->shared_bus_list);
}

/*
 * Shared bus and its children/users have reversed rate relations - user rates
 * determine bus rate. Hence switching user from one parent/bus to another may
 * change rates of both parents. Therefore we need a cross-bus lock on top of
 * individual user and bus locks. For now limit bus switch support to cansleep
 * users with cross-bus mutex only.
 */
static int tegra_clk_shared_bus_set_parent(struct clk *c, struct clk *p)
{
	const struct clk_mux_sel *sel;

	if (detach_shared_bus)
		return 0;

	if (c->parent == p)
		return 0;

	if (!(c->inputs && c->u.shared_bus_user.cross_bus_mutex &&
	      clk_cansleep(c)))
		return -ENOSYS;

	for (sel = c->inputs; sel->input != NULL; sel++) {
		if (sel->input == p)
			break;
	}
	if (!sel->input)
		return -EINVAL;

	mutex_lock(c->u.shared_bus_user.cross_bus_mutex);
	if (c->refcnt)
		clk_enable(p);

	list_move_tail(&c->u.shared_bus_user.node, &p->shared_bus_list);
	tegra_clk_shared_bus_update(p);
	tegra_clk_shared_bus_update(c->parent);

	if (c->refcnt && c->parent)
		clk_disable(c->parent);

	clk_reparent(c, p);

	mutex_unlock(c->u.shared_bus_user.cross_bus_mutex);
	return 0;
}

static int tegra_clk_shared_bus_set_rate(struct clk *c, unsigned long rate)
{
	if (c->u.shared_bus_user.cross_bus_mutex && clk_cansleep(c))
		mutex_lock(c->u.shared_bus_user.cross_bus_mutex);

	c->u.shared_bus_user.rate = rate;
	tegra_clk_shared_bus_update(c->parent);

	if (c->u.shared_bus_user.cross_bus_mutex && clk_cansleep(c)) {
		tegra_clk_shared_bus_migrate_users(c);
		mutex_unlock(c->u.shared_bus_user.cross_bus_mutex);
	}

	return 0;
}

static long tegra_clk_shared_bus_round_rate(struct clk *c, unsigned long rate)
{
	/* Defer rounding requests until aggregated. BW users must not be
	   rounded at all, others just clipped to bus range (some clients
	   may use round api to find limits) */
	if (c->u.shared_bus_user.mode != SHARED_BW) {
		if (c->div > 1)
			rate *= c->div;

		if (rate > c->parent->max_rate)
			rate = c->parent->max_rate;
		else if (rate < c->parent->min_rate)
			rate = c->parent->min_rate;

		if (c->div > 1)
			rate /= c->div;
	}
	return rate;
}

static int tegra_clk_shared_bus_enable(struct clk *c)
{
	int ret = 0;

	if (c->u.shared_bus_user.cross_bus_mutex && clk_cansleep(c))
		mutex_lock(c->u.shared_bus_user.cross_bus_mutex);

	c->u.shared_bus_user.enabled = true;
	tegra_clk_shared_bus_update(c->parent);
	if (c->u.shared_bus_user.client)
		ret = clk_enable(c->u.shared_bus_user.client);

	if (c->u.shared_bus_user.cross_bus_mutex && clk_cansleep(c)) {
		tegra_clk_shared_bus_migrate_users(c);
		mutex_unlock(c->u.shared_bus_user.cross_bus_mutex);
	}

	return ret;
}

static void tegra_clk_shared_bus_disable(struct clk *c)
{
	if (c->u.shared_bus_user.cross_bus_mutex && clk_cansleep(c))
		mutex_lock(c->u.shared_bus_user.cross_bus_mutex);

	if (c->u.shared_bus_user.client)
		clk_disable(c->u.shared_bus_user.client);
	c->u.shared_bus_user.enabled = false;
	tegra_clk_shared_bus_update(c->parent);

	if (c->u.shared_bus_user.cross_bus_mutex && clk_cansleep(c)) {
		tegra_clk_shared_bus_migrate_users(c);
		mutex_unlock(c->u.shared_bus_user.cross_bus_mutex);
	}
}

static void tegra_clk_shared_bus_reset(struct clk *c, bool assert)
{
	if (c->u.shared_bus_user.client) {
		if (c->u.shared_bus_user.client->ops &&
		    c->u.shared_bus_user.client->ops->reset)
			c->u.shared_bus_user.client->ops->reset(
				c->u.shared_bus_user.client, assert);
	}
}

static struct clk_ops tegra_clk_shared_bus_ops = {
	.init = tegra_clk_shared_bus_init,
	.enable = tegra_clk_shared_bus_enable,
	.disable = tegra_clk_shared_bus_disable,
	.set_parent = tegra_clk_shared_bus_set_parent,
	.set_rate = tegra_clk_shared_bus_set_rate,
	.round_rate = tegra_clk_shared_bus_round_rate,
	.reset = tegra_clk_shared_bus_reset,
};

/* Clock definitions */
static struct clk tegra_clk_32k = {
	.name = "clk_32k",
	.rate = 32768,
	.ops  = NULL,
	.max_rate = 32768,
};

static struct clk tegra_clk_m = {
	.name      = "clk_m",
	.flags     = ENABLE_ON_INIT,
	.ops       = &tegra_clk_m_ops,
	.reg       = 0x1fc,
	.reg_shift = 28,
	.max_rate  = 48000000,
};

static struct clk tegra_clk_m_div2 = {
	.name      = "clk_m_div2",
	.ops       = &tegra_clk_m_div_ops,
	.parent    = &tegra_clk_m,
	.mul       = 1,
	.div       = 2,
	.state     = ON,
	.max_rate  = 24000000,
};

static struct clk tegra_clk_m_div4 = {
	.name      = "clk_m_div4",
	.ops       = &tegra_clk_m_div_ops,
	.parent    = &tegra_clk_m,
	.mul       = 1,
	.div       = 4,
	.state     = ON,
	.max_rate  = 12000000,
};

static struct clk tegra_pll_ref = {
	.name      = "pll_ref",
	.flags     = ENABLE_ON_INIT,
	.ops       = &tegra_pll_ref_ops,
	.parent    = &tegra_clk_m,
	.max_rate  = 26000000,
};

static struct clk_pll_freq_table tegra_pll_c_freq_table[] = {
	{ 12000000, 600000000, 100, 1, 2},
	{ 13000000, 600000000,  92, 1, 2},	/* actual: 598.0 MHz */
	{ 16800000, 600000000,  71, 1, 2},	/* actual: 596.4 MHz */
	{ 19200000, 600000000,  62, 1, 2},	/* actual: 595.2 MHz */
	{ 26000000, 600000000,  92, 2, 2},	/* actual: 598.0 MHz */
	{ 0, 0, 0, 0, 0, 0 },
};

static struct clk tegra_pll_c = {
	.name      = "pll_c",
	.ops       = &tegra_pllxc_ops,
	.reg       = 0x80,
	.parent    = &tegra_pll_ref,
	.max_rate  = 1400000000,
	.u.pll = {
		.input_min = 12000000,
		.input_max = 800000000,
		.cf_min    = 12000000,
		.cf_max    = 19200000,	/* s/w policy, h/w capability 50 MHz */
		.vco_min   = 600000000,
		.vco_max   = 1400000000,
		.freq_table = tegra_pll_c_freq_table,
		.lock_delay = 300,
		.misc1 = 0x88 - 0x80,
		.round_p_to_pdiv = pllxc_round_p_to_pdiv,
	},
};

static struct clk tegra_pll_c_out1 = {
	.name      = "pll_c_out1",
	.ops       = &tegra_pll_div_ops,
	.flags     = DIV_U71 | PERIPH_ON_CBUS,
	.parent    = &tegra_pll_c,
	.reg       = 0x84,
	.reg_shift = 0,
	.max_rate  = 700000000,
};

static struct clk_pll_freq_table tegra_pll_cx_freq_table[] = {
	{ 12000000, 600000000, 100, 1, 2},
	{ 13000000, 600000000,  92, 1, 2},	/* actual: 598.0 MHz */
	{ 16800000, 600000000,  71, 1, 2},	/* actual: 596.4 MHz */
	{ 19200000, 600000000,  62, 1, 2},	/* actual: 595.2 MHz */
	{ 26000000, 600000000,  92, 2, 2},	/* actual: 598.0 MHz */
	{ 0, 0, 0, 0, 0, 0 },
};

static struct clk tegra_pll_c2 = {
	.name      = "pll_c2",
	.ops       = &tegra_pllcx_ops,
	.flags     = PLL_ALT_MISC_REG,
	.reg       = 0x4e8,
	.parent    = &tegra_pll_ref,
	.max_rate  = 1200000000,
	.u.pll = {
		.input_min = 12000000,
		.input_max = 48000000,
		.cf_min    = 12000000,
		.cf_max    = 19200000,
		.vco_min   = 739000000,
		.vco_max   = 1200000000,
		.freq_table = tegra_pll_cx_freq_table,
		.lock_delay = 300,
		.misc1 = 0x4f0 - 0x4e8,
		.round_p_to_pdiv = pllcx_round_p_to_pdiv,
	},
};

static struct clk tegra_pll_c3 = {
	.name      = "pll_c3",
	.ops       = &tegra_pllcx_ops,
	.flags     = PLL_ALT_MISC_REG,
	.reg       = 0x4fc,
	.parent    = &tegra_pll_ref,
	.max_rate  = 1200000000,
	.u.pll = {
		.input_min = 12000000,
		.input_max = 48000000,
		.cf_min    = 12000000,
		.cf_max    = 19200000,
		.vco_min   = 739000000,
		.vco_max   = 1200000000,
		.freq_table = tegra_pll_cx_freq_table,
		.lock_delay = 300,
		.misc1 = 0x504 - 0x4fc,
		.round_p_to_pdiv = pllcx_round_p_to_pdiv,
	},
};

static struct clk_pll_freq_table tegra_pll_m_freq_table[] = {
	{ 12000000, 800000000, 66, 1, 1},	/* actual: 792.0 MHz */
	{ 13000000, 800000000, 61, 1, 1},	/* actual: 793.0 MHz */
	{ 16800000, 800000000, 47, 1, 1},	/* actual: 789.6 MHz */
	{ 19200000, 800000000, 41, 1, 1},	/* actual: 787.2 MHz */
	{ 26000000, 800000000, 61, 2, 1},	/* actual: 793.0 MHz */
	{ 0, 0, 0, 0, 0, 0 },
};

static struct clk tegra_pll_m = {
	.name      = "pll_m",
	.flags     = PLLM,
	.ops       = &tegra_pllm_ops,
	.reg       = 0x90,
	.parent    = &tegra_pll_ref,
	.max_rate  = 800000000,
	.u.pll = {
		.input_min = 12000000,
		.input_max = 500000000,
		.cf_min    = 12000000,
		.cf_max    = 19200000,	/* s/w policy, h/w capability 50 MHz */
		.vco_min   = 400000000,
		.vco_max   = 800000000,
		.freq_table = tegra_pll_m_freq_table,
		.lock_delay = 300,
		.misc1 = 0x98 - 0x90,
		.round_p_to_pdiv = pllm_round_p_to_pdiv,
	},
};

static struct clk tegra_pll_m_out1 = {
	.name      = "pll_m_out1",
	.ops       = &tegra_pll_div_ops,
	.flags     = DIV_U71,
	.parent    = &tegra_pll_m,
	.reg       = 0x94,
	.reg_shift = 0,
	.max_rate  = 600000000,
};

static struct clk_pll_freq_table tegra_pll_p_freq_table[] = {
	{ 12000000, 216000000, 432, 12, 2, 8},
	{ 13000000, 216000000, 432, 13, 2, 8},
	{ 16800000, 216000000, 360, 14, 2, 8},
	{ 19200000, 216000000, 360, 16, 2, 8},
	{ 26000000, 216000000, 432, 26, 2, 8},
	{ 0, 0, 0, 0, 0, 0 },
};

static struct clk tegra_pll_p = {
	.name      = "pll_p",
	.flags     = ENABLE_ON_INIT | PLL_FIXED | PLL_HAS_CPCON,
	.ops       = &tegra_pllp_ops,
	.reg       = 0xa0,
	.parent    = &tegra_pll_ref,
	.max_rate  = 432000000,
	.u.pll = {
		.input_min = 2000000,
		.input_max = 31000000,
		.cf_min    = 1000000,
		.cf_max    = 6000000,
		.vco_min   = 20000000,
		.vco_max   = 1400000000,
		.freq_table = tegra_pll_p_freq_table,
		.lock_delay = 300,
	},
};

static struct clk tegra_pll_p_out1 = {
	.name      = "pll_p_out1",
	.ops       = &tegra_pll_div_ops,
	.flags     = DIV_U71 | DIV_U71_FIXED,
	.parent    = &tegra_pll_p,
	.reg       = 0xa4,
	.reg_shift = 0,
	.max_rate  = 432000000,
};

static struct clk tegra_pll_p_out2 = {
	.name      = "pll_p_out2",
	.ops       = &tegra_pll_div_ops,
	.flags     = DIV_U71 | DIV_U71_FIXED,
	.parent    = &tegra_pll_p,
	.reg       = 0xa4,
	.reg_shift = 16,
	.max_rate  = 432000000,
};

static struct clk tegra_pll_p_out3 = {
	.name      = "pll_p_out3",
	.ops       = &tegra_pll_div_ops,
	.flags     = DIV_U71 | DIV_U71_FIXED,
	.parent    = &tegra_pll_p,
	.reg       = 0xa8,
	.reg_shift = 0,
	.max_rate  = 432000000,
};

static struct clk tegra_pll_p_out4 = {
	.name      = "pll_p_out4",
	.ops       = &tegra_pll_div_ops,
	.flags     = DIV_U71 | DIV_U71_FIXED,
	.parent    = &tegra_pll_p,
	.reg       = 0xa8,
	.reg_shift = 16,
	.max_rate  = 432000000,
};

static struct clk_pll_freq_table tegra_pll_a_freq_table[] = {
	{ 9600000, 564480000, 294, 5, 1, 4},
	{ 9600000, 552960000, 288, 5, 1, 4},
	{ 9600000, 24000000,  5,   2, 1, 1},

	{ 28800000, 56448000, 49, 25, 1, 1},
	{ 28800000, 73728000, 64, 25, 1, 1},
	{ 28800000, 24000000,  5,  6, 1, 1},
	{ 0, 0, 0, 0, 0, 0 },
};

static struct clk tegra_pll_a = {
	.name      = "pll_a",
	.flags     = PLL_HAS_CPCON,
	.ops       = &tegra_pll_ops,
	.reg       = 0xb0,
	.parent    = &tegra_pll_p_out1,
	.max_rate  = 700000000,
	.u.pll = {
		.input_min = 2000000,
		.input_max = 31000000,
		.cf_min    = 1000000,
		.cf_max    = 6000000,
		.vco_min   = 20000000,
		.vco_max   = 1400000000,
		.freq_table = tegra_pll_a_freq_table,
		.lock_delay = 300,
	},
};

static struct clk tegra_pll_a_out0 = {
	.name      = "pll_a_out0",
	.ops       = &tegra_pll_div_ops,
	.flags     = DIV_U71,
	.parent    = &tegra_pll_a,
	.reg       = 0xb4,
	.reg_shift = 0,
	.max_rate  = 100000000,
};

static struct clk_pll_freq_table tegra_pll_d_freq_table[] = {
	{ 12000000, 216000000, 216, 12, 1, 4},
	{ 13000000, 216000000, 216, 13, 1, 4},
	{ 16800000, 216000000, 180, 14, 1, 4},
	{ 19200000, 216000000, 180, 16, 1, 4},
	{ 26000000, 216000000, 216, 26, 1, 4},

	{ 12000000, 594000000, 594, 12, 1, 8},
	{ 13000000, 594000000, 594, 13, 1, 8},
	{ 16800000, 594000000, 495, 14, 1, 8},
	{ 19200000, 594000000, 495, 16, 1, 8},
	{ 26000000, 594000000, 594, 26, 1, 8},

	{ 12000000, 1000000000, 1000, 12, 1, 12},
	{ 13000000, 1000000000, 1000, 13, 1, 12},
	{ 19200000, 1000000000, 625,  12, 1, 8},
	{ 26000000, 1000000000, 1000, 26, 1, 12},

	{ 0, 0, 0, 0, 0, 0 },
};

static struct clk tegra_pll_d = {
	.name      = "pll_d",
	.flags     = PLL_HAS_CPCON | PLLD,
	.ops       = &tegra_plld_ops,
	.reg       = 0xd0,
	.parent    = &tegra_pll_ref,
	.max_rate  = 1000000000,
	.u.pll = {
		.input_min = 2000000,
		.input_max = 40000000,
		.cf_min    = 1000000,
		.cf_max    = 6000000,
		.vco_min   = 40000000,
		.vco_max   = 1000000000,
		.freq_table = tegra_pll_d_freq_table,
		.lock_delay = 1000,
	},
};

static struct clk tegra_pll_d_out0 = {
	.name      = "pll_d_out0",
	.ops       = &tegra_pll_div_ops,
	.flags     = DIV_2 | PLLD,
	.parent    = &tegra_pll_d,
	.max_rate  = 500000000,
};

static struct clk tegra_pll_d2 = {
	.name      = "pll_d2",
	.flags     = PLL_HAS_CPCON | PLL_ALT_MISC_REG | PLLD,
	.ops       = &tegra_plld_ops,
	.reg       = 0x4b8,
	.parent    = &tegra_pll_ref,
	.max_rate  = 1000000000,
	.u.pll = {
		.input_min = 2000000,
		.input_max = 40000000,
		.cf_min    = 1000000,
		.cf_max    = 6000000,
		.vco_min   = 40000000,
		.vco_max   = 1000000000,
		.freq_table = tegra_pll_d_freq_table,
		.lock_delay = 1000,
	},
};

static struct clk tegra_pll_d2_out0 = {
	.name      = "pll_d2_out0",
	.ops       = &tegra_pll_div_ops,
	.flags     = DIV_2 | PLLD,
	.parent    = &tegra_pll_d2,
	.max_rate  = 500000000,
};

static struct clk_pll_freq_table tegra_pll_u_freq_table[] = {
	{ 12000000, 480000000, 960, 12, 2, 12},
	{ 13000000, 480000000, 960, 13, 2, 12},
	{ 16800000, 480000000, 400, 7,  2, 5},
	{ 19200000, 480000000, 200, 4,  2, 3},
	{ 26000000, 480000000, 960, 26, 2, 12},
	{ 0, 0, 0, 0, 0, 0 },
};

static struct clk tegra_pll_u = {
	.name      = "pll_u",
	.flags     = PLL_HAS_CPCON | PLLU,
	.ops       = &tegra_pll_ops,
	.reg       = 0xc0,
	.parent    = &tegra_pll_ref,
	.max_rate  = 480000000,
	.u.pll = {
		.input_min = 2000000,
		.input_max = 40000000,
		.cf_min    = 1000000,
		.cf_max    = 6000000,
		.vco_min   = 480000000,
		.vco_max   = 960000000,
		.freq_table = tegra_pll_u_freq_table,
		.lock_delay = 1000,
	},
};

static struct clk_pll_freq_table tegra_pll_x_freq_table[] = {
	/* 1 GHz */
	{ 12000000, 1000000000, 83, 1, 1},	/* actual: 996.0 MHz */
	{ 13000000, 1000000000, 76, 1, 1},	/* actual: 988.0 MHz */
	{ 16800000, 1000000000, 59, 1, 1},	/* actual: 991.2 MHz */
	{ 19200000, 1000000000, 52, 1, 1},	/* actual: 998.4 MHz */
	{ 26000000, 1000000000, 76, 2, 1},	/* actual: 988.0 MHz */

	{ 0, 0, 0, 0, 0, 0 },
};

static struct clk tegra_pll_x = {
	.name      = "pll_x",
	.flags     = PLL_ALT_MISC_REG | PLLX,
	.ops       = &tegra_pllxc_ops,
	.reg       = 0xe0,
	.parent    = &tegra_pll_ref,
	.max_rate  = 1800000000,
	.u.pll = {
		.input_min = 12000000,
		.input_max = 800000000,
		.cf_min    = 12000000,
		.cf_max    = 19200000,	/* s/w policy, h/w capability 50 MHz */
		.vco_min   = 700000000,
		.vco_max   = 1800000000,
		.freq_table = tegra_pll_x_freq_table,
		.lock_delay = 300,
		.misc1 = 0x510 - 0xe0,
		.round_p_to_pdiv = pllxc_round_p_to_pdiv,
	},
};

static struct clk tegra_pll_x_out0 = {
	.name      = "pll_x_out0",
	.ops       = &tegra_pll_div_ops,
	.flags     = DIV_2 | PLLX,
	.parent    = &tegra_pll_x,
	.max_rate  = 700000000,
};

/* FIXME: remove; for now, should be always checked-in as "0" */
#define USE_IRAM_TO_TEST_DFLL		0
#define USE_LP_CPU_TO_TEST_DFLL		0

static struct tegra_cl_dvfs cpu_cl_dvfs = {
#if USE_IRAM_TO_TEST_DFLL
	.cl_base = (u32)IO_ADDRESS(TEGRA_IRAM_BASE + 0x3f000),
#else
	.cl_base = (u32)IO_ADDRESS(TEGRA_CL_DVFS_BASE),
#endif
};

static struct clk tegra_dfll_cpu = {
	.name      = "dfll_cpu",
	.flags     = DFLL,
	.ops       = &tegra_dfll_ops,
	.reg	   = 0x2f4,
	.max_rate  = 1800000000,
	.u.dfll = {
		.cl_dvfs = &cpu_cl_dvfs,
	},
};

static int tegra11_dfll_cpu_late_init(void)
{
#if !USE_IRAM_TO_TEST_DFLL
#ifndef CONFIG_TEGRA_SILICON_PLATFORM
	u32 netlist, patchid;
	tegra_get_netlist_revision(&netlist, &patchid);
	if (netlist < 12) {
		pr_err("%s: CL-DVFS is not available on net %d\n",
		       __func__, netlist);
		return -ENOSYS;
	}
#endif
#endif
	return tegra_init_cl_dvfs(&tegra_dfll_cpu);
}
late_initcall(tegra11_dfll_cpu_late_init);

static struct clk tegra_pll_re_vco = {
	.name      = "pll_re_vco",
	.flags     = PLL_ALT_MISC_REG,
	.ops       = &tegra_pllre_ops,
	.reg       = 0x4c4,
	.parent    = &tegra_pll_ref,
	.max_rate  = 600000000,
	.u.pll = {
		.input_min = 12000000,
		.input_max = 1000000000,
		.cf_min    = 12000000,
		.cf_max    = 19200000,	/* s/w policy, h/w capability 38 MHz */
		.vco_min   = 300000000,
		.vco_max   = 600000000,
		.lock_delay = 300,
		.round_p_to_pdiv = pllre_round_p_to_pdiv,
	},
};

static struct clk tegra_pll_re_out = {
	.name      = "pll_re_out",
	.ops       = &tegra_pllre_out_ops,
	.parent    = &tegra_pll_re_vco,
	.reg       = 0x4c4,
	.max_rate  = 600000000,
};

static struct clk_pll_freq_table tegra_pll_e_freq_table[] = {
	/* PLLE special case: use cpcon field to store cml divider value */
	{ 12000000,  100000000, 150, 1,  18, 11},
	{ 216000000, 100000000, 200, 18, 24, 13},
#ifndef CONFIG_TEGRA_SILICON_PLATFORM
	{ 13000000,  100000000, 200, 1,  26, 13},
#endif
	{ 0, 0, 0, 0, 0, 0 },
};

static struct clk tegra_pll_e = {
	.name      = "pll_e",
	.flags     = PLL_ALT_MISC_REG,
	.ops       = &tegra_plle_ops,
	.reg       = 0xe8,
	.max_rate  = 100000000,
	.u.pll = {
		.input_min = 12000000,
		.input_max = 216000000,
		.cf_min    = 12000000,
		.cf_max    = 12000000,
		.vco_min   = 1200000000,
		.vco_max   = 2400000000U,
		.freq_table = tegra_pll_e_freq_table,
		.lock_delay = 300,
		.fixed_rate = 100000000,
	},
};

static struct clk tegra_cml0_clk = {
	.name      = "cml0",
	.parent    = &tegra_pll_e,
	.ops       = &tegra_cml_clk_ops,
	.reg       = PLLE_AUX,
	.max_rate  = 100000000,
	.u.periph  = {
		.clk_num = 0,
	},
};

static struct clk tegra_cml1_clk = {
	.name      = "cml1",
	.parent    = &tegra_pll_e,
	.ops       = &tegra_cml_clk_ops,
	.reg       = PLLE_AUX,
	.max_rate  = 100000000,
	.u.periph  = {
		.clk_num   = 1,
	},
};

static struct clk tegra_pciex_clk = {
	.name      = "pciex",
	.parent    = &tegra_pll_e,
	.ops       = &tegra_pciex_clk_ops,
	.max_rate  = 100000000,
	.u.periph  = {
		.clk_num   = 74,
	},
};

/* Audio sync clocks */
#define SYNC_SOURCE(_id)				\
	{						\
		.name      = #_id "_sync",		\
		.rate      = 24000000,			\
		.max_rate  = 24000000,			\
		.ops       = &tegra_sync_source_ops	\
	}
static struct clk tegra_sync_source_list[] = {
	SYNC_SOURCE(spdif_in),
	SYNC_SOURCE(i2s0),
	SYNC_SOURCE(i2s1),
	SYNC_SOURCE(i2s2),
	SYNC_SOURCE(i2s3),
	SYNC_SOURCE(i2s4),
	SYNC_SOURCE(vimclk),
};

static struct clk_mux_sel mux_d_audio_clk[] = {
	{ .input = &tegra_pll_a_out0,		.value = 0},
	{ .input = &tegra_pll_p,		.value = 0x8000},
	{ .input = &tegra_clk_m,		.value = 0xc000},
	{ .input = &tegra_sync_source_list[0],	.value = 0xE000},
	{ .input = &tegra_sync_source_list[1],	.value = 0xE001},
	{ .input = &tegra_sync_source_list[2],	.value = 0xE002},
	{ .input = &tegra_sync_source_list[3],	.value = 0xE003},
	{ .input = &tegra_sync_source_list[4],	.value = 0xE004},
	{ .input = &tegra_sync_source_list[5],	.value = 0xE005},
	{ .input = &tegra_pll_a_out0,		.value = 0xE006},
	{ .input = &tegra_sync_source_list[6],	.value = 0xE007},
	{ 0, 0 }
};

static struct clk_mux_sel mux_audio_sync_clk[] =
{
	{ .input = &tegra_sync_source_list[0],	.value = 0},
	{ .input = &tegra_sync_source_list[1],	.value = 1},
	{ .input = &tegra_sync_source_list[2],	.value = 2},
	{ .input = &tegra_sync_source_list[3],	.value = 3},
	{ .input = &tegra_sync_source_list[4],	.value = 4},
	{ .input = &tegra_sync_source_list[5],	.value = 5},
	{ .input = &tegra_pll_a_out0,		.value = 6},
	{ .input = &tegra_sync_source_list[6],	.value = 7},
	{ 0, 0 }
};

#define AUDIO_SYNC_CLK(_id, _index)			\
	{						\
		.name      = #_id,			\
		.inputs    = mux_audio_sync_clk,	\
		.reg       = 0x4A0 + (_index) * 4,	\
		.max_rate  = 24000000,			\
		.ops       = &tegra_audio_sync_clk_ops	\
	}
static struct clk tegra_clk_audio_list[] = {
	AUDIO_SYNC_CLK(audio0, 0),
	AUDIO_SYNC_CLK(audio1, 1),
	AUDIO_SYNC_CLK(audio2, 2),
	AUDIO_SYNC_CLK(audio3, 3),
	AUDIO_SYNC_CLK(audio4, 4),
	AUDIO_SYNC_CLK(audio, 5),	/* SPDIF */
};

#define AUDIO_SYNC_2X_CLK(_id, _index)				\
	{							\
		.name      = #_id "_2x",			\
		.flags     = PERIPH_NO_RESET,			\
		.max_rate  = 48000000,				\
		.ops       = &tegra_clk_double_ops,		\
		.reg       = 0x49C,				\
		.reg_shift = 24 + (_index),			\
		.parent    = &tegra_clk_audio_list[(_index)],	\
		.u.periph = {					\
			.clk_num = 113 + (_index),		\
		},						\
	}
static struct clk tegra_clk_audio_2x_list[] = {
	AUDIO_SYNC_2X_CLK(audio0, 0),
	AUDIO_SYNC_2X_CLK(audio1, 1),
	AUDIO_SYNC_2X_CLK(audio2, 2),
	AUDIO_SYNC_2X_CLK(audio3, 3),
	AUDIO_SYNC_2X_CLK(audio4, 4),
	AUDIO_SYNC_2X_CLK(audio, 5),	/* SPDIF */
};

#define MUX_I2S_SPDIF(_id, _index)					\
static struct clk_mux_sel mux_pllaout0_##_id##_2x_pllp_clkm[] = {	\
	{.input = &tegra_pll_a_out0, .value = 0},			\
	{.input = &tegra_clk_audio_2x_list[(_index)], .value = 1},	\
	{.input = &tegra_pll_p, .value = 2},				\
	{.input = &tegra_clk_m, .value = 3},				\
	{ 0, 0},							\
}
MUX_I2S_SPDIF(audio0, 0);
MUX_I2S_SPDIF(audio1, 1);
MUX_I2S_SPDIF(audio2, 2);
MUX_I2S_SPDIF(audio3, 3);
MUX_I2S_SPDIF(audio4, 4);
MUX_I2S_SPDIF(audio, 5);		/* SPDIF */

/* External clock outputs (through PMC) */
#define MUX_EXTERN_OUT(_id)						\
static struct clk_mux_sel mux_clkm_clkm2_clkm4_extern##_id[] = {	\
	{.input = &tegra_clk_m,		.value = 0},			\
	{.input = &tegra_clk_m_div2,	.value = 1},			\
	{.input = &tegra_clk_m_div4,	.value = 2},			\
	{.input = NULL,			.value = 3}, /* placeholder */	\
	{ 0, 0},							\
}
MUX_EXTERN_OUT(1);
MUX_EXTERN_OUT(2);
MUX_EXTERN_OUT(3);

static struct clk_mux_sel *mux_extern_out_list[] = {
	mux_clkm_clkm2_clkm4_extern1,
	mux_clkm_clkm2_clkm4_extern2,
	mux_clkm_clkm2_clkm4_extern3,
};

#define CLK_OUT_CLK(_id)					\
	{							\
		.name      = "clk_out_" #_id,			\
		.lookup    = {					\
			.dev_id    = "clk_out_" #_id,		\
			.con_id	   = "extern" #_id,		\
		},						\
		.ops       = &tegra_clk_out_ops,		\
		.reg       = 0x1a8,				\
		.inputs    = mux_clkm_clkm2_clkm4_extern##_id,	\
		.flags     = MUX_CLK_OUT,			\
		.max_rate  = 216000000,				\
		.u.periph = {					\
			.clk_num   = (_id - 1) * 8 + 2,		\
		},						\
	}
static struct clk tegra_clk_out_list[] = {
	CLK_OUT_CLK(1),
	CLK_OUT_CLK(2),
	CLK_OUT_CLK(3),
};

/* called after peripheral external clocks are initialized */
static void init_clk_out_mux(void)
{
	int i;
	struct clk *c;

	/* output clock con_id is the name of peripheral
	   external clock connected to input 3 of the output mux */
	for (i = 0; i < ARRAY_SIZE(tegra_clk_out_list); i++) {
		c = tegra_get_clock_by_name(
			tegra_clk_out_list[i].lookup.con_id);
		if (!c)
			pr_err("%s: could not find clk %s\n", __func__,
			       tegra_clk_out_list[i].lookup.con_id);
		mux_extern_out_list[i][3].input = c;
	}
}

/* Peripheral muxes */
static struct clk_mux_sel mux_cclk_g[] = {
	{ .input = &tegra_clk_m,	.value = 0},
	{ .input = &tegra_pll_c,	.value = 1},
	{ .input = &tegra_clk_32k,	.value = 2},
	{ .input = &tegra_pll_m,	.value = 3},
	{ .input = &tegra_pll_p,	.value = 4},
	{ .input = &tegra_pll_p_out4,	.value = 5},
	/* { .input = &tegra_pll_c2,	.value = 6}, - no use on tegra11x */
	/* { .input = &tegra_clk_c3,	.value = 7}, - no use on tegra11x */
	{ .input = &tegra_pll_x,	.value = 8},
	{ .input = &tegra_dfll_cpu,	.value = 15},
	{ 0, 0},
};

static struct clk_mux_sel mux_cclk_lp[] = {
	{ .input = &tegra_clk_m,	.value = 0},
	{ .input = &tegra_pll_c,	.value = 1},
	{ .input = &tegra_clk_32k,	.value = 2},
	{ .input = &tegra_pll_m,	.value = 3},
	{ .input = &tegra_pll_p,	.value = 4},
	{ .input = &tegra_pll_p_out4,	.value = 5},
	/* { .input = &tegra_pll_c2,	.value = 6}, - no use on tegra11x */
	/* { .input = &tegra_clk_c3,	.value = 7}, - no use on tegra11x */
	{ .input = &tegra_pll_x_out0,	.value = 8},
#if USE_LP_CPU_TO_TEST_DFLL
	{ .input = &tegra_dfll_cpu,	.value = 15},
#endif
	{ .input = &tegra_pll_x,	.value = 8 | SUPER_LP_DIV2_BYPASS},
	{ 0, 0},
};

static struct clk_mux_sel mux_sclk[] = {
	{ .input = &tegra_clk_m,	.value = 0},
	{ .input = &tegra_pll_c_out1,	.value = 1},
	{ .input = &tegra_pll_p_out4,	.value = 2},
	{ .input = &tegra_pll_p_out3,	.value = 3},
	{ .input = &tegra_pll_p_out2,	.value = 4},
	/* { .input = &tegra_clk_d,	.value = 5}, - no use on tegra11x */
	{ .input = &tegra_clk_32k,	.value = 6},
	{ .input = &tegra_pll_m_out1,	.value = 7},
	{ 0, 0},
};

static struct clk tegra_clk_cclk_g = {
	.name	= "cclk_g",
	.flags  = DIV_U71 | DIV_U71_INT | MUX,
	.inputs	= mux_cclk_g,
	.reg	= 0x368,
	.ops	= &tegra_super_ops,
	.max_rate = 1800000000,
};

static struct clk tegra_clk_cclk_lp = {
	.name	= "cclk_lp",
	.flags  = DIV_2 | DIV_U71 | DIV_U71_INT | MUX,
	.inputs	= mux_cclk_lp,
	.reg	= 0x370,
	.ops	= &tegra_super_ops,
	.max_rate = 620000000,
};

static struct clk tegra_clk_sclk = {
	.name	= "sclk",
	.inputs	= mux_sclk,
	.reg	= 0x28,
	.ops	= &tegra_super_ops,
	.max_rate = 334000000,
	.min_rate = 40000000,
};

static struct clk tegra_clk_virtual_cpu_g = {
	.name      = "cpu_g",
	.parent    = &tegra_clk_cclk_g,
	.ops       = &tegra_cpu_ops,
	.max_rate  = 1800000000,
	.u.cpu = {
		.main      = &tegra_pll_x,
		.backup    = &tegra_pll_p_out4,
		.dynamic   = &tegra_dfll_cpu,
		.mode      = MODE_G,
	},
};

static struct clk tegra_clk_virtual_cpu_lp = {
	.name      = "cpu_lp",
	.parent    = &tegra_clk_cclk_lp,
	.ops       = &tegra_cpu_ops,
	.max_rate  = 620000000,
	.u.cpu = {
		.main      = &tegra_pll_x,
		.backup    = &tegra_pll_p_out4,
#if USE_LP_CPU_TO_TEST_DFLL
		.dynamic   = &tegra_dfll_cpu,
#endif
		.mode      = MODE_LP,
	},
};

static struct clk_mux_sel mux_cpu_cmplx[] = {
	{ .input = &tegra_clk_virtual_cpu_g,	.value = 0},
	{ .input = &tegra_clk_virtual_cpu_lp,	.value = 1},
	{ 0, 0},
};

static struct clk tegra_clk_cpu_cmplx = {
	.name      = "cpu",
	.inputs    = mux_cpu_cmplx,
	.ops       = &tegra_cpu_cmplx_ops,
	.max_rate  = 1800000000,
};

static struct clk tegra_clk_cop = {
	.name      = "cop",
	.parent    = &tegra_clk_sclk,
	.ops       = &tegra_cop_ops,
	.max_rate  = 334000000,
};

static struct clk tegra_clk_hclk = {
	.name		= "hclk",
	.flags		= DIV_BUS,
	.parent		= &tegra_clk_sclk,
	.reg		= 0x30,
	.reg_shift	= 4,
	.ops		= &tegra_bus_ops,
	.max_rate       = 334000000,
	.min_rate       = 40000000,
};

static struct clk tegra_clk_pclk = {
	.name		= "pclk",
	.flags		= DIV_BUS,
	.parent		= &tegra_clk_hclk,
	.reg		= 0x30,
	.reg_shift	= 0,
	.ops		= &tegra_bus_ops,
	.max_rate       = 167000000,
	.min_rate       = 40000000,
};

static struct raw_notifier_head sbus_rate_change_nh;

static struct clk tegra_clk_sbus_cmplx = {
	.name	   = "sbus",
	.parent    = &tegra_clk_sclk,
	.ops       = &tegra_sbus_cmplx_ops,
	.u.system  = {
		.pclk = &tegra_clk_pclk,
		.hclk = &tegra_clk_hclk,
		.sclk_low = &tegra_pll_p_out2,
		.sclk_high = &tegra_pll_m_out1,
	},
	.rate_change_nh = &sbus_rate_change_nh,
};

static struct clk tegra_clk_blink = {
	.name		= "blink",
	.parent		= &tegra_clk_32k,
	.reg		= 0x40,
	.ops		= &tegra_blink_clk_ops,
	.max_rate	= 32768,
};


/* Multimedia modules muxes */
static struct clk_mux_sel mux_pllm_pllc2_c_c3_pllp_plla[] = {
	{ .input = &tegra_pll_m,  .value = 0},
	{ .input = &tegra_pll_c2, .value = 1},
	{ .input = &tegra_pll_c,  .value = 2},
	{ .input = &tegra_pll_c3, .value = 3},
	{ .input = &tegra_pll_p,  .value = 4},
	{ .input = &tegra_pll_a_out0, .value = 6},
	{ 0, 0},
};

static struct clk_mux_sel mux_pllm_pllc_pllp_plla[] = {
	{ .input = &tegra_pll_m, .value = 0},
	{ .input = &tegra_pll_c, .value = 1},
	{ .input = &tegra_pll_p, .value = 2},
	{ .input = &tegra_pll_a_out0, .value = 3},
	{ 0, 0},
};


/* EMC muxes */
/* FIXME: update main EMC mux to match h/w, and add EMC latency mux*/
static struct clk_mux_sel mux_pllm_pllc_pllp_clkm[] = {
	{ .input = &tegra_pll_m, .value = 0},
	/* { .input = &tegra_pll_c, .value = 1}, not used on tegra11x */
	{ .input = &tegra_pll_p, .value = 2},
	{ .input = &tegra_clk_m, .value = 3},
	{ 0, 0},
};


/* Display subsystem muxes */
static struct clk_mux_sel mux_pllp_pllm_plld_plla_pllc_plld2_clkm[] = {
	{.input = &tegra_pll_p, .value = 0},
	{.input = &tegra_pll_m, .value = 1},
	{.input = &tegra_pll_d_out0, .value = 2},
	{.input = &tegra_pll_a_out0, .value = 3},
	{.input = &tegra_pll_c, .value = 4},
	{.input = &tegra_pll_d2_out0, .value = 5},
	{.input = &tegra_clk_m, .value = 6},
	{ 0, 0},
};

static struct clk_mux_sel mux_plld_out0_plld2_out0[] = {
	{ .input = &tegra_pll_d_out0,  .value = 0},
	{ .input = &tegra_pll_d2_out0, .value = 1},
	{ 0, 0},
};

/* Peripheral muxes */
static struct clk_mux_sel mux_pllp_pllc2_c_c3_pllm_clkm[] = {
	{ .input = &tegra_pll_p,  .value = 0},
	{ .input = &tegra_pll_c2, .value = 1},
	{ .input = &tegra_pll_c,  .value = 2},
	{ .input = &tegra_pll_c3, .value = 3},
	{ .input = &tegra_pll_m,  .value = 4},
	{ .input = &tegra_clk_m,  .value = 6},
	{ 0, 0},
};

static struct clk_mux_sel mux_pllp_pllc_pllm_clkm[] = {
	{ .input = &tegra_pll_p, .value = 0},
	{ .input = &tegra_pll_c, .value = 1},
	{ .input = &tegra_pll_m, .value = 2},
	{ .input = &tegra_clk_m, .value = 3},
	{ 0, 0},
};

static struct clk_mux_sel mux_pllp_pllc_pllm[] = {
	{.input = &tegra_pll_p,     .value = 0},
	{.input = &tegra_pll_c,     .value = 1},
	{.input = &tegra_pll_m,     .value = 2},
	{ 0, 0},
};

static struct clk_mux_sel mux_pllp_clkm[] = {
	{ .input = &tegra_pll_p, .value = 0},
	{ .input = &tegra_clk_m, .value = 3},
	{ 0, 0},
};

static struct clk_mux_sel mux_pllp_pllc_clk32_clkm[] = {
	{.input = &tegra_pll_p,     .value = 0},
	{.input = &tegra_pll_c,     .value = 1},
	{.input = &tegra_clk_32k,   .value = 2},
	{.input = &tegra_clk_m,     .value = 3},
	{ 0, 0},
};

static struct clk_mux_sel mux_pllp_pllc_clkm_clk32[] = {
	{.input = &tegra_pll_p,     .value = 0},
	{.input = &tegra_pll_c,     .value = 1},
	{.input = &tegra_clk_m,     .value = 2},
	{.input = &tegra_clk_32k,   .value = 3},
	{ 0, 0},
};

static struct clk_mux_sel mux_plla_clk32_pllp_clkm_plle[] = {
	{ .input = &tegra_pll_a_out0, .value = 0},
	{ .input = &tegra_clk_32k,    .value = 1},
	{ .input = &tegra_pll_p,      .value = 2},
	{ .input = &tegra_clk_m,      .value = 3},
	{ .input = &tegra_pll_e,      .value = 4},
	{ 0, 0},
};

/* Single clock source ("fake") muxes */
static struct clk_mux_sel mux_clk_m[] = {
	{ .input = &tegra_clk_m, .value = 0},
	{ 0, 0},
};

static struct clk_mux_sel mux_pllp_out3[] = {
	{ .input = &tegra_pll_p_out3, .value = 0},
	{ 0, 0},
};

static struct clk_mux_sel mux_clk_32k[] = {
	{ .input = &tegra_clk_32k, .value = 0},
	{ 0, 0},
};

static struct raw_notifier_head emc_rate_change_nh;

static struct clk tegra_clk_emc = {
	.name = "emc",
	.ops = &tegra_emc_clk_ops,
	.reg = 0x19c,
	.max_rate = 800000000,
	.min_rate = 25000000,
	.inputs = mux_pllm_pllc_pllp_clkm,
	.flags = MUX | DIV_U71 | PERIPH_EMC_ENB,
	.u.periph = {
		.clk_num = 57,
	},
	.rate_change_nh = &emc_rate_change_nh,
};

#ifdef CONFIG_TEGRA_DUAL_CBUS
static struct clk tegra_clk_c2bus = {
	.name      = "c2bus",
	.parent    = &tegra_pll_c2,
	.ops       = &tegra_clk_cbus_ops,
	.max_rate  = 600000000,
	.mul       = 1,
	.div       = 1,
	.flags     = PERIPH_ON_CBUS,
	.shared_bus_backup = {
		.input = &tegra_pll_p,
       }
};
static struct clk tegra_clk_c3bus = {
	.name      = "c3bus",
	.parent    = &tegra_pll_c3,
	.ops       = &tegra_clk_cbus_ops,
	.max_rate  = 600000000,
	.mul       = 1,
	.div       = 1,
	.flags     = PERIPH_ON_CBUS,
	.shared_bus_backup = {
		.input = &tegra_pll_p,
       }
};

static DEFINE_MUTEX(cbus_mutex);

static struct clk_mux_sel mux_clk_cbus[] = {
	{ .input = &tegra_clk_c2bus, .value = 0},
	{ .input = &tegra_clk_c3bus, .value = 1},
	{ 0, 0},
};

#define DUAL_CBUS_CLK(_name, _dev, _con, _parent, _id, _div, _mode)\
	{						\
		.name      = _name,			\
		.lookup    = {				\
			.dev_id    = _dev,		\
			.con_id    = _con,		\
		},					\
		.ops       = &tegra_clk_shared_bus_ops,	\
		.parent = _parent,			\
		.inputs = mux_clk_cbus,			\
		.flags = MUX,				\
		.u.shared_bus_user = {			\
			.client_id = _id,		\
			.client_div = _div,		\
			.mode = _mode,			\
			.cross_bus_mutex = &cbus_mutex, \
		},					\
	}

#else
static struct clk tegra_clk_cbus = {
	.name	   = "cbus",
	.parent    = &tegra_pll_c,
	.ops       = &tegra_clk_cbus_ops,
	.max_rate  = 600000000,
	.mul	   = 1,
	.div	   = 2,
	.flags     = PERIPH_ON_CBUS,
	.shared_bus_backup = {
		.input = &tegra_pll_p,
	}
};
#endif

#define PERIPH_CLK(_name, _dev, _con, _clk_num, _reg, _max, _inputs, _flags) \
	{						\
		.name      = _name,			\
		.lookup    = {				\
			.dev_id    = _dev,		\
			.con_id	   = _con,		\
		},					\
		.ops       = &tegra_periph_clk_ops,	\
		.reg       = _reg,			\
		.inputs    = _inputs,			\
		.flags     = _flags,			\
		.max_rate  = _max,			\
		.u.periph = {				\
			.clk_num   = _clk_num,		\
		},					\
	}

#define PERIPH_CLK_EX(_name, _dev, _con, _clk_num, _reg, _max, _inputs,	\
			_flags, _ops) 					\
	{						\
		.name      = _name,			\
		.lookup    = {				\
			.dev_id    = _dev,		\
			.con_id	   = _con,		\
		},					\
		.ops       = _ops,			\
		.reg       = _reg,			\
		.inputs    = _inputs,			\
		.flags     = _flags,			\
		.max_rate  = _max,			\
		.u.periph = {				\
			.clk_num   = _clk_num,		\
		},					\
	}

#define D_AUDIO_CLK(_name, _dev, _con, _clk_num, _reg, _max, _inputs, _flags) \
	{						\
		.name      = _name,			\
		.lookup    = {				\
			.dev_id    = _dev,		\
			.con_id	   = _con,		\
		},					\
		.ops       = &tegra_periph_clk_ops,	\
		.reg       = _reg,			\
		.inputs    = _inputs,			\
		.flags     = _flags,			\
		.max_rate  = _max,			\
		.u.periph = {				\
			.clk_num   = _clk_num,		\
			.src_mask  = 0xE01F << 16,	\
			.src_shift = 16,		\
		},					\
	}

#define SHARED_CLK(_name, _dev, _con, _parent, _id, _div, _mode)\
	{						\
		.name      = _name,			\
		.lookup    = {				\
			.dev_id    = _dev,		\
			.con_id    = _con,		\
		},					\
		.ops       = &tegra_clk_shared_bus_ops,	\
		.parent = _parent,			\
		.u.shared_bus_user = {			\
			.client_id = _id,		\
			.client_div = _div,		\
			.mode = _mode,			\
		},					\
	}
struct clk tegra_list_clks[] = {
	PERIPH_CLK("apbdma",	"tegra-dma",		NULL,	34,	0,	26000000,  mux_clk_m,			0),
	PERIPH_CLK("rtc",	"rtc-tegra",		NULL,	4,	0,	32768,     mux_clk_32k,			PERIPH_NO_RESET | PERIPH_ON_APB),
	PERIPH_CLK("kbc",	"tegra-kbc",		NULL,	36,	0,	32768,	   mux_clk_32k, 		PERIPH_NO_RESET | PERIPH_ON_APB),
	PERIPH_CLK("timer",	"timer",		NULL,	5,	0,	26000000,  mux_clk_m,			0),
	PERIPH_CLK("kfuse",	"kfuse-tegra",		NULL,	40,	0,	26000000,  mux_clk_m,			0),
	PERIPH_CLK("fuse",	"fuse-tegra",		"fuse",	39,	0,	26000000,  mux_clk_m,			PERIPH_ON_APB),
	PERIPH_CLK("fuse_burn",	"fuse-tegra",		"fuse_burn",	39,	0,	26000000,  mux_clk_m,		PERIPH_ON_APB),
	PERIPH_CLK("apbif",	"tegra30-ahub",		"apbif", 107,	0,	26000000,  mux_clk_m,			0),
	PERIPH_CLK("i2s0",	"tegra30-i2s.0",	NULL,	30,	0x1d8,	26000000,  mux_pllaout0_audio0_2x_pllp_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("i2s1",	"tegra30-i2s.1",	NULL,	11,	0x100,	26000000,  mux_pllaout0_audio1_2x_pllp_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("i2s2",	"tegra30-i2s.2",	NULL,	18,	0x104,	26000000,  mux_pllaout0_audio2_2x_pllp_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("i2s3",	"tegra30-i2s.3",	NULL,	101,	0x3bc,	26000000,  mux_pllaout0_audio3_2x_pllp_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("i2s4",	"tegra30-i2s.4",	NULL,	102,	0x3c0,	26000000,  mux_pllaout0_audio4_2x_pllp_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("spdif_out",	"tegra30-spdif",	"spdif_out",	10,	0x108,	100000000, mux_pllaout0_audio_2x_pllp_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("spdif_in",	"tegra30-spdif",	"spdif_in",	10,	0x10c,	100000000, mux_pllp_pllc_pllm,		MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("pwm",	"pwm",			NULL,	17,	0x110,	432000000, mux_pllp_pllc_clk32_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	D_AUDIO_CLK("d_audio",	"tegra30-ahub",		"d_audio",	106,	0x3d0,	48000000,  mux_d_audio_clk,	MUX | DIV_U71),
	D_AUDIO_CLK("dam0",	"tegra30-dam.0",	"dam.0",	108,	0x3d8,	48000000,  mux_d_audio_clk,	MUX | DIV_U71),
	D_AUDIO_CLK("dam1",	"tegra30-dam.1",	"dam.1",	109,	0x3dc,	48000000,  mux_d_audio_clk,	MUX | DIV_U71),
	D_AUDIO_CLK("dam2",	"tegra30-dam.2",	"dam.2",	110,	0x3e0,	48000000,  mux_d_audio_clk,	MUX | DIV_U71),
	PERIPH_CLK("hda",	"tegra30-hda",			"hda",   125,	0x428,	108000000, mux_pllp_pllc_pllm_clkm,	MUX | DIV_U71),
	PERIPH_CLK("hda2codec_2x",	"tegra30-hda",	"hda2codec",   111,	0x3e4,	48000000,  mux_pllp_pllc_pllm_clkm,	MUX | DIV_U71),
	PERIPH_CLK("hda2hdmi",	"tegra30-hda",		"hda2hdmi",	128,	0,	48000000,  mux_clk_m,			0),
	PERIPH_CLK("sbc1",	"tegra11-spi.0",	NULL,	41,	0x134,	160000000, mux_pllp_pllc_pllm_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("sbc2",	"tegra11-spi.1",	NULL,	44,	0x118,	160000000, mux_pllp_pllc_pllm_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("sbc3",	"tegra11-spi.2",	NULL,	46,	0x11c,	160000000, mux_pllp_pllc_pllm_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("sbc4",	"tegra11-spi.3",	NULL,	68,	0x1b4,	160000000, mux_pllp_pllc_pllm_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("sbc5",	"tegra11-spi.4",	NULL,	104,	0x3c8,	160000000, mux_pllp_pllc_pllm_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("sbc6",	"tegra11-spi.5",	NULL,	105,	0x3cc,	160000000, mux_pllp_pllc_pllm_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("sata_oob",	"tegra_sata_oob",	NULL,	123,	0x420,	216000000, mux_pllp_pllc_pllm_clkm,	MUX | DIV_U71),
	PERIPH_CLK("sata",	"tegra_sata",		NULL,	124,	0x424,	216000000, mux_pllp_pllc_pllm_clkm,	MUX | DIV_U71),
	PERIPH_CLK("sata_cold",	"tegra_sata_cold",	NULL,	129,	0,	48000000,  mux_clk_m,			0),
	PERIPH_CLK_EX("ndflash","tegra_nand",		NULL,	13,	0x160,	240000000, mux_pllp_pllc_pllm_clkm,	MUX | DIV_U71,	&tegra_nand_clk_ops),
	PERIPH_CLK("ndspeed",	"tegra_nand_speed",	NULL,	80,	0x3f8,	240000000, mux_pllp_pllc_pllm_clkm,	MUX | DIV_U71),
	PERIPH_CLK("vfir",	"vfir",			NULL,	7,	0x168,	72000000,  mux_pllp_pllc_pllm_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("sdmmc1",	"sdhci-tegra.0",	NULL,	14,	0x150,	208000000, mux_pllp_pllc_pllm_clkm,	MUX | DIV_U71),
	PERIPH_CLK("sdmmc2",	"sdhci-tegra.1",	NULL,	9,	0x154,	104000000, mux_pllp_pllc_pllm_clkm,	MUX | DIV_U71),
	PERIPH_CLK("sdmmc3",	"sdhci-tegra.2",	NULL,	69,	0x1bc,	208000000, mux_pllp_pllc_pllm_clkm,	MUX | DIV_U71),
	PERIPH_CLK("sdmmc4",	"sdhci-tegra.3",	NULL,	15,	0x164,	104000000, mux_pllp_pllc_pllm_clkm,	MUX | DIV_U71),
	PERIPH_CLK("vcp",	"tegra-avp",		"vcp",	29,	0,	250000000, mux_clk_m, 			0),
	PERIPH_CLK("bsea",	"tegra-avp",		"bsea",	62,	0,	250000000, mux_clk_m, 			0),
	PERIPH_CLK("bsev",	"tegra-aes",		"bsev",	63,	0,	250000000, mux_clk_m, 			0),
	PERIPH_CLK("vde",	"vde",			NULL,	61,	0x1c8,	600000000, mux_pllp_pllc2_c_c3_pllm_clkm,	MUX | MUX8 | DIV_U71 | DIV_U71_INT),
	PERIPH_CLK("csite",	"csite",		NULL,	73,	0x1d4,	144000000, mux_pllp_pllc_pllm_clkm,	MUX | DIV_U71), /* max rate ??? */
	PERIPH_CLK("la",	"la",			NULL,	76,	0x1f8,	26000000,  mux_pllp_pllc_pllm_clkm,	MUX | DIV_U71),
	PERIPH_CLK("owr",	"tegra_w1",		NULL,	71,	0x1cc,	26000000,  mux_pllp_pllc_pllm_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("nor",	"tegra-nor",		NULL,	42,	0x1d0,	127000000, mux_pllp_pllc_pllm_clkm,	MUX | DIV_U71),
	PERIPH_CLK("mipi",	"mipi",			NULL,	50,	0x174,	60000000,  mux_pllp_pllc_pllm_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("i2c1",	"tegra11-i2c.0",	"i2c-div",	12,	0x124,	26000000,  mux_pllp_clkm,	MUX | DIV_U16 | PERIPH_ON_APB),
	PERIPH_CLK("i2c2",	"tegra11-i2c.1",	"i2c-div",	54,	0x198,	26000000,  mux_pllp_clkm,	MUX | DIV_U16 | PERIPH_ON_APB),
	PERIPH_CLK("i2c3",	"tegra11-i2c.2",	"i2c-div",	67,	0x1b8,	26000000,  mux_pllp_clkm,	MUX | DIV_U16 | PERIPH_ON_APB),
	PERIPH_CLK("i2c4",	"tegra11-i2c.3",	"i2c-div",	103,	0x3c4,	26000000,  mux_pllp_clkm,	MUX | DIV_U16 | PERIPH_ON_APB),
	PERIPH_CLK("i2c5",	"tegra11-i2c.4",	"i2c-div",	47,	0x128,	26000000,  mux_pllp_clkm,	MUX | DIV_U16 | PERIPH_ON_APB),
	PERIPH_CLK("i2c1-fast",	"tegra11-i2c.0",	"i2c-fast",	0,	0,	108000000, mux_pllp_out3,	PERIPH_NO_ENB),
	PERIPH_CLK("i2c2-fast",	"tegra11-i2c.1",	"i2c-fast",	0,	0,	108000000, mux_pllp_out3,	PERIPH_NO_ENB),
	PERIPH_CLK("i2c3-fast",	"tegra11-i2c.2",	"i2c-fast",	0,	0,	108000000, mux_pllp_out3,	PERIPH_NO_ENB),
	PERIPH_CLK("i2c4-fast",	"tegra11-i2c.3",	"i2c-fast",	0,	0,	108000000, mux_pllp_out3,	PERIPH_NO_ENB),
	PERIPH_CLK("i2c5-fast",	"tegra11-i2c.4",	"i2c-fast",	0,	0,	108000000, mux_pllp_out3,	PERIPH_NO_ENB),
	PERIPH_CLK("uarta",	"tegra_uart.0",		NULL,	6,	0x178,	800000000, mux_pllp_pllc_pllm_clkm,	MUX | DIV_U151 | DIV_U151_UART | PERIPH_ON_APB),
	PERIPH_CLK("uartb",	"tegra_uart.1",		NULL,	7,	0x17c,	800000000, mux_pllp_pllc_pllm_clkm,	MUX | DIV_U151 | DIV_U151_UART | PERIPH_ON_APB),
	PERIPH_CLK("uartc",	"tegra_uart.2",		NULL,	55,	0x1a0,	800000000, mux_pllp_pllc_pllm_clkm,	MUX | DIV_U151 | DIV_U151_UART | PERIPH_ON_APB),
	PERIPH_CLK("uartd",	"tegra_uart.3",		NULL,	65,	0x1c0,	800000000, mux_pllp_pllc_pllm_clkm,	MUX | DIV_U151 | DIV_U151_UART | PERIPH_ON_APB),
	PERIPH_CLK("uarte",	"tegra_uart.4",		NULL,	66,	0x1c4,	800000000, mux_pllp_pllc_pllm_clkm,	MUX | DIV_U151 | DIV_U151_UART | PERIPH_ON_APB),
	PERIPH_CLK("uarta_dbg",	"serial8250.0",		"uarta",6,	0x178,	800000000, mux_pllp_clkm,		MUX | DIV_U151 | DIV_U151_UART | PERIPH_ON_APB),
	PERIPH_CLK("uartb_dbg",	"serial8250.0",		"uartb",7,	0x17c,	800000000, mux_pllp_clkm,		MUX | DIV_U151 | DIV_U151_UART | PERIPH_ON_APB),
	PERIPH_CLK("uartc_dbg",	"serial8250.0",		"uartc",55,	0x1a0,	800000000, mux_pllp_clkm,		MUX | DIV_U151 | DIV_U151_UART | PERIPH_ON_APB),
	PERIPH_CLK("uartd_dbg",	"serial8250.0",		"uartd",65,	0x1c0,	800000000, mux_pllp_clkm,		MUX | DIV_U151 | DIV_U151_UART | PERIPH_ON_APB),
	PERIPH_CLK("uarte_dbg",	"serial8250.0",		"uarte",66,	0x1c4,	800000000, mux_pllp_clkm,		MUX | DIV_U151 | DIV_U151_UART | PERIPH_ON_APB),
	PERIPH_CLK("3d",	"3d",			NULL,	24,	0x158,	600000000, mux_pllm_pllc2_c_c3_pllp_plla,	MUX | MUX8 | DIV_U71 | DIV_U71_INT | DIV_U71_IDLE | PERIPH_MANUAL_RESET),
	PERIPH_CLK("2d",	"2d",			NULL,	21,	0x15c,	600000000, mux_pllm_pllc2_c_c3_pllp_plla,	MUX | MUX8 | DIV_U71 | DIV_U71_INT | DIV_U71_IDLE),
	PERIPH_CLK_EX("vi",	"tegra_camera",		"vi",	20,	0x148,	425000000, mux_pllm_pllc_pllp_plla,	MUX | DIV_U71 | DIV_U71_INT,	&tegra_vi_clk_ops),
	PERIPH_CLK("vi_sensor",	"tegra_camera",		"vi_sensor",	20,	0x1a8,	150000000, mux_pllm_pllc_pllp_plla,	MUX | DIV_U71 | PERIPH_NO_RESET),
	PERIPH_CLK("epp",	"epp",			NULL,	19,	0x16c,	600000000, mux_pllm_pllc2_c_c3_pllp_plla,	MUX | MUX8 | DIV_U71 | DIV_U71_INT),
#ifdef CONFIG_TEGRA_SIMULATION_PLATFORM
	PERIPH_CLK("msenc",	"msenc",		NULL,	60,	0x170,	600000000, mux_pllm_pllc_pllp_plla,	MUX | DIV_U71 | DIV_U71_INT),
#else
	PERIPH_CLK("msenc",	"msenc",		NULL,	91,	0x1f0,	600000000, mux_pllm_pllc2_c_c3_pllp_plla,	MUX | MUX8 | DIV_U71 | DIV_U71_INT),
#endif
	PERIPH_CLK("tsec",	"tsec",			NULL,	83,	0x1f4,	600000000, mux_pllp_pllc2_c_c3_pllm_clkm,	MUX | MUX8 | DIV_U71 | DIV_U71_INT),
	PERIPH_CLK("host1x",	"host1x",		NULL,	28,	0x180,	300000000, mux_pllm_pllc2_c_c3_pllp_plla,	MUX | MUX8 | DIV_U71 | DIV_U71_INT),
	PERIPH_CLK_EX("dtv",	"dtv",			NULL,	79,	0x1dc,	250000000, mux_clk_m,			0,		&tegra_dtv_clk_ops),
	PERIPH_CLK("hdmi",	"hdmi",			NULL,	51,	0x18c,	148500000, mux_pllp_pllm_plld_plla_pllc_plld2_clkm,	MUX | MUX8 | DIV_U71),
	PERIPH_CLK("disp1",	"tegradc.0",		NULL,	27,	0x138,	600000000, mux_pllp_pllm_plld_plla_pllc_plld2_clkm,	MUX | MUX8),
	PERIPH_CLK("disp2",	"tegradc.1",		NULL,	26,	0x13c,	600000000, mux_pllp_pllm_plld_plla_pllc_plld2_clkm,	MUX | MUX8),
	PERIPH_CLK("usbd",	"tegra-udc.0",		NULL,	22,	0,	480000000, mux_clk_m,			0),
	PERIPH_CLK("usb2",	"tegra-ehci.1",		NULL,	58,	0,	480000000, mux_clk_m,			0),
	PERIPH_CLK("usb3",	"tegra-ehci.2",		NULL,	59,	0,	480000000, mux_clk_m,			0),
	PERIPH_CLK_EX("dsia",	"tegradc.0",		"dsia",	48,	0xd0,	500000000, mux_plld_out0_plld2_out0,	MUX | PLLD,	&tegra_dsi_clk_ops),
	PERIPH_CLK_EX("dsib",	"tegradc.1",		"dsib",	82,	0x4b8,	500000000, mux_plld_out0_plld2_out0,	MUX | PLLD,	&tegra_dsi_clk_ops),
	PERIPH_CLK("dsi1-fixed", "tegradc.0",		"dsi-fixed",	0,	0,	108000000, mux_pllp_out3,	PERIPH_NO_ENB),
	PERIPH_CLK("dsi2-fixed", "tegradc.1",		"dsi-fixed",	0,	0,	108000000, mux_pllp_out3,	PERIPH_NO_ENB),
	PERIPH_CLK("csi",	"tegra_camera",		"csi",	52,	0,	102000000, mux_pllp_out3,		0),
	PERIPH_CLK("isp",	"tegra_camera",		"isp",	23,	0,	150000000, mux_clk_m,			0), /* same frequency as VI */
	PERIPH_CLK("csus",	"tegra_camera",		"csus",	92,	0,	150000000, mux_clk_m,			PERIPH_NO_RESET),

	PERIPH_CLK("tsensor",	"tegra-tsensor",	NULL,	100,	0x3b8,	216000000, mux_pllp_pllc_clkm_clk32,	MUX | DIV_U71),
	PERIPH_CLK("actmon",	"actmon",		NULL,	119,	0x3e8,	216000000, mux_pllp_pllc_clk32_clkm,	MUX | DIV_U71),
	PERIPH_CLK("extern1",	"extern1",		NULL,	120,	0x3ec,	216000000, mux_plla_clk32_pllp_clkm_plle,	MUX | MUX8 | DIV_U71),
	PERIPH_CLK("extern2",	"extern2",		NULL,	121,	0x3f0,	216000000, mux_plla_clk32_pllp_clkm_plle,	MUX | MUX8 | DIV_U71),
	PERIPH_CLK("extern3",	"extern3",		NULL,	122,	0x3f4,	216000000, mux_plla_clk32_pllp_clkm_plle,	MUX | MUX8 | DIV_U71),
	PERIPH_CLK("i2cslow",	"i2cslow",		NULL,	81,	0x3fc,	26000000,  mux_pllp_pllc_clk32_clkm,	MUX | DIV_U71 | PERIPH_ON_APB),
	PERIPH_CLK("pcie",	"tegra-pcie",		"pcie",	70,	0,	250000000, mux_clk_m, 			0),
	PERIPH_CLK("afi",	"tegra-pcie",		"afi",	72,	0,	250000000, mux_clk_m, 			0),
	PERIPH_CLK("se",	"se",			NULL,	127,	0x42c,	600000000, mux_pllp_pllc2_c_c3_pllm_clkm,	MUX | MUX8 | DIV_U71 | DIV_U71_INT),
	PERIPH_CLK("mselect",	"mselect",		NULL,	99,	0x3b4,	108000000, mux_pllp_clkm,		MUX | DIV_U71),
	PERIPH_CLK("cl_dvfs_ref", "tegra_cl_dvfs",	"ref",	155,	0x62c,	54000000,  mux_pllp_pllc_pllm_clkm,	MUX | DIV_U71 | DIV_U71_INT | PERIPH_ON_APB),
	PERIPH_CLK("cl_dvfs_soc", "tegra_cl_dvfs",	"soc",	155,	0x630,	54000000,  mux_pllp_pllc_pllm_clkm,	MUX | DIV_U71 | DIV_U71_INT | PERIPH_ON_APB),

	SHARED_CLK("avp.sclk",	"tegra-avp",		"sclk",	&tegra_clk_sbus_cmplx, NULL, 0, 0),
	SHARED_CLK("bsea.sclk",	"tegra-aes",		"sclk",	&tegra_clk_sbus_cmplx, NULL, 0, 0),
	SHARED_CLK("usbd.sclk",	"tegra-udc.0",		"sclk",	&tegra_clk_sbus_cmplx, NULL, 0, 0),
	SHARED_CLK("usb1.sclk",	"tegra-ehci.0",		"sclk",	&tegra_clk_sbus_cmplx, NULL, 0, 0),
	SHARED_CLK("usb2.sclk",	"tegra-ehci.1",		"sclk",	&tegra_clk_sbus_cmplx, NULL, 0, 0),
	SHARED_CLK("usb3.sclk",	"tegra-ehci.2",		"sclk",	&tegra_clk_sbus_cmplx, NULL, 0, 0),
	SHARED_CLK("wake.sclk",	"wake_sclk",		"sclk",	&tegra_clk_sbus_cmplx, NULL, 0, 0),
	SHARED_CLK("mon.avp",	"tegra_actmon",		"avp",	&tegra_clk_sbus_cmplx, NULL, 0, 0),
	SHARED_CLK("cap.sclk",	"cap_sclk",		NULL,	&tegra_clk_sbus_cmplx, NULL, 0, SHARED_CEILING),
	SHARED_CLK("floor.sclk", "floor_sclk",		NULL,	&tegra_clk_sbus_cmplx, NULL, 0, 0),
	SHARED_CLK("sbc1.sclk", "tegra11-spi.0",	"sclk", &tegra_clk_sbus_cmplx, NULL, 0, 0),
	SHARED_CLK("sbc2.sclk", "tegra11-spi.1",	"sclk", &tegra_clk_sbus_cmplx, NULL, 0, 0),
	SHARED_CLK("sbc3.sclk", "tegra11-spi.2",	"sclk", &tegra_clk_sbus_cmplx, NULL, 0, 0),
	SHARED_CLK("sbc4.sclk", "tegra11-spi.3",	"sclk", &tegra_clk_sbus_cmplx, NULL, 0, 0),
	SHARED_CLK("sbc5.sclk", "tegra11-spi.4",	"sclk", &tegra_clk_sbus_cmplx, NULL, 0, 0),
	SHARED_CLK("sbc6.sclk", "tegra11-spi.5",	"sclk", &tegra_clk_sbus_cmplx, NULL, 0, 0),

	SHARED_CLK("avp.emc",	"tegra-avp",		"emc",	&tegra_clk_emc, NULL, 0, 0),
	SHARED_CLK("cpu.emc",	"cpu",			"emc",	&tegra_clk_emc, NULL, 0, 0),
	SHARED_CLK("disp1.emc",	"tegradc.0",		"emc",	&tegra_clk_emc, NULL, 0, SHARED_BW),
	SHARED_CLK("disp2.emc",	"tegradc.1",		"emc",	&tegra_clk_emc, NULL, 0, SHARED_BW),
	SHARED_CLK("hdmi.emc",	"hdmi",			"emc",	&tegra_clk_emc, NULL, 0, 0),
	SHARED_CLK("usbd.emc",	"tegra-udc.0",		"emc",	&tegra_clk_emc, NULL, 0, 0),
	SHARED_CLK("usb1.emc",	"tegra-ehci.0",		"emc",	&tegra_clk_emc, NULL, 0, 0),
	SHARED_CLK("usb2.emc",	"tegra-ehci.1",		"emc",	&tegra_clk_emc, NULL, 0, 0),
	SHARED_CLK("usb3.emc",	"tegra-ehci.2",		"emc",	&tegra_clk_emc, NULL, 0, 0),
	SHARED_CLK("mon.emc",	"tegra_actmon",		"emc",	&tegra_clk_emc, NULL, 0, 0),
	SHARED_CLK("cap.emc",	"cap.emc",		NULL,	&tegra_clk_emc, NULL, 0, SHARED_CEILING),
	SHARED_CLK("3d.emc",	"tegra_gr3d",		"emc",	&tegra_clk_emc, NULL, 0, 0),
	SHARED_CLK("2d.emc",	"tegra_gr2d",		"emc",	&tegra_clk_emc, NULL, 0, 0),
	SHARED_CLK("msenc.emc",	"tegra_msenc",		"emc",	&tegra_clk_emc, NULL, 0, 0),
	SHARED_CLK("tsec.emc",	"tegra_tsec",		"emc",	&tegra_clk_emc, NULL, 0, 0),
	SHARED_CLK("floor.emc",	"floor.emc",		NULL,	&tegra_clk_emc, NULL, 0, 0),

#ifdef CONFIG_TEGRA_DUAL_CBUS
	DUAL_CBUS_CLK("3d.cbus",	"tegra_gr3d",		"gr3d",	&tegra_clk_c2bus, "3d",  0, 0),
	DUAL_CBUS_CLK("2d.cbus",	"tegra_gr2d",		"gr2d",	&tegra_clk_c2bus, "2d",  0, 0),
	SHARED_CLK("cap.c2bus",		"cap.c2bus",		NULL,	&tegra_clk_c2bus, NULL,  0, SHARED_CEILING),
	SHARED_CLK("floor.c2bus",	"floor.c2bus",		NULL,	&tegra_clk_c2bus, NULL,  0, 0),

	DUAL_CBUS_CLK("epp.cbus",	"tegra_gr2d",		"epp",	  &tegra_clk_c3bus, "epp", 0, 0),
	DUAL_CBUS_CLK("msenc.cbus",	"tegra_msenc",		"msenc",  &tegra_clk_c3bus, "msenc", 0, 0),
	DUAL_CBUS_CLK("tsec.cbus",	"tegra_tsec",		"tsec",   &tegra_clk_c3bus, "tsec", 0, 0),
	DUAL_CBUS_CLK("vde.cbus",	"tegra-avp",		"vde",	  &tegra_clk_c3bus, "vde", 0, 0),
	DUAL_CBUS_CLK("se.cbus",	"tegra11-se",		NULL,	  &tegra_clk_c3bus, "se",  0, 0),
	SHARED_CLK("cap.c3bus",		"cap.c3bus",		NULL,	&tegra_clk_c3bus, NULL,  0, SHARED_CEILING),
	SHARED_CLK("floor.c3bus",	"floor.c3bus",		NULL,	&tegra_clk_c3bus, NULL,  0, 0),
#else
	SHARED_CLK("3d.cbus",	"tegra_gr3d",		"gr3d",	&tegra_clk_cbus, "3d",  0, 0),
	SHARED_CLK("2d.cbus",	"tegra_gr2d",		"gr2d",	&tegra_clk_cbus, "2d",  0, 0),
	SHARED_CLK("epp.cbus",	"tegra_gr2d",		"epp",	&tegra_clk_cbus, "epp", 0, 0),
	SHARED_CLK("msenc.cbus","tegra_msenc",		"msenc",&tegra_clk_cbus, "msenc", 0, 0),
	SHARED_CLK("tsec.cbus",	"tegra_tsec",		"tsec", &tegra_clk_cbus, "tsec", 0, 0),
	SHARED_CLK("vde.cbus",	"tegra-avp",		"vde",	&tegra_clk_cbus, "vde", 0, 0),
	SHARED_CLK("se.cbus",	"tegra11-se",		NULL,	&tegra_clk_cbus, "se",  0, 0),
	SHARED_CLK("cap.cbus",	"cap.cbus",		NULL,	&tegra_clk_cbus, NULL,  0, SHARED_CEILING),
	SHARED_CLK("floor.cbus", "floor.cbus",		NULL,	&tegra_clk_cbus, NULL,  0, 0),
#endif
};

#define CLK_DUPLICATE(_name, _dev, _con)		\
	{						\
		.name	= _name,			\
		.lookup	= {				\
			.dev_id	= _dev,			\
			.con_id		= _con,		\
		},					\
	}

/* Some clocks may be used by different drivers depending on the board
 * configuration.  List those here to register them twice in the clock lookup
 * table under two names.
 */
struct clk_duplicate tegra_clk_duplicates[] = {
	CLK_DUPLICATE("usbd", "utmip-pad", NULL),
	CLK_DUPLICATE("usbd", "tegra-ehci.0", NULL),
	CLK_DUPLICATE("usbd", "tegra-otg", NULL),
	CLK_DUPLICATE("disp1", "tegra_dc_dsi_vs1.0", NULL),
	CLK_DUPLICATE("disp1.emc", "tegra_dc_dsi_vs1.0", "emc"),
	CLK_DUPLICATE("disp1", "tegra_dc_dsi_vs1.1", NULL),
	CLK_DUPLICATE("disp1.emc", "tegra_dc_dsi_vs1.1", "emc"),
	CLK_DUPLICATE("hdmi", "tegradc.0", "hdmi"),
	CLK_DUPLICATE("hdmi", "tegradc.1", "hdmi"),
	CLK_DUPLICATE("dsib", "tegradc.0", "dsib"),
	CLK_DUPLICATE("dsia", "tegradc.1", "dsia"),
	CLK_DUPLICATE("dsia", "tegra_dc_dsi_vs1.0", "dsia"),
	CLK_DUPLICATE("dsia", "tegra_dc_dsi_vs1.1", "dsia"),
	CLK_DUPLICATE("dsi1_fixed", "tegra_dc_dsi_vs1.0", "dsi_fixed"),
	CLK_DUPLICATE("dsi2_fixed", "tegra_dc_dsi_vs1.1", "dsi_fixed"),
	CLK_DUPLICATE("pwm", "tegra_pwm.0", NULL),
	CLK_DUPLICATE("pwm", "tegra_pwm.1", NULL),
	CLK_DUPLICATE("pwm", "tegra_pwm.2", NULL),
	CLK_DUPLICATE("pwm", "tegra_pwm.3", NULL),
	CLK_DUPLICATE("cop", "tegra-avp", "cop"),
	CLK_DUPLICATE("bsev", "tegra-avp", "bsev"),
	CLK_DUPLICATE("cop", "nvavp", "cop"),
	CLK_DUPLICATE("bsev", "nvavp", "bsev"),
	CLK_DUPLICATE("vde", "tegra-aes", "vde"),
	CLK_DUPLICATE("bsea", "tegra-aes", "bsea"),
	CLK_DUPLICATE("bsea", "nvavp", "bsea"),
	CLK_DUPLICATE("cml1", "tegra_sata_cml", NULL),
	CLK_DUPLICATE("cml0", "tegra_pcie", "cml"),
	CLK_DUPLICATE("pciex", "tegra_pcie", "pciex"),
	CLK_DUPLICATE("i2c1", "tegra-i2c-slave.0", NULL),
	CLK_DUPLICATE("i2c2", "tegra-i2c-slave.1", NULL),
	CLK_DUPLICATE("i2c3", "tegra-i2c-slave.2", NULL),
	CLK_DUPLICATE("i2c4", "tegra-i2c-slave.3", NULL),
	CLK_DUPLICATE("i2c5", "tegra-i2c-slave.4", NULL),
	CLK_DUPLICATE("sbc1", "tegra11-spi-slave.0", NULL),
	CLK_DUPLICATE("sbc2", "tegra11-spi-slave.1", NULL),
	CLK_DUPLICATE("sbc3", "tegra11-spi-slave.2", NULL),
	CLK_DUPLICATE("sbc4", "tegra11-spi-slave.3", NULL),
	CLK_DUPLICATE("sbc5", "tegra11-spi-slave.4", NULL),
	CLK_DUPLICATE("sbc6", "tegra11-spi-slave.5", NULL),
	CLK_DUPLICATE("vcp", "nvavp", "vcp"),
	CLK_DUPLICATE("avp.sclk", "nvavp", "sclk"),
	CLK_DUPLICATE("avp.emc", "nvavp", "emc"),
	CLK_DUPLICATE("vde.cbus", "nvavp", "vde"),
	CLK_DUPLICATE("i2c5", "tegra_cl_dvfs", "i2c"),
	CLK_DUPLICATE("host1x", "tegra_host1x", "host1x"),

};

struct clk *tegra_ptr_clks[] = {
	&tegra_clk_32k,
	&tegra_clk_m,
	&tegra_clk_m_div2,
	&tegra_clk_m_div4,
	&tegra_pll_ref,
	&tegra_pll_m,
	&tegra_pll_m_out1,
	&tegra_pll_c,
	&tegra_pll_c_out1,
	&tegra_pll_c2,
	&tegra_pll_c3,
	&tegra_pll_p,
	&tegra_pll_p_out1,
	&tegra_pll_p_out2,
	&tegra_pll_p_out3,
	&tegra_pll_p_out4,
	&tegra_pll_a,
	&tegra_pll_a_out0,
	&tegra_pll_d,
	&tegra_pll_d_out0,
	&tegra_pll_d2,
	&tegra_pll_d2_out0,
	&tegra_pll_u,
	&tegra_pll_x,
	&tegra_pll_x_out0,
	&tegra_dfll_cpu,
	&tegra_pll_re_vco,
	&tegra_pll_re_out,
	&tegra_pll_e,
	&tegra_cml0_clk,
	&tegra_cml1_clk,
	&tegra_pciex_clk,
	&tegra_clk_cclk_g,
	&tegra_clk_cclk_lp,
	&tegra_clk_sclk,
	&tegra_clk_hclk,
	&tegra_clk_pclk,
	&tegra_clk_virtual_cpu_g,
	&tegra_clk_virtual_cpu_lp,
	&tegra_clk_cpu_cmplx,
	&tegra_clk_blink,
	&tegra_clk_cop,
	&tegra_clk_sbus_cmplx,
	&tegra_clk_emc,
#ifdef CONFIG_TEGRA_DUAL_CBUS
	&tegra_clk_c2bus,
	&tegra_clk_c3bus,
#else
	&tegra_clk_cbus,
#endif
};

/* Return true from this function if the target rate can be locked without
   switching pll clients to back-up source */
static bool tegra11_is_dyn_ramp(
	struct clk *c, unsigned long rate, bool from_vco_min)
{
#if PLLCX_USE_DYN_RAMP
	/* PLLC2, PLLC3 support dynamic ramp only when output divider <= 8 */
	if ((c == &tegra_pll_c2) || (c == &tegra_pll_c3)) {
		struct clk_pll_freq_table cfg, old_cfg;
		unsigned long input_rate = clk_get_rate(c->parent);

		u32 val = clk_readl(c->reg + PLL_BASE);
		PLL_BASE_PARSE(PLLCX, old_cfg, val);
		old_cfg.p = pllcx_p[old_cfg.p];

		if (!pll_dyn_ramp_find_cfg(c, &cfg, rate, input_rate, NULL)) {
			if ((cfg.n == old_cfg.n) ||
			    PLLCX_IS_DYN(cfg.p, old_cfg.p))
				return true;
		}
	}
#endif

#if PLLXC_USE_DYN_RAMP
	/* PPLX, PLLC support dynamic ramp when changing NDIV only */
	if ((c == &tegra_pll_x) || (c == &tegra_pll_c)) {
		struct clk_pll_freq_table cfg, old_cfg;
		unsigned long input_rate = clk_get_rate(c->parent);

		if (from_vco_min) {
			old_cfg.m = PLL_FIXED_MDIV(c, input_rate);
			old_cfg.p = 1;
		} else {
			u32 val = clk_readl(c->reg + PLL_BASE);
			PLL_BASE_PARSE(PLLXC, old_cfg, val);
			old_cfg.p = pllxc_p[old_cfg.p];
		}

		if (!pll_dyn_ramp_find_cfg(c, &cfg, rate, input_rate, NULL)) {
			if ((cfg.m == old_cfg.m) && (cfg.p == old_cfg.p))
				return true;
		}
	}
#endif
	return false;
}

/*
 * Backup pll is used as transitional CPU clock source while main pll is
 * relocking; in addition all CPU rates below backup level are sourced from
 * backup pll only. Target backup levels for each CPU mode are selected high
 * enough to avoid voltage droop when CPU clock is switched between backup and
 * main plls. Actual backup rates will be rounded to match backup source fixed
 * frequency. Backup rates are also used as stay-on-backup thresholds, and must
 * be kept the same in G and LP mode (will need to add a separate stay-on-backup
 * parameter to allow different backup rates if necessary).
 *
 * Sbus threshold must be exact factor of pll_p rate.
 */
#define CPU_G_BACKUP_RATE_TARGET	200000000
#define CPU_LP_BACKUP_RATE_TARGET	200000000

static void tegra11_pllp_init_dependencies(unsigned long pllp_rate)
{
	u32 div;
	unsigned long backup_rate;

	switch (pllp_rate) {
	case 216000000:
		tegra_pll_p_out1.u.pll_div.default_rate = 28800000;
		tegra_pll_p_out3.u.pll_div.default_rate = 72000000;
		tegra_clk_sbus_cmplx.u.system.threshold = 108000000;
		break;
	case 408000000:
		tegra_pll_p_out1.u.pll_div.default_rate = 9600000;
		tegra_pll_p_out3.u.pll_div.default_rate = 102000000;
		tegra_clk_sbus_cmplx.u.system.threshold = 204000000;
		break;
	case 204000000:
		tegra_pll_p_out1.u.pll_div.default_rate = 4800000;
		tegra_pll_p_out3.u.pll_div.default_rate = 102000000;
		tegra_clk_sbus_cmplx.u.system.threshold = 204000000;
		break;
	default:
		pr_err("tegra: PLLP rate: %lu is not supported\n", pllp_rate);
		BUG();
	}
	pr_info("tegra: PLLP fixed rate: %lu\n", pllp_rate);

	div = pllp_rate / CPU_G_BACKUP_RATE_TARGET;
	backup_rate = pllp_rate / div;
	tegra_clk_virtual_cpu_g.u.cpu.backup_rate = backup_rate;

	div = pllp_rate / CPU_LP_BACKUP_RATE_TARGET;
	backup_rate = pllp_rate / div;
	tegra_clk_virtual_cpu_lp.u.cpu.backup_rate = backup_rate;
}

static void tegra11_init_one_clock(struct clk *c)
{
	clk_init(c);
	INIT_LIST_HEAD(&c->shared_bus_list);
	if (!c->lookup.dev_id && !c->lookup.con_id)
		c->lookup.con_id = c->name;
	c->lookup.clk = c;
	clkdev_add(&c->lookup);
}

bool tegra_clk_is_parent_allowed(struct clk *c, struct clk *p)
{
	/* No policy limitations for now */
	return true;
}

void __init tegra11x_init_clocks(void)
{
	int i;
	struct clk *c;

	for (i = 0; i < ARRAY_SIZE(tegra_ptr_clks); i++)
		tegra11_init_one_clock(tegra_ptr_clks[i]);

	for (i = 0; i < ARRAY_SIZE(tegra_list_clks); i++)
		tegra11_init_one_clock(&tegra_list_clks[i]);

	for (i = 0; i < ARRAY_SIZE(tegra_clk_duplicates); i++) {
		c = tegra_get_clock_by_name(tegra_clk_duplicates[i].name);
		if (!c) {
			pr_err("%s: Unknown duplicate clock %s\n", __func__,
				tegra_clk_duplicates[i].name);
			continue;
		}

		tegra_clk_duplicates[i].lookup.clk = c;
		clkdev_add(&tegra_clk_duplicates[i].lookup);
	}

	for (i = 0; i < ARRAY_SIZE(tegra_sync_source_list); i++)
		tegra11_init_one_clock(&tegra_sync_source_list[i]);
	for (i = 0; i < ARRAY_SIZE(tegra_clk_audio_list); i++)
		tegra11_init_one_clock(&tegra_clk_audio_list[i]);
	for (i = 0; i < ARRAY_SIZE(tegra_clk_audio_2x_list); i++)
		tegra11_init_one_clock(&tegra_clk_audio_2x_list[i]);

	init_clk_out_mux();
	for (i = 0; i < ARRAY_SIZE(tegra_clk_out_list); i++)
		tegra11_init_one_clock(&tegra_clk_out_list[i]);

	/* Initialize to default */
	tegra_init_cpu_edp_limits(0);
}

#ifdef CONFIG_CPU_FREQ

/*
 * Frequency table index must be sequential starting at 0 and frequencies
 * must be ascending.
 */

static struct cpufreq_frequency_table freq_table_300MHz[] = {
	{ 0, 204000 },
	{ 1, 300000 },
	{ 2, CPUFREQ_TABLE_END },
};

static struct cpufreq_frequency_table freq_table_1p0GHz[] = {
	{ 0, 102000 },
	{ 1, 204000 },
	{ 2, 312000 },
	{ 3, 456000 },
	{ 4, 608000 },
	{ 5, 760000 },
	{ 6, 816000 },
	{ 7, 912000 },
	{ 8, 1000000 },
	{ 9, CPUFREQ_TABLE_END },
};

static struct cpufreq_frequency_table freq_table_1p3GHz[] = {
	{ 0,  102000 },
	{ 1,  204000 },
	{ 2,  340000 },
	{ 3,  475000 },
	{ 4,  640000 },
	{ 5,  760000 },
	{ 6,  880000 },
	{ 7, 1000000 },
	{ 8, 1100000 },
	{ 9, 1200000 },
	{10, 1300000 },
	{11, CPUFREQ_TABLE_END },
};

static struct cpufreq_frequency_table freq_table_1p4GHz[] = {
	{ 0,  102000 },
	{ 1,  204000 },
	{ 2,  370000 },
	{ 3,  475000 },
	{ 4,  620000 },
	{ 5,  760000 },
	{ 6,  880000 },
	{ 7, 1000000 },
	{ 8, 1100000 },
	{ 9, 1200000 },
	{10, 1300000 },
	{11, 1400000 },
	{12, CPUFREQ_TABLE_END },
};

static struct tegra_cpufreq_table_data cpufreq_tables[] = {
	{ freq_table_300MHz, 0, 1 },
	{ freq_table_1p0GHz, 2, 7, 2},
	{ freq_table_1p3GHz, 2, 9, 2},
	{ freq_table_1p4GHz, 2, 10, 2},
};

static int clip_cpu_rate_limits(
	struct cpufreq_frequency_table *freq_table,
	struct cpufreq_policy *policy,
	struct clk *cpu_clk_g,
	struct clk *cpu_clk_lp)
{
	int idx, ret;

	/* clip CPU G mode maximum frequency to table entry */
	ret = cpufreq_frequency_table_target(policy, freq_table,
		cpu_clk_g->max_rate / 1000, CPUFREQ_RELATION_H, &idx);
	if (ret) {
		pr_err("%s: G CPU max rate %lu outside of cpufreq table",
		       __func__, cpu_clk_g->max_rate);
		return ret;
	}
	cpu_clk_g->max_rate = freq_table[idx].frequency * 1000;
	if (cpu_clk_g->max_rate < cpu_clk_lp->max_rate) {
		pr_err("%s: G CPU max rate %lu is below LP CPU max rate %lu",
		       __func__, cpu_clk_g->max_rate, cpu_clk_lp->max_rate);
		return -EINVAL;
	}

	/* clip CPU LP mode maximum frequency to table entry, and
	   set CPU G mode minimum frequency one table step below */
	ret = cpufreq_frequency_table_target(policy, freq_table,
		cpu_clk_lp->max_rate / 1000, CPUFREQ_RELATION_H, &idx);
	if (ret || !idx) {
		pr_err("%s: LP CPU max rate %lu %s of cpufreq table", __func__,
		       cpu_clk_lp->max_rate, ret ? "outside" : "at the bottom");
		return ret;
	}
	cpu_clk_lp->max_rate = freq_table[idx].frequency * 1000;
	cpu_clk_g->min_rate = freq_table[idx-1].frequency * 1000;
	return 0;
}

struct tegra_cpufreq_table_data *tegra_cpufreq_table_get(void)
{
	int i, ret;
	unsigned long selection_rate;
	struct clk *cpu_clk_g = tegra_get_clock_by_name("cpu_g");
	struct clk *cpu_clk_lp = tegra_get_clock_by_name("cpu_lp");

	/* For table selection use top cpu_g rate in dvfs ladder; selection
	   rate may exceed cpu max_rate (e.g., because of edp limitations on
	   cpu voltage) - in any case max_rate will be clipped to the table */
	if (cpu_clk_g->dvfs && cpu_clk_g->dvfs->num_freqs)
		selection_rate =
			cpu_clk_g->dvfs->freqs[cpu_clk_g->dvfs->num_freqs - 1];
	else
		selection_rate = cpu_clk_g->max_rate;

	for (i = 0; i < ARRAY_SIZE(cpufreq_tables); i++) {
		struct cpufreq_policy policy;
		policy.cpu = 0;	/* any on-line cpu */
		ret = cpufreq_frequency_table_cpuinfo(
			&policy, cpufreq_tables[i].freq_table);
		if (!ret) {
			if ((policy.max * 1000) == selection_rate) {
				ret = clip_cpu_rate_limits(
					cpufreq_tables[i].freq_table,
					&policy, cpu_clk_g, cpu_clk_lp);
				if (!ret)
					return &cpufreq_tables[i];
			}
		}
	}
	WARN(1, "%s: No cpufreq table matching G & LP cpu ranges", __func__);
	return NULL;
}

unsigned long tegra_emc_to_cpu_ratio(unsigned long cpu_rate)
{
	static unsigned long emc_max_rate;

	if (emc_max_rate == 0)
		emc_max_rate = clk_round_rate(
			tegra_get_clock_by_name("emc"), ULONG_MAX);

	/* Vote on memory bus frequency based on cpu frequency;
	   cpu rate is in kHz, emc rate is in Hz */
	if (cpu_rate >= 750000)
		return emc_max_rate;	/* cpu >= 750 MHz, emc max */
	else if (cpu_rate >= 450000)
		return emc_max_rate/2;	/* cpu >= 500 MHz, emc max/2 */
	else if (cpu_rate >= 250000)
		return 100000000;	/* cpu >= 250 MHz, emc 100 MHz */
	else
		return 0;		/* emc min */
}
#endif

#ifdef CONFIG_PM_SLEEP
static u32 clk_rst_suspend[RST_DEVICES_NUM + CLK_OUT_ENB_NUM +
			   PERIPH_CLK_SOURCE_NUM + 22];

void tegra_clk_suspend(void)
{
	unsigned long off;
	u32 *ctx = clk_rst_suspend;

	*ctx++ = clk_readl(OSC_CTRL) & OSC_CTRL_MASK;
	*ctx++ = clk_readl(CPU_SOFTRST_CTRL);
	*ctx++ = clk_readl(tegra_pll_c.reg + PLL_BASE);
	*ctx++ = clk_readl(tegra_pll_c.reg + PLL_MISC(&tegra_pll_c));
	*ctx++ = clk_readl(tegra_pll_a.reg + PLL_BASE);
	*ctx++ = clk_readl(tegra_pll_a.reg + PLL_MISC(&tegra_pll_a));
	*ctx++ = clk_readl(tegra_pll_d.reg + PLL_BASE);
	*ctx++ = clk_readl(tegra_pll_d.reg + PLL_MISC(&tegra_pll_d));
	*ctx++ = clk_readl(tegra_pll_d2.reg + PLL_BASE);
	*ctx++ = clk_readl(tegra_pll_d2.reg + PLL_MISC(&tegra_pll_d2));

	*ctx++ = clk_readl(tegra_pll_m_out1.reg);
	*ctx++ = clk_readl(tegra_pll_a_out0.reg);
	*ctx++ = clk_readl(tegra_pll_c_out1.reg);

	*ctx++ = clk_readl(tegra_clk_cclk_g.reg);
	*ctx++ = clk_readl(tegra_clk_cclk_g.reg + SUPER_CLK_DIVIDER);
	*ctx++ = clk_readl(tegra_clk_cclk_lp.reg);
	*ctx++ = clk_readl(tegra_clk_cclk_lp.reg + SUPER_CLK_DIVIDER);

	*ctx++ = clk_readl(tegra_clk_sclk.reg);
	*ctx++ = clk_readl(tegra_clk_sclk.reg + SUPER_CLK_DIVIDER);
	*ctx++ = clk_readl(tegra_clk_pclk.reg);

	for (off = PERIPH_CLK_SOURCE_I2S1; off <= PERIPH_CLK_SOURCE_OSC;
			off += 4) {
		if (off == PERIPH_CLK_SOURCE_EMC)
			continue;
		*ctx++ = clk_readl(off);
	}
	for (off = PERIPH_CLK_SOURCE_G3D2; off <= PERIPH_CLK_SOURCE_SE;
			off+=4) {
		*ctx++ = clk_readl(off);
	}
	for (off = AUDIO_DLY_CLK; off <= AUDIO_SYNC_CLK_SPDIF; off+=4) {
		*ctx++ = clk_readl(off);
	}
	for (off = PERIPH_CLK_SOURCE_XUSB; off <= PERIPH_CLK_SOURCE_DDS_UART;
	      off += 4) {
		*ctx++ = clk_readl(off);
	}

	*ctx++ = clk_readl(RST_DEVICES_L);
	*ctx++ = clk_readl(RST_DEVICES_H);
	*ctx++ = clk_readl(RST_DEVICES_U);
	*ctx++ = clk_readl(RST_DEVICES_V);
	*ctx++ = clk_readl(RST_DEVICES_W);

	*ctx++ = clk_readl(CLK_OUT_ENB_L);
	*ctx++ = clk_readl(CLK_OUT_ENB_H);
	*ctx++ = clk_readl(CLK_OUT_ENB_U);
	*ctx++ = clk_readl(CLK_OUT_ENB_V);
	*ctx++ = clk_readl(CLK_OUT_ENB_W);

	*ctx++ = clk_readl(MISC_CLK_ENB);
	*ctx++ = clk_readl(CLK_MASK_ARM);
}

void tegra_clk_resume(void)
{
	unsigned long off;
	const u32 *ctx = clk_rst_suspend;
	u32 val;
	u32 pllc_base;
	u32 plla_base;
	u32 plld_base;
	u32 plld2_base;
	struct clk *p;

	val = clk_readl(OSC_CTRL) & ~OSC_CTRL_MASK;
	val |= *ctx++;
	clk_writel(val, OSC_CTRL);
	clk_writel(*ctx++, CPU_SOFTRST_CTRL);

	/* Since we are going to reset devices in this function, pllc/a is
	 * required to be enabled. The actual value will be restore back later.
	 */
	pllc_base = *ctx++;
	clk_writel(pllc_base | PLL_BASE_ENABLE, tegra_pll_c.reg + PLL_BASE);
	clk_writel(*ctx++, tegra_pll_c.reg + PLL_MISC(&tegra_pll_c));

	plla_base = *ctx++;
	clk_writel(plla_base | PLL_BASE_ENABLE, tegra_pll_a.reg + PLL_BASE);
	clk_writel(*ctx++, tegra_pll_a.reg + PLL_MISC(&tegra_pll_a));

	plld_base = *ctx++;
	clk_writel(plld_base | PLL_BASE_ENABLE, tegra_pll_d.reg + PLL_BASE);
	clk_writel(*ctx++, tegra_pll_d.reg + PLL_MISC(&tegra_pll_d));

	plld2_base = *ctx++;
	clk_writel(plld2_base | PLL_BASE_ENABLE, tegra_pll_d2.reg + PLL_BASE);
	clk_writel(*ctx++, tegra_pll_d2.reg + PLL_MISC(&tegra_pll_d2));

	udelay(1000);

	clk_writel(*ctx++, tegra_pll_m_out1.reg);
	clk_writel(*ctx++, tegra_pll_a_out0.reg);
	clk_writel(*ctx++, tegra_pll_c_out1.reg);

	clk_writel(*ctx++, tegra_clk_cclk_g.reg);
	clk_writel(*ctx++, tegra_clk_cclk_g.reg + SUPER_CLK_DIVIDER);
	clk_writel(*ctx++, tegra_clk_cclk_lp.reg);
	clk_writel(*ctx++, tegra_clk_cclk_lp.reg + SUPER_CLK_DIVIDER);

	clk_writel(*ctx++, tegra_clk_sclk.reg);
	clk_writel(*ctx++, tegra_clk_sclk.reg + SUPER_CLK_DIVIDER);
	clk_writel(*ctx++, tegra_clk_pclk.reg);

	/* enable all clocks before configuring clock sources */
	clk_writel(0xfdfffff1ul, CLK_OUT_ENB_L);
	clk_writel(0xfefff7f7ul, CLK_OUT_ENB_H);
	clk_writel(0x75f79bfful, CLK_OUT_ENB_U);
	clk_writel(0xfffffffful, CLK_OUT_ENB_V);
	clk_writel(0x00003ffful, CLK_OUT_ENB_W);
	wmb();

	for (off = PERIPH_CLK_SOURCE_I2S1; off <= PERIPH_CLK_SOURCE_OSC;
			off += 4) {
		if (off == PERIPH_CLK_SOURCE_EMC)
			continue;
		clk_writel(*ctx++, off);
	}
	for (off = PERIPH_CLK_SOURCE_G3D2; off <= PERIPH_CLK_SOURCE_SE;
			off += 4) {
		clk_writel(*ctx++, off);
	}
	for (off = AUDIO_DLY_CLK; off <= AUDIO_SYNC_CLK_SPDIF; off+=4) {
		clk_writel(*ctx++, off);
	}
	for (off = PERIPH_CLK_SOURCE_XUSB; off <= PERIPH_CLK_SOURCE_DDS_UART;
			off += 4) {
		clk_writel(*ctx++, off);
	}
	clk_writel(*ctx++, RST_DEVICES_L);
	clk_writel(*ctx++, RST_DEVICES_H);
	clk_writel(*ctx++, RST_DEVICES_U);

	/* For LP0 resume, don't reset lpcpu, since we are running from it */
	val = *ctx++;
	val &= ~RST_DEVICES_V_SWR_CPULP_RST_DIS;
	clk_writel(val, RST_DEVICES_V);

	clk_writel(*ctx++, RST_DEVICES_W);
	wmb();

	clk_writel(*ctx++, CLK_OUT_ENB_L);
	clk_writel(*ctx++, CLK_OUT_ENB_H);
	clk_writel(*ctx++, CLK_OUT_ENB_U);

	/* For LP0 resume, clk to lpcpu is required to be on */
	val = *ctx++;
	val |= CLK_OUT_ENB_V_CLK_ENB_CPULP_EN;
	clk_writel(val, CLK_OUT_ENB_V);

	clk_writel(*ctx++, CLK_OUT_ENB_W);
	wmb();

	clk_writel(*ctx++, MISC_CLK_ENB);
	clk_writel(*ctx++, CLK_MASK_ARM);

	/* Restore back the actual pllc/a value */
	/* FIXME: need to root cause why pllc is required to be on
	 * clk_writel(pllc_base, tegra_pll_c.reg + PLL_BASE);
	 */
	clk_writel(plla_base, tegra_pll_a.reg + PLL_BASE);
	clk_writel(plld_base, tegra_pll_d.reg + PLL_BASE);
	clk_writel(plld2_base, tegra_pll_d2.reg + PLL_BASE);

	/* Since EMC clock is not restored, and may not preserve parent across
	   suspend, update current state, and mark EMC DFS as out of sync */
	p = tegra_clk_emc.parent;
	tegra11_periph_clk_init(&tegra_clk_emc);

	if (p != tegra_clk_emc.parent) {
		/* FIXME: old parent is left enabled here even if EMC was its
		   only child before suspend (never happens on tegra11x) */
		pr_debug("EMC parent(refcount) across suspend: %s(%d) : %s(%d)",
			p->name, p->refcnt, tegra_clk_emc.parent->name,
			tegra_clk_emc.parent->refcnt);

		BUG_ON(!p->refcnt);
		p->refcnt--;

		/* the new parent is enabled by low level code, but ref count
		   need to be updated up to the root */
		p = tegra_clk_emc.parent;
		while (p && ((p->refcnt++) == 0))
			p = p->parent;
	}
	tegra_emc_timing_invalidate();

	tegra11_pll_clk_init(&tegra_pll_u); /* Re-init utmi parameters */
	tegra11_pllp_clk_resume(&tegra_pll_p); /* Fire a bug if not restored */
}
#endif