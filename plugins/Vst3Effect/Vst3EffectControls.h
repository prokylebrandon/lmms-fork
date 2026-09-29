/*
 * Vst3EffectControls.h - model for VST3 effect controls
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

#ifndef LMMS_VST3_EFFECT_CONTROLS_H
#define LMMS_VST3_EFFECT_CONTROLS_H

#include <memory>
#include <vector>

#include <QString>

#include "EffectControls.h"
#include "Vst3ParameterModel.h"

namespace lmms
{

class Vst3Effect;
class Vst3PluginInstance;

namespace gui { class Vst3EffectControlDialog; }

/**
 * Task 2 (batch 2): builds one Vst3ParameterModel -- promoted into
 * Vst3Base in this same batch, see the handoff notes -- per plugin
 * parameter, mirroring PrestigeInstrument::buildParameterModels()/
 * teardownParameterModels() exactly. Unlike Prestige, Vst3Effect has no
 * plugin-replacement path: it loads its plugin once at construction and
 * tears it down once at destruction, so buildParameterModels()/
 * teardownParameterModels() are each called exactly once in this class's
 * lifetime, from Vst3Effect's constructor (after openPlugin() succeeds)
 * and from Vst3Effect::closePluginLocked() (before the plugin instance is
 * torn down), respectively.
 *
 * Firm rule (Task 2, documented per the continuation prompt's
 * requirement): every plugin parameter is exposed here, including any
 * mix/dry-wet control the plugin has of its own. VST3 has no standard
 * flag identifying a "mix" parameter (unlike isBypass, which IS
 * standardised), so nothing here attempts to detect or suppress one --
 * LMMS's own wet/dry (Effect::wetLevel()/dryLevel(), applied in
 * Vst3Effect::processImpl()) keeps applying unconditionally on top,
 * exactly as batch 1 already did. A plugin with its own internal mix knob
 * will show up with two mix controls -- not a bug, a known and documented
 * redundancy.
 *
 * Task 4 (batch 2): saveSettings()/loadSettings() persist the plugin's
 * own state blob and every parameter model's automation/controller
 * connection, following the same versioned <state format="combined|
 * separate">/<parameters> scheme PrestigeInstrument established (see
 * doc/prestige-vst3.md's "State-versioning scheme" section). Identity
 * (bundle path + class UID) is deliberately NOT duplicated here -- see
 * the .cpp's saveSettings() comment for why.
 */
class Vst3EffectControls : public EffectControls
{
	Q_OBJECT
public:
	explicit Vst3EffectControls(Vst3Effect* effect);
	~Vst3EffectControls() override = default;

	void saveSettings(QDomDocument& doc, QDomElement& parent) override;
	void loadSettings(const QDomElement& elem) override;
	QString nodeName() const override { return "Vst3EffectControls"; }

	int controlCount() override { return static_cast<int>(m_parameterModels.size()); }
	gui::EffectControlDialog* createView() override;

	Vst3Effect* vst3Effect() const { return m_effect; }

	//! Read-only view for the control dialog / parameter UI (Task 3).
	const std::vector<std::unique_ptr<Vst3ParameterModel>>& parameterModels() const
	{
		return m_parameterModels;
	}

	//! Called once, by Vst3Effect's constructor, right after openPlugin()
	//! succeeds. No-op if the plugin failed to load (vst3Effect()->
	//! pluginInstance() is nullptr).
	void buildParameterModels();

	//! Called once, by Vst3Effect::closePluginLocked(), BEFORE the plugin
	//! instance is reset. Detach-then-destroy: the plugin must stop being
	//! able to reach a model (via its edited-parameter callback) before
	//! either side is torn down -- mirrors PrestigeInstrument::
	//! teardownParameterModels()'s ordering exactly.
	//!
	//! PRESTIGE-Phase-4c Task B: also emits parameterModelsAboutToClear()
	//! FIRST, before any model is detached or destroyed, so
	//! Vst3ParameterGrid (owned by whatever Vst3EffectControlDialog this
	//! effect's createView() produced, if that dialog still exists -- its
	//! lifetime is independent of this object's, unlike
	//! PrestigeInstrument's 1:1 relationship with its own window) can
	//! clear every Knob it built before the FloatModel each one points at
	//! goes away. Without this, a dialog left open while its effect is
	//! removed from the chain would leave live Knob widgets pointing at
	//! freed Vst3ParameterModel/FloatModel objects -- the same hazard
	//! Vst3ParameterWindow::clearParameters()'s documented ordering
	//! requirement exists to prevent on the Prestige side, here wired
	//! through a signal instead of a direct call because, unlike
	//! PrestigeInstrument, Vst3EffectControls does not hold a pointer
	//! back to its dialog (createView() hands ownership to the caller --
	//! see EffectView.cpp -- and nothing currently threads a reference
	//! back). NOT independently exercised against the actual
	//! effect-removed-while-dialog-open sequence in this session (that
	//! would need EffectChain/EffectChainView, not part of the uploaded
	//! files) -- the connection is made in
	//! Vst3EffectControlDialog's constructor, so it only protects a
	//! dialog that was actually open (i.e. already connected) at the
	//! moment of teardown; flagged as a manual test in the handoff note.
	void teardownParameterModels();

private:
	//! Plugin -> host edits (the plugin's own editor moving a parameter,
	//! once Task 3 adds the native editor). Mirrors
	//! PrestigeInstrument::onPluginParameterEdited() exactly.
	void onPluginParameterEdited(Vst3ParamID id, double normalisedValue);

	//! Task 4: shared by saveSettings() (the plugin-owned <state> node
	//! plus the host-side <parameters> automation block) and
	//! loadSettings() (restoring both, in that order, then reconciling).
	//! Mirrors PrestigeInstrument::applySavedElement() exactly.
	void applySavedElement(const QDomElement& element);

	//! Task 4: records @p element verbatim (text form) so a missing/
	//! failed-to-load plugin's saved state survives an unrelated re-save
	//! of the project untouched. Mirrors PrestigeInstrument::
	//! retainElement() exactly.
	void retainElement(const QDomElement& element);

	//! Bumped whenever the <state>/<parameters> layout below changes
	//! shape in a way an older reader would misparse -- see
	//! saveSettings()'s "format" attribute comment for why "combined" vs
	//! "separate" specifically needed this rather than being silently
	//! backward-compatible.
	static constexpr int kSaveVersion = 1;

	Vst3Effect* m_effect;
	std::vector<std::unique_ptr<Vst3ParameterModel>> m_parameterModels;

	//! Task 4: verbatim text of the last saved <effect> child element,
	//! kept only when the plugin is (or becomes) unavailable. Empty
	//! otherwise.
	QString m_retainedXml;

signals:
	//! See teardownParameterModels()'s doc comment. Emitted before any
	//! model is detached or destroyed.
	void parameterModelsAboutToClear();
};

} // namespace lmms

#endif // LMMS_VST3_EFFECT_CONTROLS_H
