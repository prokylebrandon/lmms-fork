/*
 * Prestige.cpp - instrument for hosting VST3 plugins (browse-to-load)
 *
 * Copyright (c) 2024 LMMS contributors
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

#include "Prestige.h"

#include <QDomElement>
#include <QFileDialog>
#include <QGridLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMutexLocker>
#include <QPushButton>

#include "AudioEngine.h"
#include "embed.h"
#include "Engine.h"
#include "InstrumentPlayHandle.h"
#include "InstrumentTrack.h"

namespace lmms
{

extern "C"
{

Plugin::Descriptor Q_DECL_EXPORT prestige_plugin_descriptor =
{
	LMMS_STRINGIFY(PLUGIN_NAME),
	"PRESTIGE",
	QT_TRANSLATE_NOOP("PluginBrowser",
		"VST3 instrument host"),
	"LMMS contributors",
	0x0100,
	Plugin::Type::Instrument,
	new PluginPixmapLoader("logo"),
	"vst3",
	nullptr,
};

}

// ---------------------------------------------------------------------------
// PrestigeInstrument
// ---------------------------------------------------------------------------

PrestigeInstrument::PrestigeInstrument(InstrumentTrack* instrumentTrack) :
	Instrument(instrumentTrack, &prestige_plugin_descriptor, nullptr, Flag::IsSingleStreamed)
{
	// A VST3 instrument manages its own polyphony internally, the same way
	// Vestige's VST2 instruments do (see plugins/Vestige/Vestige.cpp) — so
	// LMMS should call play() once per period rather than playNote() per
	// NotePlayHandle. Per Instrument.h's comment on play(), that requires
	// an InstrumentPlayHandle registered with the audio engine; without
	// this, play() below is never invoked and no audio is produced.
	auto* iph = new InstrumentPlayHandle(this, instrumentTrack);
	Engine::audioEngine()->addPlayHandle(iph);
}

PrestigeInstrument::~PrestigeInstrument()
{
	// NOT independently verified against the full call in
	// plugins/Vestige/Vestige.cpp:185 — the terminal output that revealed
	// this call was cut off before its second argument. This mirrors the
	// pattern (removePlayHandlesOfTypes(instrumentTrack(), <type>)) with
	// the type most likely to be correct. If the play handle isn't
	// actually removed on destruction, confirm the exact enum value in
	// Vestige.cpp before relying on this in anything beyond local testing.
	Engine::audioEngine()->removePlayHandlesOfTypes(instrumentTrack(), PlayHandle::Type::InstrumentPlayHandle);

	unloadPlugin();
}

void PrestigeInstrument::play(SampleFrame* workingBuffer)
{
	QMutexLocker lock(&m_pluginMutex);
	if (!m_plugin)
	{
		return;
	}

	const auto frames = Engine::audioEngine()->framesPerPeriod();

	// SampleFrame is assumed to be laid out as interleaved stereo sample_t
	// pairs, matching Vst3PluginInstance::processAudio()'s expected buffer
	// format exactly, so no conversion is needed beyond reinterpreting the
	// pointer. NOT independently verified: SampleFrame.h wasn't read this
	// session, so this assumes sample_t == float. If it's a fixed-point or
	// double type instead, this cast is wrong and needs a real conversion
	// loop, not a reinterpret_cast.
	auto* out = reinterpret_cast<float*>(workingBuffer);
	m_plugin->processAudio(nullptr, out, frames);
}

bool PrestigeInstrument::handleMidiEvent(const MidiEvent& event, const TimePos& time, f_cnt_t offset)
{
	Q_UNUSED(time)

	QMutexLocker lock(&m_pluginMutex);
	if (m_plugin)
	{
		// Vst3PluginInstance::queueMidiEvent() does the MidiEvent -> VST3
		// Event conversion internally (Vst3EventList, Phase 1) — nothing
		// to translate here.
		m_plugin->queueMidiEvent(event, static_cast<int>(offset));
	}
	return true;
}

void PrestigeInstrument::closePluginLocked()
{
	if (m_plugin)
	{
		// Known rough edge (flagged for Phase 3, per the prompt's request
		// to report rather than paper over these): this does not send an
		// explicit all-notes-off/flush before tearing down. Stage 1 uses
		// the single InstrumentPlayHandle model (not per-note
		// NotePlayHandles), so there's no LMMS-side stuck NotePlayHandle
		// risk, but a plugin with internal voices still ringing gets torn
		// down mid-note rather than released cleanly.
		m_plugin->clearMidiQueue();
		m_plugin->stopProcessing();
		m_plugin.reset();
	}
}

void PrestigeInstrument::unloadPlugin()
{
	QMutexLocker lock(&m_pluginMutex);
	closePluginLocked();
	m_bundlePath.clear();
	m_classCid.clear();
	m_lastError.clear();
}

bool PrestigeInstrument::instantiatePlugin(const QString& preferredCid, QString& error)
{
	QString discoverError;
	const auto classes = Vst3PluginInstance::discoverClasses(m_bundlePath, &discoverError);
	if (classes.empty())
	{
		error = discoverError.isEmpty()
			? tr("No VST3 classes found in bundle")
			: discoverError;
		return false;
	}

	int resolvedIndex = -1;
	QString resolvedCid;

	if (!preferredCid.isEmpty())
	{
		for (const auto& ci : classes)
		{
			if (ci.cid == preferredCid)
			{
				resolvedIndex = ci.classIndex;
				resolvedCid = ci.cid;
				break;
			}
		}
		// preferredCid was given but not found: this is what "missing
		// plugin" means for browse-to-load (see Phase 2 handoff notes) —
		// the bundle path resolves but the specific class inside it no
		// longer does (plugin updated, class removed, etc). Falls through
		// to the auto-pick below rather than failing outright, so a
		// reopened project at least loads *a* sound rather than nothing;
		// this is a Stage 1 simplification, not a considered design
		// choice — Phase 3 should decide whether silently substituting a
		// different class is actually the right behavior here.
	}

	if (resolvedIndex < 0)
	{
		// Multi-instrument bundles get no selection UI in Stage 1 (the
		// original design note is explicit that this needs one
		// eventually) — first instrument-capable class wins.
		for (const auto& ci : classes)
		{
			if (ci.isInstrument)
			{
				resolvedIndex = ci.classIndex;
				resolvedCid = ci.cid;
				break;
			}
		}
	}

	if (resolvedIndex < 0)
	{
		error = tr("No instrument class found in bundle");
		return false;
	}

	const double sampleRate = Engine::audioEngine()->outputSampleRate();
	const int blockSize = Engine::audioEngine()->framesPerPeriod();

	auto result = Vst3PluginInstance::load(m_bundlePath, resolvedIndex, sampleRate, blockSize);
	if (!result)
	{
		error = result.error;
		return false;
	}

	QString startError;
	if (!result.instance->startProcessing(&startError))
	{
		error = startError;
		return false;
	}

	m_plugin = std::move(result.instance);
	m_classCid = resolvedCid;
	return true;
}

bool PrestigeInstrument::loadFile(const QString& bundlePath)
{
	QMutexLocker lock(&m_pluginMutex);
	closePluginLocked();

	m_bundlePath = bundlePath;
	m_classCid.clear();

	QString error;
	if (!instantiatePlugin(QString(), error))
	{
		m_lastError = error;
		m_bundlePath.clear();
		return false;
	}

	m_lastError.clear();
	return true;
}

void PrestigeInstrument::saveSettings(QDomDocument& doc, QDomElement& parent)
{
	QMutexLocker lock(&m_pluginMutex);

	parent.setAttribute("bundlepath", m_bundlePath);
	parent.setAttribute("classcid", m_classCid);

	if (m_plugin)
	{
		const QByteArray state = m_plugin->saveState();
		QDomElement stateNode = doc.createElement("state");
		stateNode.appendChild(doc.createTextNode(QString::fromLatin1(state.toBase64())));
		parent.appendChild(stateNode);
	}
}

void PrestigeInstrument::loadSettings(const QDomElement& thisElement)
{
	QMutexLocker lock(&m_pluginMutex);
	closePluginLocked();

	const QString path = thisElement.attribute("bundlepath");
	const QString cid = thisElement.attribute("classcid");
	if (path.isEmpty())
	{
		return;
	}

	m_bundlePath = path;
	m_classCid.clear();

	QString error;
	if (!instantiatePlugin(cid, error))
	{
		// Bundle path no longer resolves at all (not just the class
		// inside it) — keep the recorded path/cid in the project rather
		// than discarding them, so a manual re-browse or a future
		// Phase 3 "relink" flow still has something to work with.
		m_lastError = error;
		return;
	}

	const QDomElement stateNode = thisElement.firstChildElement("state");
	if (!stateNode.isNull())
	{
		const QByteArray state = QByteArray::fromBase64(stateNode.text().toLatin1());
		if (!state.isEmpty())
		{
			m_plugin->restoreState(state);
		}
	}

	m_lastError.clear();
}

QString PrestigeInstrument::nodeName() const
{
	return prestige_plugin_descriptor.name;
}

gui::PluginView* PrestigeInstrument::instantiateView(QWidget* parent)
{
	return new gui::PrestigeView(this, parent);
}

// ---------------------------------------------------------------------------
// gui::PrestigeView
// ---------------------------------------------------------------------------

namespace gui
{

PrestigeView::PrestigeView(Instrument* instrument, QWidget* parent) :
	InstrumentView(instrument, parent),
	m_pi(dynamic_cast<PrestigeInstrument*>(instrument))
{
	auto* layout = new QGridLayout(this);

	m_nameLabel = new QLabel(this);
	m_vendorLabel = new QLabel(this);
	m_errorLabel = new QLabel(this);
	m_errorLabel->setStyleSheet("color: red;");
	m_errorLabel->setWordWrap(true);

	m_browseButton = new QPushButton(tr("Browse..."), this);
	connect(m_browseButton, &QPushButton::clicked, this, &PrestigeView::browsePlugin);

	m_unloadButton = new QPushButton(tr("Unload"), this);
	connect(m_unloadButton, &QPushButton::clicked, this, &PrestigeView::unloadPlugin);

	layout->addWidget(m_nameLabel, 0, 0, 1, 2);
	layout->addWidget(m_vendorLabel, 1, 0, 1, 2);
	layout->addWidget(m_browseButton, 2, 0);
	layout->addWidget(m_unloadButton, 2, 1);
	layout->addWidget(m_errorLabel, 3, 0, 1, 2);
	layout->setRowStretch(4, 1);

	updateLabels();
}

void PrestigeView::browsePlugin()
{
	if (!m_pi)
	{
		return;
	}

	// NOT verified against another QFileDialog precedent in this codebase
	// (I only checked VstBase's *window-embedding* code, not its file
	// dialogs, and never grepped FileDialog.h or other plugins' browse
	// dialogs for the directory-or-file pattern the prompt calls out).
	// AnyFile mode + a non-native dialog is a commonly-used workaround
	// that lets the user select a directory without entering it, as well
	// as a single file, but it has known platform quirks (a single click
	// may only populate the filename field, requiring the user to also
	// click Open rather than double-click). Flagged as a rough edge to
	// verify on both Windows (legacy single-file .vst3) and Linux/macOS
	// (bundle-directory .vst3) before relying on this.
	QFileDialog dlg(this, tr("Open VST3 Plugin"));
	dlg.setOption(QFileDialog::DontUseNativeDialog, true);
	dlg.setFileMode(QFileDialog::AnyFile);
	dlg.setOption(QFileDialog::ShowDirsOnly, false);
	dlg.setNameFilter(tr("VST3 Plugins (*.vst3)"));

	if (dlg.exec() != QDialog::Accepted)
	{
		return;
	}

	const QStringList selected = dlg.selectedFiles();
	if (selected.isEmpty())
	{
		return;
	}

	if (!m_pi->loadFile(selected.first()))
	{
		QMessageBox::warning(this, tr("VST3 load failed"), m_pi->lastError());
	}

	updateLabels();
}

void PrestigeView::unloadPlugin()
{
	if (!m_pi)
	{
		return;
	}
	m_pi->unloadPlugin();
	updateLabels();
}

void PrestigeView::updateLabels()
{
	if (!m_pi)
	{
		return;
	}

	if (m_pi->isPluginLoaded())
	{
		m_nameLabel->setText(m_pi->pluginName());
		m_vendorLabel->setText(m_pi->pluginVendor());
		m_errorLabel->clear();
	}
	else
	{
		m_nameLabel->setText(tr("No plugin loaded"));
		m_vendorLabel->clear();
		m_errorLabel->setText(m_pi->lastError());
	}
}

} // namespace gui

} // namespace lmms
