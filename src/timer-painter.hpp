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

#include <QColor>
#include <QImage>
#include <QSize>
#include <QString>

/* Pure Qt drawing code: no libobs here, so it can be rendered and tested outside OBS. */

enum TimerStyle { STYLE_BLOCKS = 0, STYLE_FLIP, STYLE_RINGS, STYLE_MINIMAL, STYLE_MISSION, STYLE_COUNT };
enum FinishDisplay { FINISH_MESSAGE = 0, FINISH_ZEROS, FINISH_COUNT_UP };
enum DaysMode { DAYS_AUTO = 0, DAYS_ALWAYS, DAYS_NEVER };
enum LabelMode { LABELS_LONG = 0, LABELS_SHORT, LABELS_NONE };

struct PaintConfig {
	int style = STYLE_BLOCKS;
	int width = 1280; /* output width in pixels, height follows the layout */
	QString fontFamily;
	QString title;
	QString subtitle;
	QString finishText;
	QColor accent = QColor(0x7c, 0x5c, 0xff);
	QColor text = QColor(0xff, 0xff, 0xff);
	QColor card = QColor(0x16, 0x18, 0x22);
	QColor background = QColor(0x0b, 0x0c, 0x12);
	QColor urgent = QColor(0xff, 0x4d, 0x5e);
	double cardOpacity = 0.92;
	bool backgroundOn = false;
	double backgroundOpacity = 0.75;
	int labels = LABELS_LONG;
	int days = DAYS_AUTO;
	QString labelDays[2], labelHours[2], labelMinutes[2], labelSeconds[2]; /* [long, short] */
	bool entrance = true;
	int urgentSecs = 10;
	int finishDisplay = FINISH_MESSAGE;
	bool confetti = true;
	int hideAfter = 0; /* seconds after the end, 0 keeps it on screen */
};

struct TimeView {
	bool valid = false;   /* false: no target set yet */
	bool countUp = false; /* stopwatch */
	bool running = false;
	bool finished = false; /* a countdown reached zero */
	double secs = 0.0;     /* remaining (countdown) or elapsed (stopwatch) */
	double sinceFinish = 0.0;
	double span = 0.0; /* total length of the countdown, 0 if unknown */
};

QSize timerCanvasSize(const PaintConfig &config);
QImage paintTimer(const PaintConfig &config, const TimeView &view, double shownFor);

/* "1d 02:03:04" style text for the dock. */
QString formatTimerText(const TimeView &view);
