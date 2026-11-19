/*
 * Vst3EffectControlDialog.h - dialog for displaying VST3 effect controls
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

#ifndef LMMS_GUI_VST3_EFFECT_CONTROL_DIALOG_H
#define LMMS_GUI_VST3_EFFECT_CONTROL_DIALOG_H

#include <QPointer>

#include "EffectControlDialog.h"

class QLabel;
class QPushButton;

namespace lmms
{

class Vst3EffectControls;
class Vst3ParameterModel;

namespace gui
{

// ASSUMPTION FLAG (Task 3, batch 2): forward-declared in the lmms::gui
// namespace on the strength of every other GUI class seen so far living
// there (EffectControlDialog, PluginView, etc.) -- SubWindow.h itself was
// not attached this session, so this is inferred from usage, not
// confirmed against its own header. If it actually lives in plain
// lmms:: or the global namespace, this is a one-line fix, not a silent
// behavioural risk.
class SubWindow;
class Vst3ParameterGrid;

/**
 * PRESTIGE-Phase-4c, Task B: the per-parameter knob grid
 * (Vst3ParameterGrid, see its own header) is now built here and kept in
 * sync with Vst3EffectControls::parameterModels(), closing the gap batch
 * 2's note label used to describe ("Per-parameter automation models exist
 * ... but no knob controls are shown here yet"). onPluginParameterEdited()
 * -> setValueFromPlugin()'s existing feedback-loop guard
 * (Vst3ParameterModel::m_settingFromPlugin) is untouched -- the grid only
 * ever reads models the same way any other AutomatableModelView-based
 * widget would, via Knob::setModel(), so it needs no new synchronization
 * path of its own (Task B requirement 5).
 *
 * The editor-hosting mechanism (openEditorWindow()/closeEditorWindow(),
 * the EditorCloseFilter/EditorHostCursorFilter helpers in the .cpp) is a
 * deliberate duplicate of gui::PrestigeView's identical code, not a
 * promotion into a shared location -- see the .cpp's anonymous namespace
 * comment for why. Task B requirement 7 keeps this toggle and its
 * behavior unchanged; the grid is added alongside it, not instead of it.
 */
class Vst3EffectControlDialog : public EffectControlDialog
{
	Q_OBJECT
public:
	explicit Vst3EffectControlDialog(Vst3EffectControls* controls);
	~Vst3EffectControlDialog() override;

private slots:
	void toggleEditor();

private:
	//! userInitiated distinguishes an explicit button click (worth a
	//! warning dialog on failure) from a silent background attempt --
	//! mirrors gui::PrestigeView::openEditorWindow() exactly, minus the
	//! UI-state restore-on-project-load path: Vst3Effect has no
	//! equivalent of PrestigeInstrument's editorWanted()/
	//! uiRestoreRequested() yet, since the Task 4 state scheme in the
	//! continuation prompt has no <ui> node for the effect side (unlike
	//! Prestige's). If Phase 5 wants "editor was open" to survive a
	//! project reload for effects too, that is new scope, not something
	//! silently assumed here.
	void openEditorWindow(bool userInitiated);
	void closeEditorWindow();

	Vst3EffectControls* m_controls;

	QPointer<QWidget> m_editorHost;
	QPointer<SubWindow> m_editorWindow;
	QPushButton* m_editorButton = nullptr;

	QLabel* m_infoLabel;

	//! Task B: the scrollable per-parameter knob grid. Populated from
	//! m_controls->parameterModels() once, in the constructor -- like
	//! Vst3EffectControls itself (see its header's ownership comment),
	//! this dialog's plugin is fixed for its whole lifetime, so unlike
	//! Prestige's Vst3ParameterWindow there is no later
	//! setParameters()/clearParameters() call needed for a plugin swap
	//! that can never happen here.
	//!
	//! It IS cleared once more, though: this dialog's lifetime is
	//! independent of Vst3EffectControls'/Vst3Effect's (createView()
	//! hands ownership to EffectView, a separate GUI object -- see
	//! EffectView.cpp -- not tied to the Effect's own destruction), so if
	//! the effect is removed from the chain while this dialog is still
	//! open, Vst3EffectControls::teardownParameterModels() can run before
	//! this dialog is destroyed. Left unguarded, every Knob in m_grid
	//! would be pointing at a freed Vst3ParameterModel/FloatModel. See
	//! Vst3EffectControls::parameterModelsAboutToClear() and this file's
	//! constructor for the signal connection that calls
	//! m_grid->clearParameters() before that teardown proceeds.
	Vst3ParameterGrid* m_grid = nullptr;
};

} // namespace gui
} // namespace lmms

#endif // LMMS_GUI_VST3_EFFECT_CONTROL_DIALOG_H
