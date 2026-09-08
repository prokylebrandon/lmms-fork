/*
 * Vst3FxControls.cpp - Vst3FxControls implementation
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

#include "Vst3FxControls.h"

#include "Vst3Effect.h"
#include "Vst3FxControlDialog.h"

namespace lmms
{


Vst3FxControls::Vst3FxControls(Vst3Effect* effect, const QString& file, const QString& classId) :
	EffectControls(effect),
	Vst3ControlBase(this, file, classId, /*isInstrument=*/false)
{
}


void Vst3FxControls::saveSettings(QDomDocument& doc, QDomElement& parent)
{
	Vst3ControlBase::saveSettings(doc, parent);
}


void Vst3FxControls::loadSettings(const QDomElement& that)
{
	Vst3ControlBase::loadSettings(that);
}


gui::EffectControlDialog* Vst3FxControls::createView()
{
	return new gui::Vst3FxControlDialog(this);
}


} // namespace lmms
