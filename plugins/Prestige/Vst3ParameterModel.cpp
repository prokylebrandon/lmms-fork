/*
 * Vst3ParameterModel.cpp - bridges one VST3 parameter to LMMS's automation system
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

#include "Vst3ParameterModel.h"

#include <QDomDocument>
#include <QDomElement>

#include "Vst3PluginInstance.h"

namespace lmms
{

namespace
{

// VST3's stepCount convention (see Vst3Parameter.h): 0 = continuous,
// N = N+1 discrete steps. A continuous parameter still gets a real step
// size rather than 0, so the knob/slider LMMS builds on top of this model
// (Phase 3's parameter UI) feels smooth without being literally
// unquantised -- the same reasoning LadspaControl applies to a LADSPA
// float port's range.
float stepSizeFor(const Vst3Parameter& info)
{
	return info.stepCount > 0 ? 1.0f / static_cast<float>(info.stepCount) : 0.001f;
}

} // namespace

Vst3ParameterModel::Vst3ParameterModel(Model* parent, Vst3PluginInstance* plugin,
	const Vst3Parameter& info, double initialValue) :
	Model(parent),
	m_plugin(plugin),
	m_info(info),
	m_valueModel(static_cast<float>(initialValue), 0.0f, 1.0f, stepSizeFor(info),
		this, info.title)
{
	m_valueModel.setInitValue(static_cast<float>(info.defaultNormalisedValue));

	// Read-only parameters (meters, mostly) still get a model, so the
	// parameter UI can display them, but onModelChanged() below refuses to
	// write them back into the plugin.
	connect(&m_valueModel, &FloatModel::dataChanged, this, &Vst3ParameterModel::onModelChanged);
}

QString Vst3ParameterModel::formattedValue() const
{
	if (!m_plugin)
	{
		return {};
	}
	return m_plugin->parameterDisplayString(m_info.id, m_valueModel.value());
}

void Vst3ParameterModel::setValueFromPlugin(double normalisedValue)
{
	m_settingFromPlugin = true;
	m_valueModel.setValue(static_cast<float>(normalisedValue));
	m_settingFromPlugin = false;
}

void Vst3ParameterModel::onModelChanged()
{
	// Two independent reasons this must not reach the plugin:
	//  - m_settingFromPlugin: the plugin's own editor is the origin of
	//    this value; forwarding it back would re-enter the controller
	//    from inside its own performEdit() (see
	//    Vst3PluginInstance::onControllerEdit()'s matching comment).
	//  - !m_plugin: the plugin has already been (or is being) torn down
	//    -- see detachPlugin() -- so there is nothing left to forward to.
	if (m_settingFromPlugin || !m_plugin)
	{
		return;
	}

	// Respect the plugin's own read-only flag: LMMS must never write to
	// a parameter the plugin has marked as host-unwritable (typically a
	// meter), even though it still gets a model so the UI can show it.
	if (m_info.isReadOnly)
	{
		return;
	}

	m_plugin->queueParameterChange(m_info.id, m_valueModel.value());
}

void Vst3ParameterModel::saveSettings(QDomDocument& doc, QDomElement& parametersElement)
{
	QDomElement paramElement = doc.createElement("param");
	// The VST3 parameter ID, not this model's position in
	// PrestigeInstrument's list -- positions shift across plugin
	// versions, IDs are the plugin's own stable identity (see
	// Vst3Parameter.h / Vst3PluginInstance's discoverClasses()/cid
	// handling for the same rule applied to class identity).
	paramElement.setAttribute("id", static_cast<qulonglong>(m_info.id));
	m_valueModel.saveSettings(doc, paramElement, "value");
	parametersElement.appendChild(paramElement);
}

void Vst3ParameterModel::loadSettings(const QDomElement& paramElement)
{
	m_valueModel.loadSettings(paramElement, "value");

	// NOT independently verified: whether PrestigeInstrument::
	// loadSettings() is guaranteed to run before the rest of the
	// project's AutomationClips resolve the journalling ids they point
	// at (the generic mechanism every other AutomatableModel in LMMS
	// relies on for automation to reattach after a project reload) was
	// not checked against ProjectJournal's id-remapping code this
	// session. If a MIDI CC / automation-clip connection on a PRESTIGE
	// parameter doesn't survive a save/reload round trip, this ordering
	// assumption -- not the value restore above, which doesn't depend on
	// it -- is the first thing to check. This is exactly the round-trip
	// case Phase 3's testing section calls out.
}

} // namespace lmms
