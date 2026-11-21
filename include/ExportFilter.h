/*
 * ExportFilter.h - declaration of class ExportFilter, the base-class for all
 *                  file export filters
 *
 * Copyright (c) 2006-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#ifndef LMMS_EXPORT_FILTER_H
#define LMMS_EXPORT_FILTER_H

#include <filesystem>
#include <vector>

#include "TrackContainer.h"
#include "Plugin.h"


namespace lmms
{


class LMMS_EXPORT ExportFilter : public Plugin
{
public:
	ExportFilter( const Descriptor * _descriptor ) : Plugin( _descriptor, nullptr ) {}
	~ExportFilter() override = default;

	//! A change of the song tempo at a position on the song timeline
	struct TempoChange
	{
		tick_t tick = 0; //!< Position in LMMS ticks
		int bpm = 0; //!< Tempo in beats per minute from this position on
	};

	//! The song tempo over time: sorted by tick, one entry per change. The first entry is at tick 0
	using TempoMap = std::vector<TempoChange>;

	//! @brief Exports the project to a file
	//! @param tracks Song tracks
	//! @param patternStoreTracks PatternStore tracks
	//! @param tempoMap Song tempo over time, including tempo automation
	//! @param masterPitch Song master pitch
	//! @param filePath The path of the exported file
	//! @return If operation was successful
	virtual bool tryExport(const TrackContainer::TrackList& tracks,
		const TrackContainer::TrackList& patternTracks,
		const TempoMap& tempoMap, int masterPitch, const std::filesystem::path& filePath) = 0;

protected:

	void saveSettings( QDomDocument &, QDomElement & ) override
	{
	}

	void loadSettings( const QDomElement & ) override
	{
	}

	QString nodeName() const override
	{
		return "import_filter";
	}


private:

} ;


} // namespace lmms

#endif // LMMS_EXPORT_FILTER_H
