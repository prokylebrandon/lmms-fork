/*
 * Vst3FxControlDialog.h - generic knob editor for VST3 effects
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

#ifndef VST3_FX_CONTROL_DIALOG_H
#define VST3_FX_CONTROL_DIALOG_H

#include "EffectControlDialog.h"

namespace lmms
{

class Vst3FxControls;

namespace gui
{

//! Phase 3 default: a plain auto-generated grid of knobs, one per VST3
//! parameter -- same spirit as LadspaControlDialog, not Lv2ViewBase
//! (which is built around LinkedModelGroups for LV2's mono-doubling,
//! a design Vst3ControlBase deliberately doesn't use -- see Phase 2
//! report). Native VST3 editor embedding is the Phase 3 stretch goal,
//! not implemented here.
class Vst3FxControlDialog : public EffectControlDialog
{
	Q_OBJECT
public:
	explicit Vst3FxControlDialog(Vst3FxControls* controls);
	~Vst3FxControlDialog() override = default;
};

} // namespace gui

} // namespace lmms

#endif // VST3_FX_CONTROL_DIALOG_H
