/*
AC Event Timer
Copyright (C) 2026 alejoxcode

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#pragma once

#include "timer-painter.hpp"

#include <obs-module.h>

#define TIMER_SOURCE_ID "ac_event_timer"
#define TARGET_FORMAT "yyyy-MM-dd HH:mm:ss"

enum TimerMode { MODE_DATE = 0, MODE_DURATION, MODE_STOPWATCH };

struct TimerSource;

struct TimerStatus {
	int mode = MODE_DATE;
	TimeView view;
};

void timer_source_register();
void timer_renderer_start();
void timer_renderer_stop();

/* Controls used by the dock and the hotkeys. Safe to call from any thread. */
TimerSource *timer_from_source(obs_source_t *source);
TimerStatus timer_status(TimerSource *timer);
void timer_toggle(TimerSource *timer);
void timer_reset(TimerSource *timer);
void timer_add_seconds(TimerSource *timer, int seconds);
