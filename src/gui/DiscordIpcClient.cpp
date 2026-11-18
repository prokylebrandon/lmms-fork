/*
 * DiscordIpcClient.cpp - minimal client for Discord's local Rich Presence IPC
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

#include "DiscordIpcClient.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QLocalSocket>

namespace lmms::gui
{

namespace
{

constexpr quint32 OpHandshake = 0;
constexpr quint32 OpFrame = 1;
constexpr quint32 OpClose = 2;
constexpr quint32 OpPing = 3;
constexpr quint32 OpPong = 4;

constexpr int HeaderSize = 8;
constexpr quint32 MaxPayloadSize = 1u << 20; // real frames are a few hundred bytes
constexpr int MaxPipeIndex = 10;             // Discord uses discord-ipc-0 .. discord-ipc-9

void putU32(char* dest, quint32 value)
{
	dest[0] = static_cast<char>(value & 0xFF);
	dest[1] = static_cast<char>((value >> 8) & 0xFF);
	dest[2] = static_cast<char>((value >> 16) & 0xFF);
	dest[3] = static_cast<char>((value >> 24) & 0xFF);
}

quint32 getU32(const char* src)
{
	const auto* p = reinterpret_cast<const unsigned char*>(src);
	return static_cast<quint32>(p[0]) | (static_cast<quint32>(p[1]) << 8)
		| (static_cast<quint32>(p[2]) << 16) | (static_cast<quint32>(p[3]) << 24);
}

} // namespace


DiscordIpcClient::DiscordIpcClient(const QString& clientId, QObject* parent)
	: QObject(parent)
	, m_clientId(clientId)
	, m_socket(new QLocalSocket(this))
{
	connect(m_socket, &QLocalSocket::connected, this, &DiscordIpcClient::onConnected);
	connect(m_socket, &QLocalSocket::errorOccurred, this, &DiscordIpcClient::onSocketError);
	connect(m_socket, &QLocalSocket::disconnected, this, &DiscordIpcClient::onSocketDisconnected);
	connect(m_socket, &QLocalSocket::readyRead, this, &DiscordIpcClient::onReadyRead);
}


DiscordIpcClient::~DiscordIpcClient()
{
	// Not the orderly path (that is disconnectFromDiscord()); just make sure
	// no signal reaches a half destroyed object.
	m_socket->disconnect(this);
	m_socket->abort();
}


QStringList DiscordIpcClient::candidatePipes() const
{
	QStringList candidates;

#ifdef Q_OS_WIN
	// QLocalSocket turns "discord-ipc-N" into \\.\pipe\discord-ipc-N
	for (int i = 0; i < MaxPipeIndex; ++i)
	{
		candidates << QStringLiteral("discord-ipc-%1").arg(i);
	}
#else
	// Discord looks for its socket in the first of these that is set. Packaged
	// builds (Flatpak, Snap) keep it in a subdirectory.
	QStringList directories;
	for (const char* variable : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"})
	{
		const QString value = qEnvironmentVariable(variable);
		if (!value.isEmpty() && !directories.contains(value)) { directories << value; }
	}
	if (!directories.contains(QStringLiteral("/tmp"))) { directories << QStringLiteral("/tmp"); }

	QStringList bases;
	for (const QString& directory : directories)
	{
		bases << directory;
		bases << directory + QStringLiteral("/app/com.discordapp.Discord");
		bases << directory + QStringLiteral("/snap.discord");
	}

	for (int i = 0; i < MaxPipeIndex; ++i)
	{
		for (const QString& base : bases)
		{
			const QString path = QStringLiteral("%1/discord-ipc-%2").arg(base).arg(i);
			if (QFile::exists(path)) { candidates << path; }
		}
	}
#endif

	return candidates;
}


void DiscordIpcClient::connectToDiscord()
{
	if (m_connecting || m_state != State::Disconnected) { return; }

	m_candidates = candidatePipes();
	m_candidateIndex = 0;
	m_connecting = true;
	tryNextCandidate();
}


void DiscordIpcClient::tryNextCandidate()
{
	if (m_candidateIndex >= m_candidates.size())
	{
		m_connecting = false;
		emit connectFailed();
		return;
	}

	m_socket->abort();
	m_socket->connectToServer(m_candidates.at(m_candidateIndex++));
}


void DiscordIpcClient::onConnected()
{
	m_connecting = false;
	m_state = State::Handshaking;
	m_buffer.clear();

	QJsonObject handshake;
	handshake.insert(QStringLiteral("v"), 1);
	handshake.insert(QStringLiteral("client_id"), m_clientId);
	sendFrame(OpHandshake, QJsonDocument(handshake).toJson(QJsonDocument::Compact));
}


void DiscordIpcClient::onSocketError()
{
	if (m_connecting)
	{
		// This candidate did not work, try the next pipe
		tryNextCandidate();
		return;
	}

	handleLost();
}


void DiscordIpcClient::onSocketDisconnected()
{
	handleLost();
}


void DiscordIpcClient::handleLost()
{
	m_buffer.clear();
	if (m_state == State::Disconnected) { return; }

	m_state = State::Disconnected;
	emit disconnected();
}


void DiscordIpcClient::onReadyRead()
{
	m_buffer.append(m_socket->readAll());

	while (m_buffer.size() >= HeaderSize)
	{
		const quint32 opcode = getU32(m_buffer.constData());
		const quint32 length = getU32(m_buffer.constData() + 4);

		if (length > MaxPayloadSize)
		{
			qWarning("DiscordIpcClient: ignoring a malformed frame, closing the connection");
			m_socket->abort();
			handleLost();
			return;
		}

		if (static_cast<quint64>(m_buffer.size()) < static_cast<quint64>(HeaderSize) + length) { return; }

		const QByteArray payload = m_buffer.mid(HeaderSize, static_cast<int>(length));
		m_buffer.remove(0, HeaderSize + static_cast<int>(length));

		handleFrame(opcode, payload);

		// handleFrame() may have dropped the connection
		if (m_state == State::Disconnected) { return; }
	}
}


void DiscordIpcClient::handleFrame(quint32 opcode, const QByteArray& payload)
{
	switch (opcode)
	{
	case OpFrame:
	{
		const QJsonObject object = QJsonDocument::fromJson(payload).object();
		const QString event = object.value(QStringLiteral("evt")).toString();

		if (event == QLatin1String("READY"))
		{
			if (m_state == State::Handshaking)
			{
				m_state = State::Ready;
				emit ready();
			}
		}
		else if (event == QLatin1String("ERROR"))
		{
			const QString message = object.value(QStringLiteral("data")).toObject()
				.value(QStringLiteral("message")).toString();
			qWarning("DiscordIpcClient: Discord reported an error: %s", qPrintable(message));
		}
		break;
	}
	case OpPing:
		sendFrame(OpPong, payload);
		break;
	case OpClose:
	{
		// Sent for example when the application id is invalid
		const QString message = QJsonDocument::fromJson(payload).object()
			.value(QStringLiteral("message")).toString();
		qWarning("DiscordIpcClient: Discord closed the connection: %s", qPrintable(message));
		m_socket->abort();
		handleLost();
		break;
	}
	default:
		break;
	}
}


void DiscordIpcClient::sendFrame(quint32 opcode, const QByteArray& payload)
{
	if (m_socket->state() != QLocalSocket::ConnectedState) { return; }

	QByteArray frame(HeaderSize, '\0');
	putU32(frame.data(), opcode);
	putU32(frame.data() + 4, static_cast<quint32>(payload.size()));
	frame.append(payload);
	m_socket->write(frame);
}


void DiscordIpcClient::sendActivityCommand(const QJsonObject* activity)
{
	QJsonObject args;
	args.insert(QStringLiteral("pid"), static_cast<qint64>(QCoreApplication::applicationPid()));
	if (activity) { args.insert(QStringLiteral("activity"), *activity); }

	QJsonObject command;
	command.insert(QStringLiteral("cmd"), QStringLiteral("SET_ACTIVITY"));
	command.insert(QStringLiteral("args"), args);
	command.insert(QStringLiteral("nonce"), QString::number(++m_nonce));

	sendFrame(OpFrame, QJsonDocument(command).toJson(QJsonDocument::Compact));
}


void DiscordIpcClient::setActivity(const QJsonObject& activity)
{
	if (!isReady()) { return; }
	sendActivityCommand(&activity);
}


void DiscordIpcClient::clearActivity()
{
	if (!isReady()) { return; }
	sendActivityCommand(nullptr);
}


void DiscordIpcClient::disconnectFromDiscord()
{
	m_connecting = false;

	if (m_state == State::Ready)
	{
		clearActivity();
		// Give the pipe a moment to deliver the clear before it is closed.
		// Only reached when the user turns the feature off or LMMS quits.
		m_socket->flush();
		m_socket->waitForBytesWritten(100);
	}

	m_socket->abort();
	handleLost();
}

} // namespace lmms::gui
