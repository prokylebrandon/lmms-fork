/*
 * Vst3EffectControlDialog.cpp - dialog for displaying VST3 effect controls
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

#include "Vst3EffectControlDialog.h"

#include <QLabel>
#include <QVBoxLayout>

#include "Vst3Effect.h"
#include "Vst3EffectControls.h"

namespace lmms::gui
{

Vst3EffectControlDialog::Vst3EffectControlDialog(Vst3EffectControls* controls) :
	EffectControlDialog(controls)
{
	auto* layout = new QVBoxLayout(this);

	const Vst3Effect* effect = controls->vst3Effect();
	const QString title = (effect != nullptr && effect->pluginInstance() != nullptr)
		? tr("%1 (%2)").arg(effect->pluginInstance()->name(), effect->pluginInstance()->vendor())
		: tr("VST3 effect (not loaded)");

	m_infoLabel = new QLabel(title, this);
	m_infoLabel->setWordWrap(true);
	layout->addWidget(m_infoLabel);

	m_noteLabel = new QLabel(
		tr("Parameter automation and the plugin's native editor are not "
		   "available in this build yet."),
		this);
	m_noteLabel->setWordWrap(true);
	layout->addWidget(m_noteLabel);
}

} // namespace lmms::gui
