/*
 * DiscordPresence.h - shows the current LMMS project in Discord (Rich Presence)
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 */

#ifndef LMMS_GUI_DISCORD_PRESENCE_H
#define LMMS_GUI_DISCORD_PRESENCE_H

#include <QElapsedTimer>
#include <QObject>
#include <QString>

#include "PresencePlaceholders.h"

class QTimer;

namespace lmms::gui
{

class DiscordIpcClient;

//! Keeps the user's Discord presence in sync with the open project.
//!
//! Settings live in the "discord" section of the LMMS config:
//!   enabled   "0" / "1" (default "0")
//!   details   template for the first presence line
//!   state     template for the second presence line
//!   clientid  optional override of the built-in Discord application id
//!
//! Nothing runs while the feature is disabled. While enabled, work only happens
//! when something changed: signals mark the presence dirty, a short debounce
//! timer merges bursts, and at most one update per 15 seconds is sent (Discord's
//! own limit). Values without a change signal (RAM, CPU, scale) are polled by a
//! timer that only runs if the templates actually use one of them.
class DiscordPresence : public QObject
{
	Q_OBJECT

public:
	explicit DiscordPresence(QObject* parent = nullptr);
	~DiscordPresence() override;

	//! Re-reads the settings from the config and starts, updates or stops the presence
	void applySettings();

	const PlaceholderRegistry& registry() const { return m_registry; }

	static QString defaultDetails();
	static QString defaultState();

private slots:
	void markDirty();
	void flush();
	void pollStats();
	void onReady();
	void onDisconnected();
	void onConnectFailed();
	void reconnect();
	void shutdown();

private:
	void registerPlaceholders();
	void connectSignals();
	void stop();
	void scheduleReconnect();
	void updatePollTimer();
	void sampleCpu();
	bool usesPolledValues() const;

	PlaceholderRegistry m_registry;
	PresenceTemplate m_details;
	PresenceTemplate m_state;

	DiscordIpcClient* m_client = nullptr;
	QTimer* m_flushTimer = nullptr;
	QTimer* m_pollTimer = nullptr;
	QTimer* m_reconnectTimer = nullptr;

	bool m_enabled = false;
	bool m_clientIdValid = false;
	bool m_warnedAboutClientId = false;

	bool m_hasSent = false;
	QString m_lastDetails;
	QString m_lastState;
	QElapsedTimer m_sinceLastSend;
	qint64 m_startSeconds = 0;
	int m_retryMs = 0;

	QElapsedTimer m_cpuClock;
	qint64 m_lastCpuMicros = -1;
	QString m_cpuText;
};

} // namespace lmms::gui

#endif // LMMS_GUI_DISCORD_PRESENCE_H
