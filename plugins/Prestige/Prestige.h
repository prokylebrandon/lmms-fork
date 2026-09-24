/*
 * Prestige.h - instrument for hosting VST3 plugins (browse-to-load)
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

#ifndef LMMS_PRESTIGE_H
#define LMMS_PRESTIGE_H

#include <QMutex>
#include <memory>

#include "Instrument.h"
#include "InstrumentView.h"
#include "Vst3PluginInstance.h"

class QLabel;
class QPushButton;

namespace lmms
{

namespace gui
{
class PrestigeView;
} // namespace gui


/**
 * @brief Stage 1 PRESTIGE instrument.
 *
 * Single Plugin::Type::Instrument descriptor with subPluginFeatures =
 * nullptr (browse-to-load, per the approved Phase 2 design decision).
 * Mirrors plugins/Vestige/Vestige.cpp's overall shape, adapted to build on
 * Vst3PluginInstance (Phase 1) instead of the out-of-process VstPlugin.
 *
 * Stage 1 scope: load a bundle, resolve the first instrument-capable
 * class, play notes through it, save/restore state. No native editor
 * (IPlugView) and no per-parameter automation UI yet — those are later
 * stages of this same phase, added on top of this file without changing
 * its shape.
 */
class PrestigeInstrument : public Instrument
{
	Q_OBJECT
public:
	explicit PrestigeInstrument(InstrumentTrack* instrumentTrack);
	~PrestigeInstrument() override;

	void play(SampleFrame* workingBuffer) override;

	void saveSettings(QDomDocument& doc, QDomElement& parent) override;
	void loadSettings(const QDomElement& thisElement) override;

	QString nodeName() const override;

	bool handleMidiEvent(const MidiEvent& event, const TimePos& time, f_cnt_t offset = 0) override;

	gui::PluginView* instantiateView(QWidget* parent) override;

	/**
	 * Load a .vst3 bundle (single-file or bundle-directory form) selected
	 * via the browse dialog. Picks the first instrument-capable class
	 * found; bundles exposing more than one get no selection UI in Stage 1
	 * (that's flagged, not solved, here — see the prompt's note that a
	 * multi-instrument bundle needs a real picker eventually).
	 *
	 * Returns true on success. On failure, lastError() holds a displayable
	 * message and no plugin is loaded.
	 */
	bool loadFile(const QString& bundlePath);

	/// Stop and destroy the currently loaded plugin, if any. Safe to call
	/// with nothing loaded.
	void unloadPlugin();

	bool isPluginLoaded() const { return m_plugin != nullptr; }
	QString pluginName() const { return m_plugin ? m_plugin->name() : QString(); }
	QString pluginVendor() const { return m_plugin ? m_plugin->vendor() : QString(); }
	QString lastError() const { return m_lastError; }
	QString bundlePath() const { return m_bundlePath; }

private:
	/// Caller must already hold m_pluginMutex. Tears down m_plugin if one
	/// is loaded; does not touch m_bundlePath/m_classCid so callers can
	/// choose whether this is a full unload or a reload-in-place.
	void closePluginLocked();

	/**
	 * (Re)creates m_plugin from m_bundlePath.
	 *
	 * @param preferredCid  Class UID to resolve via discoverClasses(), as
	 *   recorded by a previous save (see doc handoff notes: cid is the
	 *   stable identity, never a positional index). Empty for a fresh
	 *   load from the browse dialog, in which case the first
	 *   instrument-capable class is picked automatically.
	 * Caller must already hold m_pluginMutex.
	 */
	bool instantiatePlugin(const QString& preferredCid, QString& error);

	QMutex m_pluginMutex;
	std::unique_ptr<Vst3PluginInstance> m_plugin;

	// Persisted identity. m_classCid is authoritative for project reload;
	// filenames/paths can move, the VST3 class UID shouldn't.
	QString m_bundlePath;
	QString m_classCid;

	QString m_lastError;

	friend class gui::PrestigeView;
};


namespace gui
{

/// Stage 1 view: load/browse control, name + vendor display, unload,
/// error display. No editor toggle and no parameter list yet (later
/// stages of this phase).
class PrestigeView : public InstrumentView
{
	Q_OBJECT
public:
	PrestigeView(Instrument* instrument, QWidget* parent);
	~PrestigeView() override = default;

protected slots:
	void browsePlugin();
	void unloadPlugin();

private:
	void updateLabels();

	PrestigeInstrument* m_pi;

	QLabel* m_nameLabel;
	QLabel* m_vendorLabel;
	QLabel* m_errorLabel;
	QPushButton* m_browseButton;
	QPushButton* m_unloadButton;
};

} // namespace gui

} // namespace lmms

#endif // LMMS_PRESTIGE_H
