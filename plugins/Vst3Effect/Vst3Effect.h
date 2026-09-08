/*
 * Vst3Effect.h - implementation of VST3 effect
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

#ifndef VST3_EFFECT_H
#define VST3_EFFECT_H

#include <vector>

#include "Effect.h"
#include "Vst3FxControls.h"

namespace lmms
{


class Vst3Effect : public Effect
{
	Q_OBJECT
public:
	Vst3Effect(Model* parent, const Descriptor::SubPluginFeatures::Key* key);

	ProcessStatus processImpl(SampleFrame* buf, const f_cnt_t frames) override;

	EffectControls* controls() override { return &m_controls; }
	Vst3FxControls* vst3Controls() { return &m_controls; }

private:
	Vst3FxControls m_controls;
	std::vector<SampleFrame> m_tmpOutputSmps;
};


} // namespace lmms

#endif // VST3_EFFECT_H
