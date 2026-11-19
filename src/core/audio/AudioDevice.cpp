/*
 * AudioDevice.cpp - base-class for audio-devices used by LMMS audio engine
 *
 * Copyright (c) 2004-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#include <cassert>
#include <cstring>

#include "AudioDevice.h"
#include "AudioEngine.h"
#include "endian_handling.h"

namespace lmms
{

AudioDevice::AudioDevice(const ch_cnt_t _channels, AudioEngine* _audioEngine)
	: m_supportsCapture(false)
	, m_sampleRate(_audioEngine->outputSampleRate())
	, m_channels(_channels)
	, m_audioEngine(_audioEngine)
{
}



AudioDevice::~AudioDevice()
{
	assert(!isRunning() && "device should have been stopped before being destroyed");
}

void AudioDevice::startProcessing()
{
	m_running.test_and_set(std::memory_order_acquire);
	startProcessingImpl();
}

void AudioDevice::stopProcessing()
{
	m_running.clear(std::memory_order_release);
	stopProcessingImpl();
}

void AudioDevice::stopProcessingThread( QThread * thread )
{
	if( !thread->wait( 30000 ) )
	{
		fprintf( stderr, "Terminating audio device thread\n" );
		thread->terminate();
		if( !thread->wait( 1000 ) )
		{
			fprintf( stderr, "Thread not terminated yet\n" );
		}
	}
}



void AudioDevice::registerPort(AudioBusHandle*)
{
}




void AudioDevice::unregisterPort(AudioBusHandle*)
{
}




void AudioDevice::renamePort(AudioBusHandle*)
{
}

int AudioDevice::toInt16leInterleaved(const SampleFrame* src, const f_cnt_t frames, int_sample_t* dst) const
{
	assert(src != nullptr);
	assert(dst != nullptr);
	assert(channels() <= DEFAULT_CHANNELS && "only mono and stereo are supported");

	// Clip, scale to 16 bits, and store as little-endian whatever the host byte order is
	const auto toInt16le = [](const sample_t s) -> int_sample_t {
		return byteswapIfBE(static_cast<int_sample_t>(AudioEngine::clip(s) * OUTPUT_SAMPLE_MULTIPLIER));
	};

	const ch_cnt_t chans = channels();
	constexpr ch_cnt_t left = 0;
	constexpr ch_cnt_t right = 1;

	if (chans == 1)
	{
		// Downmix to mono
		for (f_cnt_t frame = 0; frame < frames; ++frame)
		{
			dst[frame] = toInt16le(0.5f * (src[frame][left] + src[frame][right]));
		}
	}
	else
	{
		for (f_cnt_t frame = 0; frame < frames; ++frame)
		{
			for (ch_cnt_t chnl = 0; chnl < chans; ++chnl)
			{
				// Channels beyond stereo are unsupported (asserted above). In release builds
				// they are silenced rather than read out of bounds.
				dst[frame * chans + chnl] = toInt16le(chnl < DEFAULT_CHANNELS ? src[frame][chnl] : 0.0f);
			}
		}
	}

	return frames * chans * BYTES_PER_INT_SAMPLE;
}




void AudioDevice::clearS16Buffer( int_sample_t * _outbuf, const f_cnt_t _frames )
{

	assert( _outbuf != nullptr );

	memset( _outbuf, 0,  _frames * channels() * BYTES_PER_INT_SAMPLE );
}

} // namespace lmms
