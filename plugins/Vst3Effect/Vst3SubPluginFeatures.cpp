/*
 * Vst3SubPluginFeatures.cpp - derivation from
 *                             Plugin::Descriptor::SubPluginFeatures for
 *                             hosting VST3 effect plugins
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

#include "Vst3SubPluginFeatures.h"

#include <QDir>
#include <QFileInfo>
#include <QLabel>

#include "ConfigManager.h"
#include "Effect.h"
#include "Vst3PluginInstance.h"
#include "Vst3Types.h"

namespace lmms
{

Vst3SubPluginFeatures::Vst3SubPluginFeatures(Plugin::Type type) :
	SubPluginFeatures(type)
{
}

void Vst3SubPluginFeatures::fillDescriptionWidget(QWidget* parent, const Key* key) const
{
	new QLabel(QWidget::tr("Name: ") + key->name, parent);
	new QLabel(QWidget::tr("Vendor: ") + key->attributes.value("vendor"), parent);
	new QLabel(QWidget::tr("Category: ") + key->attributes.value("category"), parent);
	new QLabel(QWidget::tr("Bundle: ") + key->attributes.value("file"), parent);
}

void Vst3SubPluginFeatures::scanDir(const QString& dirPath, QStringList& bundlesOut) const
{
	QDir dir(dirPath);
	if (!dir.exists())
	{
		return;
	}

	// A .vst3 bundle is normally a directory (macOS/Linux, and the common
	// Windows layout too); some Windows-only distributions still ship a
	// bare .vst3 file. Match both rather than assuming one shape.
	const QStringList bundleDirs = dir.entryList(QStringList() << "*.vst3", QDir::Dirs | QDir::NoDotAndDotDot);
	for (const QString& d : bundleDirs)
	{
		bundlesOut << dir.absoluteFilePath(d);
	}

	const QStringList bundleFiles = dir.entryList(QStringList() << "*.vst3", QDir::Files);
	for (const QString& f : bundleFiles)
	{
		bundlesOut << dir.absoluteFilePath(f);
	}

	// Recurse into plain subdirectories that are not themselves .vst3
	// bundles (already handled above), same intent as
	// VstSubPluginFeatures::addPluginsFromDir()'s recursive scan minus its
	// VST2-specific *.dll/*.so globbing.
	const QStringList subDirs = dir.entryList(QStringList() << "*", QDir::Dirs | QDir::NoDotAndDotDot);
	for (const QString& sub : subDirs)
	{
		if (sub.endsWith(".vst3", Qt::CaseInsensitive))
		{
			continue;
		}
		scanDir(dir.absoluteFilePath(sub), bundlesOut);
	}
}

void Vst3SubPluginFeatures::listSubPluginKeys(const Plugin::Descriptor* desc, KeyList& kl) const
{
	QStringList bundles;
	scanDir(ConfigManager::inst()->vst3Dir(), bundles);

	for (const QString& bundlePath : bundles)
	{
		QString error;
		const std::vector<Vst3ClassInfo> classes = Vst3PluginInstance::discoverClasses(bundlePath, &error);
		if (classes.empty())
		{
			// Unreadable bundle, or nothing usable inside it -- skip
			// silently here, same as the VST2 scanner silently skipping
			// non-plugin files it happens to glob. Real validation happens
			// at load time (Vst3Effect::openPlugin), where the failure is
			// surfaced to the user via collectErrorForUI().
			continue;
		}

		for (const Vst3ClassInfo& info : classes)
		{
			// ---------------------------------------------------------
			// ASSUMPTION FLAG (batch 1, unverified against Vst3Types.h):
			// info.isInstrument / info.uid / info.classIndex / info.name /
			// info.vendor / info.category are inferred from
			// Vst3PluginInstance.h's doc comments and prestige-vst3.md
			// (which names classIndex and a UID concept explicitly), NOT
			// read from the real Vst3ClassInfo definition -- Vst3Types.h
			// was not available when this was written. Confirm the exact
			// field names/types here before building; see the batch 2
			// continuation prompt's file list.
			// ---------------------------------------------------------
			if (info.isInstrument)
			{
				continue; // instrument-only classes stay on Prestige's browse-to-load path
			}

			EffectKey::AttributeMap am;
			am["file"]     = bundlePath;
			am["uid"]      = info.uid;                           // stable identity -- never the file path or class index alone
			am["classidx"] = QString::number(info.classIndex);   // cached hint only; re-resolved by UID at load time (see Vst3Effect::openPlugin), never trusted positionally
			am["vendor"]   = info.vendor;
			am["category"] = info.category;

			const QString displayName = info.name.isEmpty()
				? QFileInfo(bundlePath).completeBaseName()
				: info.name;

			kl.push_back(Key(desc, displayName, am));
		}
	}
}

} // namespace lmms
