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

#include "timer-painter.hpp"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QVector>

#include <cmath>

namespace {

/* Everything is laid out on a 1000 unit wide canvas and scaled to the output width. */
const double VW = 1000.0;
const double PAD = 28.0;
const double TITLE_H = 58.0;
const double SUBTITLE_H = 38.0;
const double HEADER_GAP = 22.0;
const double PI = 3.14159265358979323846;

struct Unit {
	qint64 value = 0;
	qint64 prev = 0;
	QString label;
	double fraction = 0.0;
	int digits = 2;
};

struct Frame {
	QVector<Unit> units;
	double phase = 1.0; /* seconds since the last digit change, 1 when settled */
	bool urgent = false;
	bool overtime = false;
	bool dim = false; /* blinking zeros */
	double progress = -1.0;
	qint64 total = 0;
	QColor digit;
	QColor accent;
};

double clamp01(double v)
{
	return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

double easeOutCubic(double t)
{
	t = clamp01(t);
	const double inv = 1.0 - t;
	return 1.0 - inv * inv * inv;
}

double easeOutBack(double t)
{
	t = clamp01(t);
	const double c1 = 1.70158, c3 = c1 + 1.0;
	return 1.0 + c3 * std::pow(t - 1.0, 3) + c1 * std::pow(t - 1.0, 2);
}

QColor withAlpha(QColor color, double alpha)
{
	color.setAlphaF(clamp01(alpha));
	return color;
}

QFont makeFont(const PaintConfig &config, double pixelSize, QFont::Weight weight, double spacing = 0.0)
{
	QFont font;
	if (!config.fontFamily.isEmpty())
		font.setFamily(config.fontFamily);
	font.setPixelSize(qMax(4, int(std::lround(pixelSize))));
	font.setWeight(weight);
	font.setHintingPreference(QFont::PreferNoHinting);
	font.setStyleStrategy(QFont::PreferAntialias);
	if (spacing != 0.0)
		font.setLetterSpacing(QFont::AbsoluteSpacing, spacing);
	return font;
}

double digitCell(const QFontMetricsF &metrics)
{
	double width = 0.0;
	for (char ch = '0'; ch <= '9'; ++ch)
		width = qMax(width, metrics.horizontalAdvance(QChar::fromLatin1(ch)));
	return width;
}

/* Digits sit in fixed cells so the numbers never wobble as they change. */
double digitsWidth(const QString &text, const QFont &font)
{
	QFontMetricsF metrics(font);
	const double cell = digitCell(metrics);
	double width = 0.0;
	for (const QChar ch : text)
		width += ch.isDigit() ? cell : metrics.horizontalAdvance(ch);
	return width;
}

void drawDigits(QPainter &p, const QRectF &rect, const QString &text, const QFont &font, const QColor &color)
{
	QFontMetricsF metrics(font);
	const double cell = digitCell(metrics);
	double x = rect.center().x() - digitsWidth(text, font) / 2.0;
	const double baseline = rect.center().y() + metrics.capHeight() / 2.0;
	p.setFont(font);
	p.setPen(color);
	for (const QChar ch : text) {
		const double advance = metrics.horizontalAdvance(ch);
		const double slot = ch.isDigit() ? cell : advance;
		p.drawText(QPointF(x + (slot - advance) / 2.0, baseline), QString(ch));
		x += slot;
	}
}

void drawCentered(QPainter &p, const QRectF &rect, const QString &text, const QFont &font, const QColor &color)
{
	p.setFont(font);
	p.setPen(color);
	p.drawText(rect, Qt::AlignCenter | Qt::TextSingleLine, text);
}

/* Shrinks the font until the text fits the given width. */
QFont fitFont(const PaintConfig &config, const QString &text, double size, double maxWidth, QFont::Weight weight)
{
	QFont font = makeFont(config, size, weight);
	const double width = QFontMetricsF(font).horizontalAdvance(text);
	if (width > maxWidth && width > 0.0)
		font = makeFont(config, size * maxWidth / width, weight);
	return font;
}

QString pad(qint64 value, int digits)
{
	return QStringLiteral("%1").arg(value, digits, 10, QLatin1Char('0'));
}

double blockHeight(int style)
{
	switch (style) {
	case STYLE_FLIP:
		return 206.0;
	case STYLE_RINGS:
		return 216.0;
	case STYLE_MINIMAL:
		return 170.0;
	case STYLE_MISSION:
		return 196.0;
	default:
		return 190.0;
	}
}

double canvasHeight(const PaintConfig &config)
{
	double height = PAD * 2.0 + blockHeight(config.style);
	const bool title = !config.title.trimmed().isEmpty();
	const bool subtitle = !config.subtitle.trimmed().isEmpty();
	if (title)
		height += TITLE_H;
	if (subtitle)
		height += SUBTITLE_H;
	if (title || subtitle)
		height += HEADER_GAP;
	return height;
}

void splitUnits(const PaintConfig &config, qint64 total, double span, bool &showDays, bool &showHours, qint64 out[4])
{
	showDays = config.days == DAYS_ALWAYS || (config.days == DAYS_AUTO && total >= 86400);
	showHours = showDays || total >= 3600 || span >= 3600.0;
	out[0] = total / 86400;
	out[1] = showDays ? (total / 3600) % 24 : total / 3600;
	out[2] = showHours ? (total / 60) % 60 : total / 60;
	out[3] = total % 60;
}

Frame buildFrame(const PaintConfig &config, const TimeView &view)
{
	Frame frame;
	bool up = view.countUp;
	double secs = qMax(0.0, view.secs);

	if (view.finished) {
		if (config.finishDisplay == FINISH_COUNT_UP) {
			up = true;
			secs = view.sinceFinish;
			frame.overtime = true;
		} else {
			secs = 0.0;
			frame.dim = true;
		}
	}

	const qint64 total = up ? qint64(std::floor(secs)) : qint64(std::ceil(secs));
	const bool animate = view.running && !frame.dim && view.valid;
	const double phase = up ? secs - double(total) : double(total) - secs;
	frame.phase = animate ? phase : 1.0;
	frame.total = total;

	/* The value shown one tick ago, used by the flip animation. */
	const qint64 before = up ? qMax<qint64>(0, total - 1) : total + 1;

	bool showDays = false, showHours = false, d2 = false, h2 = false;
	qint64 now[4], old[4];
	splitUnits(config, total, view.span, showDays, showHours, now);
	splitUnits(config, before, view.span, d2, h2, old);
	if (d2 != showDays || h2 != showHours)
		splitUnits(config, total, view.span, d2, h2, old);

	const int labelIndex = config.labels == LABELS_SHORT ? 1 : 0;
	const double smooth = animate ? secs : double(total);
	const double fractions[4] = {
		view.span > 86400.0 ? clamp01(smooth / view.span) : clamp01(double(now[0]) / 30.0),
		std::fmod(smooth, 86400.0) / 86400.0,
		std::fmod(smooth, 3600.0) / 3600.0,
		std::fmod(smooth, 60.0) / 60.0,
	};
	const QString labels[4] = {config.labelDays[labelIndex], config.labelHours[labelIndex],
				   config.labelMinutes[labelIndex], config.labelSeconds[labelIndex]};

	for (int i = 0; i < 4; ++i) {
		if ((i == 0 && !showDays) || (i == 1 && !showHours))
			continue;
		Unit unit;
		unit.value = now[i];
		unit.prev = old[i];
		unit.label = config.labels == LABELS_NONE ? QString() : labels[i];
		unit.fraction = fractions[i];
		unit.digits = qMax(2, int(QString::number(now[i]).size()));
		frame.units.append(unit);
	}

	frame.urgent = !view.countUp && !view.finished && view.valid && config.urgentSecs > 0 &&
		       secs <= double(config.urgentSecs) && view.running;
	const bool hot = frame.urgent || frame.overtime;
	frame.digit = hot ? config.urgent : config.text;
	frame.accent = hot ? config.urgent : config.accent;

	if (view.finished && config.finishDisplay != FINISH_COUNT_UP)
		frame.progress = 1.0;
	else if (!up && view.span > 0.0)
		frame.progress = clamp01(1.0 - secs / view.span);
	else if (frame.overtime)
		frame.progress = 1.0;
	return frame;
}

/* ------------------------------------------------------------------ */

void drawBlocks(QPainter &p, const PaintConfig &config, const Frame &frame, const QRectF &area)
{
	const int count = int(frame.units.size());
	const double gap = 20.0;
	const double width = qMin(214.0, (area.width() - gap * (count - 1)) / count);
	const double total = width * count + gap * (count - 1);
	double x = area.center().x() - total / 2.0;
	const bool labels = config.labels != LABELS_NONE;

	for (const Unit &unit : frame.units) {
		const QRectF card(x, area.top(), width, area.height());
		p.setPen(Qt::NoPen);
		p.setBrush(withAlpha(config.card, config.cardOpacity));
		p.drawRoundedRect(card, 22, 22);

		const QString text = pad(unit.value, unit.digits);
		QFont font = makeFont(config, 108, QFont::Black);
		const double fit = digitsWidth(text, font);
		if (fit > width - 34.0)
			font = makeFont(config, 108.0 * (width - 34.0) / fit, QFont::Black);
		const QRectF digits(card.left(), card.top() + 14, card.width(), card.height() - (labels ? 70 : 34));
		drawDigits(p, digits, text, font, frame.digit);

		if (labels) {
			const QRectF labelRect(card.left(), card.bottom() - 60, card.width(), 28);
			drawCentered(p, labelRect, unit.label, makeFont(config, 19, QFont::Bold, 3.0),
				     withAlpha(config.text, 0.62));
		}

		/* A thin bar that drains as the unit runs out. */
		const QRectF track(card.left() + 22, card.bottom() - 20, card.width() - 44, 5);
		p.setPen(Qt::NoPen);
		p.setBrush(withAlpha(config.text, 0.12));
		p.drawRoundedRect(track, 2.5, 2.5);
		p.setBrush(frame.accent);
		p.drawRoundedRect(QRectF(track.left(), track.top(), track.width() * clamp01(unit.fraction), 5), 2.5,
				  2.5);
		x += width + gap;
	}
}

void drawFlipHalf(QPainter &p, const PaintConfig &config, const Frame &frame, const QRectF &card, const QString &text,
		  const QFont &font, bool top)
{
	p.save();
	const QRectF half = top ? QRectF(card.left(), card.top(), card.width(), card.height() / 2.0)
				: QRectF(card.left(), card.center().y(), card.width(), card.height() / 2.0);
	p.setClipRect(half);
	p.setPen(Qt::NoPen);
	const QColor base = withAlpha(top ? config.card.lighter(125) : config.card, config.cardOpacity);
	p.setBrush(base);
	p.drawRoundedRect(card, 16, 16);
	drawDigits(p, card, text, font, frame.digit);
	p.restore();
}

void drawFlip(QPainter &p, const PaintConfig &config, const Frame &frame, const QRectF &area)
{
	const bool labels = config.labels != LABELS_NONE;
	const double cardHeight = labels ? 160.0 : area.height();
	const double gap = 22.0;
	const QFont font = makeFont(config, 112, QFont::Black);
	const double cell = digitCell(QFontMetricsF(font));

	QVector<double> widths;
	double total = 0.0;
	for (const Unit &unit : frame.units) {
		widths.append(unit.digits * cell + 44.0);
		total += widths.last();
	}
	total += gap * double(frame.units.size() - 1);
	const double fitScale = total > area.width() ? area.width() / total : 1.0;

	p.save();
	p.translate(area.center().x(), area.top());
	p.scale(fitScale, fitScale);
	double x = -total / 2.0;
	const double flipTime = 0.55;

	const int unitCount = int(frame.units.size());
	for (int i = 0; i < unitCount; ++i) {
		const Unit &unit = frame.units[i];
		const QRectF card(x, 0, widths[i], cardHeight);
		const QString now = pad(unit.value, unit.digits);
		const QString old = pad(unit.prev, unit.digits);
		const bool flipping = unit.value != unit.prev && frame.phase < flipTime;

		if (!flipping) {
			drawFlipHalf(p, config, frame, card, now, font, true);
			drawFlipHalf(p, config, frame, card, now, font, false);
		} else {
			const double t = frame.phase / flipTime;
			drawFlipHalf(p, config, frame, card, now, font, true);
			drawFlipHalf(p, config, frame, card, old, font, false);
			const bool firstHalf = t < 0.5;
			const double fold = firstHalf ? 1.0 - t * 2.0 : (t - 0.5) * 2.0;
			p.save();
			p.translate(0, card.center().y());
			p.scale(1.0, qMax(0.02, fold));
			p.translate(0, -card.center().y());
			drawFlipHalf(p, config, frame, card, firstHalf ? old : now, font, firstHalf);
			const QRectF shade =
				firstHalf ? QRectF(card.left(), card.top(), card.width(), card.height() / 2)
					  : QRectF(card.left(), card.center().y(), card.width(), card.height() / 2);
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(0, 0, 0, int(110 * (1.0 - fold))));
			p.drawRect(shade);
			p.restore();
		}

		/* Hinge. */
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0, 0, 0, 150));
		p.drawRect(QRectF(card.left(), card.center().y() - 1.5, card.width(), 3));

		if (labels) {
			const QRectF labelRect(card.left() - 20, cardHeight + 12, card.width() + 40, 30);
			drawCentered(p, labelRect, unit.label, makeFont(config, 20, QFont::Bold, 3.0), frame.accent);
		}
		x += widths[i] + gap;
	}
	p.restore();
}

void drawRings(QPainter &p, const PaintConfig &config, const Frame &frame, const QRectF &area)
{
	const int count = int(frame.units.size());
	const double gap = 28.0;
	const double diameter = qMin(area.height(), (area.width() - gap * (count - 1)) / count);
	const double total = diameter * count + gap * (count - 1);
	double x = area.center().x() - total / 2.0;
	const double stroke = diameter * 0.065;
	const bool labels = config.labels != LABELS_NONE;

	for (const Unit &unit : frame.units) {
		const QRectF outer(x, area.center().y() - diameter / 2.0, diameter, diameter);
		const QRectF ring = outer.adjusted(stroke, stroke, -stroke, -stroke);

		p.setPen(Qt::NoPen);
		p.setBrush(withAlpha(config.card, config.cardOpacity));
		p.drawEllipse(outer);

		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(withAlpha(config.text, 0.12), stroke, Qt::SolidLine, Qt::FlatCap));
		p.drawEllipse(ring);

		const int spanAngle = -int(std::lround(clamp01(unit.fraction) * 360.0 * 16.0));
		if (spanAngle != 0) {
			p.setPen(QPen(withAlpha(frame.accent, 0.22), stroke * 2.1, Qt::SolidLine, Qt::RoundCap));
			p.drawArc(ring, 90 * 16, spanAngle);
			p.setPen(QPen(frame.accent, stroke, Qt::SolidLine, Qt::RoundCap));
			p.drawArc(ring, 90 * 16, spanAngle);
		}

		const QString text = pad(unit.value, unit.digits);
		const double size = diameter * (unit.digits > 2 ? 0.27 : 0.37);
		const QRectF digits(outer.left(), outer.top() - (labels ? diameter * 0.05 : 0), diameter, diameter);
		drawDigits(p, digits, text, makeFont(config, size, QFont::Black), frame.digit);
		if (labels) {
			const QRectF labelRect(outer.left(), outer.center().y() + diameter * 0.17, diameter,
					       diameter * 0.14);
			drawCentered(p, labelRect, unit.label, makeFont(config, diameter * 0.082, QFont::Bold, 2.0),
				     withAlpha(config.text, 0.62));
		}
		x += diameter + gap;
	}
}

void drawMinimal(QPainter &p, const PaintConfig &config, const Frame &frame, const QRectF &area)
{
	const bool labels = config.labels != LABELS_NONE;
	const QFont font = makeFont(config, 132, QFont::Black);
	const QFont colonFont = makeFont(config, 104, QFont::Bold);
	const double cell = digitCell(QFontMetricsF(font));
	const double colon = 56.0;

	double total = colon * double(frame.units.size() - 1);
	for (const Unit &unit : frame.units)
		total += unit.digits * cell;
	const double fitScale = total > area.width() ? area.width() / total : 1.0;

	p.save();
	p.translate(area.center().x(), area.top());
	p.scale(fitScale, fitScale);
	double x = -total / 2.0;
	const double digitsHeight = labels ? area.height() - 36.0 : area.height();
	const double blink = frame.phase >= 1.0 ? 1.0 : 0.3 + 0.7 * (0.5 + 0.5 * std::cos(2.0 * PI * frame.phase));

	const int unitCount = int(frame.units.size());
	for (int i = 0; i < unitCount; ++i) {
		const Unit &unit = frame.units[i];
		const double width = unit.digits * cell;
		drawDigits(p, QRectF(x, 0, width, digitsHeight), pad(unit.value, unit.digits), font, frame.digit);
		if (labels)
			drawCentered(p, QRectF(x - 30, digitsHeight + 4, width + 60, 30), unit.label,
				     makeFont(config, 20, QFont::Bold, 4.0), frame.accent);
		x += width;
		if (i + 1 < unitCount) {
			drawCentered(p, QRectF(x, -8, colon, digitsHeight), QStringLiteral(":"), colonFont,
				     withAlpha(frame.accent, blink));
			x += colon;
		}
	}
	p.restore();
}

void drawMission(QPainter &p, const PaintConfig &config, const Frame &frame, const TimeView &view, const QRectF &area)
{
	/* Mission control read-out: T-minus clock over a launch track. */
	const bool plus = view.countUp || frame.overtime;
	const QString prefix = plus ? QStringLiteral("T+") : QStringLiteral("T−");

	QString clock;
	int first = 0;
	const bool hasDays = frame.units.size() == 4;
	QString days;
	if (hasDays) {
		days = QString::number(frame.units[0].value) + frame.units[0].label.left(1).toUpper();
		if (config.labels == LABELS_NONE)
			days = QString::number(frame.units[0].value) + QStringLiteral("D");
		first = 1;
	}
	for (int i = first; i < int(frame.units.size()); ++i) {
		if (i > first)
			clock += QLatin1Char(':');
		clock += pad(frame.units[i].value, frame.units[i].digits);
	}

	const QFont clockFont = makeFont(config, 108, QFont::Black);
	const QFont prefixFont = makeFont(config, 84, QFont::Black);
	const QFont daysFont = makeFont(config, 64, QFont::Bold);
	const double prefixWidth = QFontMetricsF(prefixFont).horizontalAdvance(prefix);
	const double daysWidth = hasDays ? QFontMetricsF(daysFont).horizontalAdvance(days) + 26.0 : 0.0;
	const double clockWidth = digitsWidth(clock, clockFont);
	const double total = prefixWidth + 26.0 + daysWidth + clockWidth;
	const double fitScale = total > area.width() ? area.width() / total : 1.0;
	const double rowHeight = 120.0;

	p.save();
	p.translate(area.center().x(), area.top());
	p.scale(fitScale, fitScale);
	double x = -total / 2.0;
	const double baseline = rowHeight / 2.0 + QFontMetricsF(clockFont).capHeight() / 2.0;
	p.setFont(prefixFont);
	p.setPen(frame.accent);
	p.drawText(QPointF(x, baseline), prefix);
	x += prefixWidth + 26.0;
	if (hasDays) {
		p.setFont(daysFont);
		p.setPen(withAlpha(config.text, 0.7));
		p.drawText(QPointF(x, baseline), days);
		x += daysWidth;
	}
	drawDigits(p, QRectF(x, 0, clockWidth, rowHeight), clock, clockFont, frame.digit);
	p.restore();

	/* Launch track. */
	const double trackY = area.top() + 162.0;
	const QRectF track(area.left() + 10, trackY - 5, area.width() - 20, 10);
	p.setPen(Qt::NoPen);
	p.setBrush(withAlpha(config.card, qMax(0.55, config.cardOpacity)));
	p.drawRoundedRect(track.adjusted(-8, -8, 8, 8), 13, 13);
	p.setBrush(withAlpha(config.text, 0.14));
	p.drawRoundedRect(track, 5, 5);

	p.setPen(QPen(withAlpha(config.text, 0.28), 2));
	for (int i = 0; i <= 10; ++i) {
		const double tx = track.left() + track.width() * i / 10.0;
		const double length = i % 5 == 0 ? 12.0 : 7.0;
		p.drawLine(QPointF(tx, track.top() - 14 - length), QPointF(tx, track.top() - 14));
	}

	p.setPen(Qt::NoPen);
	if (frame.progress >= 0.0) {
		const double px = track.left() + track.width() * frame.progress;
		p.setBrush(frame.accent);
		p.drawRoundedRect(QRectF(track.left(), track.top(), px - track.left(), track.height()), 5, 5);
		p.setBrush(withAlpha(frame.accent, 0.28));
		p.drawEllipse(QPointF(px, trackY), 17, 17);
		p.setBrush(config.text);
		p.drawEllipse(QPointF(px, trackY), 8.5, 8.5);

		const QString percent = QString::number(int(std::floor(frame.progress * 100.0))) + QLatin1Char('%');
		const double labelX = qBound(track.left() + 40.0, px, track.right() - 40.0);
		drawCentered(p, QRectF(labelX - 60, track.bottom() + 8, 120, 24), percent,
			     makeFont(config, 18, QFont::Bold, 1.5), withAlpha(config.text, 0.7));
	} else {
		/* No known length (stopwatch): a light sweeps along the track. */
		const double sweep = std::fmod(qMax(0.0, view.secs), 3.0) / 3.0;
		const double px = track.left() + (track.width() - 120.0) * sweep;
		QLinearGradient gradient(px, 0, px + 120.0, 0);
		gradient.setColorAt(0.0, withAlpha(frame.accent, 0.0));
		gradient.setColorAt(0.8, frame.accent);
		gradient.setColorAt(1.0, withAlpha(frame.accent, 0.0));
		p.setBrush(gradient);
		p.drawRoundedRect(QRectF(px, track.top(), 120.0, track.height()), 5, 5);
	}
}

/* ------------------------------------------------------------------ */

double hash01(unsigned int seed)
{
	seed ^= seed >> 16;
	seed *= 0x7feb352dU;
	seed ^= seed >> 15;
	seed *= 0x846ca68bU;
	seed ^= seed >> 16;
	return double(seed & 0xffffff) / double(0x1000000);
}

void drawConfetti(QPainter &p, const PaintConfig &config, double since, double height)
{
	const double life = 6.5;
	if (since < 0.0 || since > life)
		return;
	const QColor palette[5] = {config.accent, config.text, config.accent.lighter(150), config.urgent,
				   QColor::fromHsv((qMax(0, config.accent.hsvHue()) + 150) % 360, 200, 255)};
	p.setPen(Qt::NoPen);
	for (unsigned int i = 0; i < 120; ++i) {
		const double delay = hash01(i * 7 + 1) * 2.2;
		const double t = since - delay;
		if (t < 0.0)
			continue;
		const double speed = 90.0 + hash01(i * 7 + 2) * 190.0;
		const double y = -24.0 + speed * t;
		if (y > height + 24.0)
			continue;
		const double x = hash01(i * 7 + 3) * VW + std::sin(t * 3.2 + hash01(i * 7 + 4) * 6.28) * 26.0;
		const double w = 9.0 + hash01(i * 7 + 5) * 9.0;
		const double alpha = clamp01((life - since) / 1.2);
		p.save();
		p.translate(x, y);
		p.rotate(hash01(i * 7 + 6) * 360.0 + t * (hash01(i * 7 + 7) - 0.5) * 700.0);
		p.setBrush(withAlpha(palette[i % 5], alpha));
		if (i % 3 == 0)
			p.drawEllipse(QRectF(-w / 2.0, -w / 2.0, w, w));
		else
			p.drawRoundedRect(QRectF(-w / 2.0, -w * 0.3, w, w * 0.6), 1.5, 1.5);
		p.restore();
	}
}

void drawMessage(QPainter &p, const PaintConfig &config, const TimeView &view, const QRectF &area)
{
	const QString text = config.finishText.trimmed();
	if (text.isEmpty())
		return;
	const double pop = easeOutBack(view.sinceFinish / 0.65);
	const double breathe = 1.0 + 0.012 * std::sin(view.sinceFinish * 2.4);
	p.save();
	p.translate(area.center());
	p.scale(pop * breathe, pop * breathe);
	const QFont font = fitFont(config, text, 118, area.width() - 40.0, QFont::Black);
	const QRectF rect(-area.width() / 2.0, -area.height() / 2.0, area.width(), area.height());
	p.setOpacity(p.opacity() * clamp01(view.sinceFinish / 0.2));
	drawCentered(p, rect.translated(0, 5), text, font, withAlpha(config.accent.darker(260), 0.55));
	drawCentered(p, rect, text, font, config.accent);
	p.restore();
}

} // namespace

QSize timerCanvasSize(const PaintConfig &config)
{
	const int width = qBound(320, config.width, 3840);
	const int height = int(std::ceil(canvasHeight(config) * width / VW));
	return QSize(width, height + (height % 2));
}

QImage paintTimer(const PaintConfig &config, const TimeView &view, double shownFor)
{
	const QSize size = timerCanvasSize(config);
	QImage image(size, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);

	double opacity = 1.0;
	double offset = 0.0;
	if (config.entrance && shownFor < 0.6) {
		const double e = easeOutCubic(shownFor / 0.6);
		opacity = e;
		offset = (1.0 - e) * 34.0;
	}
	if (view.finished && config.hideAfter > 0 && config.finishDisplay != FINISH_COUNT_UP)
		opacity *= 1.0 - clamp01((view.sinceFinish - double(config.hideAfter)) / 0.8);
	if (opacity <= 0.0)
		return image;

	QPainter p(&image);
	p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
	const double scale = size.width() / VW;
	p.scale(scale, scale);
	p.setOpacity(opacity);
	p.translate(0, offset);

	const double height = canvasHeight(config);
	if (config.backgroundOn) {
		p.setPen(Qt::NoPen);
		p.setBrush(withAlpha(config.background, config.backgroundOpacity));
		p.drawRoundedRect(QRectF(0, 0, VW, height), 30, 30);
	}

	double y = PAD;
	const QString title = config.title.trimmed();
	const QString subtitle = config.subtitle.trimmed();
	if (!title.isEmpty()) {
		const QFont font = fitFont(config, title, 42, VW - PAD * 4, QFont::ExtraBold);
		drawCentered(p, QRectF(PAD, y, VW - PAD * 2, TITLE_H), title, font, config.text);
		y += TITLE_H;
	}
	if (!subtitle.isEmpty()) {
		const QFont font = fitFont(config, subtitle, 25, VW - PAD * 4, QFont::DemiBold);
		drawCentered(p, QRectF(PAD, y, VW - PAD * 2, SUBTITLE_H), subtitle, font, withAlpha(config.text, 0.7));
		y += SUBTITLE_H;
	}
	if (!title.isEmpty() || !subtitle.isEmpty()) {
		p.setPen(Qt::NoPen);
		p.setBrush(config.accent);
		p.drawRoundedRect(QRectF(VW / 2.0 - 32, y + 5, 64, 5), 2.5, 2.5);
		y += HEADER_GAP;
	}

	const QRectF area(PAD, y, VW - PAD * 2, blockHeight(config.style));
	const bool message = view.finished && config.finishDisplay == FINISH_MESSAGE &&
			     !config.finishText.trimmed().isEmpty();

	if (message) {
		drawMessage(p, config, view, area);
	} else {
		const Frame frame = buildFrame(config, view);
		p.save();
		if (frame.urgent) {
			const double pulse = 1.0 + 0.06 * std::exp(-frame.phase * 7.0);
			p.translate(area.center());
			p.scale(pulse, pulse);
			p.translate(-area.center());
		}
		if (frame.dim)
			p.setOpacity(p.opacity() * (0.45 + 0.55 * (0.5 + 0.5 * std::cos(view.sinceFinish * PI * 2.0))));
		if (!view.valid)
			p.setOpacity(p.opacity() * 0.45);

		switch (config.style) {
		case STYLE_FLIP:
			drawFlip(p, config, frame, area);
			break;
		case STYLE_RINGS:
			drawRings(p, config, frame, area);
			break;
		case STYLE_MINIMAL:
			drawMinimal(p, config, frame, area);
			break;
		case STYLE_MISSION:
			drawMission(p, config, frame, view, area);
			break;
		default:
			drawBlocks(p, config, frame, area);
			break;
		}
		p.restore();
	}

	if (view.finished && config.confetti) {
		p.setClipRect(QRectF(0, -offset, VW, height));
		drawConfetti(p, config, view.sinceFinish, height);
	}
	p.end();
	return image;
}

QString formatTimerText(const TimeView &view)
{
	if (!view.valid)
		return QStringLiteral("--:--");
	double secs = view.finished ? view.sinceFinish : qMax(0.0, view.secs);
	const bool up = view.countUp || view.finished;
	const qint64 total = up ? qint64(std::floor(secs)) : qint64(std::ceil(secs));
	const qint64 days = total / 86400;
	QString text;
	if (days > 0)
		text = QString::number(days) + QStringLiteral("d ");
	if (days > 0 || total >= 3600)
		text += pad((total / 3600) % 24, 2) + QLatin1Char(':');
	text += pad((total / 60) % 60, 2) + QLatin1Char(':') + pad(total % 60, 2);
	return view.finished ? QStringLiteral("+") + text : text;
}
