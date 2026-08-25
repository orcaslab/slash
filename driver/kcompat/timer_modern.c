/**
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * This program is free software; you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation; version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program; if
 * not, write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/*
 * Modern timer API probe.
 *
 * Pre-6.15:  del_timer() / from_timer() are the documented spellings.
 * 6.15+:     both were removed in favour of timer_delete() /
 *            timer_container_of().
 *
 * The vendored libqdma sources still use the legacy spellings, so this
 * probe drives the compat shim that maps them onto the modern API.
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/timer.h>

struct conftest_holder {
	struct timer_list timer;
};

static void conftest_callback(struct timer_list *t)
{
	struct conftest_holder *h = timer_container_of(h, t, timer);

	(void)h;
}

static int __init conftest_init(void)
{
	static struct conftest_holder h;

	timer_setup(&h.timer, conftest_callback, 0);
	return timer_delete(&h.timer);
}

static void __exit conftest_exit(void)
{
}

module_init(conftest_init);
module_exit(conftest_exit);
MODULE_LICENSE("GPL");
