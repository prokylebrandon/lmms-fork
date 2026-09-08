/*
 * Vst3Instrument.cpp - implementation of VST3 instrument
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

#include "Vst3Instrument.h"

#include <QDebug>

#include "AudioEngine.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiEvent.h"
#include "Vst3SubPluginFeatures.h"
#include "Vst3InsView.h"
#include "LmmsCommonMacros.h"

#include "plugin_export.h"

namespace lmms
{


extern "C"
{

Plugin::Descriptor PLUGIN_EXPORT vst3instrument_plugin_descriptor =
{
	LMMS_STRINGIFY(PLUGIN_NAME),
	"VST3",
	QT_TRANSLATE_NOOP("PluginBrowser", "plugin for hosting arbitrary VST3 instruments inside LMMS."),
	"LMMS VST3 experimental fork",
	0x0100,
	Plugin::Type::Instrument,
	nullptr, // logo: none embedded yet
	nullptr,
	new Vst3SubPluginFeatures(Plugin::Type::Instrument)
};

}


Vst3Instrument::Vst3Instrument(InstrumentTrack* instrumentTrackArg, const Descriptor::SubPluginFeatures::Key* key) :
	Instrument(instrumentTrackArg, &vst3instrument_plugin_descriptor, key, Flag::IsMidiBased),
	Vst3ControlBase(this, key->attributes["file"], key->attributes["classId"], /*isInstrument=*/true)
{
	if (!Vst3ControlBase::isValid())
	{
		qCritical() << "Vst3Instrument: failed to load" << key->attributes["file"] << ":" << Vst3ControlBase::errorString();
	}
}


bool Vst3Instrument::handleMidiEvent(const MidiEvent& event, const TimePos&, f_cnt_t)
{
	// NOTE: unlike Lv2Instrument, this does not route through a
	// thread-safe ring buffer first -- it queues directly onto the VST3
	// event list Vst3ControlBase::run() will send on the next audio-thread
	// call. Fine for a single-threaded Phase 2 proof; a real merge should
	// pick up Lv2ControlBase::handleMidiInputEvent's ring-buffer pattern
	// for a MIDI event arriving from a GUI/controller thread mid-block.
	switch (event.type())
	{
		case MidiNoteOn:
			if (event.velocity() > 0)
			{
				Vst3ControlBase::noteOn(static_cast<int16_t>(event.key()), event.velocity() / 127.0f, event.key());
			}
			else
			{
				Vst3ControlBase::noteOff(static_cast<int16_t>(event.key()), 0.f, event.key());
			}
			break;
		case MidiNoteOff:
			Vst3ControlBase::noteOff(static_cast<int16_t>(event.key()), event.velocity() / 127.0f, event.key());
			break;
		default:
			break;
	}
	return true;
}


void Vst3Instrument::play(SampleFrame* buf)
{
	if (!Vst3ControlBase::isValid()) { return; }

	Vst3ControlBase::copyModelsFromLmms();

	const f_cnt_t fpp = Engine::audioEngine()->framesPerPeriod();
	Vst3ControlBase::run(fpp);

	Vst3ControlBase::copyModelsToLmms();
	Vst3ControlBase::copyBuffersToLmms(buf, fpp);
}


gui::PluginView* Vst3Instrument::instantiateView(QWidget* parent)
{
	return new gui::Vst3InsView(this, parent);
}


extern "C"
{

PLUGIN_EXPORT Plugin* lmms_plugin_main(Model* parent, void* data)
{
	using KeyType = Plugin::Descriptor::SubPluginFeatures::Key;
	try
	{
		return new Vst3Instrument(static_cast<InstrumentTrack*>(parent), static_cast<const KeyType*>(data));
	}
	catch (const std::runtime_error& e)
	{
		qCritical() << e.what();
		return nullptr;
	}
}

}


} // namespace lmms
