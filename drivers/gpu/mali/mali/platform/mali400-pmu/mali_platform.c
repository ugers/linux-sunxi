/*
 * Copyright (C) 2010-2011 ARM Limited. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation, and any use by you of this program is subject to the terms of such GNU licence.
 *
 * A copy of the licence is included with the program, and can also be obtained from Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

/**
 * @file mali_platform.c
 * Platform specific Mali driver functions for a default platform
 */
#include "mali_kernel_common.h"
#include "mali_osk.h"
#include "mali_platform.h"

#include <linux/module.h>
#include <linux/clk.h>
#include <linux/workqueue.h>
#include <linux/timer.h>
#include <mach/irqs.h>
#include <mach/clock.h>
#include <plat/sys_config.h>

#ifdef CONFIG_MALI400_BOOST
#define MALI400_BOOST_RATE	1200000000	/* Hz */
#define MALI_INIT_RATE		960000000	/* Hz */
#define MALI_BOOST_DURATION	500		/* msec */
#endif

int mali_clk_div = 3;
module_param(mali_clk_div, int,
	     S_IRUSR | S_IWUSR | S_IWGRP | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(mali_clk_div, "Clock divisor for mali");

struct clk *h_ahb_mali, *h_mali_clk, *h_ve_pll;
int mali_clk_flag = 0;

#ifdef CONFIG_MALI400_BOOST
int mali_boost_rate = MALI400_BOOST_RATE / 1000000;
module_param(mali_boost_rate, int,
	     S_IRUSR | S_IWUSR | S_IWGRP | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(mali_boost_rate, "Mali boost rate for power HAL");

int mali_boost_duration = MALI_BOOST_DURATION;
module_param(mali_boost_duration, int,
	     S_IRUSR | S_IWUSR | S_IWGRP | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(mali_boost_rate, "Mali boost duration for power HAL");

static struct timer_list boost_timer;
unsigned long mali_init_rate = MALI_INIT_RATE;
static bool boot = true;
static bool boost_on = false;
static DEFINE_MUTEX(boost_mutex);

int mali_boost(void)
{
	if (boost_on)
		return -1;

	mutex_lock(&boost_mutex);
	h_ve_pll = clk_get(NULL, "ve_pll");
	if (!h_ve_pll)
		return -1;

	clk_set_rate(h_ve_pll, mali_boost_rate * 1000000);
	mali_platform_init();
	boost_on = true;
	mutex_unlock(&boost_mutex);
	mod_timer(&boost_timer, jiffies + msecs_to_jiffies(mali_boost_duration));

	return 0;
}

static void boost(struct work_struct *boost_sched_work)
{
	mutex_lock(&boost_mutex);
	h_ve_pll = clk_get(NULL, "ve_pll");
	if (!h_ve_pll)
		MALI_PRINT(("try to get ve pll clock failed!\n"));
	clk_set_rate(h_ve_pll, MALI_INIT_RATE);
	mali_platform_init();
	boost_on = false;
	mutex_unlock(&boost_mutex);
}

static DECLARE_WORK(boost_work, boost);
static void boost_sched_work(unsigned long data)
{
	schedule_work(&boost_work);
}
#endif

_mali_osk_errcode_t mali_platform_init(void)
{
	unsigned long rate;
	int clk_div;
	int mali_used = 0;

	/* get mali ahb clock */
	h_ahb_mali = clk_get(NULL, "ahb_mali");
	if (!h_ahb_mali)
		MALI_PRINT(("try to get ahb mali clock failed!\n"));

	/* get mali clk */
	h_mali_clk = clk_get(NULL, "mali");
	if (!h_mali_clk)
		MALI_PRINT(("try to get mali clock failed!\n"));

	/* get pll4 clock */
	h_ve_pll = clk_get(NULL, "ve_pll");
	if (!h_ve_pll)
		MALI_PRINT(("try to get ve pll clock failed!\n"));

	/* set mali parent clock */
	if (clk_set_parent(h_mali_clk, h_ve_pll))
		MALI_PRINT(("try to set mali clock source failed!\n"));

	/* set mali clock */
	rate = clk_get_rate(h_ve_pll);

	if (!script_parser_fetch("mali_para", "mali_used", &mali_used, 1)) {
		if (mali_used == 1) {
			if (!script_parser_fetch
			    ("mali_para", "mali_clkdiv", &clk_div, 1)) {
				if (clk_div > 0) {
					MALI_DEBUG_PRINT(3, ("Mali: use config clk_div %d\n", clk_div));
					mali_clk_div = clk_div;
				}
			}
		}
	}

	MALI_DEBUG_PRINT(3, ("Mali: clk_div %d\n", mali_clk_div));
	rate /= mali_clk_div;

	if (clk_set_rate(h_mali_clk, rate))
		MALI_PRINT(("try to set mali clock failed!\n"));

	if (clk_reset(h_mali_clk, 0))
		MALI_PRINT(("try to reset release failed!\n"));

#ifdef CONFIG_MALI400_BOOST
	MALI_DEBUG_PRINT(3, ("Mali: clock set completed, clock is %d Mhz\n",
			rate / 1000000));
#else
	MALI_PRINT(("clock set completed, clock is %d Mhz\n",
			rate / 1000000));
#endif

	/* enable mali axi/apb clock */
	if (mali_clk_flag == 0) {
		mali_clk_flag = 1;
		if (clk_enable(h_ahb_mali))
			MALI_PRINT(("try to enable mali ahb failed!\n"));

		if (clk_enable(h_mali_clk))
			MALI_PRINT(("try to enable mali clock failed!\n"));
	}

#ifdef CONFIG_MALI400_BOOST
	if (boot) {
		MALI_PRINT(("clk_div %d\n", mali_clk_div));
		MALI_PRINT(("clock set completed, clock is %d Mhz\n",
			rate / 1000000));
		mali_init_rate = (long int)h_ve_pll;
		setup_timer(&boost_timer, boost_sched_work, 0);
		boot = false;
	}
#endif

	MALI_SUCCESS;
}

_mali_osk_errcode_t mali_platform_deinit(void)
{
	/* close mali axi/apb clock */
	if (mali_clk_flag == 1) {
		mali_clk_flag = 0;
		clk_disable(h_mali_clk);
		clk_disable(h_ahb_mali);
	}

#ifdef CONFIG_MALI400_BOOST
	del_timer(&boost_timer);
#endif

	MALI_SUCCESS;
}

_mali_osk_errcode_t mali_platform_power_mode_change(mali_power_mode power_mode)
{
	MALI_SUCCESS;
}

void mali_gpu_utilization_handler(u32 utilization)
{
}

void set_mali_parent_power_domain(void *dev)
{
}
