// SPDX-License-Identifier: BSD-3-Clause
//
// Copyright(c) 2016 Intel Corporation. All rights reserved.
//
// Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
//         Keyon Jie <yang.jie@linux.intel.com>

#include <sof/compiler_info.h>
#include <sof/debug/debug.h>
#include <sof/drivers/dw-dma.h>
#include <sof/drivers/interrupt.h>
#include <sof/drivers/ipc.h>
#include <sof/drivers/pmc.h>
#include <sof/drivers/timer.h>
#include <sof/fw-ready-metadata.h>
#include <sof/lib/agent.h>
#include <sof/lib/alloc.h>
#include <sof/lib/clk.h>
#include <sof/lib/cpu.h>
#include <sof/lib/dai.h>
#include <sof/lib/dma.h>
#include <sof/lib/mailbox.h>
#include <sof/lib/memory.h>
#include <sof/lib/notifier.h>
#include <sof/lib/shim.h>
#include <sof/platform.h>
#include <sof/schedule/edf_schedule.h>
#include <sof/schedule/ll_schedule.h>
#include <sof/schedule/ll_schedule_domain.h>
#include <sof/sof.h>
#include <sof/trace/dma-trace.h>
#include <sof/trace/trace.h>
#include <ipc/dai.h>
#include <ipc/header.h>
#include <ipc/info.h>
#include <kernel/abi.h>
#include <config.h>
#include <version.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static const struct sof_ipc_fw_ready ready
	__attribute__((section(".fw_ready"))) = {
	.hdr = {
		.cmd = SOF_IPC_FW_READY,
		.size = sizeof(struct sof_ipc_fw_ready),
	},
	/* dspbox is for DSP initiated IPC, hostbox is for host initiated IPC */
	.version = {
		.hdr.size = sizeof(struct sof_ipc_fw_version),
		.micro = SOF_MICRO,
		.minor = SOF_MINOR,
		.major = SOF_MAJOR,
#if CONFIG_DEBUG
		/* only added in debug for reproducability in releases */
		.build = SOF_BUILD,
		.date = __DATE__,
		.time = __TIME__,
#endif
		.tag = SOF_TAG,
		.abi_version = SOF_ABI_VERSION,
	},
	.flags = DEBUG_SET_FW_READY_FLAGS
};

#define NUM_BYT_WINDOWS		6
static const struct sof_ipc_window sram_window
	__attribute__((section(".fw_ready_metadata"))) = {
	.ext_hdr	= {
		.hdr.cmd = SOF_IPC_FW_READY,
		.hdr.size = sizeof(struct sof_ipc_window) +
			sizeof(struct sof_ipc_window_elem) * NUM_BYT_WINDOWS,
		.type	= SOF_IPC_EXT_WINDOW,
	},
	.num_windows	= NUM_BYT_WINDOWS,
	.window	= {
		{
			.type	= SOF_IPC_REGION_UPBOX,
			.id	= 0,	/* map to host window 0 */
			.flags	= 0, // TODO: set later
			.size	= MAILBOX_DSPBOX_SIZE,
			.offset	= MAILBOX_DSPBOX_OFFSET,
		},
		{
			.type	= SOF_IPC_REGION_DOWNBOX,
			.id	= 0,	/* map to host window 0 */
			.flags	= 0, // TODO: set later
			.size	= MAILBOX_HOSTBOX_SIZE,
			.offset	= MAILBOX_HOSTBOX_OFFSET,
		},
		{
			.type	= SOF_IPC_REGION_DEBUG,
			.id	= 0,	/* map to host window 0 */
			.flags	= 0, // TODO: set later
			.size	= MAILBOX_DEBUG_SIZE,
			.offset	= MAILBOX_DEBUG_OFFSET,
		},
		{
			.type	= SOF_IPC_REGION_TRACE,
			.id	= 0,	/* map to host window 0 */
			.flags	= 0, // TODO: set later
			.size	= MAILBOX_TRACE_SIZE,
			.offset	= MAILBOX_TRACE_OFFSET,
		},
		{
			.type	= SOF_IPC_REGION_STREAM,
			.id	= 0,	/* map to host window 0 */
			.flags	= 0, // TODO: set later
			.size	= MAILBOX_STREAM_SIZE,
			.offset	= MAILBOX_STREAM_OFFSET,
		},
		{
			.type	= SOF_IPC_REGION_EXCEPTION,
			.id	= 0,	/* map to host window 0 */
			.flags	= 0, // TODO: set later
			.size	= MAILBOX_EXCEPTION_SIZE,
			.offset	= MAILBOX_EXCEPTION_OFFSET,
		},
	},
};

SHARED_DATA struct timer timer = {
	.id = TIMER3, /* external timer */
	.irq = IRQ_NUM_EXT_TIMER,
};

int platform_boot_complete(uint32_t boot_message)
{
	uint32_t mb_offset = 0;
	uint64_t outbox = MAILBOX_HOST_OFFSET >> 3;

	mailbox_dspbox_write(mb_offset, &ready, sizeof(ready));
	mb_offset = mb_offset + sizeof(ready);

	mailbox_dspbox_write(mb_offset, &sram_window,
			     sram_window.ext_hdr.hdr.size);
	mb_offset = mb_offset + sram_window.ext_hdr.hdr.size;

	/* variable length compiler description is a last field of cc_version */
	mailbox_dspbox_write(mb_offset, &cc_version,
			     cc_version.ext_hdr.hdr.size);
	mb_offset = mb_offset + cc_version.ext_hdr.hdr.size;

	mailbox_dspbox_write(mb_offset, &probe_support,
			     probe_support.ext_hdr.hdr.size);
	mb_offset = mb_offset + probe_support.ext_hdr.hdr.size;

	mailbox_dspbox_write(mb_offset, &user_abi_version,
			     user_abi_version.ext_hdr.hdr.size);

	/* now interrupt host to tell it we are done booting */
	shim_write(SHIM_IPCDL, SOF_IPC_FW_READY | outbox);
	shim_write(SHIM_IPCDH, SHIM_IPCDH_BUSY);

	/* boot now complete so we can relax the CPU */
	/* For now skip this to gain more processing performance
	 * for SRC component.
	 */
	/* clock_set_freq(CLK_CPU, CLK_DEFAULT_CPU_HZ); */

	return 0;
}

int platform_init(struct sof *sof)
{
#if defined CONFIG_BAYTRAIL
	struct dai *ssp0;
	struct dai *ssp1;
	struct dai *ssp2;
#elif defined CONFIG_CHERRYTRAIL
	struct dai *ssp0;
	struct dai *ssp1;
	struct dai *ssp2;
	struct dai *ssp3;
	struct dai *ssp4;
	struct dai *ssp5;
#else
#error Undefined platform
#endif
	int ret;

	sof->platform_timer = &timer;
	sof->cpu_timer = &timer;

	/* clear mailbox for early trace and debug */
	trace_point(TRACE_BOOT_PLATFORM_MBOX);
	bzero((void *)MAILBOX_BASE, MAILBOX_SIZE);

	/* configure the shim */
	trace_point(TRACE_BOOT_PLATFORM_SHIM);
#if defined CONFIG_BAYTRAIL
	shim_write(SHIM_MISC, shim_read(SHIM_MISC) | 0x0000000e);
#elif defined CONFIG_CHERRYTRAIL
	shim_write(SHIM_MISC, shim_read(SHIM_MISC) | 0x00000e0e);
#endif

	/* init PMC IPC */
	trace_point(TRACE_BOOT_PLATFORM_PMC);
	platform_ipc_pmc_init();

	/* init timers, clocks and schedulers */
	trace_point(TRACE_BOOT_PLATFORM_TIMER);
	platform_timer_start(sof->platform_timer);

	trace_point(TRACE_BOOT_PLATFORM_CLOCK);
	platform_clock_init(sof);

	trace_point(TRACE_BOOT_PLATFORM_SCHED);
	scheduler_init_edf();

	/* init low latency timer domain and scheduler */
	sof->platform_timer_domain =
		timer_domain_init(sof->platform_timer, PLATFORM_DEFAULT_CLOCK,
				  CONFIG_SYSTICK_PERIOD);
	scheduler_init_ll(sof->platform_timer_domain);

	/* init the system agent */
	trace_point(TRACE_BOOT_PLATFORM_AGENT);
	sa_init(sof, CONFIG_SYSTICK_PERIOD);

	/* Set CPU to default frequency for booting */
	trace_point(TRACE_BOOT_PLATFORM_CPU_FREQ);
	clock_set_freq(CLK_CPU(cpu_get_id()), CLK_MAX_CPU_HZ);

	/* set SSP clock to 19.2M */
	trace_point(TRACE_BOOT_PLATFORM_SSP_FREQ);
	clock_set_freq(CLK_SSP, 19200000);

	/* init DMACs */
	trace_point(TRACE_BOOT_PLATFORM_DMA);
	ret = dmac_init(sof);
	if (ret < 0)
		return -ENODEV;

	/* init low latency multi channel DW-DMA domain and scheduler */
	sof->platform_dma_domain = dma_multi_chan_domain_init
			(&sof->dma_info->dma_array[PLATFORM_DW_DMA_INDEX],
			 PLATFORM_NUM_DW_DMACS,
			 PLATFORM_DEFAULT_CLOCK, true);
	scheduler_init_ll(sof->platform_dma_domain);

	/* initialise the host IPC mechanisms */
	trace_point(TRACE_BOOT_PLATFORM_IPC);
	ipc_init(sof);

	trace_point(TRACE_BOOT_PLATFORM_DAI);
	ret = dai_init(sof);
	if (ret < 0)
		return -ENODEV;

	/* mask SSP 0 - 2 interrupts */
	shim_write(SHIM_PIMR, shim_read(SHIM_PIMR) | 0x00000038);

#if defined CONFIG_CHERRYTRAIL
	/* mask SSP 3 - 5 interrupts */
	shim_write(SHIM_PIMRH, shim_read(SHIM_PIMRH) | 0x00000700);
#endif

	/* Reset M/N SSP clock dividers */
	shim_write(SHIM_SSP0_DIVL, 1);
	shim_write(SHIM_SSP0_DIVH, 0x80000001);
	shim_write(SHIM_SSP1_DIVL, 1);
	shim_write(SHIM_SSP1_DIVH, 0x80000001);
	shim_write(SHIM_SSP2_DIVL, 1);
	shim_write(SHIM_SSP2_DIVH, 0x80000001);
#if defined CONFIG_CHERRYTRAIL
	shim_write(SHIM_SSP3_DIVL, 1);
	shim_write(SHIM_SSP3_DIVH, 0x80000001);
	shim_write(SHIM_SSP4_DIVL, 1);
	shim_write(SHIM_SSP4_DIVH, 0x80000001);
	shim_write(SHIM_SSP5_DIVL, 1);
	shim_write(SHIM_SSP5_DIVH, 0x80000001);
#endif

	/* init SSP ports */
	trace_point(TRACE_BOOT_PLATFORM_SSP);
	ssp0 = dai_get(SOF_DAI_INTEL_SSP, 0, DAI_CREAT);
	if (ssp0 == NULL)
		return -ENODEV;

	ssp1 = dai_get(SOF_DAI_INTEL_SSP, 1, DAI_CREAT);
	if (ssp1 == NULL)
		return -ENODEV;

	ssp2 = dai_get(SOF_DAI_INTEL_SSP, 2, DAI_CREAT);
	if (ssp2 == NULL)
		return -ENODEV;

#if defined CONFIG_CHERRYTRAIL
	ssp3 = dai_get(SOF_DAI_INTEL_SSP, 3, DAI_CREAT);
	if (ssp3 == NULL)
		return -ENODEV;

	ssp4 = dai_get(SOF_DAI_INTEL_SSP, 4, DAI_CREAT);
	if (ssp4 == NULL)
		return -ENODEV;

	ssp5 = dai_get(SOF_DAI_INTEL_SSP, 5, DAI_CREAT);
	if (ssp5 == NULL)
		return -ENODEV;
#endif

#if CONFIG_TRACE
	/* Initialize DMA for Trace*/
	trace_point(TRACE_BOOT_PLATFORM_DMA_TRACE);
	dma_trace_init_complete(sof->dmat);
#endif

	/* show heap status */
	heap_trace_all(1);

	return 0;
}
