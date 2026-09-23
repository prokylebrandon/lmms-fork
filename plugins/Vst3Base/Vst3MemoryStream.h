/*
 * Vst3MemoryStream.h - IBStream adapter over QByteArray for state I/O
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

#ifndef LMMS_VST3_MEMORY_STREAM_H
#define LMMS_VST3_MEMORY_STREAM_H

#include <pluginterfaces/base/ibstream.h>
#include <QByteArray>
#include <cstring>
#include <algorithm>

namespace lmms
{

/**
 * @brief A simple Steinberg::IBStream backed by a QByteArray.
 *
 * Used for plugin state serialisation (IComponent::getState /
 * IEditController::getState) and deserialisation (setState).
 *
 * Reference counting is not needed — these objects are always stack- or
 * scope-owned and never shared across threads.
 */
class Vst3MemoryStream final : public Steinberg::IBStream
{
public:
    /// Construct a write stream (empty buffer).
    Vst3MemoryStream()
        : m_pos(0)
        , m_readOnly(false)
    {}

    /// Construct a read stream (caller retains ownership of @p data).
    explicit Vst3MemoryStream(const QByteArray& data)
        : m_buf(data)
        , m_pos(0)
        , m_readOnly(true)
    {}

    // ------------------------------------------------------------------
    // IBStream
    // ------------------------------------------------------------------

    Steinberg::tresult PLUGIN_API read(void* buffer, Steinberg::int32 numBytes,
                                       Steinberg::int32* bytesRead) override
    {
        if (!buffer || numBytes < 0)
            return Steinberg::kInvalidArgument;

        const Steinberg::int32 available =
            static_cast<Steinberg::int32>(m_buf.size()) - m_pos;
        const Steinberg::int32 toRead =
            std::min(numBytes, std::max<Steinberg::int32>(0, available));

        if (toRead > 0)
            std::memcpy(buffer, m_buf.constData() + m_pos, static_cast<std::size_t>(toRead));

        m_pos += toRead;
        if (bytesRead)
            *bytesRead = toRead;

        return (toRead == numBytes) ? Steinberg::kResultOk : Steinberg::kResultFalse;
    }

    Steinberg::tresult PLUGIN_API write(void* buffer, Steinberg::int32 numBytes,
                                        Steinberg::int32* bytesWritten) override
    {
        if (m_readOnly)
            return Steinberg::kResultFalse;
        if (!buffer || numBytes < 0)
            return Steinberg::kInvalidArgument;

        const int needed = m_pos + numBytes;
        if (needed > m_buf.size())
            m_buf.resize(needed);

        std::memcpy(m_buf.data() + m_pos, buffer, static_cast<std::size_t>(numBytes));
        m_pos += numBytes;

        if (bytesWritten)
            *bytesWritten = numBytes;

        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API seek(Steinberg::int64 pos, Steinberg::int32 mode,
                                       Steinberg::int64* result) override
    {
        Steinberg::int32 newPos = m_pos;
        switch (mode)
        {
        case Steinberg::IBStream::kIBSeekSet:
            newPos = static_cast<Steinberg::int32>(pos);
            break;
        case Steinberg::IBStream::kIBSeekCur:
            newPos = m_pos + static_cast<Steinberg::int32>(pos);
            break;
        case Steinberg::IBStream::kIBSeekEnd:
            newPos = static_cast<Steinberg::int32>(m_buf.size()) +
                     static_cast<Steinberg::int32>(pos);
            break;
        default:
            return Steinberg::kInvalidArgument;
        }

        if (newPos < 0)
            return Steinberg::kResultFalse;

        m_pos = newPos;
        if (result)
            *result = m_pos;

        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API tell(Steinberg::int64* pos) override
    {
        if (!pos)
            return Steinberg::kInvalidArgument;
        *pos = m_pos;
        return Steinberg::kResultOk;
    }

    // No-op reference counting — this object is always scope-owned.
    Steinberg::uint32 PLUGIN_API addRef() override  { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID /*iid*/,
                                                  void** /*obj*/) override
    {
        return Steinberg::kNoInterface;
    }

    // ------------------------------------------------------------------
    // Accessors
    // ------------------------------------------------------------------

    const QByteArray& buffer() const { return m_buf; }
    QByteArray takeBuffer() { return std::move(m_buf); }

private:
    QByteArray       m_buf;
    Steinberg::int32 m_pos;
    bool             m_readOnly;
};

} // namespace lmms

#endif // LMMS_VST3_MEMORY_STREAM_H
