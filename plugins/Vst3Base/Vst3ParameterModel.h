/*
 * Vst3ParameterModel.h - bridges one VST3 parameter to LMMS's automation system
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

#ifndef LMMS_VST3_PARAMETER_MODEL_H
#define LMMS_VST3_PARAMETER_MODEL_H

#include <QString>

#include "vst3base_export.h"
#include "AutomatableModel.h"
#include "Vst3Parameter.h"

class QDomDocument;
class QDomElement;

namespace lmms
{

class Vst3PluginInstance;

/**
 * @brief Bridges one VST3 parameter to LMMS's automation system.
 *
 * One instance per VST3 parameter, created right after a plugin loads and
 * destroyed before it unloads (PrestigeInstrument::buildParameterModels()/
 * teardownParameterModels() on the instrument side; Vst3EffectControls'
 * equivalents on the effect side).
 *
 * Promoted here from plugins/Prestige in Phase 4 batch 2 (Task 2): it has
 * no instrument-specific dependencies -- it only ever touches
 * Vst3PluginInstance's generic parameter API (queueParameterChange(),
 * getParameterNormalized(), parameterDisplayString()) plus generic
 * QDomDocument/QDomElement save/load -- so both Prestige (instrument) and
 * Vst3Effect (effect) share this one implementation rather than each
 * keeping their own copy. This is the first thing in Vst3Base that
 * depends on LMMS's Model/AutomatableModel system; previously Vst3Base
 * had no LMMS-GUI dependencies at all. Costs nothing at the build-system
 * level -- BuildPlugin.cmake already links every plugin target, including
 * vst3base, against the lmms core library -- but it is a real widening of
 * Vst3Base's architectural surface, not just a file move, and Phase 5
 * should know that.
 *
 * Wraps a single FloatModel holding the parameter's normalised [0, 1]
 * value. LMMS automation, the generic parameter UI, and manual edits all
 * go through that FloatModel; nothing outside this class touches
 * Vst3PluginInstance's parameter API directly.
 *
 * Two directions, one guard against feeding back into each other:
 *
 *   LMMS -> plugin:  FloatModel::dataChanged() -> onModelChanged() ->
 *                     Vst3PluginInstance::queueParameterChange().
 *
 *   plugin -> LMMS:   the plugin's own editor calls IComponentHandler::
 *                     performEdit() -> Vst3PluginInstance::onControllerEdit()
 *                     -> the ParameterEditedCallback registered by the
 *                     owner (PrestigeInstrument::buildParameterModels() /
 *                     Vst3EffectControls' equivalent) -> setValueFromPlugin() ->
 *                     m_valueModel.setValue() with m_settingFromPlugin
 *                     held true, so the dataChanged() that setValue()
 *                     emits does not loop back into queueParameterChange()
 *                     and re-enter the same controller it just came from.
 *
 * (AutomatableModel::setValue() already no-ops when the fitted value is
 * unchanged, which covers the plugin echoing back the exact value it just
 * reported; the guard here additionally covers a plugin that echoes back a
 * slightly different value, e.g. from its own quantisation, which would
 * otherwise still round-trip once.)
 *
 * Identity is always Vst3ParamID (Vst3Parameter::id) -- never this model's
 * position in the owner's parameter list -- both at runtime and in
 * saveSettings()/loadSettings(), since a plugin update can reorder or drop
 * parameters between a project's save and its later load.
 *
 * Main/GUI thread only, like the rest of Vst3PluginInstance's non-
 * processAudio() surface.
 */
class VST3BASE_EXPORT Vst3ParameterModel : public Model
{
	Q_OBJECT
public:
	/**
	 * @param parent        Owning PrestigeInstrument or Vst3EffectControls.
	 *                      Model parent, per LMMS convention (see
	 *                      LadspaControl for the established precedent of
	 *                      one small Model-derived wrapper per
	 *                      dynamically-hosted-plugin control).
	 * @param plugin        Instance to forward LMMS-side edits to. Never
	 *                      null at construction; cleared by
	 *                      detachPlugin() before the plugin is destroyed.
	 * @param info          Static metadata, read once at construction.
	 * @param initialValue  Current normalised value, read by the caller
	 *                      from the controller (Vst3PluginInstance::
	 *                      getParameterNormalized()) so a restored /
	 *                      non-default plugin state is reflected
	 *                      immediately instead of snapping to
	 *                      info.defaultNormalisedValue.
	 */
	Vst3ParameterModel(Model* parent, Vst3PluginInstance* plugin,
		const Vst3Parameter& info, double initialValue);
	~Vst3ParameterModel() override = default;

	Vst3ParamID id() const { return m_info.id; }
	const Vst3Parameter& info() const { return m_info; }

	FloatModel* valueModel() { return &m_valueModel; }
	const FloatModel* valueModel() const { return &m_valueModel; }

	/**
	 * Formatted display string for the model's current value (e.g.
	 * "440.0 Hz"), via Vst3PluginInstance::parameterDisplayString().
	 * Empty once detachPlugin() has been called.
	 */
	QString formattedValue() const;

	/**
	 * Called (GUI thread) when the plugin's own editor changes this
	 * parameter. Sets m_valueModel without re-queuing the change back to
	 * the plugin -- see the class comment for why.
	 */
	void setValueFromPlugin(double normalisedValue);

	/**
	 * Called before the owning Vst3PluginInstance is destroyed (unload,
	 * replacement, or project close). After this, LMMS-side edits to
	 * valueModel() are still accepted (so a stray automation event or
	 * open parameter-UI widget doesn't crash) but are silently dropped
	 * instead of forwarded, since there is nothing left to forward them
	 * to.
	 */
	void detachPlugin() { m_plugin = nullptr; }

	/**
	 * Appends a <param id="..."> child to @p parametersElement, containing
	 * this model's own saved state (value, plus any automation/controller
	 * connection -- see AutomatableModel::saveSettings()). The connection
	 * is the reason this exists separately from Vst3PluginInstance::
	 * saveState(): the plugin's own state blob has no concept of an LMMS
	 * automation clip or a MIDI CC connection attached to one of its
	 * parameters, so without this, those connections would be silently
	 * dropped by every project save.
	 */
	void saveSettings(QDomDocument& doc, QDomElement& parametersElement);

	/**
	 * Restores from an already-located <param> element -- the caller
	 * matches by id() itself, since
	 * parameters can be reordered or dropped by a plugin update between a
	 * project's save and its later load.
	 *
	 * For an isProgramChange parameter, the saved numeric value is a
	 * snapshot of whatever program/preset index was active in a *previous*
	 * session -- it is not meaningful to re-apply on top of state that
	 * restoreState()/restoreComponentState() (called by the owner just
	 * before this) already restored from the plugin's own preset blob.
	 * Applying it anyway would silently overwrite a freshly-loaded preset
	 * with a stale program index and undo it with no warning (see
	 * applySavedElement()'s ordering comment and doc/prestige-vst3.md /
	 * PRESTIGE-Phase-4c's Task A). So this still restores the model's
	 * automation clip / controller connection -- the only place those are
	 * recorded at all -- but suppresses forwarding the resulting value to
	 * the plugin, the same way setValueFromPlugin() suppresses the
	 * opposite direction. An automation clip attached to a program-change
	 * parameter still drives the plugin during playback as normal; only
	 * the one-time "snap to last saved value" on project load is skipped.
	 */
	void loadSettings(const QDomElement& paramElement);

private slots:
	void onModelChanged();

private:
	Vst3PluginInstance* m_plugin; // not owned; cleared by detachPlugin()
	Vst3Parameter        m_info;
	FloatModel            m_valueModel;
	bool                  m_settingFromPlugin = false;
};

} // namespace lmms

#endif // LMMS_VST3_PARAMETER_MODEL_H
