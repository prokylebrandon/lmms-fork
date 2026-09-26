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

#include "EffectControls.h"

namespace lmms
{

class Vst3Effect;

namespace gui { class Vst3EffectControlDialog; }

/**
 * BATCH 1 SCOPE: this is deliberately the minimal EffectControls needed to
 * make Vst3Effect a real, addable/reorderable/bypassable/removable/
 * savable member of an EffectChain. It does NOT yet expose the plugin's
 * own parameters as automatable LMMS models, and does NOT yet persist
 * plugin state (component/controller blobs). Both are batch 2 -- see the
 * continuation prompt. controlCount() is 0 until batch 2 adds per-
 * parameter models (mirroring, or promoting into Vst3Base, Prestige's
 * Vst3ParameterModel machinery -- see that decision flagged in the
 * continuation prompt).
 */
class Vst3EffectControls : public EffectControls
{
public:
	explicit Vst3EffectControls(Vst3Effect* effect);
	~Vst3EffectControls() override = default;

	void saveSettings(QDomDocument& doc, QDomElement& parent) override;
	void loadSettings(const QDomElement& elem) override;
	QString nodeName() const override { return "Vst3EffectControls"; }

	int controlCount() override { return 0; }
	gui::EffectControlDialog* createView() override;

	Vst3Effect* vst3Effect() const { return m_effect; }

private:
	Vst3Effect* m_effect;
};

} // namespace lmms

#endif // LMMS_VST3_EFFECT_CONTROLS_H
