/*
 * Vst3InsView.cpp - generic knob editor for VST3 instruments
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

#include "Vst3InsView.h"

#include <cmath>

#include <QGridLayout>

#include "Knob.h"
#include "Vst3Instrument.h"

namespace lmms::gui
{


Vst3InsView::Vst3InsView(Vst3Instrument* instrument, QWidget* parent) :
	InstrumentView(instrument, parent)
{
	auto grid = new QGridLayout(this);

	const std::size_t count = instrument->controlCount();
	const int cols = count > 0 ? std::max(1, static_cast<int>(std::sqrt(static_cast<double>(count)))) : 1;

	for (std::size_t i = 0; i < count; ++i)
	{
		auto knob = new Knob(KnobType::Bright26, instrument->labelAt(i), this,
			Knob::LabelRendering::WidgetFont, instrument->labelAt(i));
		knob->setModel(instrument->modelAt(i));
		knob->setHintText(tr("Value:"), "");
		grid->addWidget(knob, static_cast<int>(i) / cols, static_cast<int>(i) % cols);
	}
}


} // namespace lmms::gui
