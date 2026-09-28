/*
 * Vst3EffectControls.cpp - model for VST3 effect controls
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

#include "Vst3EffectControls.h"

#include <QDomDocument>
#include <QDomElement>
#include <QTextStream>

#include "Engine.h"
#include "Song.h"
#include "Vst3Effect.h"
#include "Vst3EffectControlDialog.h"
#include "Vst3PluginInstance.h"

namespace lmms
{

Vst3EffectControls::Vst3EffectControls(Vst3Effect* effect) :
	EffectControls(effect),
	m_effect(effect)
{
}

void Vst3EffectControls::buildParameterModels()
{
	Q_ASSERT(m_parameterModels.empty());
	Vst3PluginInstance* plugin = m_effect->pluginInstance();
	if (!plugin)
	{
		return;
	}

	const auto& params = plugin->parameters();
	m_parameterModels.reserve(params.size());
	for (const auto& info : params)
	{
		// getParameterNormalized() only returns nullopt for an id that
		// isn't in plugin->parameters() at all, which can't happen here
		// since info.id came from that same list -- the fallback is just
		// defensive, not expected to trigger.
		const double current = plugin->getParameterNormalized(info.id).value_or(info.defaultNormalisedValue);
		m_parameterModels.push_back(std::make_unique<Vst3ParameterModel>(this, plugin, info, current));
	}

	// Wire plugin -> host edits (the plugin's own editor moving a
	// parameter, once Task 3's native editor exists) to the matching
	// model. LMMS -> plugin is the reverse direction and is wired inside
	// each Vst3ParameterModel itself.
	plugin->setParameterEditedCallback(
		[this](Vst3ParamID id, double value) { onPluginParameterEdited(id, value); });
}

void Vst3EffectControls::teardownParameterModels()
{
	Vst3PluginInstance* plugin = m_effect->pluginInstance();
	if (plugin)
	{
		// Stop the plugin from being able to reach a model mid-teardown,
		// while the plugin instance is still valid enough to call this on.
		plugin->setParameterEditedCallback(nullptr);
	}

	// Tell every model its plugin is gone before destroying any of them --
	// detach-then-destroy, never the other order.
	for (auto& model : m_parameterModels)
	{
		model->detachPlugin();
	}
	m_parameterModels.clear();
}

void Vst3EffectControls::onPluginParameterEdited(Vst3ParamID id, double normalisedValue)
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

void Vst3EffectControls::retainElement(const QDomElement& element)
{
	m_retainedXml.clear();
	QTextStream stream(&m_retainedXml);
	element.save(stream, -1); // -1: no added whitespace, so text nodes (base64 state) round-trip exactly
}

void Vst3EffectControls::saveSettings(QDomDocument& doc, QDomElement& parent)
{
	// Plugin identity (bundle path + class UID) already round-trips
	// generically through the Effect key-attribute mechanism VstEffect
	// also relies on -- the same "file"/"uid" attributes
	// Vst3SubPluginFeatures::listSubPluginKeys() writes (see Task 0). This
	// class deliberately does NOT duplicate bundlepath/classcid/
	// pluginname/pluginvendor the way PrestigeInstrument::saveSettings()
	// does: Prestige (an Instrument) has no equivalent generic identity
	// round-trip, but an Effect always does. Writing identity twice here
	// would risk two sources of truth disagreeing after a plugin update
	// changes the class list.
	parent.setAttribute("version", kSaveVersion);

	Vst3PluginInstance* plugin = m_effect->pluginInstance();

	if (!plugin && !m_retainedXml.isEmpty())
	{
		// Missing/failed-to-load plugin: write back exactly what was
		// loaded, so opening and re-saving a project on a machine without
		// the plugin loses nothing. Mirrors PrestigeInstrument::
		// saveSettings()'s identical retained-XML branch.
		QDomDocument retained;
		if (retained.setContent(m_retainedXml))
		{
			const QDomElement root = retained.documentElement();

			const QDomNamedNodeMap attributes = root.attributes();
			for (int i = 0; i < attributes.length(); ++i)
			{
				const QDomAttr attribute = attributes.item(i).toAttr();
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
		// ourselves; fall through and save just the version above.
	}

	if (!plugin)
	{
		return;
	}

	// --- plugin-owned state ---------------------------------------------
	// Vst3PluginInstance::saveState() packs component and controller
	// state into ONE blob for the common case (hasSeparateControllerState()
	// is false). Where the plugin genuinely separates them, store the two
	// pieces under their own elements instead. The "format" attribute is
	// what a reader checks BEFORE calling .text() on <state> (see
	// applySavedElement()), so a "separate" blob is never misread as one
	// combined text node.
	QDomElement stateNode = doc.createElement("state");
	if (plugin->hasSeparateControllerState())
	{
		stateNode.setAttribute("format", "separate");

		const QByteArray compState = plugin->saveComponentState();
		QDomElement compNode = doc.createElement("component");
		compNode.appendChild(doc.createTextNode(QString::fromLatin1(compState.toBase64())));
		stateNode.appendChild(compNode);

		const QByteArray ctrlState = plugin->saveControllerState();
		QDomElement ctrlNode = doc.createElement("controller");
		ctrlNode.appendChild(doc.createTextNode(QString::fromLatin1(ctrlState.toBase64())));
		stateNode.appendChild(ctrlNode);
	}
	else
	{
		stateNode.setAttribute("format", "combined");
		const QByteArray state = plugin->saveState();
		stateNode.appendChild(doc.createTextNode(QString::fromLatin1(state.toBase64())));
	}
	parent.appendChild(stateNode);

	// --- host-side parameter/automation state ----------------------------
	// Separate from the plugin's own state blob above. The plugin's state
	// has no concept of an LMMS automation clip or MIDI CC connection
	// attached to one of its parameters, so without this block those
	// connections would be silently lost on every project save. Keyed by
	// VST3 parameter ID throughout, same as the identity attributes
	// handled generically elsewhere.
	QDomElement paramsNode = doc.createElement("parameters");
	for (auto& model : m_parameterModels)
	{
		model->saveSettings(doc, paramsNode);
	}
	parent.appendChild(paramsNode);
}

void Vst3EffectControls::applySavedElement(const QDomElement& element)
{
	Vst3PluginInstance* plugin = m_effect->pluginInstance();
	Q_ASSERT(plugin != nullptr);

	// Plugin-owned state.
	const QDomElement stateNode = element.firstChildElement("state");
	if (!stateNode.isNull())
	{
		// Check "format" BEFORE touching .text(): for a "separate" node,
		// .text() would concatenate the <component> and <controller>
		// child elements' base64 text into one garbled string
		// (QDomElement::text() walks every descendant text node).
		// Missing/"combined" is the plain single-text-node blob
		// restoreState() expects.
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
				restored = plugin->restoreComponentState(compState) && restored;
			}

			const QByteArray ctrlState = QByteArray::fromBase64(ctrlNode.text().toLatin1());
			if (!ctrlState.isEmpty())
			{
				anyState = true;
				// Only meaningful if this instance's controller is also
				// separate; a plugin that changed shape between save and
				// load simply has this saved piece silently unusable,
				// same treatment as any other saved data a different
				// plugin can't apply.
				restored = plugin->restoreControllerState(ctrlState) && restored;
			}
		}
		else
		{
			const QByteArray state = QByteArray::fromBase64(stateNode.text().toLatin1());
			if (!state.isEmpty())
			{
				anyState = true;
				restored = plugin->restoreState(state);
			}
		}

		if (anyState && !restored)
		{
			// The plugin stays loaded and usable, at whatever state
			// Vst3Effect::openPlugin() left it in; say so rather than
			// pretend.
			qWarning("Vst3Effect: state restore failed for %s", qPrintable(plugin->name()));
			Engine::getSong()->collectError(QObject::tr("Plugin state could not be restored."));
		}
	}

	// restoreState() above may have moved parameter values inside the
	// plugin (or, for a plugin that doesn't restore every parameter from
	// its state blob, left some at whatever loading initialised them to).
	// Refresh every model from the controller now, before applying the
	// saved <parameters> block below, so the two sources of truth can't
	// disagree and the UI can't drift from the plugin.
	for (auto& model : m_parameterModels)
	{
		const auto current = plugin->getParameterNormalized(model->id());
		if (current)
		{
			model->setValueFromPlugin(*current);
		}
	}

	// Saved automation/controller-connection state (see saveSettings())
	// takes precedence over the plugin's own restored values above, since
	// it's the only place an LMMS automation clip or MIDI CC connection on
	// a parameter is recorded at all. Matched by id, not position: a
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
}

void Vst3EffectControls::loadSettings(const QDomElement& elem)
{
	// Elements saved before Task 4 existed (batch 1's saveSettings() was a
	// no-op) carry no version attribute and no <state>/<parameters>
	// children -- treated as version 0, which naturally means "nothing to
	// restore" below rather than needing its own migration branch.
	const int version = elem.attribute("version", "0").toInt();

	if (version > kSaveVersion)
	{
		// Saved by a newer build than this one. Do not guess at a layout
		// that may have changed: keep the element exactly as found (it is
		// written back unchanged on the next save) and say so.
		qWarning("Vst3Effect: project saved with format version %d, this build understands up to %d",
			version, kSaveVersion);
		retainElement(elem);
		return;
	}

	Vst3PluginInstance* plugin = m_effect->pluginInstance();
	if (!plugin)
	{
		// Vst3Effect::openPlugin() already ran (by construction order --
		// see Vst3Effect's constructor) and failed, or the plugin is
		// otherwise unavailable. Keep this saved data so re-saving the
		// project doesn't lose it -- same treatment PrestigeInstrument
		// gives a missing plugin.
		retainElement(elem);
		return;
	}

	applySavedElement(elem);
}

gui::EffectControlDialog* Vst3EffectControls::createView()
{
	return new gui::Vst3EffectControlDialog(this);
}

} // namespace lmms
