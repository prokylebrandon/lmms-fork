/*
 * Vst3Effect.cpp - implementation of VST3 effect
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

#include "Vst3Effect.h"

#include <QDebug>

#include "Vst3SubPluginFeatures.h"
#include "Engine.h"
#include "AudioEngine.h"
#include "LmmsCommonMacros.h"

#include "plugin_export.h"

namespace lmms
{


extern "C"
{

Plugin::Descriptor PLUGIN_EXPORT vst3effect_plugin_descriptor =
{
	LMMS_STRINGIFY(PLUGIN_NAME),
	"VST3",
	QT_TRANSLATE_NOOP("PluginBrowser", "plugin for hosting arbitrary VST3 effects inside LMMS."),
	"LMMS VST3 experimental fork",
	0x0100,
	Plugin::Type::Effect,
	nullptr, // logo: none embedded yet
	nullptr,
	new Vst3SubPluginFeatures(Plugin::Type::Effect)
};

}


Vst3Effect::Vst3Effect(Model* parent, const Descriptor::SubPluginFeatures::Key* key) :
	Effect(&vst3effect_plugin_descriptor, parent, key),
	m_controls(this, key->attributes["file"], key->attributes["classId"]),
	m_tmpOutputSmps(Engine::audioEngine()->framesPerPeriod())
{
	if (!m_controls.isValid())
	{
		qCritical() << "Vst3Effect: failed to load" << key->attributes["file"] << ":" << m_controls.errorString();
	}
}


Effect::ProcessStatus Vst3Effect::processImpl(SampleFrame* buf, const f_cnt_t frames)
{
	if (!m_controls.isValid()) { return ProcessStatus::Sleep; }

	Q_ASSERT(frames <= static_cast<f_cnt_t>(m_tmpOutputSmps.size()));

	m_controls.copyBuffersFromLmms(buf, frames);
	m_controls.copyModelsFromLmms();
	m_controls.run(frames);
	m_controls.copyModelsToLmms();
	m_controls.copyBuffersToLmms(m_tmpOutputSmps.data(), frames);

	bool corrupt = wetLevel() < 0; // #3261 - if w < 0, bash w := 0, d := 1
	const float d = corrupt ? 1 : dryLevel();
	const float w = corrupt ? 0 : wetLevel();
	for (f_cnt_t f = 0; f < frames; ++f)
	{
		buf[f][0] = d * buf[f][0] + w * m_tmpOutputSmps[f][0];
		buf[f][1] = d * buf[f][1] + w * m_tmpOutputSmps[f][1];
	}

	return ProcessStatus::ContinueIfNotQuiet;
}


extern "C"
{

PLUGIN_EXPORT Plugin* lmms_plugin_main(Model* parent, void* data)
{
	using KeyType = Plugin::Descriptor::SubPluginFeatures::Key;
	try
	{
		return new Vst3Effect(parent, static_cast<const KeyType*>(data));
	}
	catch (const std::runtime_error& e)
	{
		qCritical() << e.what();
		return nullptr;
	}
}

}


} // namespace lmms
