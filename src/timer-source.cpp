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

#include "timer-source.hpp"

#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/threading.h>

#include <QDateTime>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct TimerSource {
	obs_source_t *source = nullptr;

	std::mutex mutex; /* guards everything below */
	PaintConfig config;
	int mode = MODE_DATE;
	QString target;
	qint64 targetMs = 0;
	qint64 refMs = 0;
	qint64 durationMs = 0;
	bool running = false;
	qint64 anchorMs = 0; /* duration: end time, stopwatch: start time */
	qint64 holdMs = 0;   /* duration: time left while paused, stopwatch: time counted while paused */
	bool autoStart = false;
	std::string switchScene;
	bool loaded = false;
	bool keepRef = false;

	bool armed = false;
	bool fired = false;
	qint64 shownAtMs = 0;

	QImage pending;
	bool dirty = false;

	gs_texture_t *texture = nullptr; /* graphics thread only */
	uint32_t textureWidth = 0;
	uint32_t textureHeight = 0;
	std::atomic<uint32_t> width{0};
	std::atomic<uint32_t> height{0};

	obs_hotkey_id hotkeys[4] = {OBS_INVALID_HOTKEY_ID, OBS_INVALID_HOTKEY_ID, OBS_INVALID_HOTKEY_ID,
				    OBS_INVALID_HOTKEY_ID};
};

namespace {

qint64 nowMs()
{
	return QDateTime::currentMSecsSinceEpoch();
}

QColor colorFromObs(long long value)
{
	return QColor(int(value & 0xff), int((value >> 8) & 0xff), int((value >> 16) & 0xff));
}

long long obsColor(int r, int g, int b)
{
	return 0xff000000LL | (static_cast<long long>(b) << 16) | (static_cast<long long>(g) << 8) | r;
}

qint64 parseTarget(const QString &text)
{
	const QString trimmed = text.trimmed();
	if (trimmed.isEmpty())
		return 0;
	static const char *formats[] = {"yyyy-MM-dd HH:mm:ss", "yyyy-MM-dd HH:mm", "yyyy-MM-ddTHH:mm:ss",
					"yyyy-MM-ddTHH:mm",    "dd/MM/yyyy HH:mm", "yyyy-MM-dd"};
	for (const char *format : formats) {
		const QDateTime parsed = QDateTime::fromString(trimmed, QString::fromLatin1(format));
		if (parsed.isValid())
			return parsed.toMSecsSinceEpoch();
	}
	return 0;
}

/* Call with timer->mutex held. */
TimeView viewLocked(TimerSource *timer, qint64 now)
{
	TimeView view;
	switch (timer->mode) {
	case MODE_DATE:
		if (timer->targetMs <= 0)
			return view;
		view.valid = true;
		view.running = true;
		view.secs = double(timer->targetMs - now) / 1000.0;
		view.span = timer->refMs > 0 ? double(timer->targetMs - timer->refMs) / 1000.0 : 0.0;
		break;
	case MODE_DURATION:
		view.valid = true;
		view.running = timer->running;
		view.secs = double(timer->running ? timer->anchorMs - now : timer->holdMs) / 1000.0;
		view.span = double(timer->durationMs) / 1000.0;
		break;
	default:
		view.valid = true;
		view.countUp = true;
		view.running = timer->running;
		view.secs = double(timer->running ? now - timer->anchorMs : timer->holdMs) / 1000.0;
		return view;
	}
	if (view.secs <= 0.0) {
		view.finished = true;
		view.sinceFinish = -view.secs;
		view.secs = 0.0;
	}
	return view;
}

/* Runtime state lives in the source settings so it survives restarts. */
void saveRuntime(TimerSource *timer)
{
	bool running;
	qint64 anchor, hold, ref;
	{
		std::lock_guard<std::mutex> lock(timer->mutex);
		running = timer->running;
		anchor = timer->anchorMs;
		hold = timer->holdMs;
		ref = timer->refMs;
	}
	obs_data_t *settings = obs_source_get_settings(timer->source);
	obs_data_set_bool(settings, "running", running);
	obs_data_set_int(settings, "anchor_ms", anchor);
	obs_data_set_int(settings, "hold_ms", hold);
	obs_data_set_int(settings, "ref_ms", ref);
	obs_data_release(settings);
}

void switchSceneTask(void *param)
{
	char *name = static_cast<char *>(param);
	obs_source_t *scene = obs_get_source_by_name(name);
	if (scene) {
		if (obs_source_is_scene(scene))
			obs_frontend_set_current_scene(scene);
		obs_source_release(scene);
	}
	bfree(name);
}

/* ------------------------------------------------------------------ */
/* One background thread paints every visible timer, so a busy or blocked
 * OBS window never freezes the countdown that viewers see. */

struct Renderer {
	std::mutex mutex;
	std::vector<TimerSource *> timers;
	std::thread thread;
	std::condition_variable wake;
	bool stop = false;
} renderer;

void renderLoop()
{
	os_set_thread_name("ac-event-timer");
	std::unique_lock<std::mutex> lock(renderer.mutex);
	while (!renderer.stop) {
		bool anyVisible = false;
		const qint64 now = nowMs();

		for (TimerSource *timer : renderer.timers) {
			PaintConfig config;
			TimeView view;
			double shownFor;
			std::string scene;
			{
				std::lock_guard<std::mutex> guard(timer->mutex);
				view = viewLocked(timer, now);
				if (!view.finished) {
					timer->armed = view.valid;
					timer->fired = false;
				} else if (timer->armed && !timer->fired) {
					/* Only fires for a zero we actually watched arrive, never for a
					 * date that was already in the past when OBS opened. */
					timer->fired = true;
					scene = timer->switchScene;
				}
				config = timer->config;
				shownFor = double(now - timer->shownAtMs) / 1000.0;
			}
			if (!scene.empty())
				obs_queue_task(OBS_TASK_UI, switchSceneTask, bstrdup(scene.c_str()), false);

			if (!obs_source_showing(timer->source))
				continue;
			anyVisible = true;
			QImage image = paintTimer(config, view, shownFor);
			std::lock_guard<std::mutex> guard(timer->mutex);
			timer->pending = std::move(image);
			timer->dirty = true;
		}
		renderer.wake.wait_for(lock, std::chrono::milliseconds(anyVisible ? 33 : 120));
	}
}

/* ------------------------------------------------------------------ */

const char *timerGetName(void *)
{
	return obs_module_text("SourceName");
}

void timerUpdate(void *data, obs_data_t *settings)
{
	auto *timer = static_cast<TimerSource *>(data);
	bool persist = false;
	{
		std::lock_guard<std::mutex> lock(timer->mutex);
		PaintConfig &c = timer->config;
		c.style = int(obs_data_get_int(settings, "style"));
		c.width = int(obs_data_get_int(settings, "width"));
		c.title = QString::fromUtf8(obs_data_get_string(settings, "title"));
		c.subtitle = QString::fromUtf8(obs_data_get_string(settings, "subtitle"));
		c.finishText = QString::fromUtf8(obs_data_get_string(settings, "finish_text"));
		c.accent = colorFromObs(obs_data_get_int(settings, "accent"));
		c.text = colorFromObs(obs_data_get_int(settings, "text_color"));
		c.card = colorFromObs(obs_data_get_int(settings, "card_color"));
		c.background = colorFromObs(obs_data_get_int(settings, "bg_color"));
		c.urgent = colorFromObs(obs_data_get_int(settings, "urgent_color"));
		c.cardOpacity = double(obs_data_get_int(settings, "card_opacity")) / 100.0;
		c.backgroundOn = obs_data_get_bool(settings, "bg_on");
		c.backgroundOpacity = double(obs_data_get_int(settings, "bg_opacity")) / 100.0;
		c.labels = int(obs_data_get_int(settings, "labels"));
		c.days = int(obs_data_get_int(settings, "days"));
		c.entrance = obs_data_get_bool(settings, "entrance");
		c.urgentSecs = int(obs_data_get_int(settings, "urgent_secs"));
		c.finishDisplay = int(obs_data_get_int(settings, "finish_display"));
		c.confetti = obs_data_get_bool(settings, "confetti");
		c.hideAfter = int(obs_data_get_int(settings, "hide_after"));

		obs_data_t *font = obs_data_get_obj(settings, "font");
		c.fontFamily = QString::fromUtf8(obs_data_get_string(font, "face"));
		obs_data_release(font);

		c.labelDays[0] = QString::fromUtf8(obs_module_text("Unit.Days"));
		c.labelHours[0] = QString::fromUtf8(obs_module_text("Unit.Hours"));
		c.labelMinutes[0] = QString::fromUtf8(obs_module_text("Unit.Minutes"));
		c.labelSeconds[0] = QString::fromUtf8(obs_module_text("Unit.Seconds"));
		c.labelDays[1] = QString::fromUtf8(obs_module_text("Unit.D"));
		c.labelHours[1] = QString::fromUtf8(obs_module_text("Unit.H"));
		c.labelMinutes[1] = QString::fromUtf8(obs_module_text("Unit.M"));
		c.labelSeconds[1] = QString::fromUtf8(obs_module_text("Unit.S"));

		timer->autoStart = obs_data_get_bool(settings, "auto_start");
		timer->switchScene = obs_data_get_string(settings, "switch_scene");

		const int mode = int(obs_data_get_int(settings, "mode"));
		const QString target = QString::fromUtf8(obs_data_get_string(settings, "target"));
		const qint64 duration =
			(obs_data_get_int(settings, "dur_h") * 3600 + obs_data_get_int(settings, "dur_m") * 60 +
			 obs_data_get_int(settings, "dur_s")) *
			1000;

		if (!timer->loaded) {
			timer->running = obs_data_get_bool(settings, "running");
			timer->anchorMs = obs_data_get_int(settings, "anchor_ms");
			timer->holdMs = obs_data_get_int(settings, "hold_ms");
			timer->refMs = obs_data_get_int(settings, "ref_ms");
			if (!obs_data_has_user_value(settings, "hold_ms") && mode == MODE_DURATION)
				timer->holdMs = duration;
		} else if (mode != timer->mode || (mode == MODE_DURATION && duration != timer->durationMs)) {
			/* A new mode or length starts from a clean, stopped timer. */
			timer->running = false;
			timer->holdMs = mode == MODE_DURATION ? duration : 0;
			timer->anchorMs = 0;
			persist = true;
		}

		const bool targetChanged = timer->loaded && target != timer->target;
		if (timer->refMs <= 0 || (targetChanged && !timer->keepRef)) {
			timer->refMs = nowMs();
			persist = true;
		}
		timer->keepRef = false;
		timer->mode = mode;
		timer->target = target;
		timer->targetMs = parseTarget(target);
		timer->durationMs = duration;
		timer->loaded = true;

		const QSize size = timerCanvasSize(c);
		timer->width = uint32_t(size.width());
		timer->height = uint32_t(size.height());
	}
	if (persist)
		saveRuntime(timer);
}

void hotkeyToggle(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		timer_toggle(static_cast<TimerSource *>(data));
}

void hotkeyReset(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		timer_reset(static_cast<TimerSource *>(data));
}

void hotkeyPlus(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		timer_add_seconds(static_cast<TimerSource *>(data), 60);
}

void hotkeyMinus(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		timer_add_seconds(static_cast<TimerSource *>(data), -60);
}

void *timerCreate(obs_data_t *settings, obs_source_t *source)
{
	auto *timer = new TimerSource();
	timer->source = source;
	timer->shownAtMs = nowMs();
	timerUpdate(timer, settings);

	timer->hotkeys[0] = obs_hotkey_register_source(source, "ac_event_timer.toggle",
						       obs_module_text("Hotkey.Toggle"), hotkeyToggle, timer);
	timer->hotkeys[1] = obs_hotkey_register_source(source, "ac_event_timer.reset", obs_module_text("Hotkey.Reset"),
						       hotkeyReset, timer);
	timer->hotkeys[2] = obs_hotkey_register_source(source, "ac_event_timer.plus", obs_module_text("Hotkey.Plus"),
						       hotkeyPlus, timer);
	timer->hotkeys[3] = obs_hotkey_register_source(source, "ac_event_timer.minus", obs_module_text("Hotkey.Minus"),
						       hotkeyMinus, timer);

	std::lock_guard<std::mutex> lock(renderer.mutex);
	renderer.timers.push_back(timer);
	return timer;
}

void timerDestroy(void *data)
{
	auto *timer = static_cast<TimerSource *>(data);
	{
		std::lock_guard<std::mutex> lock(renderer.mutex);
		auto &list = renderer.timers;
		list.erase(std::remove(list.begin(), list.end(), timer), list.end());
	}
	for (obs_hotkey_id id : timer->hotkeys)
		obs_hotkey_unregister(id);
	if (timer->texture) {
		obs_enter_graphics();
		gs_texture_destroy(timer->texture);
		obs_leave_graphics();
	}
	delete timer;
}

uint32_t timerWidth(void *data)
{
	return static_cast<TimerSource *>(data)->width;
}

uint32_t timerHeight(void *data)
{
	return static_cast<TimerSource *>(data)->height;
}

void timerShow(void *data)
{
	auto *timer = static_cast<TimerSource *>(data);
	std::lock_guard<std::mutex> lock(timer->mutex);
	timer->shownAtMs = nowMs();
}

void timerActivate(void *data)
{
	auto *timer = static_cast<TimerSource *>(data);
	bool restart;
	{
		std::lock_guard<std::mutex> lock(timer->mutex);
		restart = timer->autoStart && timer->mode != MODE_DATE;
	}
	if (restart) {
		timer_reset(timer);
		timer_toggle(timer);
	}
}

void timerRender(void *data, gs_effect_t *effect)
{
	auto *timer = static_cast<TimerSource *>(data);
	{
		std::lock_guard<std::mutex> lock(timer->mutex);
		if (timer->dirty && !timer->pending.isNull()) {
			const uint32_t w = uint32_t(timer->pending.width());
			const uint32_t h = uint32_t(timer->pending.height());
			if (!timer->texture || timer->textureWidth != w || timer->textureHeight != h) {
				if (timer->texture)
					gs_texture_destroy(timer->texture);
				timer->texture = gs_texture_create(w, h, GS_BGRA, 1, nullptr, GS_DYNAMIC);
				timer->textureWidth = w;
				timer->textureHeight = h;
			}
			if (timer->texture)
				gs_texture_set_image(timer->texture, timer->pending.constBits(),
						     uint32_t(timer->pending.bytesPerLine()), false);
			timer->dirty = false;
		}
	}
	if (!timer->texture)
		return;

	/* The painted image is premultiplied, same as OBS image sources. */
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), timer->texture);
	gs_draw_sprite(timer->texture, 0, timer->textureWidth, timer->textureHeight);
	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous);
}

void timerDefaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "mode", MODE_DURATION);
	obs_data_set_default_string(settings, "target", "");
	obs_data_set_default_int(settings, "dur_h", 0);
	obs_data_set_default_int(settings, "dur_m", 5);
	obs_data_set_default_int(settings, "dur_s", 0);
	obs_data_set_default_bool(settings, "auto_start", false);
	obs_data_set_default_string(settings, "title", "");
	obs_data_set_default_string(settings, "subtitle", "");
	obs_data_set_default_string(settings, "finish_text", obs_module_text("Default.FinishText"));
	obs_data_set_default_int(settings, "style", STYLE_BLOCKS);
	obs_data_set_default_int(settings, "width", 1280);
	obs_data_set_default_int(settings, "accent", obsColor(0x7c, 0x5c, 0xff));
	obs_data_set_default_int(settings, "text_color", obsColor(0xff, 0xff, 0xff));
	obs_data_set_default_int(settings, "card_color", obsColor(0x16, 0x18, 0x22));
	obs_data_set_default_int(settings, "card_opacity", 92);
	obs_data_set_default_bool(settings, "bg_on", false);
	obs_data_set_default_int(settings, "bg_color", obsColor(0x0b, 0x0c, 0x12));
	obs_data_set_default_int(settings, "bg_opacity", 75);
	obs_data_set_default_int(settings, "labels", LABELS_LONG);
	obs_data_set_default_int(settings, "days", DAYS_AUTO);
	obs_data_set_default_bool(settings, "entrance", true);
	obs_data_set_default_int(settings, "urgent_secs", 10);
	obs_data_set_default_int(settings, "urgent_color", obsColor(0xff, 0x4d, 0x5e));
	obs_data_set_default_int(settings, "finish_display", FINISH_MESSAGE);
	obs_data_set_default_bool(settings, "confetti", true);
	obs_data_set_default_int(settings, "hide_after", 0);
	obs_data_set_default_string(settings, "switch_scene", "");

	obs_data_t *font = obs_data_create();
#ifdef _WIN32
	obs_data_set_default_string(font, "face", "Segoe UI");
#elif defined(__APPLE__)
	obs_data_set_default_string(font, "face", "Helvetica Neue");
#else
	obs_data_set_default_string(font, "face", "Sans Serif");
#endif
	obs_data_set_default_int(font, "size", 64);
	obs_data_set_default_obj(settings, "font", font);
	obs_data_release(font);
}

bool modeChanged(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const long long mode = obs_data_get_int(settings, "mode");
	obs_property_set_visible(obs_properties_get(props, "target"), mode == MODE_DATE);
	obs_property_set_visible(obs_properties_get(props, "target_help"), mode == MODE_DATE);
	obs_property_set_visible(obs_properties_get(props, "dur_h"), mode == MODE_DURATION);
	obs_property_set_visible(obs_properties_get(props, "dur_m"), mode == MODE_DURATION);
	obs_property_set_visible(obs_properties_get(props, "dur_s"), mode == MODE_DURATION);
	obs_property_set_visible(obs_properties_get(props, "auto_start"), mode != MODE_DATE);
	obs_property_set_visible(obs_properties_get(props, "group_finish"), mode != MODE_STOPWATCH);
	return true;
}

bool backgroundChanged(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const bool on = obs_data_get_bool(settings, "bg_on");
	obs_property_set_visible(obs_properties_get(props, "bg_color"), on);
	obs_property_set_visible(obs_properties_get(props, "bg_opacity"), on);
	return true;
}

obs_properties_t *timerProperties(void *)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_t *time = obs_properties_create();
	obs_property_t *mode = obs_properties_add_list(time, "mode", obs_module_text("Mode"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, obs_module_text("Mode.Date"), MODE_DATE);
	obs_property_list_add_int(mode, obs_module_text("Mode.Duration"), MODE_DURATION);
	obs_property_list_add_int(mode, obs_module_text("Mode.Stopwatch"), MODE_STOPWATCH);
	obs_property_set_modified_callback(mode, modeChanged);
	obs_properties_add_text(time, "target", obs_module_text("Target"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(time, "target_help", obs_module_text("Target.Help"), OBS_TEXT_INFO);
	obs_properties_add_int(time, "dur_h", obs_module_text("Hours"), 0, 999, 1);
	obs_properties_add_int(time, "dur_m", obs_module_text("Minutes"), 0, 59, 1);
	obs_properties_add_int(time, "dur_s", obs_module_text("Seconds"), 0, 59, 1);
	obs_properties_add_bool(time, "auto_start", obs_module_text("AutoStart"));
	obs_properties_add_group(props, "group_time", obs_module_text("Group.Time"), OBS_GROUP_NORMAL, time);

	obs_properties_t *text = obs_properties_create();
	obs_properties_add_text(text, "title", obs_module_text("Title"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(text, "subtitle", obs_module_text("Subtitle"), OBS_TEXT_DEFAULT);
	obs_properties_add_group(props, "group_text", obs_module_text("Group.Text"), OBS_GROUP_NORMAL, text);

	obs_properties_t *look = obs_properties_create();
	obs_property_t *style = obs_properties_add_list(look, "style", obs_module_text("Style"), OBS_COMBO_TYPE_LIST,
							OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(style, obs_module_text("Style.Blocks"), STYLE_BLOCKS);
	obs_property_list_add_int(style, obs_module_text("Style.Flip"), STYLE_FLIP);
	obs_property_list_add_int(style, obs_module_text("Style.Rings"), STYLE_RINGS);
	obs_property_list_add_int(style, obs_module_text("Style.Minimal"), STYLE_MINIMAL);
	obs_property_list_add_int(style, obs_module_text("Style.Mission"), STYLE_MISSION);
	obs_properties_add_font(look, "font", obs_module_text("Font"));
	obs_properties_add_color(look, "accent", obs_module_text("Accent"));
	obs_properties_add_color(look, "text_color", obs_module_text("TextColor"));
	obs_properties_add_color(look, "card_color", obs_module_text("CardColor"));
	obs_properties_add_int_slider(look, "card_opacity", obs_module_text("CardOpacity"), 0, 100, 1);
	obs_property_t *bg = obs_properties_add_bool(look, "bg_on", obs_module_text("Background"));
	obs_property_set_modified_callback(bg, backgroundChanged);
	obs_properties_add_color(look, "bg_color", obs_module_text("BackgroundColor"));
	obs_properties_add_int_slider(look, "bg_opacity", obs_module_text("BackgroundOpacity"), 0, 100, 1);
	obs_property_t *labels = obs_properties_add_list(look, "labels", obs_module_text("Labels"), OBS_COMBO_TYPE_LIST,
							 OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(labels, obs_module_text("Labels.Long"), LABELS_LONG);
	obs_property_list_add_int(labels, obs_module_text("Labels.Short"), LABELS_SHORT);
	obs_property_list_add_int(labels, obs_module_text("Labels.None"), LABELS_NONE);
	obs_property_t *days = obs_properties_add_list(look, "days", obs_module_text("Days"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(days, obs_module_text("Days.Auto"), DAYS_AUTO);
	obs_property_list_add_int(days, obs_module_text("Days.Always"), DAYS_ALWAYS);
	obs_property_list_add_int(days, obs_module_text("Days.Never"), DAYS_NEVER);
	obs_property_t *width = obs_properties_add_list(look, "width", obs_module_text("Quality"), OBS_COMBO_TYPE_LIST,
							OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(width, obs_module_text("Quality.Small"), 800);
	obs_property_list_add_int(width, obs_module_text("Quality.Medium"), 1280);
	obs_property_list_add_int(width, obs_module_text("Quality.Large"), 1920);
	obs_properties_add_bool(look, "entrance", obs_module_text("Entrance"));
	obs_properties_add_group(props, "group_look", obs_module_text("Group.Look"), OBS_GROUP_NORMAL, look);

	obs_properties_t *finish = obs_properties_create();
	obs_properties_add_int(finish, "urgent_secs", obs_module_text("UrgentSecs"), 0, 3600, 1);
	obs_properties_add_color(finish, "urgent_color", obs_module_text("UrgentColor"));
	obs_property_t *display = obs_properties_add_list(finish, "finish_display", obs_module_text("FinishDisplay"),
							  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(display, obs_module_text("FinishDisplay.Message"), FINISH_MESSAGE);
	obs_property_list_add_int(display, obs_module_text("FinishDisplay.Zeros"), FINISH_ZEROS);
	obs_property_list_add_int(display, obs_module_text("FinishDisplay.CountUp"), FINISH_COUNT_UP);
	obs_properties_add_text(finish, "finish_text", obs_module_text("FinishText"), OBS_TEXT_DEFAULT);
	obs_properties_add_bool(finish, "confetti", obs_module_text("Confetti"));
	obs_properties_add_int(finish, "hide_after", obs_module_text("HideAfter"), 0, 3600, 1);
	obs_property_t *scene = obs_properties_add_list(finish, "switch_scene", obs_module_text("SwitchScene"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(scene, obs_module_text("SwitchScene.None"), "");
	char **names = obs_frontend_get_scene_names();
	for (char **name = names; name && *name; ++name)
		obs_property_list_add_string(scene, *name, *name);
	bfree(names);
	obs_properties_add_group(props, "group_finish", obs_module_text("Group.Finish"), OBS_GROUP_NORMAL, finish);

	return props;
}

} // namespace

/* ------------------------------------------------------------------ */

TimerSource *timer_from_source(obs_source_t *source)
{
	if (!source || strcmp(obs_source_get_unversioned_id(source), TIMER_SOURCE_ID) != 0)
		return nullptr;
	return static_cast<TimerSource *>(obs_obj_get_data(source));
}

TimerStatus timer_status(TimerSource *timer)
{
	TimerStatus status;
	std::lock_guard<std::mutex> lock(timer->mutex);
	status.mode = timer->mode;
	status.view = viewLocked(timer, nowMs());
	return status;
}

void timer_toggle(TimerSource *timer)
{
	{
		std::lock_guard<std::mutex> lock(timer->mutex);
		const qint64 now = nowMs();
		if (timer->mode == MODE_DATE)
			return;
		if (timer->mode == MODE_DURATION) {
			if (timer->running) {
				timer->holdMs = std::max<qint64>(0, timer->anchorMs - now);
				timer->running = false;
			} else {
				/* Pressing play on a finished timer starts it over. */
				if (timer->holdMs <= 0)
					timer->holdMs = timer->durationMs;
				timer->anchorMs = now + timer->holdMs;
				timer->running = true;
			}
		} else {
			if (timer->running) {
				timer->holdMs = now - timer->anchorMs;
				timer->running = false;
			} else {
				timer->anchorMs = now - timer->holdMs;
				timer->running = true;
			}
		}
	}
	saveRuntime(timer);
}

void timer_reset(TimerSource *timer)
{
	{
		std::lock_guard<std::mutex> lock(timer->mutex);
		if (timer->mode == MODE_DATE) {
			timer->refMs = nowMs();
		} else {
			timer->running = false;
			timer->anchorMs = 0;
			timer->holdMs = timer->mode == MODE_DURATION ? timer->durationMs : 0;
		}
	}
	saveRuntime(timer);
}

void timer_add_seconds(TimerSource *timer, int seconds)
{
	const qint64 delta = qint64(seconds) * 1000;
	QString newTarget;
	{
		std::lock_guard<std::mutex> lock(timer->mutex);
		const qint64 now = nowMs();
		if (timer->mode == MODE_DATE) {
			if (timer->targetMs <= 0)
				return;
			const qint64 moved = std::max(timer->targetMs, now) + delta;
			newTarget = QDateTime::fromMSecsSinceEpoch(moved).toString(QStringLiteral(TARGET_FORMAT));
			timer->keepRef = true;
		} else if (timer->mode == MODE_DURATION) {
			if (timer->running)
				timer->anchorMs = std::max(timer->anchorMs, now) + delta;
			else
				timer->holdMs = std::max<qint64>(0, timer->holdMs + delta);
		} else {
			return;
		}
	}
	if (!newTarget.isEmpty()) {
		obs_data_t *change = obs_data_create();
		obs_data_set_string(change, "target", newTarget.toUtf8().constData());
		obs_source_update(timer->source, change);
		obs_data_release(change);
	} else {
		saveRuntime(timer);
	}
}

void timer_source_register()
{
	struct obs_source_info info = {};
	info.id = TIMER_SOURCE_ID;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.icon_type = OBS_ICON_TYPE_TEXT;
	info.get_name = timerGetName;
	info.create = timerCreate;
	info.destroy = timerDestroy;
	info.update = timerUpdate;
	info.get_defaults = timerDefaults;
	info.get_properties = timerProperties;
	info.get_width = timerWidth;
	info.get_height = timerHeight;
	info.video_render = timerRender;
	info.show = timerShow;
	info.activate = timerActivate;
	obs_register_source(&info);
}

void timer_renderer_start()
{
	renderer.stop = false;
	renderer.thread = std::thread(renderLoop);
}

void timer_renderer_stop()
{
	{
		std::lock_guard<std::mutex> lock(renderer.mutex);
		renderer.stop = true;
	}
	renderer.wake.notify_all();
	if (renderer.thread.joinable())
		renderer.thread.join();
}
