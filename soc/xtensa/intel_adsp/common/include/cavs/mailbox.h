/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright(c) 2019 Intel Corporation. All rights reserved.
 *
 * Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
 *         Keyon Jie <yang.jie@linux.intel.com>
 */

#ifdef __PLATFORM_LIB_MAILBOX_H__

#ifndef __CAVS_LIB_MAILBOX_H__
#define __CAVS_LIB_MAILBOX_H__

#include <sof/lib/memory.h>
#include <stdint.h>

/*
 * The Window Region on HPSRAM for cAVS platforms is organised like this :-
 * +--------------------------------------------------------------------------+
 * | Offset              | Region         |  Size                             |
 * +---------------------+----------------+-----------------------------------+
 * | SRAM_TRACE_BASE     | Trace Buffer W3|  SRAM_TRACE_SIZE                  |
 * +---------------------+----------------+-----------------------------------+
 * | SRAM_DEBUG_BASE     | Debug data  W2 |  SRAM_DEBUG_SIZE                  |
 * +---------------------+----------------+-----------------------------------+
 * | SRAM_INBOX_BASE     | Inbox  W1      |  SRAM_INBOX_SIZE                  |
 * +---------------------+----------------+-----------------------------------+
 * | SRAM_OUTBOX_BASE    | Outbox W0      |  SRAM_MAILBOX_SIZE                |
 * +---------------------+----------------+-----------------------------------+
 * | SRAM_SW_REG_BASE    | SW Registers W0|  SRAM_SW_REG_SIZE                 |
 * +---------------------+----------------+-----------------------------------+
 *
 * Note: For suecreek SRAM_SW_REG window does not exist - MAILBOX_SW_REG_BASE
 *	 and MAILBOX_SW_REG_BASE are equal to 0
 */

 /* window 3 - trace */
#define MAILBOX_TRACE_SIZE	SRAM_TRACE_SIZE
#define MAILBOX_TRACE_BASE	SRAM_TRACE_BASE

 /* window 2 debug, exception and stream */
#define MAILBOX_DEBUG_SIZE	SRAM_DEBUG_SIZE
#define MAILBOX_DEBUG_BASE	SRAM_DEBUG_BASE

#define MAILBOX_EXCEPTION_SIZE	SRAM_EXCEPT_SIZE
#define MAILBOX_EXCEPTION_BASE	SRAM_EXCEPT_BASE
#define MAILBOX_EXCEPTION_OFFSET  SRAM_DEBUG_SIZE

#define MAILBOX_STREAM_SIZE    SRAM_STREAM_SIZE
#define MAILBOX_STREAM_BASE    SRAM_STREAM_BASE
#define MAILBOX_STREAM_OFFSET  (SRAM_DEBUG_SIZE + SRAM_EXCEPT_SIZE)

 /* window 1 inbox/downlink and FW registers */
#define MAILBOX_HOSTBOX_SIZE	SRAM_INBOX_SIZE
#define MAILBOX_HOSTBOX_BASE	SRAM_INBOX_BASE

 /* window 0 */
#define MAILBOX_DSPBOX_SIZE	SRAM_OUTBOX_SIZE
#define MAILBOX_DSPBOX_BASE	SRAM_OUTBOX_BASE

#define MAILBOX_SW_REG_SIZE	SRAM_SW_REG_SIZE
#define MAILBOX_SW_REG_BASE	SRAM_SW_REG_BASE

static inline void mailbox_sw_reg_write(size_t offset, uint32_t src)
{
	volatile uint32_t *ptr;

	ptr = (volatile uint32_t *)(MAILBOX_SW_REG_BASE + offset);
	ptr = cache_to_uncache(ptr);
	*ptr = src;
}

#endif /* __CAVS_LIB_MAILBOX_H__ */

#else

#error "This file shouldn't be included from outside of platform/lib/mailbox.h"

#endif /* __PLATFORM_LIB_MAILBOX_H__ */
