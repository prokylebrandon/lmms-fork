/*
 * Vst3InsView.h - generic knob editor for VST3 instruments
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

#ifndef VST3_INS_VIEW_H
#define VST3_INS_VIEW_H

#include "InstrumentView.h"

namespace lmms
{

class Vst3Instrument;

namespace gui
{

//! Phase 3 default: a plain auto-generated grid of knobs, one per VST3
//! parameter. See Vst3FxControlDialog for why this doesn't reuse
//! Lv2ViewBase/Lv2InsView (LinkedModelGroups mono-doubling machinery
//! Vst3ControlBase deliberately doesn't have). No background artwork --
//! same "generic, not styled" spirit as LV2's own editor.
class Vst3InsView : public InstrumentView
{
	Q_OBJECT
public:
	Vst3InsView(Vst3Instrument* instrument, QWidget* parent);
	~Vst3InsView() override = default;
};

} // namespace gui

} // namespace lmms

#endif // VST3_INS_VIEW_H
