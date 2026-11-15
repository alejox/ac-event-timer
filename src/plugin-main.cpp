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

#include "timer-dock.hpp"
#include "timer-source.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QMainWindow>
#include <QPointer>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static QPointer<TimerDock> timerDock;

MODULE_EXPORT const char *obs_module_name(void)
{
	return "AC Event Timer";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Animated countdowns for events and launches, with a control dock.";
}

static void frontendEvent(enum obs_frontend_event event, void *)
{
	/* Stop touching sources once OBS starts tearing things down. */
	if ((event == OBS_FRONTEND_EVENT_EXIT || event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN) && timerDock)
		timerDock->shutdown();
}

bool obs_module_load(void)
{
	timer_source_register();
	timer_renderer_start();

	auto *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!mainWindow) {
		obs_log(LOG_WARNING, "no main window, the dock was not created");
		return true;
	}

	/* OBS takes ownership of the widget and destroys it with the main window. */
	auto *dock = new TimerDock(mainWindow);
	if (!obs_frontend_add_dock_by_id("ac-event-timer-dock", obs_module_text("DockTitle"), dock)) {
		obs_log(LOG_ERROR, "could not register the dock");
		delete dock;
		return true;
	}
	timerDock = dock;
	obs_frontend_add_event_callback(frontendEvent, nullptr);

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(frontendEvent, nullptr);
	timer_renderer_stop();
	obs_log(LOG_INFO, "plugin unloaded");
}
