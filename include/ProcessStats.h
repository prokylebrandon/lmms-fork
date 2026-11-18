/*
 * ProcessStats.h - memory and CPU time of the running process
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

#ifndef LMMS_GUI_PROCESS_STATS_H
#define LMMS_GUI_PROCESS_STATS_H

#include <cstdint>

namespace lmms::gui::ProcessStats
{

//! Resident memory of this process in bytes, or 0 if the platform gives no answer
std::uint64_t residentBytes();

//! User + kernel CPU time used by this process so far, in microseconds, or -1 if unavailable
std::int64_t cpuTimeMicros();

} // namespace lmms::gui::ProcessStats

#endif // LMMS_GUI_PROCESS_STATS_H
