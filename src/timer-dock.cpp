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

#include <QComboBox>
#include <QDateTimeEdit>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

enum Preset { PRESET_STARTING = 0, PRESET_LAUNCH, PRESET_BREAK, PRESET_STOPWATCH };

QString tr_(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

void setClass(QWidget *widget, const char *name)
{
	widget->setProperty("class", name);
}

QPushButton *chipButton(const QString &text, QWidget *parent)
{
	auto *button = new QPushButton(text, parent);
	button->setCursor(Qt::PointingHandCursor);
	button->setStyleSheet(
		QStringLiteral("QPushButton { padding: 2px 8px; border-radius: 9px; min-height: 18px; }"));
	button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
	button->setMinimumWidth(34);
	return button;
}

void setChip(QLabel *chip, const QString &text, const char *rgb)
{
	if (chip->text() == text)
		return;
	chip->setText(text);
	chip->setStyleSheet(QStringLiteral("QLabel { color: rgb(%1); background: rgba(%1, 0.16); border-radius: 8px;"
					   " padding: 1px 8px; font-weight: bold; }")
				    .arg(QLatin1String(rgb)));
}

/* OBS themes size fonts in pixels or points depending on the theme. */
void scaleFont(QFont &font, double factor)
{
	if (font.pixelSize() > 0)
		font.setPixelSize(int(font.pixelSize() * factor));
	else if (font.pointSizeF() > 0)
		font.setPointSizeF(font.pointSizeF() * factor);
}

void compact(QComboBox *combo)
{
	combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	combo->setMinimumContentsLength(6);
}

bool collectTimers(void *param, obs_source_t *source)
{
	if (strcmp(obs_source_get_unversioned_id(source), TIMER_SOURCE_ID) == 0)
		static_cast<QStringList *>(param)->append(QString::fromUtf8(obs_source_get_uuid(source)));
	return true;
}

QDateTime nextTime(int hour, int addDays)
{
	QDateTime when(QDate::currentDate().addDays(addDays), QTime(hour, 0));
	if (when <= QDateTime::currentDateTime())
		when = when.addDays(1);
	return when;
}

} // namespace

/* ------------------------------------------------------------------ */

TimerCard::TimerCard(obs_source_t *source, QWidget *parent) : QFrame(parent)
{
	weak_ = obs_source_get_weak_source(source);
	uuid_ = QString::fromUtf8(obs_source_get_uuid(source));

	setObjectName(QStringLiteral("acetCard"));
	setStyleSheet(QStringLiteral("QFrame#acetCard { background: rgba(128, 128, 128, 0.14); border-radius: 8px; }"));

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(10, 8, 10, 8);
	layout->setSpacing(6);

	auto *top = new QHBoxLayout();
	top->setSpacing(6);
	name_ = new QLabel(this);
	QFont bold = name_->font();
	bold.setBold(true);
	name_->setFont(bold);
	name_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	top->addWidget(name_, 1);
	live_ = new QLabel(tr_("Dock.Live"), this);
	live_->setStyleSheet(QStringLiteral("QLabel { color: rgb(255, 77, 94); font-weight: bold; }"));
	live_->setVisible(false);
	top->addWidget(live_);
	chip_ = new QLabel(this);
	top->addWidget(chip_);
	layout->addLayout(top);

	time_ = new QLabel(QStringLiteral("--:--"), this);
	/* The OBS theme sets font sizes through its stylesheet, so the size goes there too. */
	time_->setStyleSheet(QStringLiteral("QLabel { font-size: 26px; font-weight: 700; }"));
	layout->addWidget(time_);

	auto *buttons = new QHBoxLayout();
	buttons->setSpacing(4);
	play_ = new QPushButton(this);
	play_->setCursor(Qt::PointingHandCursor);
	buttons->addWidget(play_, 2);
	reset_ = new QPushButton(QStringLiteral("↺"), this);
	reset_->setToolTip(tr_("Dock.Reset"));
	reset_->setAccessibleName(tr_("Dock.Reset"));
	buttons->addWidget(reset_, 1);
	minus_ = new QPushButton(QStringLiteral("−1 min"), this);
	buttons->addWidget(minus_, 1);
	plus_ = new QPushButton(QStringLiteral("+1 min"), this);
	buttons->addWidget(plus_, 1);
	layout->addLayout(buttons);

	auto *bottom = new QHBoxLayout();
	edit_ = new QToolButton(this);
	edit_->setText(tr_("Dock.Edit"));
	edit_->setCheckable(true);
	edit_->setArrowType(Qt::RightArrow);
	edit_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	edit_->setAutoRaise(true);
	edit_->setCursor(Qt::PointingHandCursor);
	bottom->addWidget(edit_);
	bottom->addStretch(1);
	auto *more = new QToolButton(this);
	more->setText(tr_("Dock.AllOptions"));
	more->setAutoRaise(true);
	more->setCursor(Qt::PointingHandCursor);
	bottom->addWidget(more);
	layout->addLayout(bottom);

	/* Quick editor. */
	editor_ = new QWidget(this);
	auto *form = new QFormLayout(editor_);
	form->setContentsMargins(0, 0, 0, 2);
	form->setSpacing(6);
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	form->setRowWrapPolicy(QFormLayout::WrapAllRows);

	mode_ = new QComboBox(editor_);
	mode_->addItem(tr_("Mode.Date"), MODE_DATE);
	mode_->addItem(tr_("Mode.Duration"), MODE_DURATION);
	mode_->addItem(tr_("Mode.Stopwatch"), MODE_STOPWATCH);
	compact(mode_);
	form->addRow(tr_("Mode"), mode_);

	dateRow_ = new QWidget(editor_);
	auto *dateLayout = new QVBoxLayout(dateRow_);
	dateLayout->setContentsMargins(0, 0, 0, 0);
	dateLayout->setSpacing(4);
	date_ = new QDateTimeEdit(dateRow_);
	date_->setCalendarPopup(true);
	date_->setDisplayFormat(QStringLiteral("dd MMM yyyy  HH:mm"));
	dateLayout->addWidget(date_);
	auto *dateChips = new QGridLayout();
	dateChips->setSpacing(4);
	int chipIndex = 0;
	struct DateChip {
		const char *key;
		int kind;
	};
	const DateChip dateChipList[] = {{"Chip.In15", 0}, {"Chip.In1h", 1}, {"Chip.Tonight", 2}, {"Chip.Tomorrow", 3}};
	for (const DateChip &chip : dateChipList) {
		auto *button = chipButton(tr_(chip.key), dateRow_);
		const int kind = chip.kind;
		connect(button, &QPushButton::clicked, this, [this, kind]() {
			const QDateTime now = QDateTime::currentDateTime();
			QDateTime when;
			if (kind == 0)
				when = now.addSecs(15 * 60);
			else if (kind == 1)
				when = now.addSecs(3600);
			else if (kind == 2)
				when = nextTime(20, 0);
			else
				when = QDateTime(QDate::currentDate().addDays(1), QTime(20, 0));
			applyTarget(when);
			loadEditor();
		});
		dateChips->addWidget(button, chipIndex / 2, chipIndex % 2);
		++chipIndex;
	}
	dateLayout->addLayout(dateChips);
	form->addRow(tr_("Dock.When"), dateRow_);

	durationRow_ = new QWidget(editor_);
	auto *durationLayout = new QVBoxLayout(durationRow_);
	durationLayout->setContentsMargins(0, 0, 0, 0);
	durationLayout->setSpacing(4);
	auto *spins = new QHBoxLayout();
	spins->setSpacing(4);
	hours_ = new QSpinBox(durationRow_);
	hours_->setRange(0, 999);
	hours_->setSuffix(QStringLiteral(" h"));
	minutes_ = new QSpinBox(durationRow_);
	minutes_->setRange(0, 59);
	minutes_->setSuffix(QStringLiteral(" min"));
	seconds_ = new QSpinBox(durationRow_);
	seconds_->setRange(0, 59);
	seconds_->setSuffix(QStringLiteral(" s"));
	spins->addWidget(hours_);
	spins->addWidget(minutes_);
	spins->addWidget(seconds_);
	durationLayout->addLayout(spins);
	auto *durationChips = new QHBoxLayout();
	durationChips->setSpacing(4);
	for (int mins : {1, 5, 10, 15, 30}) {
		auto *button = chipButton(QStringLiteral("%1 min").arg(mins), durationRow_);
		connect(button, &QPushButton::clicked, this, [this, mins]() {
			applyDuration(mins * 60);
			loadEditor();
		});
		durationChips->addWidget(button);
	}
	durationLayout->addLayout(durationChips);
	form->addRow(tr_("Dock.Length"), durationRow_);

	title_ = new QLineEdit(editor_);
	title_->setPlaceholderText(tr_("Dock.TitleHint"));
	form->addRow(tr_("Title"), title_);

	style_ = new QComboBox(editor_);
	style_->addItem(tr_("Style.Blocks"), STYLE_BLOCKS);
	style_->addItem(tr_("Style.Flip"), STYLE_FLIP);
	style_->addItem(tr_("Style.Rings"), STYLE_RINGS);
	style_->addItem(tr_("Style.Minimal"), STYLE_MINIMAL);
	style_->addItem(tr_("Style.Mission"), STYLE_MISSION);
	compact(style_);
	form->addRow(tr_("Style"), style_);

	finishRow_ = new QWidget(editor_);
	auto *finishLayout = new QVBoxLayout(finishRow_);
	finishLayout->setContentsMargins(0, 0, 0, 0);
	finishLayout->setSpacing(4);
	finishText_ = new QLineEdit(finishRow_);
	finishText_->setPlaceholderText(tr_("FinishText"));
	finishLayout->addWidget(finishText_);
	scene_ = new QComboBox(finishRow_);
	compact(scene_);
	finishLayout->addWidget(scene_);
	form->addRow(tr_("Dock.AtZero"), finishRow_);

	editor_->setVisible(false);
	layout->addWidget(editor_);

	/* Wiring. */
	connect(play_, &QPushButton::clicked, this, [this]() {
		obs_source_t *src = acquire();
		if (TimerSource *timer = timer_from_source(src))
			timer_toggle(timer);
		obs_source_release(src);
		refresh();
	});
	connect(reset_, &QPushButton::clicked, this, [this]() {
		obs_source_t *src = acquire();
		if (TimerSource *timer = timer_from_source(src))
			timer_reset(timer);
		obs_source_release(src);
		refresh();
	});
	auto nudge = [this](int seconds) {
		obs_source_t *src = acquire();
		if (TimerSource *timer = timer_from_source(src))
			timer_add_seconds(timer, seconds);
		obs_source_release(src);
		refresh();
		if (expanded())
			loadEditor();
	};
	connect(minus_, &QPushButton::clicked, this, [nudge]() { nudge(-60); });
	connect(plus_, &QPushButton::clicked, this, [nudge]() { nudge(60); });
	connect(edit_, &QToolButton::toggled, this, [this](bool on) { setExpanded(on); });
	connect(more, &QToolButton::clicked, this, [this]() {
		obs_source_t *src = acquire();
		if (src)
			obs_frontend_open_source_properties(src);
		obs_source_release(src);
	});

	connect(mode_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
		if (loading_)
			return;
		const int mode = mode_->currentData().toInt();
		if (mode == MODE_DATE && !date_->property("set").toBool())
			applyTarget(QDateTime::currentDateTime().addSecs(3600));
		apply("mode", mode);
		loadEditor();
	});
	connect(date_, &QDateTimeEdit::dateTimeChanged, this, [this](const QDateTime &when) {
		if (!loading_)
			applyTarget(when);
	});
	auto durationEdited = [this](int) {
		if (!loading_)
			applyDuration(hours_->value() * 3600 + minutes_->value() * 60 + seconds_->value());
	};
	connect(hours_, QOverload<int>::of(&QSpinBox::valueChanged), this, durationEdited);
	connect(minutes_, QOverload<int>::of(&QSpinBox::valueChanged), this, durationEdited);
	connect(seconds_, QOverload<int>::of(&QSpinBox::valueChanged), this, durationEdited);
	connect(title_, &QLineEdit::textEdited, this, [this](const QString &text) { apply("title", text); });
	connect(finishText_, &QLineEdit::textEdited, this, [this](const QString &text) { apply("finish_text", text); });
	connect(style_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
		if (!loading_)
			apply("style", style_->currentData().toInt());
	});
	connect(scene_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
		if (!loading_)
			apply("switch_scene", scene_->currentData().toString());
	});

	refresh();
}

TimerCard::~TimerCard()
{
	obs_weak_source_release(weak_);
}

obs_source_t *TimerCard::acquire() const
{
	return obs_weak_source_get_source(weak_);
}

bool TimerCard::expanded() const
{
	return edit_->isChecked();
}

void TimerCard::setExpanded(bool on)
{
	if (edit_->isChecked() != on) {
		edit_->setChecked(on); /* re-enters through the toggled signal */
		return;
	}
	edit_->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
	if (on)
		loadEditor();
	editor_->setVisible(on);
}

void TimerCard::updateEditorMode(int mode)
{
	auto *form = static_cast<QFormLayout *>(editor_->layout());
	form->setRowVisible(dateRow_, mode == MODE_DATE);
	form->setRowVisible(durationRow_, mode == MODE_DURATION);
	form->setRowVisible(finishRow_, mode != MODE_STOPWATCH);
}

void TimerCard::loadEditor()
{
	obs_source_t *src = acquire();
	if (!src)
		return;
	obs_data_t *settings = obs_source_get_settings(src);
	loading_ = true;

	const int mode = int(obs_data_get_int(settings, "mode"));
	mode_->setCurrentIndex(mode_->findData(mode));
	updateEditorMode(mode);

	const QDateTime target = QDateTime::fromString(QString::fromUtf8(obs_data_get_string(settings, "target")),
						       QStringLiteral(TARGET_FORMAT));
	date_->setProperty("set", target.isValid());
	date_->setDateTime(target.isValid() ? target : QDateTime::currentDateTime().addSecs(3600));

	hours_->setValue(int(obs_data_get_int(settings, "dur_h")));
	minutes_->setValue(int(obs_data_get_int(settings, "dur_m")));
	seconds_->setValue(int(obs_data_get_int(settings, "dur_s")));
	if (!title_->hasFocus())
		title_->setText(QString::fromUtf8(obs_data_get_string(settings, "title")));
	if (!finishText_->hasFocus())
		finishText_->setText(QString::fromUtf8(obs_data_get_string(settings, "finish_text")));
	style_->setCurrentIndex(style_->findData(int(obs_data_get_int(settings, "style"))));

	const QString current = QString::fromUtf8(obs_data_get_string(settings, "switch_scene"));
	scene_->clear();
	scene_->addItem(tr_("SwitchScene.None"), QString());
	char **names = obs_frontend_get_scene_names();
	for (char **name = names; name && *name; ++name)
		scene_->addItem(tr_("Dock.GoTo").arg(QString::fromUtf8(*name)), QString::fromUtf8(*name));
	bfree(names);
	scene_->setCurrentIndex(qMax(0, scene_->findData(current)));

	loading_ = false;
	obs_data_release(settings);
	obs_source_release(src);
}

void TimerCard::apply(const char *key, const QString &value)
{
	obs_source_t *src = acquire();
	if (!src)
		return;
	obs_data_t *change = obs_data_create();
	obs_data_set_string(change, key, value.toUtf8().constData());
	obs_source_update(src, change);
	obs_data_release(change);
	obs_source_release(src);
}

void TimerCard::apply(const char *key, long long value)
{
	obs_source_t *src = acquire();
	if (!src)
		return;
	obs_data_t *change = obs_data_create();
	obs_data_set_int(change, key, value);
	obs_source_update(src, change);
	obs_data_release(change);
	obs_source_release(src);
}

void TimerCard::applyTarget(const QDateTime &when)
{
	date_->setProperty("set", true);
	apply("target", when.toString(QStringLiteral(TARGET_FORMAT)));
}

void TimerCard::applyDuration(int totalSeconds)
{
	obs_source_t *src = acquire();
	if (!src)
		return;
	obs_data_t *change = obs_data_create();
	obs_data_set_int(change, "dur_h", totalSeconds / 3600);
	obs_data_set_int(change, "dur_m", (totalSeconds / 60) % 60);
	obs_data_set_int(change, "dur_s", totalSeconds % 60);
	obs_source_update(src, change);
	obs_data_release(change);
	obs_source_release(src);
}

void TimerCard::refresh()
{
	obs_source_t *src = acquire();
	TimerSource *timer = timer_from_source(src);
	if (!timer) {
		obs_source_release(src);
		return;
	}

	const QString name = QString::fromUtf8(obs_source_get_name(src));
	if (name_->text() != name) {
		name_->setText(name);
		name_->setToolTip(name);
	}
	live_->setVisible(obs_source_active(src));

	const TimerStatus status = timer_status(timer);
	const TimeView &view = status.view;
	time_->setText(formatTimerText(view));

	const bool date = status.mode == MODE_DATE;
	const bool stopwatch = status.mode == MODE_STOPWATCH;
	if (!view.valid)
		setChip(chip_, tr_("Status.NoDate"), "150, 150, 150");
	else if (view.finished)
		setChip(chip_, tr_("Status.Finished"), "255, 77, 94");
	else if (date)
		setChip(chip_, tr_("Status.Scheduled"), "124, 160, 255");
	else if (view.running)
		setChip(chip_, tr_("Status.Running"), "60, 200, 120");
	else
		setChip(chip_, tr_("Status.Paused"), "235, 170, 50");

	play_->setVisible(!date);
	const QString playText = view.running && !view.finished ? QStringLiteral("⏸  ") + tr_("Dock.Pause")
								: QStringLiteral("▶  ") + tr_("Dock.Start");
	if (play_->text() != playText)
		play_->setText(playText);
	reset_->setVisible(!date);
	minus_->setVisible(!stopwatch);
	plus_->setVisible(!stopwatch);
	minus_->setEnabled(view.valid);
	plus_->setEnabled(view.valid);

	obs_source_release(src);
}

/* ------------------------------------------------------------------ */

TimerDock::TimerDock(QWidget *parent) : QWidget(parent)
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(8, 8, 8, 8);
	layout->setSpacing(8);

	auto *header = new QHBoxLayout();
	auto *caption = new QLabel(tr_("Dock.Header"), this);
	setClass(caption, "text-muted");
	QFont captionFont = caption->font();
	captionFont.setBold(true);
	scaleFont(captionFont, 0.85);
	caption->setFont(captionFont);
	header->addWidget(caption);
	header->addStretch(1);
	count_ = new QLabel(this);
	setClass(count_, "text-muted");
	header->addWidget(count_);
	layout->addLayout(header);

	auto *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	auto *content = new QWidget(scroll);
	list_ = new QVBoxLayout(content);
	list_->setContentsMargins(0, 0, 0, 0);
	list_->setSpacing(8);

	empty_ = new QWidget(content);
	auto *emptyLayout = new QVBoxLayout(empty_);
	emptyLayout->setContentsMargins(8, 24, 8, 24);
	auto *emptyIcon = new QLabel(QStringLiteral("⏱"), empty_);
	emptyIcon->setStyleSheet(QStringLiteral("QLabel { font-size: 34px; }"));
	emptyIcon->setAlignment(Qt::AlignCenter);
	emptyLayout->addWidget(emptyIcon);
	auto *emptyTitle = new QLabel(tr_("Dock.EmptyTitle"), empty_);
	QFont emptyFont = emptyTitle->font();
	emptyFont.setBold(true);
	emptyTitle->setFont(emptyFont);
	emptyTitle->setAlignment(Qt::AlignCenter);
	emptyLayout->addWidget(emptyTitle);
	auto *emptyHint = new QLabel(tr_("Dock.EmptyHint"), empty_);
	setClass(emptyHint, "text-muted");
	emptyHint->setWordWrap(true);
	emptyHint->setAlignment(Qt::AlignCenter);
	emptyLayout->addWidget(emptyHint);
	list_->addWidget(empty_);
	list_->addStretch(1);
	scroll->setWidget(content);
	layout->addWidget(scroll, 1);

	auto *add = new QPushButton(QStringLiteral("+  ") + tr_("Dock.New"), this);
	add->setCursor(Qt::PointingHandCursor);
	auto *menu = new QMenu(add);
	const char *presets[] = {"Preset.Starting", "Preset.Launch", "Preset.Break", "Preset.Stopwatch"};
	for (int i = 0; i < 4; ++i)
		menu->addAction(tr_(presets[i]), this, [this, i]() { createTimer(i); });
	add->setMenu(menu);
	layout->addWidget(add);

	timer_ = new QTimer(this);
	timer_->setInterval(250);
	connect(timer_, &QTimer::timeout, this, [this]() { poll(); });
	timer_->start();
}

TimerDock::~TimerDock() {}

void TimerDock::shutdown()
{
	timer_->stop();
	rebuild(QStringList());
}

void TimerDock::poll()
{
	QStringList uuids;
	obs_enum_sources(collectTimers, &uuids);
	if (uuids != uuids_)
		rebuild(uuids);
	for (TimerCard *card : cards_)
		card->refresh();
}

void TimerDock::rebuild(const QStringList &uuids)
{
	QStringList open;
	for (TimerCard *card : cards_) {
		if (card->expanded())
			open.append(card->uuid());
		list_->removeWidget(card);
		card->deleteLater();
	}
	cards_.clear();
	uuids_ = uuids;

	for (const QString &uuid : uuids) {
		obs_source_t *source = obs_get_source_by_uuid(uuid.toUtf8().constData());
		if (!source)
			continue;
		auto *card = new TimerCard(source, this);
		obs_source_release(source);
		list_->insertWidget(list_->count() - 1, card);
		cards_.append(card);
		/* A single timer opens ready to edit; more than one stays compact. */
		card->setExpanded(open.contains(uuid) || (open.isEmpty() && uuids.size() == 1));
	}
	empty_->setVisible(cards_.isEmpty());
	count_->setText(cards_.isEmpty() ? QString() : QString::number(cards_.size()));
}

void TimerDock::createTimer(int preset)
{
	obs_source_t *sceneSource = obs_frontend_preview_program_mode_active()
					    ? obs_frontend_get_current_preview_scene()
					    : obs_frontend_get_current_scene();
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (!scene) {
		obs_source_release(sceneSource);
		return;
	}

	obs_data_t *settings = obs_data_create();
	QString base;
	switch (preset) {
	case PRESET_LAUNCH:
		base = tr_("Preset.Launch");
		obs_data_set_int(settings, "mode", MODE_DATE);
		obs_data_set_string(settings, "target",
				    nextTime(20, 0).toString(QStringLiteral(TARGET_FORMAT)).toUtf8().constData());
		obs_data_set_string(settings, "title", obs_module_text("Preset.Launch.Title"));
		obs_data_set_int(settings, "style", STYLE_BLOCKS);
		break;
	case PRESET_BREAK:
		base = tr_("Preset.Break");
		obs_data_set_int(settings, "mode", MODE_DURATION);
		obs_data_set_int(settings, "dur_m", 10);
		obs_data_set_string(settings, "title", obs_module_text("Preset.Break.Title"));
		obs_data_set_string(settings, "finish_text", obs_module_text("Preset.Break.Finish"));
		obs_data_set_int(settings, "style", STYLE_MINIMAL);
		obs_data_set_bool(settings, "auto_start", true);
		obs_data_set_bool(settings, "confetti", false);
		break;
	case PRESET_STOPWATCH:
		base = tr_("Preset.Stopwatch");
		obs_data_set_int(settings, "mode", MODE_STOPWATCH);
		obs_data_set_int(settings, "style", STYLE_MISSION);
		obs_data_set_int(settings, "width", 800);
		break;
	default:
		base = tr_("Preset.Starting");
		obs_data_set_int(settings, "mode", MODE_DURATION);
		obs_data_set_int(settings, "dur_m", 5);
		obs_data_set_string(settings, "title", obs_module_text("Preset.Starting.Title"));
		obs_data_set_string(settings, "finish_text", obs_module_text("Preset.Starting.Finish"));
		obs_data_set_int(settings, "style", STYLE_RINGS);
		obs_data_set_bool(settings, "auto_start", true);
		break;
	}

	QString name = base;
	for (int i = 2;; ++i) {
		obs_source_t *existing = obs_get_source_by_name(name.toUtf8().constData());
		if (!existing)
			break;
		obs_source_release(existing);
		name = QStringLiteral("%1 %2").arg(base).arg(i);
	}

	obs_source_t *source = obs_source_create(TIMER_SOURCE_ID, name.toUtf8().constData(), settings, nullptr);
	obs_data_release(settings);
	if (source) {
		obs_sceneitem_t *item = obs_scene_add(scene, source);
		struct obs_video_info video = {};
		if (item && obs_get_video_info(&video)) {
			const float width = float(obs_source_get_width(source));
			const float height = float(obs_source_get_height(source));
			const float fit = qMin(1.0f, float(video.base_width) * 0.7f / qMax(1.0f, width));
			struct vec2 scale = {};
			struct vec2 position = {};
			vec2_set(&scale, fit, fit);
			vec2_set(&position, (float(video.base_width) - width * fit) / 2.0f,
				 (float(video.base_height) - height * fit) / 2.0f);
			obs_sceneitem_set_scale(item, &scale);
			obs_sceneitem_set_pos(item, &position);
		}
		obs_source_release(source);
	}
	obs_source_release(sceneSource);
	poll();
}
