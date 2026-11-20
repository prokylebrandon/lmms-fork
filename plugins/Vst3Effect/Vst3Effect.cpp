/*
 * Vst3Effect.cpp - class for hosting VST3 effect plugins in LMMS's
 *                  Effects section
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

#include "Vst3Effect.h"
#include "AudioFileDevice.h"

#include <cassert>
#include <cstring>

#include "Song.h"
#include "Vst3SubPluginFeatures.h"
#include "Vst3Types.h"

#include "embed.h"
#include "plugin_export.h"

namespace lmms
{

extern "C"
{

Plugin::Descriptor PLUGIN_EXPORT vst3effect_plugin_descriptor =
{
	LMMS_STRINGIFY(PLUGIN_NAME),
	"VST3",
	QT_TRANSLATE_NOOP("PluginBrowser",
				"plugin for hosting arbitrary VST3 effects inside LMMS."),
	"LMMS contributors",
	0x0100,
	Plugin::Type::Effect,
	new PluginPixmapLoader("logo"),
	nullptr,
	new Vst3SubPluginFeatures(Plugin::Type::Effect)
};

}


//! Looks for the bundle holding the effect class `uid` in the configured VST3 folder
//! and the standard VST3 folders. Empty if it is nowhere to be found.
static QString findBundleForClass(const QString& uid)
{
	if (uid.isEmpty()) { return QString(); }

	Vst3SubPluginFeatures features(Plugin::Type::Effect);
	Plugin::Descriptor::SubPluginFeatures::KeyList keys;
	features.listSubPluginKeys(&vst3effect_plugin_descriptor, keys);
	for (const auto& k : keys)
	{
		if (k.attributes.value("uid") == uid) { return k.attributes.value("file"); }
	}
	return QString();
}


Vst3Effect::Vst3Effect(Model* parent, const Descriptor::SubPluginFeatures::Key* key) :
	Effect(&vst3effect_plugin_descriptor, parent, key),
	m_pluginMutex(),
	m_key(*key),
	m_controls(this)
{
	const QString uid = m_key.attributes["uid"];
	bool loaded = openPlugin(m_key.attributes["file"], uid, false);
	if (!loaded && m_classMissing)
	{
		// The saved location is gone (or no longer holds this class): look for the plugin
		// elsewhere. Only runs for a missing plugin, never for a normal load.
		const QString found = findBundleForClass(uid);
		// Not found: this second call reports the original "not found" error to the user
		loaded = openPlugin(found.isEmpty() ? m_key.attributes["file"] : found, uid, true);
	}
	if (loaded)
	{
		// Task 2 (batch 2): build the per-parameter automatable models now
		// that the plugin instance exists. Mirrors PrestigeInstrument's
		// constructor -> instantiatePlugin() -> buildParameterModels()
		// ordering, collapsed into one constructor since Vst3Effect
		// (unlike Prestige) has no separate load-a-different-plugin path.
		m_controls.buildParameterModels();
	}
	setDisplayName(m_key.name);
	setDontRun(!loaded);

	// Follow the audio engine's sample rate / block size (device change, export at another rate).
	connect(Engine::audioEngine(), &AudioEngine::sampleRateChanged, this, [this]()
	{
	    QMutexLocker ml(&m_pluginMutex);
	    if (!m_instance) { return; }
	    QString err;
	    if (!m_instance->reconfigure(Engine::audioEngine()->outputSampleRate(), Engine::audioEngine()->framesPerPeriod(), &err, dynamic_cast<AudioFileDevice*>(Engine::audioEngine()->audioDev()) != nullptr))
	    {
	        qWarning("Vst3Effect: could not apply the new sample rate / block size: %s", qPrintable(err));
	    }
	});
}


Vst3Effect::~Vst3Effect()
{
	closePluginLocked();
}


bool Vst3Effect::openPlugin(const QString& bundlePath, const QString& uid, bool reportMissing)
{
	// Identity is the class UID, never a positional classIndex carried
	// over between sessions: a bundle whose classes have been reordered
	// by a plugin update still resolves correctly here, and a bundle that
	// no longer contains this UID fails loudly instead of silently
	// loading a different class. Mirrors PRESTIGE's state-versioning rule
	// (doc/prestige-vst3.md, "Rules a Phase 4 reader/writer should copy").
	QString discoverError;
	const std::vector<Vst3ClassInfo> classes =
		Vst3PluginInstance::discoverClasses(bundlePath, &discoverError);

	int classIndex = -1;
	QString className;
	for (const Vst3ClassInfo& info : classes)
	{
		// Task 0 (batch 2) resolution, verified against the real
		// Vst3Types.h: the class UID field is named `cid` (QString),
		// not `uid` as batch 1 assumed -- fixed below. classIndex/name/
		// vendor/category/isInstrument were all guessed correctly in
		// batch 1 and needed no changes. `cid` is already a plain
		// QString, so no separate hex-encoding step is needed to carry
		// it into this class's `uid` parameter or the AttributeMap
		// below.
		if (info.cid == uid)
		{
			classIndex = info.classIndex;
			className = info.name;
			break;
		}
	}

	m_classMissing = classIndex < 0;
	if (classIndex < 0 && !reportMissing)
	{
		return false;
	}

	if (classIndex < 0)
	{
		collectErrorForUI(uid.isEmpty()
			? tr("The VST3 bundle %1 could not be read: %2").arg(bundlePath, discoverError)
			: tr("The VST3 effect class %1 was not found in %2.").arg(uid, bundlePath));
		return false;
	}

	QMutexLocker ml(&m_pluginMutex);

	auto result = Vst3PluginInstance::load(bundlePath, classIndex,
		Engine::audioEngine()->outputSampleRate(),
		Engine::audioEngine()->framesPerPeriod());
	if (!result)
	{
		collectErrorForUI(result.error);
		return false;
	}

	m_instance = std::move(result.instance);

	QString startError;
	if (!m_instance->startProcessing(&startError))
	{
		collectErrorForUI(startError.isEmpty()
			? tr("The VST3 effect %1 failed to start processing.").arg(bundlePath)
			: startError);
		m_instance.reset();
		return false;
	}

	m_key.attributes["file"] = bundlePath;
	m_key.attributes["uid"] = uid;

	// A key loaded from a project has no name, so take it from the plugin itself
	if (m_key.name.isEmpty() && !className.isEmpty()) { m_key.name = className; }

	return true;
}


bool Vst3Effect::locate()
{
	if (m_instance) { return true; }

	// Search first, outside the engine lock: this opens every bundle it scans
	const QString uid = m_key.attributes["uid"];
	const QString found = findBundleForClass(uid);
	if (found.isEmpty()) { return false; }

	// The effect is live in the chain: keep the audio thread out while the plugin comes up
	Engine::audioEngine()->requestChangeInModel();
	const bool loaded = openPlugin(found, uid, true);
	if (loaded)
	{
		m_controls.buildParameterModels();
		m_controls.restoreRetained();
		setDontRun(false);
	}
	Engine::audioEngine()->doneChangeInModel();
	return loaded;
}


void Vst3Effect::closePluginLocked()
{
	QMutexLocker ml(&m_pluginMutex);
	if (m_instance)
	{
		// Detach-then-destroy (Task 2, batch 2): the parameter models, and
		// the plugin's edited-parameter callback pointing back at them,
		// must be torn down before m_instance itself goes away -- mirrors
		// PrestigeInstrument::closePluginLocked()'s ordering exactly (see
		// Vst3EffectControls::teardownParameterModels()'s own comment).
		m_controls.teardownParameterModels();
		m_instance->stopProcessing();
		m_instance.reset();
	}
}


bool Vst3Effect::createEditor(int* width, int* height,
	Vst3PluginInstance::EditorResizeCallback onResize, QString* error)
{
	if (!m_instance)
	{
		if (error) { *error = tr("No plugin loaded"); }
		return false;
	}
	return m_instance->createEditor(width, height, std::move(onResize), error);
}


bool Vst3Effect::attachEditor(void* nativeParent, QString* error)
{
	if (!m_instance)
	{
		if (error) { *error = tr("No plugin loaded"); }
		return false;
	}
	return m_instance->attachEditor(nativeParent, error);
}


void Vst3Effect::closeEditor()
{
	if (m_instance)
	{
		m_instance->closeEditor();
	}
}


Effect::ProcessStatus Vst3Effect::processImpl(SampleFrame* buf, const f_cnt_t frames)
{
	assert(m_instance != nullptr);
	static thread_local auto tempBuf = std::array<SampleFrame, MAXIMUM_BUFFER_SIZE>();

	std::memcpy(tempBuf.data(), buf, sizeof(SampleFrame) * frames);

	// Same try-lock-except-while-exporting pattern as VstEffect: never
	// skip audio during a bounce (block indefinitely), but never let the
	// realtime audio thread block on a reload/teardown in progress during
	// normal playback either (skip this block's processing rather than
	// stall -- the dry signal still passes through via the wet/dry mix
	// below, it just doesn't get the wet contribution for that one
	// block).
	if (m_pluginMutex.tryLock(Engine::getSong()->isExporting() ? -1 : 0))
	{
		// Tempo / transport for the plugin's ProcessContext.
		auto* song = Engine::getSong();
		auto& timeline = song->getTimeline();
		auto& timeSig = song->getTimeSigModel();
		const auto framesPerTick = static_cast<double>(Engine::framesPerTick());
		const double ticks = static_cast<double>(timeline.ticks()) + (framesPerTick > 0.0 ? timeline.frameOffset() / framesPerTick : 0.0);
		m_instance->setTransport(static_cast<double>(song->getTempo()), song->isPlaying() || song->isExporting(),
			ticks / (DefaultTicksPerBar / 4.0), timeSig.getNumerator(), timeSig.getDenominator());
		m_instance->processAudio(reinterpret_cast<const float*>(tempBuf.data()),
			reinterpret_cast<float*>(tempBuf.data()), frames);
		m_pluginMutex.unlock();
	}

	// LMMS's own wet/dry only -- see the class-level doc comment on the
	// resolved plugin-own-mix-control decision (Task 2, batch 2).
	const float w = wetLevel();
	const float d = dryLevel();
	for (f_cnt_t f = 0; f < frames; ++f)
	{
		buf[f][0] = w * tempBuf[f][0] + d * buf[f][0];
		buf[f][1] = w * tempBuf[f][1] + d * buf[f][1];
	}

	return ProcessStatus::ContinueIfNotQuiet;
}


// processBypassedImpl() is deliberately NOT overridden: the base Effect's
// default (a no-op, leaving buf untouched) is a correct pure bypass, and
// is the same choice VstEffect.cpp makes for the identical reason -- see
// Effect::processAudioBuffer's dispatch between processImpl/
// processBypassedImpl, which already keeps this out of processImpl via a
// flag check, matching the Phase 4 spec's explicit instruction not to
// re-implement that branch here.
//
// NOT done yet (batch 2 candidate, not required by the spec): actually
// deactivating the VST3 processor (stopProcessing()/startProcessing())
// while bypassed for real CPU savings, beyond just skipping the
// processAudio() call. Deferred deliberately -- stopProcessing() is a
// main-thread call that must not race a concurrent processAudio(), and
// wiring that safely through onEnabledChanged() needs its own careful
// pass rather than being bolted on here.


extern "C"
{

// necessary for getting instance out of shared lib
PLUGIN_EXPORT Plugin* lmms_plugin_main(Model* parent, void* data)
{
	return new Vst3Effect(parent,
		static_cast<const Plugin::Descriptor::SubPluginFeatures::Key*>(data));
}

}


} // namespace lmms
