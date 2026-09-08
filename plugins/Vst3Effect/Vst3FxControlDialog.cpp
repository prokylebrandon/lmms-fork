/*
 * Vst3FxControlDialog.cpp - generic knob editor for VST3 effects
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

#include "Vst3FxControlDialog.h"

#include <cmath>

#include <QGridLayout>
#include <QGroupBox>
#include <QVBoxLayout>

#include "Knob.h"
#include "Vst3FxControls.h"

namespace lmms::gui
{


Vst3FxControlDialog::Vst3FxControlDialog(Vst3FxControls* controls) :
	EffectControlDialog(controls)
{
	auto mainLay = new QVBoxLayout(this);

	auto group = new QGroupBox(this);
	auto grid = new QGridLayout(group);

	const std::size_t count = controls->controlCount();
	const int cols = count > 0 ? std::max(1, static_cast<int>(std::sqrt(static_cast<double>(count)))) : 1;

	for (std::size_t i = 0; i < count; ++i)
	{
		auto knob = new Knob(KnobType::Bright26, controls->labelAt(i), group,
			Knob::LabelRendering::WidgetFont, controls->labelAt(i));
		knob->setModel(controls->modelAt(i));
		knob->setHintText(tr("Value:"), "");
		grid->addWidget(knob, static_cast<int>(i) / cols, static_cast<int>(i) % cols);
	}

	mainLay->addWidget(group);
}


} // namespace lmms::gui
