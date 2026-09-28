/*
 * Vst3Effect.h - class for hosting VST3 effect plugins in LMMS's Effects
 *                section
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

#ifndef LMMS_VST3_EFFECT_H
#define LMMS_VST3_EFFECT_H

#include <memory>

#include <QMutex>

#include "Effect.h"
#include "Vst3EffectControls.h"
#include "Vst3PluginInstance.h"

namespace lmms
{

/**
 * Hosts exactly one VST3 effect class, loaded through Vst3Base's
 * Vst3PluginInstance -- the same host layer Prestige (the instrument,
 * Phases 1-3) uses. This class does not duplicate any VST3 hosting logic;
 * everything COM/SDK-facing lives in Vst3Base.
 *
 * Audio processing mirrors plugins/VstEffect/VstEffect.cpp's shape
 * deliberately: copy into a scratch buffer, process in place, then apply
 * LMMS's own wet/dry via Effect::wetLevel()/dryLevel(). Resolved in batch
 * 2 Task 2 (see Vst3EffectControls' class comment): LMMS's own wet/dry is
 * always applied, unconditionally, on top of whatever the plugin itself
 * does. A plugin's own internal mix/dry-wet parameter, if it has one, is
 * exposed like any other automatable parameter rather than detected and
 * suppressed -- VST3 has no standard flag for "this is a mix parameter,"
 * so there is no reliable way to single one out. A plugin with its own
 * mix knob will show up with two -- a known, documented redundancy, not a
 * bug.
 *
 * Unlike PrestigeInstrument, this class has no plugin-replacement path:
 * the plugin is loaded once, at construction, from the EffectKey it was
 * given, and never swapped for a different one in place (changing the
 * effect means removing and re-adding a chain entry). createEditor()/
 * attachEditor()/closeEditor() (Task 3, batch 2) forward directly to
 * m_instance with no locking, mirroring PrestigeInstrument's identical
 * choice -- both are GUI-thread-only calls, and construction/destruction
 * (the only things that ever replace m_instance) also only ever happen on
 * the GUI thread, so there is nothing for them to race against.
 */
class Vst3Effect : public Effect
{
public:
	Vst3Effect(Model* parent, const Descriptor::SubPluginFeatures::Key* key);
	~Vst3Effect() override;

	ProcessStatus processImpl(SampleFrame* buf, const f_cnt_t frames) override;

	EffectControls* controls() override
	{
		return &m_controls;
	}

	//! May return nullptr if the plugin failed to load (dontRun() will
	//! also be true in that case).
	Vst3PluginInstance* pluginInstance() const { return m_instance.get(); }

	//! Task 3 (batch 2): forwards to Vst3PluginInstance's native editor
	//! API (createEditor()/attachEditor()/closeEditor()). Mirrors
	//! PrestigeInstrument's identically-named methods exactly -- see the
	//! class comment for why no locking is needed here.
	bool createEditor(int* width, int* height,
		Vst3PluginInstance::EditorResizeCallback onResize, QString* error);
	bool attachEditor(void* nativeParent, QString* error);
	void closeEditor();

private:
	//! Resolves `uid` against the bundle's current class list (identity is
	//! always the class UID, never a positional/cached classIndex --
	//! see Vst3SubPluginFeatures.cpp's ASSUMPTION FLAG comment for the
	//! Vst3ClassInfo field names this depends on), loads and starts the
	//! plugin. Returns true iff it is ready to process audio.
	bool openPlugin(const QString& bundlePath, const QString& uid);

	//! Single teardown funnel -- called from the destructor today, and
	//! from any future reload/replace path batch 2 adds. Takes
	//! m_pluginMutex, the same lock processImpl() takes before touching
	//! m_instance, so teardown can never race a concurrent audio-thread
	//! process call. Mirrors PRESTIGE's closePluginLocked() pattern (see
	//! doc/prestige-vst3.md, "Crash-containment / concurrency patterns
	//! established"). Also tears down m_controls' parameter models
	//! (Task 2, batch 2) before m_instance itself goes away -- detach-
	//! then-destroy, same ordering PrestigeInstrument uses.
	void closePluginLocked();

	std::unique_ptr<Vst3PluginInstance> m_instance;
	QMutex m_pluginMutex;
	EffectKey m_key;

	Vst3EffectControls m_controls;
};

} // namespace lmms

#endif // LMMS_VST3_EFFECT_H
