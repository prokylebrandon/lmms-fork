/*
 * Vst3ParameterChanges.h - IParameterChanges for delivering queued param changes
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

#ifndef LMMS_VST3_PARAMETER_CHANGES_H
#define LMMS_VST3_PARAMETER_CHANGES_H

#include <pluginterfaces/vst/ivstparameterchanges.h>
#include "Vst3Types.h"

#include <vector>

namespace lmms
{

/**
 * @brief A single-point IParamValueQueue for one parameter.
 *
 * Each queued parameter change for a block gets its own queue with one
 * point at sample offset 0.  This is sufficient for LMMS's block-level
 * automation granularity; sub-block accuracy can be added later.
 */
class Vst3SingleParamQueue final : public Steinberg::Vst::IParamValueQueue
{
public:
    Vst3SingleParamQueue(Vst3ParamID id, double normValue)
        : m_id(id)
    {
        m_point = { 0, normValue };
    }

    Steinberg::Vst::ParamID PLUGIN_API getParameterId() override
    {
        return static_cast<Steinberg::Vst::ParamID>(m_id);
    }

    Steinberg::int32 PLUGIN_API getPointCount() override { return 1; }

    Steinberg::tresult PLUGIN_API getPoint(
        Steinberg::int32         index,
        Steinberg::int32&        sampleOffset,
        Steinberg::Vst::ParamValue& value) override
    {
        if (index != 0) return Steinberg::kInvalidArgument;
        sampleOffset = m_point.sampleOffset;
        value        = m_point.value;
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API addPoint(
        Steinberg::int32              sampleOffset,
        Steinberg::Vst::ParamValue    value,
        Steinberg::int32&             index) override
    {
        // Replace the single point
        m_point = { sampleOffset, value };
        index   = 0;
        return Steinberg::kResultOk;
    }

    Steinberg::uint32  PLUGIN_API addRef()  override { return 1; }
    Steinberg::uint32  PLUGIN_API release() override { return 1; }
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID, void**) override
    {
        return Steinberg::kNoInterface;
    }

private:
    Vst3ParamID m_id;
    struct Point { Steinberg::int32 sampleOffset; double value; };
    Point m_point;
};

// ---------------------------------------------------------------------------

/**
 * @brief IParameterChanges that holds one queue per changed parameter.
 *
 * Call addChange() for each queued parameter mutation before processAudio(),
 * then clear() after. Stack-owned; no heap allocation per block once
 * the internal vectors are at capacity.
 */
class Vst3ParameterChanges final : public Steinberg::Vst::IParameterChanges
{
public:
    static constexpr int kMaxChanges = 128;

    Vst3ParameterChanges() { m_queues.reserve(kMaxChanges); }

    void clear()
    {
        m_queues.clear();
        m_queuePtrs.clear();
    }

    void addChange(Vst3ParamID id, double normValue)
    {
        if (static_cast<int>(m_queues.size()) >= kMaxChanges) return;
        m_queues.emplace_back(id, normValue);
        m_queuePtrs.push_back(&m_queues.back());
    }

    // ------------------------------------------------------------------
    // IParameterChanges
    // ------------------------------------------------------------------

    Steinberg::int32 PLUGIN_API getParameterCount() override
    {
        return static_cast<Steinberg::int32>(m_queuePtrs.size());
    }

    Steinberg::Vst::IParamValueQueue* PLUGIN_API
    getParameterData(Steinberg::int32 index) override
    {
        if (index < 0 || index >= static_cast<Steinberg::int32>(m_queuePtrs.size()))
            return nullptr;
        return m_queuePtrs[static_cast<std::size_t>(index)];
    }

    Steinberg::Vst::IParamValueQueue* PLUGIN_API
    addParameterData(const Steinberg::Vst::ParamID& id,
                     Steinberg::int32&               index) override
    {
        if (static_cast<int>(m_queues.size()) >= kMaxChanges)
            return nullptr;
        m_queues.emplace_back(static_cast<Vst3ParamID>(id), 0.0);
        m_queuePtrs.push_back(&m_queues.back());
        index = static_cast<Steinberg::int32>(m_queuePtrs.size()) - 1;
        return m_queuePtrs.back();
    }

    Steinberg::uint32  PLUGIN_API addRef()  override { return 1; }
    Steinberg::uint32  PLUGIN_API release() override { return 1; }
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID, void**) override
    {
        return Steinberg::kNoInterface;
    }

private:
    // Store by value to avoid per-block heap allocation
    std::vector<Vst3SingleParamQueue>          m_queues;
    std::vector<Steinberg::Vst::IParamValueQueue*> m_queuePtrs;
};

} // namespace lmms

#endif // LMMS_VST3_PARAMETER_CHANGES_H
