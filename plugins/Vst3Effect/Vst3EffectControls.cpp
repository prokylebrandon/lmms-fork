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

#include "Vst3Effect.h"
#include "Vst3EffectControlDialog.h"

namespace lmms
{

Vst3EffectControls::Vst3EffectControls(Vst3Effect* effect) :
	EffectControls(effect),
	m_effect(effect)
{
}

void Vst3EffectControls::saveSettings(QDomDocument& doc, QDomElement& parent)
{
	Q_UNUSED(doc)
	Q_UNUSED(parent)

	// Plugin identity (bundle path + class UID) already round-trips
	// generically through the Effect key-attribute mechanism VstEffect
	// also relies on -- neither class overrides saveSettings for that
	// part.
	//
	// NOT YET PERSISTED (batch 2): per-parameter automation state and the
	// plugin's own component/controller state
	// (Vst3PluginInstance::saveState() / saveComponentState() /
	// saveControllerState()), following the versioned <state
	// format="combined|separate"> scheme documented in
	// doc/prestige-vst3.md's "Phase 3 Part 2 additions (for Phase 4)"
	// section. Until batch 2 lands, reloading a project will re-run
	// Vst3Effect::openPlugin() against the saved bundle/uid and get the
	// plugin's DEFAULT state, not whatever the user last dialed in.
}

void Vst3EffectControls::loadSettings(const QDomElement& elem)
{
	Q_UNUSED(elem)
}

gui::EffectControlDialog* Vst3EffectControls::createView()
{
	return new gui::Vst3EffectControlDialog(this);
}

} // namespace lmms
