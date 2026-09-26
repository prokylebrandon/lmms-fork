/*
 * Vst3SubPluginFeatures.h - derivation from
 *                           Plugin::Descriptor::SubPluginFeatures for
 *                           hosting VST3 effect plugins
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

#ifndef LMMS_VST3_SUBPLUGIN_FEATURES_H
#define LMMS_VST3_SUBPLUGIN_FEATURES_H

#include "Plugin.h"

namespace lmms
{

/**
 * Inner-layer registration for VST3 effects (see PRESTIGE-Phase-4's
 * "Two-layer plugin registration" section). listSubPluginKeys() scans
 * ConfigManager::vst3Dir() for .vst3 bundles and, for each one, enumerates
 * its effect-capable classes via Vst3PluginInstance::discoverClasses() --
 * the same factory wrapper Prestige (Phase 2) uses on the instrument side.
 * Instrument-only classes inside a bundle are filtered out here; they stay
 * on Prestige's own browse-to-load path, not this picker.
 */
class Vst3SubPluginFeatures : public Plugin::Descriptor::SubPluginFeatures
{
public:
	explicit Vst3SubPluginFeatures(Plugin::Type type);

	void fillDescriptionWidget(QWidget* parent, const Key* key) const override;

	void listSubPluginKeys(const Plugin::Descriptor* desc, KeyList& kl) const override;

private:
	//! Recursively collects .vst3 bundle paths under dirPath. A bundle may
	//! be a directory (the common case) or a single file -- both shapes
	//! are matched rather than assuming one (Phase 4 spec explicitly warns
	//! against a scanner that only globs *.dll/*.so-style flat files).
	void scanDir(const QString& dirPath, QStringList& bundlesOut) const;
};

} // namespace lmms

#endif // LMMS_VST3_SUBPLUGIN_FEATURES_H
