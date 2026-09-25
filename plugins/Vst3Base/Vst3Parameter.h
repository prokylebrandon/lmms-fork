/*
 * Vst3Parameter.h - VST3 parameter metadata abstraction
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

#ifndef LMMS_VST3_PARAMETER_H
#define LMMS_VST3_PARAMETER_H

#include "Vst3Types.h"
#include <QString>

namespace lmms
{

/**
 * @brief Immutable metadata for a single VST3 parameter.
 *
 * Identity is always the VST3 parameter ID (pid), never a positional
 * index.  Persisting or keying off array positions is the mistake most
 * likely to silently corrupt saved projects — don't do it.
 *
 * Values are always in VST3's normalised [0, 1] range.  The plugin's
 * controller converts to/from display units via parameterDisplayString().
 */
struct Vst3Parameter
{
    Vst3ParamID id           = kVst3NoParamID;
    QString     title;
    QString     shortTitle;
    QString     units;
    double      defaultNormalisedValue = 0.0;
    int         stepCount   = 0;     ///< 0 = continuous, 1 = toggle, N = N+1 discrete steps
    bool        isAutomatable = true;
    bool        isReadOnly    = false;
    bool        isBypass      = false;
    bool        isProgramChange = false;

    /// From VST3's kIsHidden flag: the plugin doesn't want this parameter
    /// shown in a generic host UI (it may still be automatable/readable --
    /// hidden is a UI hint, not a capability restriction). The parameter
    /// window hides these by default, with a "show hidden" option; see
    /// doc/prestige-vst3.md's Phase 3 notes for why PRESTIGE still builds
    /// a model and exposes it (e.g. to automation) rather than dropping it
    /// entirely.
    bool        isHidden      = false;
};

} // namespace lmms

#endif // LMMS_VST3_PARAMETER_H
