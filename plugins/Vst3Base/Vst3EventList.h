/*
 * Vst3EventList.h - IEventList adapter for MIDI→VST3 event conversion
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

#ifndef LMMS_VST3_EVENT_LIST_H
#define LMMS_VST3_EVENT_LIST_H

#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/vsttypes.h>
#include "MidiEvent.h"

#include <vector>
#include <cmath>

namespace lmms
{

/**
 * @brief Stack-allocated IEventList that converts LMMS MidiEvents to VST3 Events.
 *
 * Holds events for a single processing block.  Call clear() between blocks.
 * addMidiEvent() translates a LMMS MidiEvent to its VST3 Vst::Event equivalent.
 *
 * Covered event types:
 *   NoteOn, NoteOff, PitchBend, ControllerChange, ProgramChange.
 *
 * Sample-accurate offsets are preserved — sampleOffset is passed directly
 * into Vst::Event::sampleOffset so events land at the correct sample within
 * the current block, not collapsed to the start.
 *
 * No heap allocation after construction (events vector is pre-reserved).
 */
class Vst3EventList final : public Steinberg::Vst::IEventList
{
public:
    static constexpr int kMaxEvents = 256;

    Vst3EventList()
    {
        m_events.reserve(kMaxEvents);
    }

    void clear() { m_events.clear(); }

    /**
     * Convert @p midiEvent to a VST3 event and append it to the list.
     * @p sampleOffset is the position within the current processing block.
     */
    void addMidiEvent(const MidiEvent& midiEvent, Steinberg::int32 sampleOffset)
    {
        if (static_cast<int>(m_events.size()) >= kMaxEvents)
            return;

        Steinberg::Vst::Event e{};
        e.busIndex     = 0;
        e.sampleOffset = sampleOffset;
        e.ppqPosition  = 0.0; // filled by host context if available
        e.flags        = Steinberg::Vst::Event::kIsLive;

        switch (midiEvent.type())
        {
        case MidiNoteOn:
            if (midiEvent.velocity() == 0)
            {
                // velocity-0 NoteOn is treated as NoteOff by convention
                e.type               = Steinberg::Vst::Event::kNoteOffEvent;
                e.noteOff.channel    = midiEvent.channel();
                e.noteOff.pitch      = static_cast<Steinberg::int16>(midiEvent.key());
                e.noteOff.velocity   = 0.0f;
                e.noteOff.noteId     = -1;
                e.noteOff.tuning     = 0.0f;
            }
            else
            {
                e.type             = Steinberg::Vst::Event::kNoteOnEvent;
                e.noteOn.channel   = midiEvent.channel();
                e.noteOn.pitch     = static_cast<Steinberg::int16>(midiEvent.key());
                // Normalise velocity: MIDI [1,127] → VST3 (0,1]
                e.noteOn.velocity  = static_cast<float>(midiEvent.velocity()) / 127.0f;
                e.noteOn.length    = 0;  // unknown at note-on time
                e.noteOn.noteId    = -1; // host assigns noteId; -1 = unassigned
                e.noteOn.tuning    = 0.0f;
            }
            m_events.push_back(e);
            break;

        case MidiNoteOff:
            e.type               = Steinberg::Vst::Event::kNoteOffEvent;
            e.noteOff.channel    = midiEvent.channel();
            e.noteOff.pitch      = static_cast<Steinberg::int16>(midiEvent.key());
            e.noteOff.velocity   = static_cast<float>(midiEvent.velocity()) / 127.0f;
            e.noteOff.noteId     = -1;
            e.noteOff.tuning     = 0.0f;
            m_events.push_back(e);
            break;

        case MidiPitchBend:
        {
            // LMMS stores pitch bend as a 14-bit signed value in param(0).
            // VST3 expects a PolyPressure or ParameterChange — use a
            // PolyPressure-like LegacyMIDICCOut event so the plugin receives
            // it correctly regardless of its parameter mapping.
            // For maximum compatibility, emit as a kLegacyMIDICCOutEvent.
            e.type = Steinberg::Vst::Event::kLegacyMIDICCOutEvent;
            e.midiCCOut.channel    = midiEvent.channel();
            e.midiCCOut.controlNumber = Steinberg::Vst::kPitchBend;
            // Split 14-bit value into LSB/MSB
            const uint16_t raw = static_cast<uint16_t>(midiEvent.pitchBend() + 0x2000);
            e.midiCCOut.value  = raw & 0x7F;
            e.midiCCOut.value2 = (raw >> 7) & 0x7F;
            m_events.push_back(e);
            break;
        }

        case MidiControlChange:
            e.type = Steinberg::Vst::Event::kLegacyMIDICCOutEvent;
            e.midiCCOut.channel       = midiEvent.channel();
            e.midiCCOut.controlNumber = midiEvent.controllerNumber();
            e.midiCCOut.value         = midiEvent.controllerValue();
            e.midiCCOut.value2        = 0;
            m_events.push_back(e);
            break;

        case MidiProgramChange:
            e.type = Steinberg::Vst::Event::kLegacyMIDICCOutEvent;
            e.midiCCOut.channel       = midiEvent.channel();
            e.midiCCOut.controlNumber = Steinberg::Vst::kCtrlProgramChange;
            e.midiCCOut.value         = static_cast<Steinberg::uint8>(midiEvent.param(0) & 0x7F);
            e.midiCCOut.value2        = 0;
            m_events.push_back(e);
            break;

        default:
            // Other event types (SysEx, Active Sensing, etc.) are silently
            // dropped — VST3 has no equivalent for most of them.
            break;
        }
    }

    // ------------------------------------------------------------------
    // IEventList
    // ------------------------------------------------------------------

    Steinberg::int32 PLUGIN_API getEventCount() override
    {
        return static_cast<Steinberg::int32>(m_events.size());
    }

    Steinberg::tresult PLUGIN_API getEvent(Steinberg::int32 index,
                                            Steinberg::Vst::Event& e) override
    {
        if (index < 0 || index >= static_cast<Steinberg::int32>(m_events.size()))
            return Steinberg::kInvalidArgument;
        e = m_events[static_cast<std::size_t>(index)];
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API addEvent(Steinberg::Vst::Event& e) override
    {
        if (static_cast<int>(m_events.size()) >= kMaxEvents)
            return Steinberg::kResultFalse;
        m_events.push_back(e);
        return Steinberg::kResultOk;
    }

    // No-op reference counting — stack/scope owned.
    Steinberg::uint32  PLUGIN_API addRef() override  { return 1; }
    Steinberg::uint32  PLUGIN_API release() override { return 1; }
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID /*iid*/,
                                                  void** /*obj*/) override
    {
        return Steinberg::kNoInterface;
    }

private:
    std::vector<Steinberg::Vst::Event> m_events;
};

} // namespace lmms

#endif // LMMS_VST3_EVENT_LIST_H
