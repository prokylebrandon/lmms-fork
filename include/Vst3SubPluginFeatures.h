/*
 * Vst3SubPluginFeatures.h - derivation from
 *                          Plugin::Descriptor::SubPluginFeatures for
 *                          hosting VST3 plugins
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

#include "lmmsconfig.h"

#ifdef LMMS_HAVE_VST3

#include <QStringList>

#include "lmms_export.h"
#include "Plugin.h"

namespace lmms
{


//! Discovery for VST3 plugins.
//!
//! Scans the standard per-platform VST3 locations -- on Windows (the
//! target platform), that's %LOCALAPPDATA%\Programs\Common\VST3 (per-user,
//! priority) and C:\Program Files\Common Files\VST3 (system-wide), per
//! Steinberg's own spec. LMMS_VST3_PATH overrides with a ';'- or
//! ':'-separated list, for pointing at a dev/test plugin without installing
//! it system-wide -- see Phase 0/2 report for why this doesn't touch
//! ConfigManager for a full settings-UI-configurable path instead.
class LMMS_EXPORT Vst3SubPluginFeatures : public Plugin::Descriptor::SubPluginFeatures
{
public:
	explicit Vst3SubPluginFeatures(Plugin::Type type);

	void fillDescriptionWidget(QWidget* parent, const Key* k) const override;
	void listSubPluginKeys(const Plugin::Descriptor* desc, KeyList& kl) const override;

	//! Directories this scans, in priority order -- exposed mainly for the
	//! report/tests
	static QStringList scanDirectories();

private:
	QString displayName(const Key& k) const override;
	QString description(const Key& k) const override;

	static void addBundlesFromDir(QStringList* bundles, const QString& dir);
};


} // namespace lmms

#endif // LMMS_HAVE_VST3

#endif // LMMS_VST3_SUBPLUGIN_FEATURES_H
