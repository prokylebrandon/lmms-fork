/*
 * DiscordIpcClient.h - minimal client for Discord's local Rich Presence IPC
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

#ifndef LMMS_GUI_DISCORD_IPC_CLIENT_H
#define LMMS_GUI_DISCORD_IPC_CLIENT_H

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

class QLocalSocket;

namespace lmms::gui
{

//! Talks to a running Discord client over its local IPC pipe
//! (named pipe "discord-ipc-N" on Windows, Unix socket "discord-ipc-N" elsewhere).
//!
//! Everything is asynchronous on the GUI event loop; no threads are involved.
//! Frames are 4 bytes opcode + 4 bytes payload length (both little endian) + JSON.
class DiscordIpcClient : public QObject
{
	Q_OBJECT

public:
	enum class State
	{
		Disconnected,
		Handshaking,
		Ready
	};

	explicit DiscordIpcClient(const QString& clientId, QObject* parent = nullptr);
	~DiscordIpcClient() override;

	//! Looks for a running Discord and starts the handshake. Returns immediately;
	//! the outcome is reported through ready() or connectFailed().
	void connectToDiscord();

	//! Clears the activity (if any) and closes the connection
	void disconnectFromDiscord();

	State state() const { return m_state; }
	bool isReady() const { return m_state == State::Ready; }

	//! Sends the given activity object (the "activity" part of SET_ACTIVITY)
	void setActivity(const QJsonObject& activity);

	//! Removes the activity from the user's profile
	void clearActivity();

signals:
	//! Handshake finished, activities may be sent
	void ready();

	//! No Discord client could be reached
	void connectFailed();

	//! An established connection was closed by Discord or broke
	void disconnected();

private slots:
	void onConnected();
	void onSocketError();
	void onSocketDisconnected();
	void onReadyRead();

private:
	void tryNextCandidate();
	QStringList candidatePipes() const;
	void sendFrame(quint32 opcode, const QByteArray& payload);
	void sendActivityCommand(const QJsonObject* activity);
	void handleFrame(quint32 opcode, const QByteArray& payload);
	void handleLost();

	QString m_clientId;
	QLocalSocket* m_socket;
	State m_state = State::Disconnected;
	bool m_connecting = false;
	QStringList m_candidates;
	int m_candidateIndex = 0;
	QByteArray m_buffer;
	quint64 m_nonce = 0;
};

} // namespace lmms::gui

#endif // LMMS_GUI_DISCORD_IPC_CLIENT_H
