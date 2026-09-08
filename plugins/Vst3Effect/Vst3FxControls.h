/*
 * Vst3FxControls.h - Vst3FxControls implementation
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

#ifndef LMMS_VST3_FX_CONTROLS_H
#define LMMS_VST3_FX_CONTROLS_H

#include "EffectControls.h"
#include "Vst3ControlBase.h"

namespace lmms
{


class Vst3Effect;

namespace gui
{
class Vst3FxControlDialog;
}


class Vst3FxControls : public EffectControls, public Vst3ControlBase
{
	Q_OBJECT
public:
	Vst3FxControls(Vst3Effect* effect, const QString& file, const QString& classId);

	void saveSettings(QDomDocument& doc, QDomElement& parent) override;
	void loadSettings(const QDomElement& that) override;
	inline QString nodeName() const override { return Vst3ControlBase::nodeName(); }

	int controlCount() override { return static_cast<int>(Vst3ControlBase::controlCount()); }
	gui::EffectControlDialog* createView() override;

private:
	friend class gui::Vst3FxControlDialog;
	friend class Vst3Effect;
};


} // namespace lmms

#endif // LMMS_VST3_FX_CONTROLS_H
