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

#include <QDebug>
#include <QDomDocument>
#include <QDomElement>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMutexLocker>
#include <QObject>
#include <QPushButton>
#include <QTextStream>
#include <QTimer>

#include "AudioEngine.h"
#include "embed.h"
#include "Engine.h"
#include "GuiApplication.h"
#include "InstrumentPlayHandle.h"
#include "InstrumentTrack.h"
#include "MainWindow.h"
#include "SampleFrame.h"
#include "Song.h"
#include "Vst3ParameterModel.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace lmms
{

// play() hands LMMS's SampleFrame buffer to Vst3PluginInstance::processAudio()
// as a plain interleaved float* (L R L R ...).  That is only valid if a
// SampleFrame is exactly one left + one right float with no padding.  This
// turns that assumption into a compile error instead of silent garbage audio.
static_assert(sizeof(SampleFrame) == 2 * sizeof(float),
	"PRESTIGE assumes SampleFrame is a tightly packed interleaved float pair");

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

	// No status signals here: the object is going away, and closePluginLocked()
	// already announces pluginAboutToClose() to anything holding the plugin.
	QMutexLocker lock(&m_pluginMutex);
	closePluginLocked();
	resetRecordLocked();
}

void PrestigeInstrument::play(SampleFrame* workingBuffer)
{
	// Never block the audio thread: if the GUI thread is in the middle of a
	// load/unload (which holds this mutex for as long as a DLL takes to load),
	// skip this period instead of stalling audio.  Offline export must not
	// drop blocks, so it waits.  Same policy as Vestige::play().
	if (!m_pluginMutex.tryLock(Engine::getSong()->isExporting() ? -1 : 0))
	{
		return;
	}

	if (m_plugin)
	{
		const auto frames = Engine::audioEngine()->framesPerPeriod();

		// Layout is guaranteed by the static_assert at the top of this file
		// (sample_t is float; SampleFrame is a tightly packed L/R pair).
		auto* out = reinterpret_cast<float*>(workingBuffer);
		m_plugin->processAudio(nullptr, out, frames);
	}

	m_pluginMutex.unlock();
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
		// An open editor window holds a native child of the view we are about
		// to destroy.  Let the GUI close it (detaching the plugin's IPlugView
		// first) before the plugin goes away.  Runs on the GUI thread: plugin
		// load/unload/project-load are all GUI-thread operations. The
		// parameter window listens too and unbinds its knob from the models
		// below, which are still alive at this point.
		emit pluginAboutToClose();

		// Parameter models must stop reaching into the plugin, and the
		// plugin must stop reaching into them, before either side is torn
		// down. This is the Phase 3 "audit for stale parameter models left
		// pointing at the previous plugin" item: doing it here, inside the
		// one function every unload/replace/reload path already funnels
		// through, is what makes it apply everywhere rather than needing to
		// be repeated at each call site.
		teardownParameterModels();

		// Part 2 fix for the rough edge flagged in Part 1: send an explicit
		// NoteOff for anything still held down, and deliver it into the
		// plugin, BEFORE we stop processing -- a plugin with internal
		// voices still ringing now gets a clean release instead of being
		// torn down mid-note. Order matters: flushActiveNotes() requires
		// m_processing still true (it delivers synchronously via one more
		// processAudio() call), so it must run before stopProcessing().
		// It reuses queueMidiEvent()'s own drop/count handling internally,
		// so a queue that is somehow already full here degrades the same
		// way any other MIDI burst would, rather than needing separate
		// handling.
		//
		// This covers plugin replacement equally with plain unload: both
		// funnel through this one function (see the class comment above
		// teardownParameterModels()), which is what the Part 2 prompt
		// asked to verify.
		//
		// Residual caveat, NOT fully verified: Vst3PluginInstance tracks
		// "currently held" purely from the NoteOn/NoteOff events PRESTIGE
		// itself queued (see its m_activeNotes doc comment) -- it cannot
		// know about a note the plugin considers active for some other
		// reason (e.g. its own internal arpeggiator/sequencer holding a
		// voice with no corresponding host-sent NoteOn). Such a plugin can
		// still ring past teardown; nothing observed in this codebase
		// exercises that case, so it's flagged rather than assumed absent.
		m_plugin->flushActiveNotes();
		m_plugin->clearMidiQueue();
		m_plugin->stopProcessing();
		m_plugin.reset();
	}
}

void PrestigeInstrument::resetRecordLocked()
{
	m_bundlePath.clear();
	m_classCid.clear();
	m_savedName.clear();
	m_savedVendor.clear();
	m_lastError.clear();
	m_warning.clear();
	m_retainedXml.clear();
	m_status = LoadStatus::Empty;
	m_uiEditorOpen = false;
	m_uiParametersOpen = false;
}

void PrestigeInstrument::unloadPlugin()
{
	{
		QMutexLocker lock(&m_pluginMutex);
		closePluginLocked();
		resetRecordLocked();
	}
	emit parameterModelsChanged();
	emit pluginStatusChanged();
}

bool PrestigeInstrument::fail(LoadStatus status, const QString& userMessage)
{
	m_status = status;
	m_lastError = userMessage;
	return false;
}

bool PrestigeInstrument::instantiatePlugin(const QString& preferredCid)
{
	// User-facing messages are deliberately short and non-technical. The
	// detail that helps a developer (paths, VST3 result codes, exception
	// text from the loader) goes to the debug log via qWarning().

	// QFileInfo::exists() is true for both forms of a .vst3: a single file
	// (Windows legacy) and a bundle directory (macOS, Linux, modern
	// Windows). Nothing here assumes a particular extension or layout.
	if (!QFileInfo::exists(m_bundlePath))
	{
		qWarning("PRESTIGE: plugin bundle does not exist: %s", qPrintable(m_bundlePath));
		return fail(LoadStatus::Missing, tr("The VST3 plugin could not be found."));
	}

	QString discoverError;
	const auto classes = Vst3PluginInstance::discoverClasses(m_bundlePath, &discoverError);
	if (classes.empty())
	{
		qWarning("PRESTIGE: no VST3 classes in %s (%s)", qPrintable(m_bundlePath), qPrintable(discoverError));
		return fail(LoadStatus::LoadFailed, tr("The VST3 plugin could not be loaded."));
	}

	int resolvedIndex = -1;
	QString resolvedCid;

	if (!preferredCid.isEmpty())
	{
		// Strict match on the recorded class UID. A bundle that resolves but
		// no longer contains that class (plugin updated, class removed) is
		// reported rather than silently replaced by whatever instrument
		// happens to be first: substituting a different sound under
		// someone's saved automation and state is worse than saying so.
		for (const auto& ci : classes)
		{
			if (ci.cid == preferredCid)
			{
				resolvedIndex = ci.classIndex;
				resolvedCid = ci.cid;
				break;
			}
		}
		if (resolvedIndex < 0)
		{
			qWarning("PRESTIGE: class %s not found in %s", qPrintable(preferredCid), qPrintable(m_bundlePath));
			return fail(LoadStatus::ClassNotFound,
				tr("The plugin saved in this project is no longer available in this file."));
		}
	}
	else
	{
		// Multi-instrument bundles get no selection UI yet (the original
		// design note is explicit that this needs one eventually) — first
		// instrument-capable class wins.
		for (const auto& ci : classes)
		{
			if (ci.isInstrument)
			{
				resolvedIndex = ci.classIndex;
				resolvedCid = ci.cid;
				break;
			}
		}
		if (resolvedIndex < 0)
		{
			qWarning("PRESTIGE: no instrument class in %s", qPrintable(m_bundlePath));
			return fail(LoadStatus::NoInstrument, tr("No compatible instrument class was found."));
		}
	}

	const double sampleRate = Engine::audioEngine()->outputSampleRate();
	const int blockSize = Engine::audioEngine()->framesPerPeriod();

	auto result = Vst3PluginInstance::load(m_bundlePath, resolvedIndex, sampleRate, blockSize);
	if (!result)
	{
		qWarning("PRESTIGE: load failed for %s: %s", qPrintable(m_bundlePath), qPrintable(result.error));
		return fail(LoadStatus::LoadFailed, tr("The VST3 plugin could not be loaded."));
	}

	QString startError;
	if (!result.instance->startProcessing(&startError))
	{
		qWarning("PRESTIGE: startProcessing failed for %s: %s", qPrintable(m_bundlePath), qPrintable(startError));
		return fail(LoadStatus::InitFailed, tr("Plugin initialization failed."));
	}

	m_plugin = std::move(result.instance);
	m_classCid = resolvedCid;
	m_savedName = m_plugin->name();
	m_savedVendor = m_plugin->vendor();
	m_status = LoadStatus::Loaded;
	m_lastError.clear();

	// Callers (loadFile()/loadSettings()) already ran closePluginLocked()
	// before this, so m_parameterModels is guaranteed empty here — no
	// leftover model can end up pointed at this new m_plugin by accident.
	buildParameterModels();

	return true;
}

void PrestigeInstrument::buildParameterModels()
{
	Q_ASSERT(m_parameterModels.empty());
	if (!m_plugin)
	{
		return;
	}

	const auto& params = m_plugin->parameters();
	m_parameterModels.reserve(params.size());
	for (const auto& info : params)
	{
		// getParameterNormalized() only returns nullopt for an id that
		// isn't in m_plugin->parameters() at all, which can't happen here
		// since info.id came from that same list — the fallback is just
		// defensive, not expected to trigger.
		const double current = m_plugin->getParameterNormalized(info.id).value_or(info.defaultNormalisedValue);
		m_parameterModels.push_back(std::make_unique<Vst3ParameterModel>(this, m_plugin.get(), info, current));
	}

	// Wire plugin -> host edits (the plugin's own editor moving a
	// parameter) to the matching model. LMMS -> plugin is the reverse
	// direction and is wired inside each Vst3ParameterModel itself.
	m_plugin->setParameterEditedCallback(
		[this](Vst3ParamID id, double value) { onPluginParameterEdited(id, value); });
}

void PrestigeInstrument::teardownParameterModels()
{
	if (m_plugin)
	{
		// Stop the plugin from being able to reach a model mid-teardown,
		// while m_plugin is still valid enough to call this on.
		m_plugin->setParameterEditedCallback(nullptr);
	}

	// Tell every model its plugin is gone before destroying any of them,
	// mirroring how Vst3PluginInstance detaches its own host-callback
	// objects (Vst3ComponentHandlerImpl/Vst3PlugFrameImpl) before
	// releasing them: detach-then-destroy, never the other order.
	for (auto& model : m_parameterModels)
	{
		model->detachPlugin();
	}
	m_parameterModels.clear();
}

void PrestigeInstrument::onPluginParameterEdited(Vst3ParamID id, double normalisedValue)
{
	for (auto& model : m_parameterModels)
	{
		if (model->id() == id)
		{
			model->setValueFromPlugin(normalisedValue);
			return;
		}
	}
}

bool PrestigeInstrument::createEditor(int* width, int* height, EditorResizeCallback onResize, QString* error)
{
	if (!m_plugin)
	{
		if (error) { *error = tr("No plugin loaded"); }
		return false;
	}
	return m_plugin->createEditor(width, height, std::move(onResize), error);
}

bool PrestigeInstrument::attachEditor(void* nativeParent, QString* error)
{
	if (!m_plugin)
	{
		if (error) { *error = tr("No plugin loaded"); }
		return false;
	}
	return m_plugin->attachEditor(nativeParent, error);
}

void PrestigeInstrument::closeEditor()
{
	if (m_plugin)
	{
		m_plugin->closeEditor();
	}
}

void PrestigeInstrument::loadFile(const QString& bundlePath)
{
	QMutexLocker lock(&m_pluginMutex);

	// If the project recorded a plugin we could not run, its saved data is
	// being retained. Remember that identity so a failed browse (wrong file,
	// unloadable file) does not make the record of the missing plugin vanish.
	const bool hadRetained = !m_retainedXml.isEmpty();
	const QString previousPath = m_bundlePath;
	const QString previousCid = m_classCid;

	closePluginLocked();

	m_bundlePath = bundlePath;
	m_classCid.clear();
	m_warning.clear();

	if (!instantiatePlugin(QString()))
	{
		if (hadRetained)
		{
			m_bundlePath = previousPath;
			m_classCid = previousCid;
		}
		else
		{
			m_bundlePath.clear();
			m_classCid.clear();
			m_savedName.clear();
			m_savedVendor.clear();
		}
		emit pluginStatusChanged();
		return;
	}

	// A plugin that was recorded but missing, and has now been located: if
	// the chosen bundle contains the very class the project recorded, put
	// the project's saved settings for it back ("relink"). A different
	// plugin replaces them, deliberately (the view asks before that
	// happens).
	bool relinked = false;
	if (hadRetained)
	{
		if (m_classCid == previousCid)
		{
			QDomDocument retained;
			if (retained.setContent(m_retainedXml))
			{
				const QDomElement root = retained.documentElement();
				if (root.attribute("version", "0").toInt() <= kSaveVersion)
				{
					applySavedElement(root);
					relinked = true;
				}
			}
		}
		m_retainedXml.clear();
	}

	emit parameterModelsChanged();
	emit pluginStatusChanged();
	if (relinked)
	{
		emit uiRestoreRequested();
	}
}

void PrestigeInstrument::retainElement(const QDomElement& element)
{
	m_retainedXml.clear();
	QTextStream stream(&m_retainedXml);
	element.save(stream, -1); // -1: no added whitespace, so text nodes (base64 state) round-trip exactly
}

void PrestigeInstrument::saveSettings(QDomDocument& doc, QDomElement& parent)
{
	QMutexLocker lock(&m_pluginMutex);

	// A recorded plugin that is not running (missing, failed, or saved by a
	// newer PRESTIGE): write back exactly what was loaded, so opening and
	// re-saving a project on a machine without the plugin loses nothing.
	if (!m_plugin && !m_retainedXml.isEmpty())
	{
		QDomDocument retained;
		if (retained.setContent(m_retainedXml))
		{
			const QDomElement root = retained.documentElement();

			const QDomNamedNodeMap attributes = root.attributes();
			for (int i = 0; i < attributes.length(); ++i)
			{
				const QDomAttr attribute = attributes.item(i).toAttr();
				// JournallingObject::saveState() has already set "id" (and
				// anything else it owns) on parent; only fill in the gaps.
				if (!parent.hasAttribute(attribute.name()))
				{
					parent.setAttribute(attribute.name(), attribute.value());
				}
			}
			for (QDomNode child = root.firstChild(); !child.isNull(); child = child.nextSibling())
			{
				parent.appendChild(doc.importNode(child, true));
			}
			return;
		}
		// Unparseable retained data cannot happen for something we wrote
		// ourselves; fall through and save the recorded identity below.
	}

	// --- piece 1: format version -----------------------------------------
	parent.setAttribute("version", kSaveVersion);

	// --- piece 2: plugin identity (bundle location + class UID) ------------
	parent.setAttribute("bundlepath", m_bundlePath);
	parent.setAttribute("classcid", m_classCid);
	// Name/vendor are only for naming the plugin to the user if it is
	// missing when the project is next opened; identity is path + cid.
	parent.setAttribute("pluginname", m_savedName);
	parent.setAttribute("pluginvendor", m_savedVendor);

	if (m_plugin)
	{
		// --- piece 3: plugin-owned state -----------------------------------
		// Vst3PluginInstance::saveState() packs component and controller
		// state into ONE blob; that combined format is still what most
		// plugins get, since most expose one object for both interfaces
		// (Vst3PluginInstance::hasSeparateControllerState() is false).
		// Where the plugin genuinely separates them, store the two pieces
		// under their own elements instead, so a save/restore round trip
		// no longer has to reassemble/split a blob whose internal layout
		// is really the plugin's business, not PRESTIGE's. The "format"
		// attribute is what a reader checks BEFORE calling .text() on
		// <state> -- see applySavedElement() -- so a "separate" blob is
		// never misread as one combined text node (and vice versa); that
		// is also why this needed kSaveVersion bumped to 2, so a Part-1-
		// only reader (kSaveVersion 1, no format check) refuses the file
		// instead of doing exactly that misread.
		QDomElement stateNode = doc.createElement("state");
		if (m_plugin->hasSeparateControllerState())
		{
			stateNode.setAttribute("format", "separate");

			const QByteArray compState = m_plugin->saveComponentState();
			QDomElement compNode = doc.createElement("component");
			compNode.appendChild(doc.createTextNode(QString::fromLatin1(compState.toBase64())));
			stateNode.appendChild(compNode);

			const QByteArray ctrlState = m_plugin->saveControllerState();
			QDomElement ctrlNode = doc.createElement("controller");
			ctrlNode.appendChild(doc.createTextNode(QString::fromLatin1(ctrlState.toBase64())));
			stateNode.appendChild(ctrlNode);
		}
		else
		{
			stateNode.setAttribute("format", "combined");
			const QByteArray state = m_plugin->saveState();
			stateNode.appendChild(doc.createTextNode(QString::fromLatin1(state.toBase64())));
		}
		parent.appendChild(stateNode);

		// --- piece 4: host-side parameter/automation state ------------------
		// Separate from the plugin's own state blob above. The plugin's state
		// has no concept of an LMMS automation clip or MIDI CC connection
		// attached to one of its parameters, so without this block those
		// connections would be silently lost on every project save. Keyed by
		// VST3 parameter ID throughout, same as bundlepath/classcid above.
		QDomElement paramsNode = doc.createElement("parameters");
		for (auto& model : m_parameterModels)
		{
			model->saveSettings(doc, paramsNode);
		}
		parent.appendChild(paramsNode);
	}

	// --- piece 5: UI state ---------------------------------------------------
	QDomElement uiNode = doc.createElement("ui");
	uiNode.setAttribute("editor", m_uiEditorOpen ? 1 : 0);
	uiNode.setAttribute("parameters", m_uiParametersOpen ? 1 : 0);
	parent.appendChild(uiNode);
}

void PrestigeInstrument::applySavedElement(const QDomElement& element)
{
	if (!m_plugin)
	{
		return;
	}

	// Plugin-owned state.
	const QDomElement stateNode = element.firstChildElement("state");
	if (!stateNode.isNull())
	{
		// Check "format" BEFORE touching .text(): for a "separate" node,
		// .text() would concatenate the <component> and <controller> child
		// elements' base64 text into one garbled string (QDomElement::text()
		// walks every descendant text node) -- exactly the misread
		// saveSettings()'s comment on this attribute exists to prevent.
		// Missing/"combined" (including every version-0/1 project, which
		// predates this attribute existing at all) is the plain single
		// text-node blob restoreState() has always expected.
		const QString format = stateNode.attribute("format", "combined");
		bool restored = true;
		bool anyState = false;

		if (format == QLatin1String("separate"))
		{
			const QDomElement compNode = stateNode.firstChildElement("component");
			const QDomElement ctrlNode = stateNode.firstChildElement("controller");

			const QByteArray compState = QByteArray::fromBase64(compNode.text().toLatin1());
			if (!compState.isEmpty())
			{
				anyState = true;
				restored = m_plugin->restoreComponentState(compState) && restored;
			}

			const QByteArray ctrlState = QByteArray::fromBase64(ctrlNode.text().toLatin1());
			if (!ctrlState.isEmpty())
			{
				anyState = true;
				// Only meaningful if this instance's controller is also
				// separate; a plugin that changed shape between save and
				// load (unlikely, but not something to crash over) simply
				// has this saved piece silently unusable, same treatment
				// as any other saved data a different plugin can't apply.
				restored = m_plugin->restoreControllerState(ctrlState) && restored;
			}
		}
		else
		{
			const QByteArray state = QByteArray::fromBase64(stateNode.text().toLatin1());
			if (!state.isEmpty())
			{
				anyState = true;
				restored = m_plugin->restoreState(state);
			}
		}

		if (anyState && !restored)
		{
			// The plugin stays loaded and usable, at whatever state
			// instantiatePlugin() left it in; say so rather than pretend.
			qWarning("PRESTIGE: state restore failed for %s", qPrintable(m_plugin->name()));
			m_warning = tr("Plugin state could not be restored.");
		}
	}

	// restoreState() above may have moved parameter values inside the
	// plugin (or, for a plugin that doesn't restore every parameter from
	// its state blob, left some of them at whatever instantiatePlugin()
	// initialised them to). Refresh every model from the controller now,
	// before applying the saved <parameters> block below, so the two
	// sources of truth can't disagree and the UI can't drift from the
	// plugin.
	for (auto& model : m_parameterModels)
	{
		const auto current = m_plugin->getParameterNormalized(model->id());
		if (current)
		{
			model->setValueFromPlugin(*current);
		}
	}

	// Saved automation/controller-connection state (see saveSettings())
	// takes precedence over the plugin's own restored values above, since
	// it's the only place an LMMS automation clip or MIDI CC connection on
	// a parameter is recorded at all. Matched by id, not by position: a
	// plugin update between save and load can reorder or drop parameters.
	const QDomElement paramsNode = element.firstChildElement("parameters");
	if (!paramsNode.isNull())
	{
		for (auto paramElement = paramsNode.firstChildElement("param"); !paramElement.isNull();
			paramElement = paramElement.nextSiblingElement("param"))
		{
			const auto id = static_cast<Vst3ParamID>(paramElement.attribute("id").toULongLong());
			for (auto& model : m_parameterModels)
			{
				if (model->id() == id)
				{
					model->loadSettings(paramElement);
					break;
				}
			}
		}
	}

	// UI state. Only recorded here; the view acts on it when it receives
	// uiRestoreRequested().
	const QDomElement uiNode = element.firstChildElement("ui");
	m_uiEditorOpen = uiNode.attribute("editor") == QLatin1String("1");
	m_uiParametersOpen = uiNode.attribute("parameters") == QLatin1String("1");
}

void PrestigeInstrument::loadSettings(const QDomElement& thisElement)
{
	QMutexLocker lock(&m_pluginMutex);

	closePluginLocked();
	resetRecordLocked();

	// Projects saved before versioning existed carry no attribute: version
	// 0. Its layout is version 1 minus name/vendor/ui, so it needs no
	// migration step, only tolerance for the missing pieces.
	const int version = thisElement.attribute("version", "0").toInt();

	const QString path = thisElement.attribute("bundlepath");
	const QString cid = thisElement.attribute("classcid");

	if (version > kSaveVersion)
	{
		// Saved by a newer PRESTIGE than this one. Do not guess at a layout
		// that may have changed: keep the element exactly as found (it is
		// written back unchanged on save) and say so.
		qWarning("PRESTIGE: project saved with format version %d, this build understands up to %d",
			version, kSaveVersion);
		m_bundlePath = path;
		m_classCid = cid;
		m_savedName = thisElement.attribute("pluginname");
		m_savedVendor = thisElement.attribute("pluginvendor");
		retainElement(thisElement);
		fail(LoadStatus::UnsupportedVersion,
			tr("This project was saved by a newer version of PRESTIGE. The plugin was not loaded "
				"and its saved data has been left unchanged."));
		emit parameterModelsChanged();
		emit pluginStatusChanged();
		return;
	}

	if (path.isEmpty())
	{
		emit parameterModelsChanged();
		emit pluginStatusChanged();
		return;
	}

	m_bundlePath = path;
	m_classCid = cid;
	m_savedName = thisElement.attribute("pluginname");
	m_savedVendor = thisElement.attribute("pluginvendor");

	if (!instantiatePlugin(cid))
	{
		// Keep the recorded path/cid/name AND the project's saved data for
		// this plugin: the recorded identity stays in place (instantiatePlugin()
		// only overwrites it on success), and the element is retained
		// verbatim so re-saving without the plugin present does not destroy
		// its state, parameters or automation connections.
		retainElement(thisElement);
		emit parameterModelsChanged();
		emit pluginStatusChanged();
		return;
	}

	applySavedElement(thisElement);

	emit parameterModelsChanged();
	emit pluginStatusChanged();
	emit uiRestoreRequested();
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

namespace
{

/// Watches a SubWindow for its Close event so a callback can run BEFORE the
/// window goes away (for the editor: detach the plugin view first). Never
/// consumes the event: normal close handling still runs afterwards.
class EditorCloseFilter : public QObject
{
public:
	EditorCloseFilter(QObject* parent, std::function<void()> onClose) :
		QObject(parent),
		m_onClose(std::move(onClose))
	{
	}

protected:
	bool eventFilter(QObject* watched, QEvent* event) override
	{
		if (event->type() == QEvent::Close && m_onClose)
		{
			m_onClose();
		}
		return QObject::eventFilter(watched, event);
	}

private:
	std::function<void()> m_onClose;
};

/// IPlugView sizes are physical pixels on Windows/Linux but logical units on
/// macOS (see the comment above IPlugView in the SDK's iplugview.h), while Qt
/// widget sizes are always logical.  Without this, on a 125%/150% scaled
/// Windows display the plugin would draw into only part of an oversized frame.
QSize toWidgetSize(const QWidget* reference, int viewWidth, int viewHeight)
{
#ifdef __APPLE__
	Q_UNUSED(reference)
	return QSize(viewWidth, viewHeight);
#else
	const qreal ratio = reference->devicePixelRatioF();
	return QSize(qRound(viewWidth / ratio), qRound(viewHeight / ratio));
#endif
}

} // namespace

PrestigeView::PrestigeView(Instrument* instrument, QWidget* parent) :
	InstrumentView(instrument, parent),
	m_pi(dynamic_cast<PrestigeInstrument*>(instrument))
{
	auto* layout = new QGridLayout(this);

	m_nameLabel = new QLabel(this);
	m_vendorLabel = new QLabel(this);

	m_warningLabel = new QLabel(this);
	m_warningLabel->setStyleSheet("color: #c80;");
	m_warningLabel->setWordWrap(true);

	m_errorLabel = new QLabel(this);
	m_errorLabel->setStyleSheet("color: red;");
	m_errorLabel->setWordWrap(true);

	m_browseButton = new QPushButton(tr("Browse..."), this);
	connect(m_browseButton, &QPushButton::clicked, this, &PrestigeView::browsePlugin);

	m_unloadButton = new QPushButton(tr("Unload"), this);
	connect(m_unloadButton, &QPushButton::clicked, this, &PrestigeView::unloadPlugin);

	m_editorButton = new QPushButton(tr("Show editor"), this);
	connect(m_editorButton, &QPushButton::clicked, this, &PrestigeView::toggleEditor);

	m_paramsButton = new QPushButton(tr("Parameters..."), this);
	connect(m_paramsButton, &QPushButton::clicked, this, &PrestigeView::toggleParameters);

	layout->addWidget(m_nameLabel, 0, 0, 1, 2);
	layout->addWidget(m_vendorLabel, 1, 0, 1, 2);
	layout->addWidget(m_browseButton, 2, 0);
	layout->addWidget(m_unloadButton, 2, 1);
	layout->addWidget(m_editorButton, 3, 0);
	layout->addWidget(m_paramsButton, 3, 1);
	layout->addWidget(m_warningLabel, 4, 0, 1, 2);
	layout->addWidget(m_errorLabel, 5, 0, 1, 2);
	layout->setRowStretch(6, 1);

	if (m_pi)
	{
		// The editor window must close before the plugin it shows is
		// destroyed (unload, replace, or project reload), and the parameter
		// window must let go of the parameter models before they die.
		connect(m_pi.data(), &PrestigeInstrument::pluginAboutToClose,
			this, &PrestigeView::onPluginAboutToClose);
		connect(m_pi.data(), &PrestigeInstrument::parameterModelsChanged,
			this, &PrestigeView::onParameterModelsChanged);
		connect(m_pi.data(), &PrestigeInstrument::pluginStatusChanged,
			this, &PrestigeView::updateLabels);
		connect(m_pi.data(), &PrestigeInstrument::uiRestoreRequested,
			this, &PrestigeView::onRestoreUi);
	}

	updateLabels();

	// A view created after the plugin was already loaded (the usual order
	// is the other way round during project load; both happen).
	onRestoreUi();
}

PrestigeView::~PrestigeView()
{
	closeEditorWindow();

	if (m_paramsWidget)
	{
		// Unbind the inspector knob from whatever model it is showing before
		// the window (and, possibly, the models) go away.
		m_paramsWidget->clearParameters(QString());
	}
	if (m_paramsWindow)
	{
		m_paramsWindow->hide();
		m_paramsWindow->deleteLater();
		m_paramsWindow.clear();
	}
}

void PrestigeView::browsePlugin()
{
	if (!m_pi)
	{
		return;
	}

	// Choosing a plugin while the project holds saved settings for a
	// missing one: say what will happen to them BEFORE it happens.
	if (m_pi->hasRetainedState())
	{
		QString text;
		if (m_pi->status() == PrestigeInstrument::LoadStatus::UnsupportedVersion)
		{
			text = tr("This project was saved by a newer version of PRESTIGE. Loading a plugin here "
				"replaces its saved data. Continue?");
		}
		else
		{
			const QString name = m_pi->savedPluginName().isEmpty()
				? tr("the missing plugin")
				: m_pi->savedPluginName();
			text = tr("This project has saved settings for %1. They are restored only if you choose "
				"that same plugin; choosing a different plugin replaces them. Continue?").arg(name);
		}
		if (QMessageBox::question(this, tr("Choose VST3 plugin"), text) != QMessageBox::Yes)
		{
			return;
		}
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

	m_pi->loadFile(selected.first());
	if (!m_pi->isPluginLoaded())
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

	// Unloading a missing plugin throws away the settings the project has
	// been keeping for it. That is the one action here that destroys saved
	// work, so it asks first.
	if (m_pi->hasRetainedState())
	{
		const auto answer = QMessageBox::question(this, tr("Remove plugin"),
			tr("Unloading removes the saved settings this project holds for the missing plugin. Continue?"));
		if (answer != QMessageBox::Yes)
		{
			return;
		}
	}

	m_pi->unloadPlugin();
	updateLabels();
}

void PrestigeView::toggleEditor()
{
	if (m_editorWindow)
	{
		closeEditorWindow();
		if (m_pi) { m_pi->setEditorWanted(false); }
	}
	else
	{
		openEditorWindow(true);
	}
}

void PrestigeView::toggleParameters()
{
	if (m_paramsWindow && m_paramsWindow->isVisible())
	{
		m_paramsWindow->hide();
		if (m_pi) { m_pi->setParametersWindowWanted(false); }
	}
	else
	{
		openParametersWindow();
	}
}

void PrestigeView::onPluginAboutToClose()
{
	closeEditorWindow();

	// The models are still alive here (see PrestigeInstrument::
	// closePluginLocked()); drop every reference to them now.
	if (m_paramsWidget)
	{
		m_paramsWidget->clearParameters(tr("No plugin loaded."));
	}
}

void PrestigeView::onParameterModelsChanged()
{
	refreshParameterWindow();
}

void PrestigeView::onRestoreUi()
{
	if (!m_pi || !m_pi->isPluginLoaded())
	{
		return;
	}

	const bool wantEditor = m_pi->editorWanted();
	const bool wantParameters = m_pi->parametersWindowWanted();
	if (!wantEditor && !wantParameters)
	{
		return;
	}

	// Deferred: this can be reached from the middle of a project load, when
	// the main window is not necessarily ready to receive new sub-windows.
	QTimer::singleShot(0, this, [this, wantEditor, wantParameters]()
	{
		if (!m_pi || !m_pi->isPluginLoaded())
		{
			return;
		}
		if (wantEditor && !m_editorWindow)
		{
			openEditorWindow(false);
		}
		if (wantParameters && !(m_paramsWindow && m_paramsWindow->isVisible()))
		{
			openParametersWindow();
		}
	});
}

void PrestigeView::openEditorWindow(bool userInitiated)
{
	if (!m_pi || !m_pi->isPluginLoaded())
	{
		return;
	}

	// Already open: just bring it to the front.
	if (m_editorWindow)
	{
		m_editorWindow->show();
		m_editorWindow->raise();
		return;
	}

	// Step 1: ask the plugin for a view and its initial size.  A plugin with
	// no editor lands here, and that is a normal, non-error situation.
	int width = 0;
	int height = 0;
	QString error;
	const bool created = m_pi->createEditor(&width, &height,
		[this](int w, int h)
		{
			// Plugin asked to resize its editor (IPlugFrame::resizeView).
			if (m_editorHost) { m_editorHost->setFixedSize(toWidgetSize(m_editorHost, w, h)); }
			if (m_editorWindow) { m_editorWindow->adjustSize(); }
		},
		&error);

	if (!created)
	{
		m_pi->setEditorWanted(false);
		if (userInitiated)
		{
			QMessageBox::information(this, tr("PRESTIGE"),
				error.isEmpty() ? tr("Plugin editor is unavailable") : error);
		}
		return;
	}

	// Step 2: a native host widget sized to the plugin's view.  The plugin
	// attaches its own child window to this widget's native handle, so the
	// widget must be a real native window (WA_NativeWindow).
	auto* host = new QWidget;
	host->setAttribute(Qt::WA_NativeWindow);
	// WA_PaintOnScreen: tell Qt not to use its backing store for this widget.
	// Without it Qt composites its own backing-store paint over the embedded
	// native child window on every repaint, producing overdraw artefacts
	// (the "line glitches" seen when other LMMS windows overlap the editor).
	host->setAttribute(Qt::WA_PaintOnScreen);
	host->setFixedSize(toWidgetSize(host, width, height));

	// Same wrapper Vestige uses for its native editor windows.  Closing must
	// not delete the widget out from under the plugin, so DeleteOnClose is off
	// and we destroy the window ourselves, after the view is detached.
	SubWindow* window = getGUI()->mainWindow()->addWindowedWidget(host);
	window->setAttribute(Qt::WA_DeleteOnClose, false);
	window->setWindowTitle(m_pi->pluginName());

	m_editorHost = host;
	m_editorWindow = window;

	// Step 3: attach.  winId() forces creation of the native HWND (the call
	// to winId() below is what actually creates it).  On Windows we must also
	// set WS_CLIPCHILDREN on the host HWND and WS_CLIPSIBLINGS on the plugin's
	// own HWND so that GDI clips their painting to their respective window
	// rectangles and they do not overdraw sibling Qt widgets.  This is a
	// Windows-only requirement; the VST3 SDK's IPlugView contract is silent on
	// it, so it must be applied by the host.
	//
	// Order matters: winId() must be called first (it creates the HWND), then
	// SetWindowLongPtr can touch it, and only then is the handle passed to
	// attachEditor() so the plugin creates its child inside an already-clipping
	// parent.
	const WId hostWinId = host->winId();
#ifdef Q_OS_WIN
	{
		const HWND hwnd = reinterpret_cast<HWND>(hostWinId);
		LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
		SetWindowLongPtr(hwnd, GWL_STYLE, style | WS_CLIPCHILDREN | WS_CLIPSIBLINGS);
	}
#endif
	if (!m_pi->attachEditor(reinterpret_cast<void*>(hostWinId), &error))
	{
		closeEditorWindow();
		m_pi->setEditorWanted(false);
		if (userInitiated)
		{
			QMessageBox::warning(this, tr("VST3 editor"),
				error.isEmpty() ? tr("Plugin editor is unavailable") : error);
		}
		return;
	}

	// If the user closes the window with its own close button, detach the
	// plugin view first, and remember that the user closed it.
	window->installEventFilter(new EditorCloseFilter(window, [this]()
	{
		closeEditorWindow();
		if (m_pi) { m_pi->setEditorWanted(false); }
	}));

	window->show();
	m_editorButton->setText(tr("Hide editor"));
	m_pi->setEditorWanted(true);
}

void PrestigeView::closeEditorWindow()
{
	// Plugin view FIRST: it has to detach from (and destroy its child of) the
	// native parent window before that window is destroyed.  Closing the
	// editor never touches the processor or plugin state; reopening builds a
	// fresh view against the same controller.
	if (m_pi)
	{
		m_pi->closeEditor();
	}

	if (m_editorWindow)
	{
		// deleteLater(): this may be running inside the window's own close
		// event, so it must not be deleted synchronously.
		m_editorWindow->hide();
		m_editorWindow->deleteLater();
		m_editorWindow.clear();
	}
	m_editorHost.clear();

	if (m_editorButton)
	{
		m_editorButton->setText(tr("Show editor"));
	}
}

void PrestigeView::openParametersWindow()
{
	if (!m_pi || !m_pi->isPluginLoaded())
	{
		return;
	}

	if (!m_paramsWindow)
	{
		auto* widget = new Vst3ParameterWindow;

		// Same wrapper as the native editor window. It stays alive (hidden)
		// when the user closes it, so its search text and selection survive
		// closing and reopening; ~PrestigeView() destroys it.
		SubWindow* window = getGUI()->mainWindow()->addWindowedWidget(widget);
		window->setAttribute(Qt::WA_DeleteOnClose, false);
		window->installEventFilter(new EditorCloseFilter(window, [this]()
		{
			if (m_pi) { m_pi->setParametersWindowWanted(false); }
		}));

		m_paramsWidget = widget;
		m_paramsWindow = window;
		refreshParameterWindow();
	}

	m_paramsWindow->show();
	m_paramsWindow->raise();
	m_pi->setParametersWindowWanted(true);
}

void PrestigeView::refreshParameterWindow()
{
	if (!m_paramsWidget || !m_pi)
	{
		return;
	}

	std::vector<Vst3ParameterModel*> models;
	models.reserve(m_pi->parameterModels().size());
	for (const auto& model : m_pi->parameterModels())
	{
		models.push_back(model.get());
	}

	QString emptyMessage;
	if (!m_pi->isPluginLoaded())
	{
		emptyMessage = tr("No plugin loaded.");
	}
	else if (models.empty())
	{
		emptyMessage = tr("This plugin exposes no parameters.");
	}

	m_paramsWidget->setParameters(models, emptyMessage);

	if (m_paramsWindow)
	{
		const QString name = m_pi->pluginName();
		m_paramsWindow->setWindowTitle(name.isEmpty() ? tr("PRESTIGE - Parameters") : tr("%1 - Parameters").arg(name));
	}
}

void PrestigeView::updateLabels()
{
	if (!m_pi)
	{
		return;
	}

	const bool loaded = m_pi->isPluginLoaded();
	const bool hasSaved = m_pi->hasSavedPlugin();

	m_editorButton->setEnabled(loaded);
	m_paramsButton->setEnabled(loaded);
	m_unloadButton->setEnabled(loaded || hasSaved);

	const QString warning = m_pi->warning();
	m_warningLabel->setText(warning);
	m_warningLabel->setVisible(!warning.isEmpty());

	if (loaded)
	{
		m_nameLabel->setText(m_pi->pluginName());
		m_vendorLabel->setText(m_pi->pluginVendor());
		m_errorLabel->clear();
	}
	else if (hasSaved)
	{
		// A plugin is recorded in the project but is not running: name it,
		// say why, and say what is being kept.
		QString name = m_pi->savedPluginName();
		if (name.isEmpty())
		{
			name = QFileInfo(m_pi->bundlePath()).completeBaseName();
		}
		m_nameLabel->setText(tr("Plugin not loaded: %1").arg(name));
		m_vendorLabel->setText(m_pi->savedPluginVendor());

		QString message = m_pi->lastError();
		if (m_pi->status() == PrestigeInstrument::LoadStatus::Missing)
		{
			message += QLatin1Char('\n') + tr("Expected at: %1").arg(m_pi->bundlePath());
		}
		if (m_pi->hasRetainedState()
			&& m_pi->status() != PrestigeInstrument::LoadStatus::UnsupportedVersion)
		{
			message += QLatin1Char('\n')
				+ tr("Its saved settings are kept in this project. Use Browse... to locate it again.");
		}
		m_errorLabel->setText(message.trimmed());
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

extern "C"
{

Q_DECL_EXPORT lmms::Plugin* lmms_plugin_main(lmms::Model* m, void*)
{
	return new lmms::PrestigeInstrument(static_cast<lmms::InstrumentTrack*>(m));
}

}
