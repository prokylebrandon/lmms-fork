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

/**
 * Task 3 (batch 2) adds the native editor toggle. Per-parameter knob
 * controls are still NOT shown here -- Task 2 built the parameter MODELS
 * (Vst3EffectControls::parameterModels()), but neither Task 2 nor Task 3
 * asked for a knob UI to interact with them, unlike Prestige's separate
 * Vst3ParameterWindow on the instrument side. Flagged in the constructor's
 * note label and the batch 2 handoff notes -- until that gap is closed,
 * "automate at least one parameter" (the batch's acceptance criteria) may
 * not be reachable purely through this dialog.
 *
 * The editor-hosting mechanism (openEditorWindow()/closeEditorWindow(),
 * the EditorCloseFilter/EditorHostCursorFilter helpers in the .cpp) is a
 * deliberate duplicate of gui::PrestigeView's identical code, not a
 * promotion into a shared location -- see the .cpp's anonymous namespace
 * comment for why.
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
	QLabel* m_noteLabel;
};

} // namespace gui
} // namespace lmms

#endif // LMMS_GUI_VST3_EFFECT_CONTROL_DIALOG_H
