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
#include <QString>
#include <functional>
#include <memory>
#include <vector>

#include "Instrument.h"
#include "InstrumentView.h"
#include "SubWindow.h"
#include "Vst3ParameterWindow.h"
#include "Vst3PluginInstance.h"

class QLabel;
class QPushButton;
class QSpinBox;
class QTimer;
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
 * Scope so far: load a bundle, resolve an instrument-capable class, play
 * notes through it, show the plugin's native editor (IPlugView), expose one
 * Vst3ParameterModel per VST3 parameter for LMMS automation, a searchable
 * parameter list/inspector window, versioned project state, and graceful
 * handling of a plugin that is missing when a project is opened.
 *
 * Phase 3 Part 2 progress: closePluginLocked() now sends an explicit
 * NoteOff for every note it believes is held before tearing a plugin down
 * (see Vst3PluginInstance::flushActiveNotes()), covering both plain unload
 * and in-place replacement, since both already funnelled through the one
 * function. Component/controller state is stored as two separate pieces
 * when the plugin genuinely exposes them as separate objects (see
 * Vst3PluginInstance::hasSeparateControllerState()), one combined blob
 * otherwise, same as before.
 *
 * Still to come: presets, the crash-containment audit, cross-platform
 * grep, the test suite, and docs/handoff notes -- see
 * PRESTIGE-Phase-3-Part-2.md's punch list for the full, current state of
 * each item.
 *
 * Saved project format (see saveSettings()/loadSettings()):
 *
 *   <prestige version="2" bundlepath=".." classcid=".." pluginname=".."
 *             pluginvendor="..">
 *     <state format="combined">base64</state>
 *       -- OR, when the plugin has a genuinely separate controller object --
 *     <state format="separate">
 *       <component>base64</component>
 *       <controller>base64</controller>
 *     </state>
 *     <parameters><param id=".." ../>...</parameters>   host-side, by ID
 *     <ui editor="0|1" parameters="0|1"/>       UI state
 *   </prestige>
 *
 * Each of those is its own piece of saved data. A project saved before
 * versioning existed has no "version" attribute and is read as version 0,
 * which has the same layout minus name/vendor/ui (and, since it predates
 * the "format" attribute too, is always the "combined" shape). A version-1
 * project (Part 1 / early Part 2) always has format="combined" as well --
 * "separate" only appears from this build onward, which is why it needed
 * version bumped to 2: an older PRESTIGE build (kSaveVersion 1, no format
 * check before reading <state>'s text) would otherwise misread a
 * "separate" node's two child elements as one garbled combined blob
 * instead of refusing the file. See applySavedElement()'s comment on the
 * "format" attribute for the read side of this.
 */
class PrestigeInstrument : public Instrument
{
	Q_OBJECT
public:
	using EditorResizeCallback = Vst3PluginInstance::EditorResizeCallback;

	/// Version written to the "version" attribute of every saved PRESTIGE
	/// element. Bump it whenever the saved layout changes in a way an older
	/// PRESTIGE could misread; loadSettings() refuses (without altering the
	/// data) anything newer than this.
	///
	/// 1 -> 2 (Phase 3 Part 2): <state> can now be format="separate" (two
	/// child elements) instead of always one combined text node -- see the
	/// class comment above and applySavedElement()'s "format" handling.
	static constexpr int kSaveVersion = 2;

	/// Why there is (or is not) a running plugin. Anything other than Empty
	/// and Loaded means a plugin is recorded in the project but could not be
	/// brought up; lastError() then holds a user-facing message.
	enum class LoadStatus
	{
		Empty,              ///< nothing loaded, nothing recorded
		Loaded,             ///< plugin loaded and running
		Missing,            ///< recorded bundle path does not exist
		LoadFailed,         ///< path exists but the plugin could not be loaded
		ClassNotFound,      ///< bundle loads, but the recorded class UID is not in it
		NoInstrument,       ///< bundle has no instrument-capable class
		InitFailed,         ///< plugin loaded but its processor could not be started
		UnsupportedVersion  ///< project saved by a newer PRESTIGE; left untouched
	};

	explicit PrestigeInstrument(InstrumentTrack* instrumentTrack);
	~PrestigeInstrument() override;

	void play(SampleFrame* workingBuffer) override;
	void playNote(NotePlayHandle* n, SampleFrame* workingBuffer) override;

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
	 * If a plugin is recorded in the project but currently missing, and
	 * the chosen bundle contains the recorded class UID, the project's saved
	 * settings for it are applied to the newly loaded plugin ("relink").
	 * Choosing a different plugin discards those saved settings.
	 *
	 * Overrides Plugin::loadFile (void return). On failure, lastError()
	 * holds a displayable message and isPluginLoaded() returns false.
	 */
	void loadFile(const QString& bundlePath) override;

	/// Stop and destroy the currently loaded plugin, if any, and forget
	/// everything recorded about it (including retained settings of a
	/// missing plugin). Safe to call with nothing loaded.
	void unloadPlugin();

	bool isPluginLoaded() const { return m_plugin != nullptr; }
	QString pluginName() const { return m_plugin ? m_plugin->name() : m_savedName; }
	QString pluginVendor() const { return m_plugin ? m_plugin->vendor() : m_savedVendor; }
	QString bundlePath() const { return m_bundlePath; }

	LoadStatus status() const { return m_status; }

	/// User-facing message for the current failure, or empty. Technical
	/// detail (VST3 result codes, exception text) goes to the debug log,
	/// never here.
	QString lastError() const { return m_lastError; }

	/// User-facing, non-fatal problem (the plugin is running, but e.g. its
	/// saved state could not be restored). Empty when there is none.
	QString warning() const { return m_warning; }

	/// A plugin is recorded (bundle path known), whether or not it loaded.
	bool hasSavedPlugin() const { return !m_bundlePath.isEmpty(); }

	/// The project's saved settings for a plugin that failed to load are
	/// being kept verbatim so they survive re-saving, and can be reapplied
	/// if the plugin turns up again.
	bool hasRetainedState() const { return !m_retainedXml.isEmpty(); }

	QString savedPluginName() const { return m_savedName; }
	QString savedPluginVendor() const { return m_savedVendor; }

	/// Per-note pitch-bend range in semitones. Used only when per-note bend goes out as pitch bend on
	/// per-note MIDI channels (MPE style) because the plugin has no tuning note expression. It must
	/// equal the range the plugin applies to those channels (PRESTIGE cannot read it). The plugin
	/// also needs its own MPE option switched on by the user. Saved as the optional project
	/// attribute "bendrange"; projects without it load with the default.
	static constexpr int kBendRangeMin = 1;
	static constexpr int kBendRangeMax = 96;
	static constexpr int kBendRangeDefault = 2;
	int bendRange() const { return m_bendRange.load(std::memory_order_relaxed); }
	void setBendRange(int semitones) { m_bendRange.store(qBound(kBendRangeMin, semitones, kBendRangeMax), std::memory_order_relaxed); }

	/// One model per VST3 parameter of the currently loaded plugin, in
	/// discovery order (NOT necessarily parameter-ID order). Empty when
	/// no plugin is loaded. For the parameter-management UI and for LMMS
	/// automation to attach to; see Vst3ParameterModel.
	const std::vector<std::unique_ptr<Vst3ParameterModel>>& parameterModels() const
	{
		return m_parameterModels;
	}

	// ---- UI state (persisted; GUI thread) ---------------------------------
	//
	// "Wanted" flags are what the user last chose, not whether a window
	// exists right now: a view can be destroyed and rebuilt without the
	// user having closed anything.
	bool editorWanted() const { return m_uiEditorOpen; }
	void setEditorWanted(bool wanted) { m_uiEditorOpen = wanted; }
	bool parametersWindowWanted() const { return m_uiParametersOpen; }
	void setParametersWindowWanted(bool wanted) { m_uiParametersOpen = wanted; }

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
	/// open editor / parameter window can let go of it first. Parameter
	/// models are still alive when this fires.
	void pluginAboutToClose();

	/// Emitted (GUI thread) after the set of parameter models changed: a
	/// plugin finished loading, or everything was unloaded. Listeners
	/// re-read parameterModels().
	void parameterModelsChanged();

	/// Emitted (GUI thread) after status(), lastError(), warning() or the
	/// recorded plugin identity changed, so the view can refresh its labels.
	void pluginStatusChanged();

	/// Emitted (GUI thread) after a project's saved UI state was read, so
	/// the view can reopen the windows the user had open.
	void uiRestoreRequested();

private:
	/// Caller must already hold m_pluginMutex. Tears down m_plugin if one
	/// is loaded; does not touch m_bundlePath/m_classCid so callers can
	/// choose whether this is a full unload or a reload-in-place.
	void closePluginLocked();

	/// Caller must already hold m_pluginMutex. Forgets every recorded
	/// identity/status/retained/UI field.
	void resetRecordLocked();

	/**
	 * (Re)creates m_plugin from m_bundlePath.
	 *
	 * @param preferredCid  Class UID to resolve via discoverClasses(), as
	 *   recorded by a previous save (cid is the stable identity, never a
	 *   positional index). If given it is matched STRICTLY: a bundle that no
	 *   longer contains it fails with LoadStatus::ClassNotFound instead of
	 *   silently substituting another class. Empty for a fresh load from the
	 *   browse dialog, in which case the first instrument-capable class is
	 *   picked automatically.
	 *
	 * Caller must already hold m_pluginMutex. On failure sets m_status and
	 * m_lastError (user-facing) and logs the technical detail; on success
	 * sets m_status = Loaded and the saved name/vendor, and builds the
	 * parameter models.
	 */
	bool instantiatePlugin(const QString& preferredCid);

	/// Records a failure. Always returns false so callers can `return fail(...)`.
	bool fail(LoadStatus status, const QString& userMessage);

	/// Applies a saved <prestige> element's plugin state, parameter block
	/// and UI flags to the just-loaded plugin. Caller holds m_pluginMutex;
	/// m_plugin must be set.
	void applySavedElement(const QDomElement& element);

	/// Keeps a verbatim copy of @p element so a plugin that could not be
	/// loaded does not cost the project its saved data.
	void retainElement(const QDomElement& element);

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

	/// Plugin reported (restartComponent(kParamValuesChanged)) that it changed
	/// parameter values itself, e.g. by loading one of its own presets.
	/// Re-reads every model from the plugin; never writes back to it.
	void onPluginParametersChanged();

	/// Caller holds m_pluginMutex. Forget all per-note routing state (channels, owners, bend slots).
	void resetNoteRoutingLocked();
	/// Caller holds m_pluginMutex. Sends a MIDI event to the plugin; in MPE mode note events get a per-note channel.
	void routeMidiEventLocked(const MidiEvent& event, f_cnt_t offset);
	/// Caller holds m_pluginMutex. Picks a member channel 1..15 for a new note.
	int allocateMpeChannelLocked();
	/// Caller holds m_pluginMutex. Sends one note's bend to the plugin (tuning expression or per-note channel).
	void forwardNoteBendLocked(int key, float semitones);

	QMutex m_pluginMutex;

	// Per-note pitch bend in semitones, indexed by the MIDI key the plugin received (= VST3 noteId).
	// playNote() (audio thread, no lock) writes m_pendingBend; play() (under m_pluginMutex)
	// forwards changes to the plugin and remembers what it sent in m_sentBend.
	std::atomic<float> m_pendingBend[MidiMaxKey + 1] {};
	float m_sentBend[MidiMaxKey + 1] {};

	// Per-note bend routing. m_mpeBend is decided once after load (instantiatePlugin()): true when the
	// plugin has no tuning note expression but maps pitch bend per channel. Then every note gets its own
	// MIDI channel 1..15 (channel 0 stays the MPE master) and its bend goes out as pitch bend on it.
	bool m_mpeBend = false;
	std::atomic<int> m_bendRange { kBendRangeDefault };
	// Per MIDI key: channel + 1 the held note on it was sent on (0 = key not held). Written under m_pluginMutex, read by playNote().
	std::atomic<int> m_keyChannelPlus1[MidiMaxKey + 1] {};
	// Per MIDI key: the NotePlayHandle that owns the bend slot (the newest note on the key, bound in its first
	// playNote()). Only compared, never dereferenced.
	std::atomic<const NotePlayHandle*> m_keyOwner[MidiMaxKey + 1] {};
	// Channel table, guarded by m_pluginMutex. Stamp = last allocate/release time, used for least-recently-used choice.
	bool m_channelHeld[MidiChannelCount] {};
	bool m_channelBent[MidiChannelCount] {};
	int m_channelKey[MidiChannelCount] {};
	std::uint64_t m_channelStamp[MidiChannelCount] {};
	std::uint64_t m_channelClock = 0;
	QTimer* m_outputTimer = nullptr; // GUI-thread poll of plugin output values (meters, process() failures)
	QMetaObject::Connection m_rateConnection; // AudioEngine::sampleRateChanged -> reconfigure the plugin
	std::unique_ptr<Vst3PluginInstance> m_plugin;
	std::vector<std::unique_ptr<Vst3ParameterModel>> m_parameterModels;

	// Persisted identity. m_classCid is authoritative for project reload;
	// filenames/paths can move, the VST3 class UID shouldn't. Name/vendor
	// are kept so a missing plugin can still be named to the user.
	QString m_bundlePath;
	QString m_classCid;
	QString m_savedName;
	QString m_savedVendor;

	LoadStatus m_status = LoadStatus::Empty;
	QString m_lastError;
	QString m_warning;

	// Verbatim XML of the <prestige> element as loaded, kept only while a
	// recorded plugin is not running (missing, failed, or saved by a newer
	// PRESTIGE). saveSettings() writes it back unchanged in that case.
	QString m_retainedXml;

	// Persisted UI state.
	bool m_uiEditorOpen = false;
	bool m_uiParametersOpen = false;

	friend class gui::PrestigeView;
};


namespace gui
{

/// View: load/browse control, name + vendor display, unload, native editor
/// toggle, parameter window toggle, error/warning display.
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
	void toggleParameters();

private slots:
	void onPluginAboutToClose();
	void onParameterModelsChanged();
	void onRestoreUi();

private:
	void updateLabels();

	/// @param userInitiated  true when the user asked for the window: only
	///   then are failures reported in a dialog. A window being restored
	///   from saved UI state fails silently.
	void openEditorWindow(bool userInitiated);

	/// Detach the plugin view, then destroy the editor window.  Idempotent.
	/// Does NOT change the persisted "editor wanted" flag: the window can be
	/// torn down by an unload or a view rebuild without the user closing it.
	void closeEditorWindow();

	void openParametersWindow();

	/// Hands the parameter window the instrument's current models.
	void refreshParameterWindow();

	// QPointer: the instrument can be destroyed before its view when a track
	// is removed, and the editor window is destroyed via deleteLater().
	QPointer<PrestigeInstrument> m_pi;
	QPointer<SubWindow> m_editorWindow;
	QPointer<QWidget>   m_editorHost;
	QPointer<SubWindow> m_paramsWindow;
	QPointer<Vst3ParameterWindow> m_paramsWidget;

	QLabel* m_nameLabel;
	QLabel* m_vendorLabel;
	QLabel* m_warningLabel;
	QLabel* m_errorLabel;
	QPushButton* m_browseButton;
	QPushButton* m_unloadButton;
	QPushButton* m_editorButton;
	QPushButton* m_paramsButton;
QSpinBox* m_bendRangeSpin = nullptr;
};

} // namespace gui

} // namespace lmms

#endif // LMMS_PRESTIGE_H
