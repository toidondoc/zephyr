// SPDX-License-Identifier: BSD-3-Clause
//
// Copyright(c) 2018 Intel Corporation. All rights reserved.
//
// Author: Tomasz Lauda <tomasz.lauda@linux.intel.com>
//         Janusz Jankowski <janusz.jankowski@linux.intel.com>

/**
 * \file platform/intel/cavs/pm_runtime.c
 * \brief Runtime power management implementation for Apollolake, Cannonlake
 *        and Icelake
 * \author Tomasz Lauda <tomasz.lauda@linux.intel.com>
 */

#include <sof/debug/panic.h>
#include <sof/drivers/interrupt.h>
#include <sof/lib/alloc.h>
#include <sof/lib/dai.h>
#include <sof/lib/io.h>
#include <sof/lib/memory.h>
#include <sof/lib/pm_memory.h>
#include <sof/lib/pm_runtime.h>
#include <sof/lib/shim.h>
#include <sof/lib/wait.h>
#include <sof/platform.h>
#include <sof/spinlock.h>
#include <sof/trace/trace.h>
#include <ipc/topology.h>
#include <user/trace.h>
#include <config.h>
#include <version.h>
#include <stdint.h>

#define trace_power(format, ...)	\
	trace_event(TRACE_CLASS_POWER, format, ##__VA_ARGS__)

#if !CONFIG_SUECREEK
#include <cavs/lib/power_down.h>
#endif

/**
 * \brief Forces Host DMAs to exit L1.
 */
static inline void cavs_pm_runtime_force_host_dma_l1_exit(void)
{
	struct pm_runtime_data *prd = pm_runtime_data_get();
	uint32_t flags;

	spin_lock_irq(&prd->lock, flags);

	if (!(shim_read(SHIM_SVCFG) & SHIM_SVCFG_FORCE_L1_EXIT)) {
		shim_write(SHIM_SVCFG,
			   shim_read(SHIM_SVCFG) | SHIM_SVCFG_FORCE_L1_EXIT);

		wait_delay(PLATFORM_FORCE_L1_EXIT_TIME);

		shim_write(SHIM_SVCFG,
			   shim_read(SHIM_SVCFG) & ~(SHIM_SVCFG_FORCE_L1_EXIT));
	}

	spin_unlock_irq(&prd->lock, flags);
}

static inline void cavs_pm_runtime_enable_dsp(bool enable)
{
	struct pm_runtime_data *prd = pm_runtime_data_get();
	struct cavs_pm_runtime_data *pprd = prd->platform_data;
	uint32_t flags;

	/* request is always run on dsp0 and applies to dsp0,
	 * so no global lock is required.
	 */
	irq_local_disable(flags);

	enable ? --pprd->dsp_d0_sref : ++pprd->dsp_d0_sref;

	platform_shared_commit(prd, sizeof(*prd));

	irq_local_enable(flags);

	trace_power("pm_runtime_enable_dsp dsp_d0_sref %d", pprd->dsp_d0_sref);
}

static inline bool cavs_pm_runtime_is_active_dsp(void)
{
	struct pm_runtime_data *prd = pm_runtime_data_get();
	struct cavs_pm_runtime_data *pprd = prd->platform_data;

	platform_shared_commit(prd, sizeof(*prd));

	return pprd->dsp_d0_sref > 0;
}

#if CONFIG_CAVS_SSP
static inline void cavs_pm_runtime_dis_ssp_clk_gating(uint32_t index)
{
#if CONFIG_APOLLOLAKE
	uint32_t shim_reg;

	shim_reg = shim_read(SHIM_CLKCTL) |
		(index < DAI_NUM_SSP_BASE ?
			SHIM_CLKCTL_I2SFDCGB(index) :
			SHIM_CLKCTL_I2SEFDCGB(index - DAI_NUM_SSP_BASE));

	shim_write(SHIM_CLKCTL, shim_reg);

	trace_power("dis-ssp-clk-gating index %d CLKCTL %08x", index, shim_reg);
#endif
}

static inline void cavs_pm_runtime_en_ssp_clk_gating(uint32_t index)
{
#if CONFIG_APOLLOLAKE
	uint32_t shim_reg;

	shim_reg = shim_read(SHIM_CLKCTL) &
		~(index < DAI_NUM_SSP_BASE ?
			SHIM_CLKCTL_I2SFDCGB(index) :
			SHIM_CLKCTL_I2SEFDCGB(index - DAI_NUM_SSP_BASE));

	shim_write(SHIM_CLKCTL, shim_reg);

	trace_power("en-ssp-clk-gating index %d CLKCTL %08x", index, shim_reg);
#endif
}

static inline void cavs_pm_runtime_en_ssp_power(uint32_t index)
{
#if CONFIG_TIGERLAKE
	uint32_t reg;

	trace_power("en_ssp_power index %d", index);

	io_reg_write(I2SLCTL, io_reg_read(I2SLCTL) | I2SLCTL_SPA(index));

	/* Check if powered on. */
	do {
		reg = io_reg_read(I2SLCTL);
	} while (!(reg & I2SLCTL_CPA(index)));

	trace_power("en_ssp_power I2SLCTL %08x", reg);
#endif
}

static inline void cavs_pm_runtime_dis_ssp_power(uint32_t index)
{
#if CONFIG_TIGERLAKE
	uint32_t reg;

	trace_power("dis_ssp_power index %d", index);

	io_reg_write(I2SLCTL, io_reg_read(I2SLCTL) & (~I2SLCTL_SPA(index)));

	/* Check if powered off. */
	do {
		reg = io_reg_read(I2SLCTL);
	} while (reg & I2SLCTL_CPA(index));

	trace_power("dis_ssp_power I2SLCTL %08x", reg);
#endif
}
#endif

#if CONFIG_CAVS_DMIC
static inline void cavs_pm_runtime_dis_dmic_clk_gating(uint32_t index)
{
#if CONFIG_APOLLOLAKE || CONFIG_CANNONLAKE
	(void)index;
	uint32_t shim_reg;

	shim_reg = shim_read(SHIM_CLKCTL) | SHIM_CLKCTL_DMICFDCGB;

	shim_write(SHIM_CLKCTL, shim_reg);

	trace_power("dis-dmic-clk-gating index %d CLKCTL %08x", index,
		    shim_reg);
#endif
#if CONFIG_CANNONLAKE || CONFIG_ICELAKE || CONFIG_SUECREEK || CONFIG_TIGERLAKE
	/* Disable DMIC clock gating */
	io_reg_write(DMICLCTL,
		    (io_reg_read(DMICLCTL) | DMIC_DCGD));
#endif
}

static inline void cavs_pm_runtime_en_dmic_clk_gating(uint32_t index)
{
#if CONFIG_APOLLOLAKE || CONFIG_CANNONLAKE
	(void)index;
	uint32_t shim_reg;

	shim_reg = shim_read(SHIM_CLKCTL) & ~SHIM_CLKCTL_DMICFDCGB;

	shim_write(SHIM_CLKCTL, shim_reg);

	trace_power("en-dmic-clk-gating index %d CLKCTL %08x", index, shim_reg);
#endif
#if CONFIG_CANNONLAKE || CONFIG_ICELAKE || CONFIG_SUECREEK || CONFIG_TIGERLAKE
	/* Enable DMIC clock gating */
	io_reg_write(DMICLCTL,
		    (io_reg_read(DMICLCTL) & ~DMIC_DCGD));
#endif
}
static inline void cavs_pm_runtime_en_dmic_power(uint32_t index)
{
	(void) index;
#if CONFIG_CANNONLAKE || CONFIG_ICELAKE || CONFIG_SUECREEK || CONFIG_TIGERLAKE
	/* Enable DMIC power */
	io_reg_write(DMICLCTL,
		    (io_reg_read(DMICLCTL) | DMICLCTL_SPA));
#endif
}
static inline void cavs_pm_runtime_dis_dmic_power(uint32_t index)
{
	(void) index;
#if CONFIG_CANNONLAKE || CONFIG_ICELAKE || CONFIG_SUECREEK || CONFIG_TIGERLAKE
	/* Disable DMIC power */
	io_reg_write(DMICLCTL,
		    (io_reg_read(DMICLCTL) & (~DMICLCTL_SPA)));
#endif
}
#endif /* #if defined(CONFIG_CAVS_DMIC) */

static inline void cavs_pm_runtime_dis_dwdma_clk_gating(uint32_t index)
{
#if CONFIG_APOLLOLAKE
	uint32_t shim_reg;

	shim_reg = shim_read(SHIM_CLKCTL) | SHIM_CLKCTL_LPGPDMAFDCGB(index);

	shim_write(SHIM_CLKCTL, shim_reg);

	trace_power("dis-dwdma-clk-gating index %d CLKCTL %08x", index,
		    shim_reg);
#elif CONFIG_CANNONLAKE
	uint32_t shim_reg;

	shim_reg = shim_read(SHIM_GPDMA_CLKCTL(index)) |
		   SHIM_CLKCTL_LPGPDMAFDCGB;

	shim_write(SHIM_GPDMA_CLKCTL(index), shim_reg);

	trace_power("dis-dwdma-clk-gating index %d GPDMA_CLKCTL %08x", index,
		    shim_reg);
#endif
}

static inline void cavs_pm_runtime_en_dwdma_clk_gating(uint32_t index)
{
#if CONFIG_APOLLOLAKE
	uint32_t shim_reg;

	shim_reg = shim_read(SHIM_CLKCTL) & ~SHIM_CLKCTL_LPGPDMAFDCGB(index);

	shim_write(SHIM_CLKCTL, shim_reg);

	trace_power("en-dwdma-clk-gating index %d CLKCTL %08x", index,
		    shim_reg);
#elif CONFIG_CANNONLAKE
	uint32_t shim_reg;

	shim_reg = shim_read(SHIM_GPDMA_CLKCTL(index)) &
		   ~SHIM_CLKCTL_LPGPDMAFDCGB;

	shim_write(SHIM_GPDMA_CLKCTL(index), shim_reg);

	trace_power("en-dwdma-clk-gating index %d GPDMA_CLKCTL %08x", index,
		    shim_reg);
#endif
}

static inline void cavs_pm_runtime_core_dis_memory(uint32_t index)
{
#if CAVS_VERSION >= CAVS_VERSION_1_8
	void *core_memory_ptr;
	extern uintptr_t _sof_core_s_start;

	/* Address is calculated for index (0 for the master core) minus one
	 * since _sof_core_s_start is first slave core stack address
	 */
	core_memory_ptr = (char *)&_sof_core_s_start
		+ (index - 1) * SOF_CORE_S_SIZE;

	set_power_gate_for_memory_address_range(core_memory_ptr,
						SOF_CORE_S_SIZE, 0);

#endif
}

static inline void cavs_pm_runtime_core_en_memory(uint32_t index)
{
#if CAVS_VERSION >= CAVS_VERSION_1_8
	void *core_memory_ptr;
	extern uintptr_t _sof_core_s_start;

	/* Address is calculated for index (0 for the master core) minus one
	 * since _sof_core_s_start is first slave core stack address
	 */
	core_memory_ptr = (char *)&_sof_core_s_start
		+ (index - 1) * SOF_CORE_S_SIZE;

	set_power_gate_for_memory_address_range(core_memory_ptr,
						SOF_CORE_S_SIZE, 1);

#endif
}

static inline void cavs_pm_runtime_dis_dsp_pg(uint32_t index)
{
#if CAVS_VERSION >= CAVS_VERSION_1_8
	uint32_t lps_ctl;

	if (index == PLATFORM_MASTER_CORE_ID) {
		lps_ctl = shim_read(SHIM_LPSCTL);

		shim_write16(SHIM_PWRCTL, shim_read16(SHIM_PWRCTL) |
			     SHIM_PWRCTL_TCPDSPPG(index) |
			     SHIM_PWRCTL_TCPCTLPG);

		lps_ctl &= ~SHIM_LPSCTL_BID;
		lps_ctl &= ~SHIM_LPSCTL_BATTR_0;
		lps_ctl |= SHIM_LPSCTL_FDSPRUN;
		shim_write(SHIM_LPSCTL, lps_ctl);
	} else {
		shim_write16(SHIM_PWRCTL, shim_read16(SHIM_PWRCTL) |
			     SHIM_PWRCTL_TCPDSPPG(index));
	}
#endif
}

static inline void cavs_pm_runtime_en_dsp_pg(uint32_t index)
{
#if CAVS_VERSION >= CAVS_VERSION_1_8
	uint32_t lps_ctl;

	if (index == PLATFORM_MASTER_CORE_ID) {
		lps_ctl = shim_read(SHIM_LPSCTL);

#if CONFIG_ICELAKE
		shim_write16(SHIM_PWRCTL, SHIM_PWRCTL_TCPCTLPG);
#else
		shim_write16(SHIM_PWRCTL, 0);
#endif
		lps_ctl |= SHIM_LPSCTL_BID | SHIM_LPSCTL_BATTR_0;
		lps_ctl &= ~SHIM_LPSCTL_FDSPRUN;
		shim_write(SHIM_LPSCTL, lps_ctl);
	} else {
		shim_write16(SHIM_PWRCTL, shim_read16(SHIM_PWRCTL) &
			     ~SHIM_PWRCTL_TCPDSPPG(index));
	}
#endif
}

void platform_pm_runtime_init(struct pm_runtime_data *prd)
{
	struct cavs_pm_runtime_data *pprd;

	pprd = rzalloc(SOF_MEM_ZONE_SYS, 0, SOF_MEM_CAPS_RAM, sizeof(*pprd));
	prd->platform_data = pprd;
}

void platform_pm_runtime_get(enum pm_runtime_context context, uint32_t index,
			     uint32_t flags)
{
	/* Action based on context */
	switch (context) {
#if CONFIG_CAVS_SSP
	case SSP_CLK:
		cavs_pm_runtime_dis_ssp_clk_gating(index);
		break;
	case SSP_POW:
		cavs_pm_runtime_en_ssp_power(index);
		break;
#endif
#if CONFIG_CAVS_DMIC
	case DMIC_CLK:
		cavs_pm_runtime_dis_dmic_clk_gating(index);
		break;
	case DMIC_POW:
		cavs_pm_runtime_en_dmic_power(index);
		break;
#endif
	case DW_DMAC_CLK:
		cavs_pm_runtime_dis_dwdma_clk_gating(index);
		break;
	case CORE_MEMORY_POW:
		cavs_pm_runtime_core_en_memory(index);
		break;
	case PM_RUNTIME_DSP:
		cavs_pm_runtime_dis_dsp_pg(index);
		break;
	default:
		break;
	}
}

void platform_pm_runtime_put(enum pm_runtime_context context, uint32_t index,
			     uint32_t flags)
{
	switch (context) {
	case PM_RUNTIME_HOST_DMA_L1:
		cavs_pm_runtime_force_host_dma_l1_exit();
		break;
#if CONFIG_CAVS_SSP
	case SSP_CLK:
		cavs_pm_runtime_en_ssp_clk_gating(index);
		break;
	case SSP_POW:
		cavs_pm_runtime_dis_ssp_power(index);
		break;
#endif
#if CONFIG_CAVS_DMIC
	case DMIC_CLK:
		cavs_pm_runtime_en_dmic_clk_gating(index);
		break;
	case DMIC_POW:
		cavs_pm_runtime_dis_dmic_power(index);
		break;
#endif
	case DW_DMAC_CLK:
		cavs_pm_runtime_en_dwdma_clk_gating(index);
		break;
	case CORE_MEMORY_POW:
		cavs_pm_runtime_core_dis_memory(index);
		break;
	case PM_RUNTIME_DSP:
		cavs_pm_runtime_en_dsp_pg(index);
		break;
	default:
		break;
	}
}

void platform_pm_runtime_enable(uint32_t context, uint32_t index)
{
	switch (context) {
	case PM_RUNTIME_DSP:
		cavs_pm_runtime_enable_dsp(true);
		break;
	default:
		break;
	}
}

void platform_pm_runtime_disable(uint32_t context, uint32_t index)
{
	switch (context) {
	case PM_RUNTIME_DSP:
		cavs_pm_runtime_enable_dsp(false);
		break;
	default:
		break;
	}
}

bool platform_pm_runtime_is_active(uint32_t context, uint32_t index)
{
	switch (context) {
	case PM_RUNTIME_DSP:
		return cavs_pm_runtime_is_active_dsp();
	default:
		assert(false); /* unsupported query */
		return false;
	}

}

#if !CONFIG_SUECREEK
void platform_pm_runtime_power_off(void)
{
	uint32_t hpsram_mask[PLATFORM_HPSRAM_SEGMENTS], i;
	/* power down entire HPSRAM */
	for (i = 0; i < PLATFORM_HPSRAM_SEGMENTS; i++)
		hpsram_mask[i] = HPSRAM_MASK(i);

	power_down(true, hpsram_mask);
}
#endif
