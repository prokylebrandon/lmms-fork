/*
 * Vst3Instrument.h - implementation of VST3 instrument
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

#ifndef VST3_INSTRUMENT_H
#define VST3_INSTRUMENT_H

#include "Instrument.h"
#include "Vst3ControlBase.h"

namespace lmms
{


class Vst3Instrument : public Instrument, public Vst3ControlBase
{
	Q_OBJECT
public:
	Vst3Instrument(InstrumentTrack* instrumentTrackArg, const Descriptor::SubPluginFeatures::Key* key);
	~Vst3Instrument() override = default;

	void saveSettings(QDomDocument& doc, QDomElement& that) override
	{
		Vst3ControlBase::saveSettings(doc, that);
	}
	void loadSettings(const QDomElement& that) override
	{
		Vst3ControlBase::loadSettings(that);
	}
	QString nodeName() const override { return Vst3ControlBase::nodeName(); }

	bool hasNoteInput() const override { return Vst3ControlBase::hasNoteInput(); }
	bool handleMidiEvent(const MidiEvent& event, const TimePos& time = TimePos(), f_cnt_t offset = 0) override;
	void play(SampleFrame* buf) override;

	gui::PluginView* instantiateView(QWidget* parent) override;
};


} // namespace lmms

#endif // VST3_INSTRUMENT_H
