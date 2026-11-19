/*
 * Vst3OutputParamSlots.h - audio thread -> GUI thread hand-off for
 *                          plugin-originated parameter values (meters etc.)
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

#ifndef LMMS_VST3_OUTPUT_PARAM_SLOTS_H
#define LMMS_VST3_OUTPUT_PARAM_SLOTS_H

#include "Vst3Types.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace lmms
{

/**
 * @brief Latest-value-per-parameter mailbox between the audio thread (one
 *        producer) and the GUI thread (one consumer).
 *
 * A plugin reports meter readings through ProcessData::outputParameterChanges
 * on every block (hundreds of times a second), far more often than the GUI
 * repaints. A FIFO would fill up and then drop the NEWEST values, leaving the
 * meters stale, so this keeps only the most recent value per parameter id
 * and a "dirty" flag instead.
 *
 * publish()  - audio thread only. Never allocates, never locks.
 * drain()    - GUI thread only.
 *
 * A parameter id is given a slot the first time it is seen; slots are never
 * freed. If a plugin reports more than kCapacity distinct ids, the extras are
 * counted in droppedCount() and ignored.
 */
class Vst3OutputParamSlots
{
public:
    static constexpr std::size_t kCapacity = 128;

    //! Audio thread only.
    void publish(Vst3ParamID id, double value) noexcept
    {
        const auto key = static_cast<std::uint64_t>(id);
        const std::size_t n = m_count.load(std::memory_order_acquire);

        for (std::size_t i = 0; i < n; ++i)
        {
            Slot& s = m_slots[i];
            if (s.id.load(std::memory_order_relaxed) == key)
            {
                s.value.store(value, std::memory_order_relaxed);
                s.dirty.store(true, std::memory_order_release);
                return;
            }
        }

        if (n >= kCapacity)
        {
            m_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        Slot& s = m_slots[n];
        s.id.store(key, std::memory_order_relaxed);
        s.value.store(value, std::memory_order_relaxed);
        s.dirty.store(true, std::memory_order_release);
        m_count.store(n + 1, std::memory_order_release); // publish the slot
    }

    //! GUI thread only. Calls fn(Vst3ParamID id, double normalisedValue) once
    //! for every parameter whose value changed since the previous drain().
    template <typename Fn>
    void drain(Fn&& fn)
    {
        const std::size_t n = m_count.load(std::memory_order_acquire);
        for (std::size_t i = 0; i < n; ++i)
        {
            Slot& s = m_slots[i];
            if (s.dirty.exchange(false, std::memory_order_acq_rel))
            {
                fn(static_cast<Vst3ParamID>(s.id.load(std::memory_order_relaxed)),
                   s.value.load(std::memory_order_relaxed));
            }
        }
    }

    //! Number of reports ignored because kCapacity distinct ids were in use.
    std::uint64_t droppedCount() const noexcept { return m_dropped.load(std::memory_order_relaxed); }

private:
    struct Slot
    {
        std::atomic<std::uint64_t> id{0};
        std::atomic<double>        value{0.0};
        std::atomic<bool>          dirty{false};
    };

    std::array<Slot, kCapacity> m_slots{};
    std::atomic<std::size_t>    m_count{0};
    std::atomic<std::uint64_t>  m_dropped{0};
};

} // namespace lmms

#endif // LMMS_VST3_OUTPUT_PARAM_SLOTS_H
