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
#include <pluginterfaces/vst/ivstmidicontrollers.h>
#include <pluginterfaces/vst/ivstnoteexpression.h>
#include <pluginterfaces/vst/vsttypes.h>
#include "MidiEvent.h"

#include <vector>
#include <algorithm>
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
 * No heap allocation for the common case (events vector is pre-reserved to
 * kReserveEvents). Past that, it grows like any std::vector rather than
 * refusing new events: an earlier version capped the list at kMaxEvents and
 * silently discarded anything past it, which drops real, distinct notes
 * rather than merely costing an occasional reallocation. That cap turned
 * out to be reachable in practice -- a Song Editor loop region shorter than
 * one audio period causes Song::processNextBuffer() to walk the loop
 * multiple times within a single period, and a run of periods where
 * PrestigeInstrument::play() couldn't take m_pluginMutex (see its own
 * comment) lets several periods' worth of already-queued notes land in one
 * drain -- either of which can push a block's event count well past what
 * kMaxEvents assumed was a safe ceiling. There is no upper bound the host
 * can safely assume here, so there is no cap.
 */
class Vst3EventList final : public Steinberg::Vst::IEventList
{
public:
    static constexpr int kReserveEvents = 256; // sized for the common case; NOT a hard limit -- see class comment.

    Vst3EventList()
    {
        m_events.reserve(kReserveEvents);
    }

    void clear() { m_events.clear(); }

    /**
     * Convert @p midiEvent to a VST3 event and append it to the list.
     * @p sampleOffset is the position within the current processing block.
     */
    void addMidiEvent(const MidiEvent& midiEvent, Steinberg::int32 sampleOffset)
    {
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
                e.noteOff.noteId     = static_cast<Steinberg::int32>(midiEvent.key());
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
                e.noteOn.noteId    = static_cast<Steinberg::int32>(midiEvent.key()); // VST3 noteId = MIDI key (unique: LMMS runs one note per key)
                e.noteOn.tuning    = 0.0f;
            }
            m_events.push_back(e);
            break;

        case MidiNoteOff:
            e.type               = Steinberg::Vst::Event::kNoteOffEvent;
            e.noteOff.channel    = midiEvent.channel();
            e.noteOff.pitch      = static_cast<Steinberg::int16>(midiEvent.key());
            e.noteOff.velocity   = static_cast<float>(midiEvent.velocity()) / 127.0f;
            e.noteOff.noteId     = static_cast<Steinberg::int32>(midiEvent.key());
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
            const uint16_t raw = static_cast<uint16_t>(midiEvent.pitchBend() & 0x3FFF);
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

    /**
     * Append a tuning note-expression event (per-note pitch bend) for the note started with
     * @p noteId (this class uses the MIDI key as noteId). @p semitones is clamped to +-120 and
     * normalised as VST3 defines tuning: 0 = -120, 0.5 = 0, 1 = +120 semitones.
     */
    void addNoteTuning(Steinberg::int32 noteId, float semitones, Steinberg::int32 sampleOffset)
    {
        Steinberg::Vst::Event e{};
        e.busIndex     = 0;
        e.sampleOffset = sampleOffset;
        e.ppqPosition  = 0.0;
        e.flags        = Steinberg::Vst::Event::kIsLive;
        e.type         = Steinberg::Vst::Event::kNoteExpressionValueEvent;
        e.noteExpressionValue.typeId = Steinberg::Vst::kTuningTypeID;
        e.noteExpressionValue.noteId = noteId;
        const float st = std::clamp(semitones, -120.0f, 120.0f);
        e.noteExpressionValue.value  = (st + 120.0f) / 240.0f;
        m_events.push_back(e);
    }

    /// Sample offset of the most recent NoteOn for noteId in this block, or 0 when the note did not start in this block (a bend for a note that is already sounding applies from the start of the block).
    Steinberg::int32 noteOnOffset(Steinberg::int32 noteId) const
    {
        for (std::size_t i = m_events.size(); i-- > 0;)
        {
            const auto& ev = m_events[i];
            if (ev.type == Steinberg::Vst::Event::kNoteOnEvent && ev.noteOn.noteId == noteId) { return ev.sampleOffset; }
        }
        return 0;
    }

    /// VST3 wants events in ascending sampleOffset order. Stable insertion sort (equal offsets keep their order; no allocation, this runs on the audio thread). Almost always already sorted, so O(n).
    void sortBySampleOffset()
    {
        for (std::size_t i = 1; i < m_events.size(); ++i)
        {
            const Steinberg::Vst::Event ev = m_events[i];
            std::size_t j = i;
            while (j > 0 && m_events[j - 1].sampleOffset > ev.sampleOffset) { m_events[j] = m_events[j - 1]; --j; }
            m_events[j] = ev;
        }
    }

    /// Largest sample offset of any event added so far, or 0 when the list is empty (events are not kept sorted, so the last one added is not necessarily the latest).
    Steinberg::int32 lastSampleOffset() const { Steinberg::int32 latest = 0; for (const auto& ev : m_events) { latest = std::max(latest, ev.sampleOffset); } return latest; }

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