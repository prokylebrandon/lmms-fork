/*
 * Vst3Types.h - Shared VST3 hosting types
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

#ifndef LMMS_VST3_TYPES_H
#define LMMS_VST3_TYPES_H

#include <cstdint>
#include <QString>

namespace lmms
{

/// VST3 parameter ID type (mirrors Steinberg::Vst::ParamID = uint32_t)
using Vst3ParamID = uint32_t;

/// Constant used by VST3 for "no valid parameter ID"
static constexpr Vst3ParamID kVst3NoParamID = 0xFFFFFFFF;

/**
 * @brief Metadata about a single VST3 audio class inside a bundle.
 *
 * Populated by Vst3PluginInstance::discoverClasses().  All fields are
 * read-only after construction; identity is classIndex (position in the
 * factory's class list), but the real stable identity for project files
 * is cid (the class UID).
 */
struct Vst3ClassInfo
{
    int     classIndex  = 0;
    QString name;
    QString vendor;
    QString category;     ///< e.g. "Audio Module Class"
    QString subCategories; ///< e.g. "Instrument|Synth"
    bool    isInstrument = false;

    /// 16-byte class UID as a hex string (stable across SDK versions)
    QString cid;
};

} // namespace lmms

#endif // LMMS_VST3_TYPES_H
