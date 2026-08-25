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
 * Maps the legacy timer spellings used by the vendored libqdma sources onto
 * the modern API. Force-included (-include) only when the timer_modern probe
 * reports that the legacy names are gone, so the submodule stays pristine.
 */

#ifndef SLASH_QDMA_TIMER_COMPAT_H
#define SLASH_QDMA_TIMER_COMPAT_H

#include <linux/timer.h>

#ifndef del_timer
#define del_timer(t) timer_delete(t)
#endif

#ifndef from_timer
#define from_timer(var, callback_timer, timer_fieldname) \
	timer_container_of(var, callback_timer, timer_fieldname)
#endif

#endif /* SLASH_QDMA_TIMER_COMPAT_H */
