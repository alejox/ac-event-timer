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

#include <obs.h>

#include <QFrame>
#include <QStringList>
#include <QWidget>

class QComboBox;
class QDateTimeEdit;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTimer;
class QToolButton;
class QVBoxLayout;

/* One card per Event Timer source: live read-out, transport buttons and a quick editor. */
class TimerCard : public QFrame {
public:
	TimerCard(obs_source_t *source, QWidget *parent);
	~TimerCard() override;

	void refresh();
	void setExpanded(bool expanded);
	bool expanded() const;
	const QString &uuid() const { return uuid_; }

private:
	obs_source_t *acquire() const;
	void loadEditor();
	void apply(const char *key, const QString &value);
	void apply(const char *key, long long value);
	void applyTarget(const QDateTime &when);
	void applyDuration(int totalSeconds);
	void updateEditorMode(int mode);

	obs_weak_source_t *weak_ = nullptr;
	QString uuid_;
	bool loading_ = false;

	QLabel *name_ = nullptr;
	QLabel *chip_ = nullptr;
	QLabel *live_ = nullptr;
	QLabel *time_ = nullptr;
	QPushButton *play_ = nullptr;
	QPushButton *reset_ = nullptr;
	QPushButton *minus_ = nullptr;
	QPushButton *plus_ = nullptr;
	QToolButton *edit_ = nullptr;
	QWidget *editor_ = nullptr;
	QComboBox *mode_ = nullptr;
	QWidget *dateRow_ = nullptr;
	QWidget *durationRow_ = nullptr;
	QDateTimeEdit *date_ = nullptr;
	QSpinBox *hours_ = nullptr;
	QSpinBox *minutes_ = nullptr;
	QSpinBox *seconds_ = nullptr;
	QLineEdit *title_ = nullptr;
	QComboBox *style_ = nullptr;
	QWidget *finishRow_ = nullptr;
	QLineEdit *finishText_ = nullptr;
	QComboBox *scene_ = nullptr;
};

class TimerDock : public QWidget {
public:
	explicit TimerDock(QWidget *parent = nullptr);
	~TimerDock() override;

	void shutdown();

private:
	void poll();
	void rebuild(const QStringList &uuids);
	void createTimer(int preset);

	QTimer *timer_ = nullptr;
	QVBoxLayout *list_ = nullptr;
	QWidget *empty_ = nullptr;
	QLabel *count_ = nullptr;
	QList<TimerCard *> cards_;
	QStringList uuids_;
};
