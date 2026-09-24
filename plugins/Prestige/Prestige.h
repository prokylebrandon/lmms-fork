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
#include <QPointer>
#include <functional>
#include <memory>
#include <vector>

#include "Instrument.h"
#include "InstrumentView.h"
#include "SubWindow.h"
#include "Vst3PluginInstance.h"

class QLabel;
class QPushButton;
class QWidget;

namespace lmms
{

class Vst3ParameterModel;

namespace gui
{
class PrestigeView;
} // namespace gui


/**
 * @brief PRESTIGE instrument.
 *
 * Single Plugin::Type::Instrument descriptor with subPluginFeatures =
 * nullptr (browse-to-load, per the approved Phase 2 design decision).
 * Mirrors plugins/Vestige/Vestige.cpp's overall shape, adapted to build on
 * Vst3PluginInstance (Phase 1) instead of the out-of-process VstPlugin.
 *
 * Scope so far: load a bundle, resolve the first instrument-capable
 * class, play notes through it, save/restore state, show the plugin's
 * native editor (IPlugView), and expose one Vst3ParameterModel per VST3
 * parameter for LMMS automation (Phase 3). No parameter list UI yet, and
 * no plugin-replacement-without-unload path yet -- loadFile() and
 * loadSettings() still always tear down before loading, rather than
 * following Phase 3's full 9-step replacement sequence.
 */
class PrestigeInstrument : public Instrument
{
	Q_OBJECT
public:
	using EditorResizeCallback = Vst3PluginInstance::EditorResizeCallback;

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
	 * found; bundles exposing more than one get no selection UI yet
	 * (flagged, not solved, here).
	 *
	 * Overrides Plugin::loadFile (void return). On failure, lastError()
	 * holds a displayable message and isPluginLoaded() returns false.
	 */
	void loadFile(const QString& bundlePath) override;

	/// Stop and destroy the currently loaded plugin, if any. Safe to call
	/// with nothing loaded.
	void unloadPlugin();

	bool isPluginLoaded() const { return m_plugin != nullptr; }
	QString pluginName() const { return m_plugin ? m_plugin->name() : QString(); }
	QString pluginVendor() const { return m_plugin ? m_plugin->vendor() : QString(); }
	QString lastError() const { return m_lastError; }
	QString bundlePath() const { return m_bundlePath; }

	/// One model per VST3 parameter of the currently loaded plugin, in
	/// discovery order (NOT necessarily parameter-ID order). Empty when
	/// no plugin is loaded. For the Phase 3 parameter-management UI and
	/// for LMMS automation to attach to; see Vst3ParameterModel.
	const std::vector<std::unique_ptr<Vst3ParameterModel>>& parameterModels() const
	{
		return m_parameterModels;
	}

	// ---- Native editor (GUI thread only) ---------------------------------
	//
	// Thin wrappers over Vst3PluginInstance's editor API.  They deliberately
	// do NOT take m_pluginMutex: attaching a view can take a long time, and
	// holding the audio mutex for that long would make the audio thread skip
	// blocks.  Controller/view work is independent of the processor, and the
	// plugin is only ever destroyed on the GUI thread, so this is safe.

	bool createEditor(int* width, int* height, EditorResizeCallback onResize, QString* error);
	bool attachEditor(void* nativeParent, QString* error);
	void closeEditor();

signals:
	/// Emitted (GUI thread) just before the loaded plugin is destroyed, so any
	/// open editor window can detach its view and close first.
	void pluginAboutToClose();

private:
	/// Caller must already hold m_pluginMutex. Tears down m_plugin if one
	/// is loaded; does not touch m_bundlePath/m_classCid so callers can
	/// choose whether this is a full unload or a reload-in-place.
	void closePluginLocked();

	/**
	 * (Re)creates m_plugin from m_bundlePath.
	 *
	 * @param preferredCid  Class UID to resolve via discoverClasses(), as
	 *   recorded by a previous save (cid is the stable identity, never a
	 *   positional index). Empty for a fresh load from the browse dialog, in
	 *   which case the first instrument-capable class is picked
	 *   automatically.
	 * Caller must already hold m_pluginMutex.
	 */
	bool instantiatePlugin(const QString& preferredCid, QString& error);

	/// Builds m_parameterModels from m_plugin->parameters() and registers
	/// the plugin -> host edit callback. Caller must already hold
	/// m_pluginMutex, and m_plugin must already be set (called at the end
	/// of a successful instantiatePlugin()). m_parameterModels must be
	/// empty on entry -- closePluginLocked() guarantees this by tearing
	/// the previous set down first, which is also what keeps this from
	/// ever leaving a model pointed at a plugin that replaced it (Phase 3:
	/// audited stale-model source of crash-on-automate).
	void buildParameterModels();

	/// Detaches and destroys m_parameterModels, and clears the plugin's
	/// edit callback. Caller must already hold m_pluginMutex. Must run,
	/// in this order, before m_plugin itself is torn down (see
	/// closePluginLocked()): the callback is cleared while m_plugin is
	/// still valid, then every model is told its plugin is gone before
	/// any model is actually destroyed.
	void teardownParameterModels();

	/// Vst3PluginInstance::ParameterEditedCallback, registered on m_plugin
	/// by buildParameterModels(). Looks up the model by id() (VST3
	/// parameter ID, not position) and forwards via
	/// Vst3ParameterModel::setValueFromPlugin().
	void onPluginParameterEdited(Vst3ParamID id, double normalisedValue);

	QMutex m_pluginMutex;
	std::unique_ptr<Vst3PluginInstance> m_plugin;
	std::vector<std::unique_ptr<Vst3ParameterModel>> m_parameterModels;

	// Persisted identity. m_classCid is authoritative for project reload;
	// filenames/paths can move, the VST3 class UID shouldn't.
	QString m_bundlePath;
	QString m_classCid;

	QString m_lastError;

	friend class gui::PrestigeView;
};


namespace gui
{

/// View: load/browse control, name + vendor display, unload, native editor
/// toggle, error display.  No parameter list yet (Phase 3).
class PrestigeView : public InstrumentView
{
	Q_OBJECT
public:
	PrestigeView(Instrument* instrument, QWidget* parent);
	~PrestigeView() override;

protected slots:
	void browsePlugin();
	void unloadPlugin();
	void toggleEditor();

private:
	void updateLabels();
	void openEditorWindow();

	/// Detach the plugin view, then destroy the editor window.  Idempotent.
	/// Also connected to PrestigeInstrument::pluginAboutToClose.
	void closeEditorWindow();

	// QPointer: the instrument can be destroyed before its view when a track
	// is removed, and the editor window is destroyed via deleteLater().
	QPointer<PrestigeInstrument> m_pi;
	QPointer<SubWindow> m_editorWindow;
	QPointer<QWidget>   m_editorHost;

	QLabel* m_nameLabel;
	QLabel* m_vendorLabel;
	QLabel* m_errorLabel;
	QPushButton* m_browseButton;
	QPushButton* m_unloadButton;
	QPushButton* m_editorButton;
};

} // namespace gui

} // namespace lmms

#endif // LMMS_PRESTIGE_H
