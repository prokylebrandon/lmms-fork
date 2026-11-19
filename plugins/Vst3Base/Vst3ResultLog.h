/*
 * Vst3ResultLog.h - log VST3 call results that must not be ignored silently
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

#ifndef LMMS_VST3_RESULT_LOG_H
#define LMMS_VST3_RESULT_LOG_H

#include <pluginterfaces/base/funknown.h>

#include <QtGlobal>

namespace lmms
{

/**
 * Logs a VST3 tresult that is neither kResultOk nor kNotImplemented. The VST3
 * spec lets a plugin answer kNotImplemented for any optional call, so that is
 * not a failure. (The numeric values differ between platforms, which is why
 * this uses the SDK's constants.) Never call this from the audio thread:
 * logging allocates.
 */
inline void Vst3WarnIfFailed(Steinberg::tresult result, const char* what)
{
    if (result == Steinberg::kResultOk || result == Steinberg::kNotImplemented) { return; }
    qWarning("Vst3PluginInstance: %s failed (tresult=%d)", what, static_cast<int>(result));
}

} // namespace lmms

#endif // LMMS_VST3_RESULT_LOG_H
