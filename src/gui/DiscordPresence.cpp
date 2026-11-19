/*
 * DiscordPresence.cpp - shows the current LMMS project in Discord (Rich Presence)
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

#include "DiscordPresence.h"

#include <algorithm>

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonObject>
#include <QThread>
#include <QTimer>

#include "AudioEngine.h"
#include "ConfigManager.h"
#include "DiscordIpcClient.h"
#include "Engine.h"
#include "GuiApplication.h"
#include "PatternStore.h"
#include "PianoRoll.h"
#include "ProcessStats.h"
#include "Song.h"
#include "lmmsversion.h"

namespace lmms::gui
{

namespace
{

//! The Discord application the presence belongs to. It decides the name shown
//! ("Playing <name>") and the artwork. Register one at
//! https://discord.com/developers/applications and paste its Application ID here
//! (or set "clientid" in the "discord" section of the config).
constexpr const char* DefaultClientId = "1572597519693910116";

constexpr const char* ConfigSection = "discord";

constexpr int DebounceMs = 750;           // merges bursts of changes into one update
constexpr int MinUpdateIntervalMs = 15000; // Discord: one presence update per 15 seconds
constexpr int PollIntervalMs = 5000;       // RAM / CPU / scale refresh while they are in use
constexpr int FirstRetryMs = 15000;
constexpr int MaxRetryMs = 60000;

//! A Discord application id is a numeric snowflake, so anything else is a typo or the placeholder
bool isPlausibleClientId(const QString& id)
{
	if (id.size() < 15 || id.size() > 22) { return false; }
	return std::all_of(id.cbegin(), id.cend(), [](QChar c) { return c >= QLatin1Char('0') && c <= QLatin1Char('9'); });
}

QString formatBytes(std::uint64_t bytes)
{
	constexpr double mib = 1024.0 * 1024.0;
	const double value = static_cast<double>(bytes) / mib;
	if (value >= 1024.0) { return QString::number(value / 1024.0, 'f', 1) + QStringLiteral(" GB"); }
	return QString::number(static_cast<qint64>(value)) + QStringLiteral(" MB");
}

} // namespace


QString DiscordPresence::defaultDetails()
{
	return QStringLiteral("Working on %projectname%");
}


QString DiscordPresence::defaultState()
{
	return QStringLiteral("%bpm% BPM | %timesignature%");
}


DiscordPresence::DiscordPresence(QObject* parent)
	: QObject(parent)
	, m_startSeconds(QDateTime::currentSecsSinceEpoch())
	, m_retryMs(FirstRetryMs)
{
	const QString clientId = ConfigManager::inst()->value(ConfigSection, "clientid", DefaultClientId).trimmed();
	m_clientIdValid = isPlausibleClientId(clientId);

	m_client = new DiscordIpcClient(clientId, this);
	connect(m_client, &DiscordIpcClient::ready, this, &DiscordPresence::onReady);
	connect(m_client, &DiscordIpcClient::disconnected, this, &DiscordPresence::onDisconnected);
	connect(m_client, &DiscordIpcClient::connectFailed, this, &DiscordPresence::onConnectFailed);

	m_flushTimer = new QTimer(this);
	m_flushTimer->setSingleShot(true);
	connect(m_flushTimer, &QTimer::timeout, this, &DiscordPresence::flush);

	m_pollTimer = new QTimer(this);
	m_pollTimer->setInterval(PollIntervalMs);
	connect(m_pollTimer, &QTimer::timeout, this, &DiscordPresence::pollStats);

	m_reconnectTimer = new QTimer(this);
	m_reconnectTimer->setSingleShot(true);
	connect(m_reconnectTimer, &QTimer::timeout, this, &DiscordPresence::reconnect);

	registerPlaceholders();
	connectSignals();

	// Clears the presence properly when LMMS quits
	connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &DiscordPresence::shutdown);
}


DiscordPresence::~DiscordPresence()
{
	stop();
}


void DiscordPresence::registerPlaceholders()
{
	using Source = PlaceholderRegistry::Source;

	m_registry.add(QStringLiteral("projectname"), tr("Project name (\"Untitled\" for a new project)"), [this]
	{
		const QString file = Engine::getSong()->projectFileName();
		return file.isEmpty() ? tr("Untitled") : QFileInfo(file).completeBaseName();
	});

	m_registry.add(QStringLiteral("bpm"), tr("Tempo"), []
	{
		return QString::number(Engine::getSong()->getTempo());
	});

	m_registry.add(QStringLiteral("timesignature"), tr("Time signature, e.g. 4/4"), []
	{
		MeterModel& meter = Engine::getSong()->getTimeSigModel();
		return QStringLiteral("%1/%2").arg(meter.getNumerator()).arg(meter.getDenominator());
	});

	m_registry.add(QStringLiteral("scale"), tr("Key and scale chosen in the Piano Roll (empty if none)"), []
	{
		auto* gui = getGUI();
		return gui && gui->pianoRoll() ? gui->pianoRoll()->currentScaleLabel() : QString{};
	}, Source::Polled);

	m_registry.add(QStringLiteral("version"), tr("LMMS version"), []
	{
		return QString::fromUtf8(LMMS_VERSION);
	});

	m_registry.add(QStringLiteral("samplerate"), tr("Audio output sample rate"), []
	{
		const auto rate = Engine::audioEngine()->outputSampleRate();
		return rate % 1000 == 0
			? QStringLiteral("%1 kHz").arg(rate / 1000)
			: QStringLiteral("%1 kHz").arg(rate / 1000.0, 0, 'f', 1);
	});

	// LMMS has no live bit depth or bitrate; both exist only while exporting.
	// The placeholder is kept so templates using it do not show raw text.
	m_registry.add(QStringLiteral("bitrate"), tr("Not available (LMMS only has this while exporting)"), []
	{
		return QString{};
	});

	m_registry.add(QStringLiteral("trackcount"), tr("Number of tracks in the Song Editor"), []
	{
		return QString::number(Engine::getSong()->tracks().size());
	});

	m_registry.add(QStringLiteral("patterncount"), tr("Number of patterns in the Pattern Editor"), []
	{
		return QString::number(Engine::patternStore()->numOfPatterns());
	});

	m_registry.add(QStringLiteral("status"), tr("Playing, Recording, Paused, Exporting or Working"), [this]
	{
		const Song* song = Engine::getSong();
		if (song->isExporting()) { return tr("Exporting"); }
		if (song->isRecording()) { return tr("Recording"); }
		if (song->isPlaying()) { return tr("Playing"); }
		if (song->isPaused()) { return tr("Paused"); }
		return tr("Working");
	});

	m_registry.add(QStringLiteral("ram"), tr("Memory used by LMMS"), []
	{
		const std::uint64_t bytes = ProcessStats::residentBytes();
		return bytes > 0 ? formatBytes(bytes) : QString{};
	}, Source::Polled);

	m_registry.add(QStringLiteral("cpu"),
		tr("CPU used by LMMS (percent of the whole machine, not the audio engine load)"), [this]
	{
		return m_cpuText;
	}, Source::Polled);
}


void DiscordPresence::connectSignals()
{
	Song* song = Engine::getSong();

	connect(song, &Song::projectFileNameChanged, this, &DiscordPresence::markDirty);
	connect(song, &Song::projectLoaded, this, &DiscordPresence::markDirty);
	connect(song, &Song::tempoChanged, this, &DiscordPresence::markDirty);
	connect(song, &Song::timeSignatureChanged, this, &DiscordPresence::markDirty);
	connect(song, &TrackContainer::trackAdded, this, &DiscordPresence::markDirty);
	connect(song, &TrackContainer::trackRemoved, this, &DiscordPresence::markDirty);

	// Playback state can change from outside the GUI thread; queue it
	connect(song, &Song::playbackStateChanged, this, &DiscordPresence::markDirty, Qt::QueuedConnection);

	PatternStore* patterns = Engine::patternStore();
	connect(patterns, &TrackContainer::trackAdded, this, &DiscordPresence::markDirty);
	connect(patterns, &TrackContainer::trackRemoved, this, &DiscordPresence::markDirty);
	connect(patterns, &PatternStore::trackUpdated, this, &DiscordPresence::markDirty);

	connect(Engine::audioEngine(), &AudioEngine::sampleRateChanged, this, &DiscordPresence::markDirty,
		Qt::QueuedConnection);
}


void DiscordPresence::applySettings()
{
	ConfigManager* config = ConfigManager::inst();

	const bool enable = config->value(ConfigSection, "enabled", "0").toInt() != 0;
	m_details.setText(config->value(ConfigSection, "details", defaultDetails()));
	m_state.setText(config->value(ConfigSection, "state", defaultState()));

	if (!enable)
	{
		stop();
		return;
	}

	if (!m_clientIdValid)
	{
		if (!m_warnedAboutClientId)
		{
			qWarning("Discord presence: no valid Discord Application ID is configured, so it stays off");
			m_warnedAboutClientId = true;
		}
		stop();
		return;
	}

	m_enabled = true;
	updatePollTimer();

	if (m_client->state() == DiscordIpcClient::State::Disconnected && !m_reconnectTimer->isActive())
	{
		m_client->connectToDiscord();
	}

	markDirty();
}


void DiscordPresence::stop()
{
	m_enabled = false;
	m_flushTimer->stop();
	m_pollTimer->stop();
	m_reconnectTimer->stop();
	m_retryMs = FirstRetryMs;
	m_hasSent = false;
	m_lastCpuMicros = -1;
	m_cpuText.clear();

	m_client->disconnectFromDiscord();
}


void DiscordPresence::shutdown()
{
	stop();
}


bool DiscordPresence::usesPolledValues() const
{
	const auto anyPolled = [this](const PresenceTemplate& text)
	{
		const auto& names = text.placeholders();
		return std::any_of(names.cbegin(), names.cend(), [this](const QString& name)
		{
			return m_registry.contains(name) && m_registry.sourceOf(name) == PlaceholderRegistry::Source::Polled;
		});
	};

	return anyPolled(m_details) || anyPolled(m_state);
}


void DiscordPresence::updatePollTimer()
{
	const bool needed = m_enabled && m_client->isReady() && usesPolledValues();

	if (needed && !m_pollTimer->isActive())
	{
		sampleCpu(); // baseline for the first CPU reading
		m_pollTimer->start();
	}
	else if (!needed)
	{
		m_pollTimer->stop();
	}
}


void DiscordPresence::sampleCpu()
{
	const qint64 cpuMicros = ProcessStats::cpuTimeMicros();
	if (cpuMicros < 0) { return; }

	if (m_lastCpuMicros >= 0 && m_cpuClock.isValid())
	{
		const qint64 wallMicros = m_cpuClock.nsecsElapsed() / 1000;
		const int cores = std::max(1, QThread::idealThreadCount());
		if (wallMicros > 0)
		{
			const double percent = 100.0 * static_cast<double>(cpuMicros - m_lastCpuMicros)
				/ (static_cast<double>(wallMicros) * cores);
			m_cpuText = QString::number(qRound(std::clamp(percent, 0.0, 100.0))) + QLatin1Char('%');
		}
	}

	m_lastCpuMicros = cpuMicros;
	m_cpuClock.restart();
}


void DiscordPresence::pollStats()
{
	sampleCpu();
	markDirty();
}


void DiscordPresence::markDirty()
{
	if (!m_enabled || !m_client->isReady()) { return; }
	if (!m_flushTimer->isActive()) { m_flushTimer->start(DebounceMs); }
}


void DiscordPresence::flush()
{
	if (!m_enabled || !m_client->isReady()) { return; }

	// Discord rejects text shorter than two characters, so such a line is left out
	const auto line = [this](const PresenceTemplate& text)
	{
		const QString rendered = sanitizePresenceText(text.render(m_registry));
		return rendered.size() >= 2 ? rendered : QString{};
	};

	const QString details = line(m_details);
	const QString state = line(m_state);

	if (m_hasSent && details == m_lastDetails && state == m_lastState) { return; }

	if (m_hasSent && m_sinceLastSend.isValid())
	{
		const qint64 remaining = MinUpdateIntervalMs - m_sinceLastSend.elapsed();
		if (remaining > 0)
		{
			// Too early; the text is rendered again when the timer fires so the newest values win
			m_flushTimer->start(static_cast<int>(remaining));
			return;
		}
	}

	QJsonObject activity;
	if (!details.isEmpty()) { activity.insert(QStringLiteral("details"), details); }
	if (!state.isEmpty()) { activity.insert(QStringLiteral("state"), state); }

	QJsonObject timestamps;
	timestamps.insert(QStringLiteral("start"), m_startSeconds);
	QJsonObject assets;
	assets.insert(QStringLiteral("large_image"), QStringLiteral("lmms"));
	assets.insert(QStringLiteral("large_text"), QStringLiteral("LMMS"));
	activity.insert(QStringLiteral("assets"), assets);

	activity.insert(QStringLiteral("timestamps"), timestamps);

	m_client->setActivity(activity);

	m_hasSent = true;
	m_lastDetails = details;
	m_lastState = state;
	m_sinceLastSend.restart();
}


void DiscordPresence::onReady()
{
	m_retryMs = FirstRetryMs;
	m_hasSent = false; // a fresh connection has nothing to compare against
	updatePollTimer();
	markDirty();
}


void DiscordPresence::onDisconnected()
{
	m_pollTimer->stop();
	m_flushTimer->stop();
	m_hasSent = false;
	scheduleReconnect();
}


void DiscordPresence::onConnectFailed()
{
	scheduleReconnect();
}


void DiscordPresence::scheduleReconnect()
{
	if (!m_enabled) { return; }

	// Discord is probably not running (yet): look again later, less often each time
	m_reconnectTimer->start(m_retryMs);
	m_retryMs = std::min(m_retryMs * 2, MaxRetryMs);
}


void DiscordPresence::reconnect()
{
	if (m_enabled) { m_client->connectToDiscord(); }
}

} // namespace lmms::gui
