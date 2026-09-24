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
 * One instance per VST3 parameter, created (PrestigeInstrument::
 * buildParameterModels()) right after a plugin loads and destroyed
 * (teardownParameterModels()) before it unloads. Deliberately lives here in
 * plugins/Prestige rather than in Vst3Base: Vst3Base stays free of any
 * dependency on LMMS's Model/AutomatableModel system, matching how it has
 * no other LMMS-GUI dependencies today. If Phase 4's VST3 effect ends up
 * needing the identical bridge, promoting this file (or duplicating the
 * ~80 lines of it) is a Phase 4 decision, not a Phase 3 one.
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
 *                     -> the ParameterEditedCallback registered in
 *                     buildParameterModels() -> setValueFromPlugin() ->
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
 * position in PrestigeInstrument's parameter list -- both at runtime and in
 * saveSettings()/loadSettings(), since a plugin update can reorder or drop
 * parameters between a project's save and its later load.
 *
 * Main/GUI thread only, like the rest of Vst3PluginInstance's non-
 * processAudio() surface.
 */
class Vst3ParameterModel : public Model
{
	Q_OBJECT
public:
	/**
	 * @param parent        Owning PrestigeInstrument. Model parent, per
	 *                      LMMS convention (see LadspaControl for the
	 *                      established precedent of one small Model-
	 *                      derived wrapper per dynamically-hosted-plugin
	 *                      control).
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
	 * replacement, or project close) -- see
	 * PrestigeInstrument::teardownParameterModels(). After this, LMMS-side
	 * edits to valueModel() are still accepted (so a stray automation
	 * event or open parameter-UI widget doesn't crash) but are silently
	 * dropped instead of forwarded, since there is nothing left to forward
	 * them to.
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
	 * (PrestigeInstrument::loadSettings()) matches by id() itself, since
	 * parameters can be reordered or dropped by a plugin update between a
	 * project's save and its later load.
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
