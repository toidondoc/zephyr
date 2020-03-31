// SPDX-License-Identifier: BSD-3-Clause
//
// Copyright(c) 2019 Intel Corporation. All rights reserved.
//
// Author: Tomasz Lauda <tomasz.lauda@linux.intel.com>
//         Janusz Jankowski <janusz.jankowski@linux.intel.com>

#include <sof/common.h>
#include <sof/drivers/ssp.h>
#include <sof/lib/clk.h>

const struct freq_table platform_cpu_freq[] = {
	{ 100000000, 100000 },
	{ 200000000, 200000 },
	{ CLK_MAX_CPU_HZ, 400000 },
};

const uint32_t cpu_freq_enc[] = {
	0x3,
	0x1,
	0x0,
};

STATIC_ASSERT(NUM_CPU_FREQ == ARRAY_SIZE(platform_cpu_freq),
	      invalid_number_of_cpu_frequencies);

const struct freq_table *cpu_freq = platform_cpu_freq;

/* IMPORTANT: array should be filled in increasing order
 * (regarding to .freq field)
 */
const struct freq_table platform_ssp_freq[] = {
	{ 19200000, 19200 },
	{ 24576000, 24576 },
	{ 96000000, 96000 },
};

const uint32_t platform_ssp_freq_sources[] = {
	SSP_CLOCK_XTAL_OSCILLATOR,
	SSP_CLOCK_AUDIO_CARDINAL,
	SSP_CLOCK_PLL_FIXED,
};

STATIC_ASSERT(NUM_SSP_FREQ == ARRAY_SIZE(platform_ssp_freq),
	      invalid_number_of_ssp_frequencies);

const struct freq_table *ssp_freq = platform_ssp_freq;
const uint32_t *ssp_freq_sources = platform_ssp_freq_sources;
