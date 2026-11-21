/*
 * MidiExport.cpp - support for exporting MIDI files
 *
 * Copyright (c) 2015 Mohamed Abdel Maksoud <mohamed at amaksoud.com>
 * Copyright (c) 2017 Hyunjin Song <tteu.ingog/at/gmail.com>
 * Copyright (c) 2020 EmoonX
 * Copyright (c) 2026 Dalton Messmer <messmer.dalton/at/gmail.com>
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

#include "MidiExport.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <vector>
#include <stack>

#include "AutomationClip.h"
#include "Instrument.h"
#include "DataFile.h"
#include "InstrumentTrack.h"
#include "LocaleHelper.h"
#include "PatternTrack.h"
#include "plugin_export.h"

namespace lmms
{

extern "C"
{

//! Standardized plugin descriptor for MIDI exporter
Plugin::Descriptor PLUGIN_EXPORT midiexport_plugin_descriptor =
{
	LMMS_STRINGIFY(PLUGIN_NAME),
	"MIDI Export",
	QT_TRANSLATE_NOOP("PluginBrowser",
		"Filter for exporting MIDI files from LMMS"),
	"Mohamed Abdel Maksoud <mohamed at amaksoud.com> and "
		"Hyunjin Song <tteu.ingog/at/gmail.com>",
	0x0100,
	Plugin::Type::ExportFilter,
	nullptr,
	nullptr,
	nullptr,
};

PLUGIN_EXPORT Plugin* lmms_plugin_main(Model*, void*)
{
	return new MidiExport{};
}

} // extern "C"

void MidiExport::Clip::write(const QDomNode& root,
	int basePitch, double baseVolume, tick_t baseTime)
{
	// TODO: interpret steps="12" muted="0" type="1" name="Piano1" len="259"
	for (QDomNode node = root.firstChild(); !node.isNull();
		node = node.nextSibling())
	{
		QDomElement element = node.toElement();

		// Ignore zero-length notes
		if (element.attribute("len", "0") == "0") { continue; }

		// Adjust note attributes based on base measures
		const int pitch = basePitch + element.attribute("key", "0").toInt();
		double volume = LocaleHelper::toDouble(element.attribute("vol", "100"));
		volume *= baseVolume * (127.0 / 200.0);

		auto note = Note {
			.pitch = std::clamp<std::uint8_t>(pitch, 0, 127),
			.volume = std::min<std::uint8_t>(std::lround(volume), 127),
			.time = baseTime + std::max(element.attribute("pos", "0").toInt(), 0),
			.duration = element.attribute("len", "0").toInt(),
			.type = static_cast<Note::Type>(element.attribute("type", "0").toInt())
		};

		// Append note to vector
		m_notes.push_back(note);
	}
}

void MidiExport::Clip::writeToTrack(MidiFile::Track& midiTrack) const
{
	for (const Note& note : m_notes)
	{
		midiTrack.addNote(note.pitch, note.volume,
			note.time / 48.0, note.duration / 48.0);
	}
}

void MidiExport::Clip::processPatternNotes(tick_t cutPos)
{
	// Sort in reverse order
	std::sort(m_notes.rbegin(), m_notes.rend());

	tick_t cur = std::numeric_limits<tick_t>::max();
	tick_t next = std::numeric_limits<tick_t>::max();
	for (Note& note : m_notes)
	{
		if (note.time < cur)
		{
			// Set last two notes positions
			next = cur;
			cur = note.time;
		}
		if (note.type == Note::Type::Step)
		{
			// Note should have positive duration that neither
			// overlaps next one nor exceeds cutPos
			note.duration = std::min(std::min(DefaultBeatLength, next - cur), cutPos - note.time);
		}
	}
}

void MidiExport::Clip::writeToPattern(Clip& patternClip,
	tick_t len, tick_t base, tick_t start, tick_t end)
{
	// Avoid misplaced start and end positions
	if (start >= end) { return; }

	// Adjust positions relatively to base pos
	start -= base;
	end -= base;

	std::sort(m_notes.begin(), m_notes.end());
	for (Note note : m_notes)
	{
		// Insert periodically repeating notes from <t0> and spaced
		// by <len> to mimic pattern clip behavior
		tick_t t0 = note.time + std::ceil((start - note.time) / len) * len;
		for (tick_t time = t0; time < end; time += len)
		{
			note.time = base + time;
			patternClip.m_notes.push_back(note);
		}
	}
}

MidiExport::MidiExport()
	: ExportFilter(&midiexport_plugin_descriptor)
{}

bool MidiExport::tryExport(const TrackContainer::TrackList& tracks,
	const TrackContainer::TrackList& patternStoreTracks,
	const TempoMap& tempoMap, int masterPitch, const std::filesystem::path& filePath)
{
	// Count number of instrument (and PatternStore) tracks
	const auto numTracks = std::ranges::count_if(tracks, [](const Track* t) {
		return t->type() == Track::Type::Instrument;
	}) + patternStoreTracks.size();

	// Write header info
	auto file = MidiFile(filePath, numTracks);
	file.m_header.writeToBuffer();

	// Every track gets the tempo at the start of the song (120 is the MIDI default if there is none)
	const int tempo = tempoMap.empty() ? 120 : tempoMap.front().bpm;

	// The later tempo changes go in the first track only, where format 1 files keep their tempo map.
	// This has to happen before the track is processed, because that writes the track to its buffer.
	if (!file.m_tracks.empty())
	{
		for (std::size_t i = 1; i < tempoMap.size(); ++i)
		{
			file.m_tracks.front().addTempoChange(tempoMap[i].bpm, tempoMap[i].tick / 48.0);
		}
	}

	// Iterate through "normal" tracks
	std::size_t trackIdx = 0;
	for (Track* track : tracks)
	{
		assert(track != nullptr);
		if (track->type() == Track::Type::Instrument)
		{
			processTrack(*track, file.m_tracks[trackIdx++], tempo, masterPitch);
		}
		else if (track->type() == Track::Type::Pattern)
		{
			processPatternTrack(*track);
		}
	}

	// Iterate through PatternStore tracks
	for (Track* track : patternStoreTracks)
	{
		assert(track != nullptr);
		processTrack(*track, file.m_tracks[trackIdx++], tempo, masterPitch, true);
	}

	// Write all buffered data to stream
	file.writeAllToStream();

	// Always returns success... for now?
	return true;
}

namespace
{

//! A node of a note's detuning curve: position in ticks from the note start, values in semitones
struct DetuneNode
{
	tick_t pos = 0;
	float in = 0.0f;
	float out = 0.0f;
};

//! The detuning curve of one note on the song timeline
struct DetuneSegment
{
	tick_t start = 0;
	tick_t end = 0;
	bool linear = true; // discrete curves hold each value, linear and cubic curves are read as linear
	std::vector<DetuneNode> nodes;

	float valueAt(tick_t tick) const
	{
		const tick_t rel = tick - start;
		// Like LMMS: no detuning before the first node, the last node's value after the last one
		if (nodes.empty() || rel < nodes.front().pos) { return 0.0f; }

		std::size_t i = 0;
		while (i + 1 < nodes.size() && nodes[i + 1].pos <= rel) { ++i; }
		const DetuneNode& a = nodes[i];
		if (rel == a.pos) { return a.in; }
		if (i + 1 >= nodes.size() || !linear) { return a.out; }

		const DetuneNode& b = nodes[i + 1];
		const auto span = static_cast<float>(b.pos - a.pos);
		return span > 0.0f ? a.out + (b.in - a.out) * static_cast<float>(rel - a.pos) / span : a.out;
	}
};

//! Reads the detuning curves of all notes of a track from its saved state, sorted by start time
std::vector<DetuneSegment> collectDetuneSegments(const QDomElement& root)
{
	std::vector<DetuneSegment> segments;
	for (QDomElement clip = root.firstChildElement("midiclip"); !clip.isNull();
		clip = clip.nextSiblingElement("midiclip"))
	{
		const tick_t clipStart = std::max(clip.attribute("pos", "0").toInt(), 0);
		for (QDomElement note = clip.firstChildElement(); !note.isNull(); note = note.nextSiblingElement())
		{
			const int length = note.attribute("len", "0").toInt();
			if (length <= 0) { continue; }

			const QDomElement curve = note.firstChildElement("automationclip").firstChildElement("detuning");
			if (curve.isNull()) { continue; }

			DetuneSegment segment;
			segment.start = clipStart + std::max(note.attribute("pos", "0").toInt(), 0);
			segment.end = segment.start + length;
			segment.linear = curve.attribute("prog", "1").toInt() != 0;
			for (QDomElement time = curve.firstChildElement("time"); !time.isNull();
				time = time.nextSiblingElement("time"))
			{
				DetuneNode node;
				node.pos = time.attribute("pos", "0").toInt();
				node.in = static_cast<float>(LocaleHelper::toDouble(time.attribute("value", "0")));
				node.out = time.hasAttribute("outValue")
					? static_cast<float>(LocaleHelper::toDouble(time.attribute("outValue"))) : node.in;
				segment.nodes.push_back(node);
			}
			if (segment.nodes.empty()) { continue; }

			std::sort(segment.nodes.begin(), segment.nodes.end(),
				[](const DetuneNode& a, const DetuneNode& b) { return a.pos < b.pos; });
			segments.push_back(std::move(segment));
		}
	}
	std::sort(segments.begin(), segments.end(),
		[](const DetuneSegment& a, const DetuneSegment& b) { return a.start < b.start; });
	return segments;
}

} // namespace

//! Writes the pitch of an instrument track as pitch bend messages: the Pitch knob (its own value, or the curves of
//! the automation clips that control it) plus the detuning curve of the note that is sounding. A MIDI channel has one
//! bend value, so when detuned notes overlap, the most recently started one decides.
//! Detuning is not exported for beat pattern tracks, whose clips are repeated by the pattern writer.
static void writePitchBends(InstrumentTrack& instTrack, MidiFile::Track& midiTrack,
	const QDomElement& root, bool isPattern)
{
	constexpr tick_t SampleSpacing = 4; // LMMS ticks between bend messages on a curve (48 ticks make a beat)
	constexpr std::uint16_t CentreBend = 8192;

	FloatModel* pitchModel = instTrack.pitchModel();

	// Pitch knob in cents: a step function over the sampled ticks, or just its own value when it is not automated
	std::map<tick_t, float> knob;
	float knobStart = 0.0f;
	std::set<tick_t> ticks{0};

	bool automated = false;
	const auto clips = AutomationClip::clipsForModel(pitchModel);
	for (AutomationClip* clip : clips)
	{
		if (!clip->hasAutomation() || clip->isMuted()) { continue; }
		automated = true;

		const auto start = static_cast<tick_t>(clip->startPosition());
		const auto offset = static_cast<tick_t>(clip->startTimeOffset());
		const auto end = start + static_cast<tick_t>(clip->length());
		const auto& nodes = clip->getTimeMap();

		const auto addAt = [&](int relative)
		{
			const tick_t absolute = start + relative - offset;
			if (absolute < start || absolute > end) { return; }
			knob[absolute] = clip->valueAt(TimePos(relative));
			ticks.insert(absolute);
		};

		// Both sides of every node (so jumps come out right), plus regular samples in between for the curves
		for (auto it = nodes.cbegin(); it != nodes.cend(); ++it)
		{
			addAt(it.key());
			addAt(it.key() + 1);
		}
		for (int relative = nodes.firstKey(); relative < nodes.lastKey(); relative += SampleSpacing)
		{
			addAt(relative);
		}
	}
	// Until the first automation clip starts LMMS keeps the knob's own value, which is not known here: assume centre
	if (!automated) { knobStart = pitchModel->value(); }

	std::vector<DetuneSegment> detune;
	if (!isPattern) { detune = collectDetuneSegments(root); }
	for (const DetuneSegment& segment : detune)
	{
		ticks.insert(segment.start);
		ticks.insert(segment.end); // back to no detuning when the note ends
		for (tick_t tick = segment.start; tick < segment.end; tick += SampleSpacing) { ticks.insert(tick); }
		for (const DetuneNode& node : segment.nodes)
		{
			for (const tick_t tick : {segment.start + node.pos, segment.start + node.pos + 1})
			{
				if (tick >= segment.start && tick < segment.end) { ticks.insert(tick); }
			}
		}
	}

	const auto knobAt = [&](tick_t tick)
	{
		const auto it = knob.upper_bound(tick);
		return it == knob.begin() ? knobStart : std::prev(it)->second;
	};
	const auto detuneAt = [&](tick_t tick)
	{
		const DetuneSegment* active = nullptr;
		for (const DetuneSegment& segment : detune)
		{
			if (segment.start > tick) { break; }
			if (tick < segment.end) { active = &segment; }
		}
		return active ? active->valueAt(tick) : 0.0f;
	};

	// Total pitch offset in semitones at every tick that matters
	std::vector<std::pair<tick_t, float>> semitones;
	float maxAbs = 0.0f;
	for (const tick_t tick : ticks)
	{
		const float value = knobAt(tick) / 100.0f + detuneAt(tick);
		semitones.emplace_back(tick, value);
		maxAbs = std::max(maxAbs, std::abs(value));
	}
	if (maxAbs < 0.001f) { return; } // nothing to export

	// At least the track's own pitch range, wider if a detuning curve reaches further
	const int range = std::clamp(
		std::max(instTrack.pitchRangeModel()->value(), static_cast<int>(std::ceil(maxAbs))), 1, 96);
	const auto toBend = [range](float value)
	{
		const long bend = std::lround(8192.0f + value / static_cast<float>(range) * 8192.0f);
		return static_cast<std::uint16_t>(std::clamp(bend, 0L, 16383L));
	};

	std::uint16_t current = CentreBend; // a MIDI channel starts centred
	bool rangeWritten = false;
	for (const auto& [tick, value] : semitones)
	{
		const std::uint16_t bend = toBend(value);
		if (bend == current) { continue; }
		if (!rangeWritten)
		{
			midiTrack.addPitchBendRange(static_cast<std::uint8_t>(range), 0);
			rangeWritten = true;
		}
		midiTrack.addPitchBend(bend, tick / 48.0);
		current = bend;
	}
}

void MidiExport::processTrack(Track& track, MidiFile::Track& midiTrack,
	int tempo, int masterPitch, bool isPattern)
{
	// Cast track as a instrument one and save info from it to element
	auto& instTrack = dynamic_cast<InstrumentTrack&>(track);
	QDomElement root = instTrack.saveState(m_dataFile, m_dataFile.content());

	// Use the MIDI patch from the instrument, if it supports it.
	// Note that this only works decently if the current bank is a GM 1~128 one
	// (which would be needed as the default either way for successful import).
	// Pattern tracks are always bank 128 (see MidiImport), patch 0.
	auto patch = instTrack.instrument()->midiPatch().value_or(MidiPatch{});
	if (patch.bank == 128)
	{
		// Drum track, so set its channel to 10
		midiTrack.setChannel(9);
	}
	else
	{
		midiTrack.setChannel(m_channel);

		// Update the channel for the next MIDI file track
		++m_channel;
		if (m_channel == 9) { ++m_channel; } // skip drum channel
		m_channel %= 16; // TODO: Give user option for how to handle wrap around?
	}

	// Add info about tempo, track name, bank select, and program change
	midiTrack.addTempo(tempo, 0);
	midiTrack.addName(track.name().toStdString(), 0);

	// The MIDI standard has no track color, so it is stored as a sequencer-specific meta event
	if (const auto& color = track.color(); color.has_value())
	{
		midiTrack.addColor(static_cast<std::uint8_t>(color->red()),
			static_cast<std::uint8_t>(color->green()), static_cast<std::uint8_t>(color->blue()), 0);
	}
	midiTrack.addPatch(patch, 0);

	writePitchBends(instTrack, midiTrack, root, isPattern);

	// ---- Instrument track ---- //
	QDomNode trackNode = root.firstChildElement("instrumenttrack");
	QDomElement trackElem = trackNode.toElement();
	int basePitch = 69 - trackElem.attribute("basenote", "69").toInt();
	// Adjust to masterPitch if enabled
	if (trackElem.attribute("usemasterpitch", "1").toInt())
	{
		basePitch += masterPitch;
	}
	// Volume ranges in [0.0, 2.0]
	double baseVolume = LocaleHelper::toDouble(
		trackElem.attribute("volume", "100")) / 100.0;

	// ---- Clips ---- //
	std::uint8_t patternId = 0;
	for (QDomNode clipNode = root.firstChildElement("midiclip");
		!clipNode.isNull();
		clipNode = clipNode.nextSiblingElement("midiclip"))
	{
		QDomElement clipElem = clipNode.toElement();
		Clip clip;
		if (!isPattern)
		{
			// Base time == initial position
			tick_t baseTime = std::max(clipElem.attribute("pos", "0").toInt(), 0);

			// Write track notes to clip
			clip.write(clipNode, basePitch, baseVolume, baseTime);

			// Write clip info to MIDI file track
			clip.processPatternNotes(INT_MAX);
			clip.writeToTrack(midiTrack);
		}
		else
		{
			// Write to-be repeated pattern clip notes to clip
			// (notice base time of 0)
			clip.write(clipNode, basePitch, baseVolume, 0);

			// Write clip to track
			writePatternClip(clip, clipElem, patternId++, midiTrack);
		}
	}

	// Write track data to buffer
	midiTrack.writeToBuffer();
}

void MidiExport::writePatternClip(Clip& clip, const QDomElement& clipElem,
	std::uint8_t patternIdx, MidiFile::Track& midiTrack)
{
	// Workaround for nested PatternClips
	tick_t pos = 0;
	tick_t len = 12 * clipElem.attribute("steps", "1").toInt();

	// Iterate through PatternClip pairs of current list
	// TODO: This *may* need some corrections?
	const std::vector<std::pair<tick_t, tick_t>>& plist = m_plists.at(patternIdx);
	std::stack<std::pair<tick_t, tick_t>> st;
	Clip patternClip;
	for (const std::pair<tick_t, tick_t>& p : plist)
	{
		while (!st.empty() && st.top().second <= p.first)
		{
			clip.writeToPattern(patternClip, len, st.top().first, pos, st.top().second);
			pos = st.top().second;
			st.pop();
		}
		if (!st.empty() && st.top().second <= p.second)
		{
			clip.writeToPattern(patternClip, len, st.top().first, pos, p.first);
			pos = p.first;
			while (!st.empty() && st.top().second <= p.second)
			{
				st.pop();
			}
		}
		st.push(p);
		pos = p.first;
	}
	while (!st.empty())
	{
		clip.writeToPattern(patternClip, len, st.top().first, pos, st.top().second);
		pos = st.top().second;
		st.pop();
	}
	// Write clip info to MIDI file track
	patternClip.processPatternNotes(pos);
	patternClip.writeToTrack(midiTrack);
}

void MidiExport::processPatternTrack(Track& track)
{
	// Cast track as a pattern track and save info from it to element
	auto& patternTrack = dynamic_cast<PatternTrack&>(track);
	QDomElement root = patternTrack.saveState(m_dataFile, m_dataFile.content());

	// Build lists of (start, end) pairs from pattern clip note objects
	std::vector<std::pair<tick_t, tick_t>> plist;
	for (QDomNode patternClipNode = root.firstChildElement("patternclip");
		!patternClipNode.isNull();
		patternClipNode = patternClipNode.nextSiblingElement("patternclip"))
	{
		QDomElement patternClipElem = patternClipNode.toElement();
		tick_t start = std::max(patternClipElem.attribute("pos", "0").toInt(), 0);
		tick_t end = start + patternClipElem.attribute("len", "0").toInt();
		plist.emplace_back(start, end);
	}

	// Sort list in ascending order and append it to matrix
	std::sort(plist.begin(), plist.end());
	m_plists.emplace_back(std::move(plist));
}

} // namespace lmms
