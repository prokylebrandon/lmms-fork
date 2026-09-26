/*
 * Vst3EffectControlDialog.h - dialog for displaying VST3 effect controls
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

#ifndef LMMS_GUI_VST3_EFFECT_CONTROL_DIALOG_H
#define LMMS_GUI_VST3_EFFECT_CONTROL_DIALOG_H

#include "EffectControlDialog.h"

class QLabel;

namespace lmms
{

class Vst3EffectControls;

namespace gui
{

/**
 * BATCH 1 SCOPE: confirms the plugin loaded and shows its identity. Does
 * NOT yet show per-parameter controls or a way to open the plugin's native
 * editor -- both need Vst3PluginInstance::createEditor()/attachEditor(),
 * hosted through a native-window-hosting widget. Prestige's instrument
 * side (Phase 2/3) almost certainly already built one of these; batch 2
 * should reuse it rather than duplicate it (see continuation prompt).
 *
 * Base class contract assumed here (EffectControlDialog.h was not
 * available when this was written): constructible from an EffectControls*,
 * itself a QWidget. Mirrors VstEffectControlDialog's constructor shape,
 * minus everything that file does that depends on VstPlugin-specific
 * embedding/preset machinery this class doesn't have yet.
 */
class Vst3EffectControlDialog : public EffectControlDialog
{
public:
	explicit Vst3EffectControlDialog(Vst3EffectControls* controls);
	~Vst3EffectControlDialog() override = default;

private:
	QLabel* m_infoLabel;
	QLabel* m_noteLabel;
};

} // namespace gui
} // namespace lmms

#endif // LMMS_GUI_VST3_EFFECT_CONTROL_DIALOG_H
