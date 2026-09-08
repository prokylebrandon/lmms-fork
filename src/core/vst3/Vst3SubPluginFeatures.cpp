/*
 * Vst3SubPluginFeatures.cpp - implementation of VST3 plugin discovery
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

#include "Vst3SubPluginFeatures.h"

#ifdef LMMS_HAVE_VST3

#include <cstdlib>

#include <QDir>
#include <QFileInfo>
#include <QStringList>

#include "public.sdk/source/vst/hosting/module.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace lmms
{


Vst3SubPluginFeatures::Vst3SubPluginFeatures(Plugin::Type type) :
	Plugin::Descriptor::SubPluginFeatures(type)
{
}


QStringList Vst3SubPluginFeatures::scanDirectories()
{
	if (const char* env = std::getenv("LMMS_VST3_PATH"))
	{
		// Support a ';'/':' separated override like the plugin itself would use
		const QString sep = QString::fromLocal8Bit(env).contains(';') ? ";" : ":";
		return QString::fromLocal8Bit(env).split(sep, Qt::SkipEmptyParts);
	}

#if defined(Q_OS_WIN)
	// Per Steinberg's VST3 spec: both are scanned, per-user takes priority.
	// %LOCALAPPDATA%\Programs\Common\VST3 and C:\Program Files\Common Files\VST3
	QStringList dirs;
	const QString localAppData = QString::fromLocal8Bit(std::getenv("LOCALAPPDATA"));
	if (!localAppData.isEmpty()) { dirs << localAppData + "/Programs/Common/VST3"; }
	const QString programFiles = QString::fromLocal8Bit(std::getenv("PROGRAMFILES"));
	dirs << (programFiles.isEmpty() ? "C:/Program Files" : programFiles) + "/Common Files/VST3";
	return dirs;
#elif defined(Q_OS_MAC)
	return { QDir::homePath() + "/Library/Audio/Plug-Ins/VST3", "/Library/Audio/Plug-Ins/VST3" };
#else
	return { QDir::homePath() + "/.vst3", "/usr/lib/vst3", "/usr/local/lib/vst3" };
#endif
}


void Vst3SubPluginFeatures::addBundlesFromDir(QStringList* bundles, const QString& dir)
{
	QDir d(dir);
	if (!d.exists()) { return; }

	// A .vst3 is either a bundle directory (Contents/<arch>/<name> inside,
	// used on all three platforms when the plugin ships extra resources)
	// or -- Windows only, still fully valid per the VST3 module spec -- a
	// single flat DLL file literally named "Whatever.vst3". Module::create
	// handles both forms; we just need to find candidates of either kind
	// and not recurse *into* one we've already identified as a bundle.
	const QFileInfoList entries = d.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
	for (const QFileInfo& fi : entries)
	{
		if (fi.fileName().endsWith(".vst3", Qt::CaseInsensitive))
		{
			bundles->append(fi.absoluteFilePath());
		}
		else if (fi.isDir())
		{
			addBundlesFromDir(bundles, fi.absoluteFilePath());
		}
	}
}


void Vst3SubPluginFeatures::listSubPluginKeys(const Plugin::Descriptor* desc, KeyList& kl) const
{
	QStringList bundles;
	for (const QString& dir : scanDirectories()) { addBundlesFromDir(&bundles, dir); }

	for (const QString& bundlePath : bundles)
	{
		std::string error;
		VST3::Hosting::Module::Ptr module = VST3::Hosting::Module::create(bundlePath.toStdString(), error);
		if (!module) { continue; } // unreadable/incompatible bundle -- skip, don't abort the whole scan

		for (auto& ci : module->getFactory().classInfos())
		{
			if (ci.category() != kVstAudioEffectClass) { continue; }

			Plugin::Descriptor::SubPluginFeatures::Key::AttributeMap am;
			am["file"] = bundlePath;
			am["classId"] = QString::fromStdString(ci.ID().toString());
			const QString label = QString::fromStdString(module->getName()) + " - " + QString::fromStdString(ci.name());
			kl.push_back(Key(desc, label, am));
		}
	}
}


void Vst3SubPluginFeatures::fillDescriptionWidget(QWidget*, const Key*) const
{
	// Phase 3 concern (generic knob GUI reuse) -- intentionally minimal here.
}


QString Vst3SubPluginFeatures::displayName(const Key& k) const
{
	return k.isValid() ? k.name : QString();
}


QString Vst3SubPluginFeatures::description(const Key& k) const
{
	return k.isValid() ? QString("VST3: %1").arg(k.name) : QString();
}


} // namespace lmms

#endif // LMMS_HAVE_VST3
