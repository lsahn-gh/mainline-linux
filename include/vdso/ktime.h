/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __VDSO_KTIME_H
#define __VDSO_KTIME_H

#include <vdso/jiffies.h>

/*
 * The resolution of the clocks. The resolution value is returned in
 * the clock_getres() system call to give application programmers an
 * idea of the (in)accuracy of timers. Timer values are rounded up to
 * this resolution values.
 */
/*
 * IAMROOT, 2022.09.17:
 * - ex) hz = 1000
 *   (10^9 + 1000 / 2) / 1000 = 1000_000 nsec = 1 msec
 */
#define LOW_RES_NSEC		TICK_NSEC
#define KTIME_LOW_RES		(LOW_RES_NSEC)

#endif /* __VDSO_KTIME_H */
