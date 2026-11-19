/*
 * Vst3ParameterGrid.h - scrollable grid of per-parameter knobs for the
 *                        VST3 effect control dialog
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

#ifndef LMMS_GUI_VST3_PARAMETER_GRID_H
#define LMMS_GUI_VST3_PARAMETER_GRID_H

#include <QPointer>
#include <QWidget>

#include <vector>

class QGridLayout;
class QLabel;
class QLineEdit;
class QScrollArea;
class QPushButton;

namespace lmms
{
struct Vst3Parameter;
class Vst3ParameterModel;
}

namespace lmms::gui
{

/**
 * PRESTIGE-Phase-4c, Task B: a scrollable grid of real LMMS Knob widgets,
 * one per exposed VST3 parameter, bound directly to each parameter's
 * Vst3ParameterModel::valueModel() via the normal Knob::setModel()/
 * AutomatableModelView path -- so right-click "Automate" / "Connect to
 * controller", drag-to-automation-clip and the value display all work
 * through LMMS's existing mechanism, not anything custom here (requirement
 * 1 of Task B).
 *
 * This is a *sibling* of Prestige's Vst3ParameterWindow
 * (plugins/Prestige/Vst3ParameterWindow.h), not a reuse of it: that class
 * shows a searchable table plus ONE inspector knob for whichever row is
 * selected; this class shows MANY knobs at once, arranged in a grid, which
 * is what Task B's requirement 1 ("a scrollable grid of ... Knob widgets")
 * asks for specifically. Both duplicate a fair amount of small helper code
 * (title/value/type formatting) rather than share it, per Task B
 * requirement 8: Vst3Base must not gain a GUI dependency, and Prestige
 * doesn't depend on Vst3Effect or vice versa, so there is no existing
 * shared location for either side to put a common helper without inventing
 * one nobody asked for. Left as documented duplication, same as the
 * editor-hosting code Vst3EffectControlDialog.cpp already duplicates from
 * PrestigeView for the identical reason.
 *
 * Lazy creation (Task B requirement 4): a real Knob is a nontrivial
 * widget (its own paint cache, drag/wheel/context-menu handling, an
 * AutomatableModelView subscription). Building hundreds of them up front
 * for a plugin with a large parameter set is exactly the "dialog stalls
 * on open" failure Task B warns about, so this class creates a Knob only
 * for a parameter that is both (a) currently passing the search filter
 * and (b) within the currently-shown page. Scrolling to the bottom of a
 * long filtered list, or widening the filter, reveals more rows by
 * extending the page rather than by rebuilding everything already shown.
 * The chosen limit is kInitialPageSize parameters shown at once, grown by
 * kPageGrowth each time "Show more" is used or the scrollbar nears the
 * bottom -- see the .cpp for the exact numbers and the reasoning for
 * choosing them.
 *
 * Filtering matches the parameter's title, unit string, and numeric VST3
 * ID (decimal or 0x-prefixed hex) against the search box, case-
 * insensitively -- deliberately similar to Vst3ParameterFilterProxy in
 * Prestige's window, but reimplemented rather than shared for the same
 * reason as the rest of this class.
 *
 * Flag handling (Task B requirement 2), decided per-parameter from
 * Vst3Parameter's own flags, not inferred:
 *   - isHidden        -> not shown at all, not even behind the filter.
 *   - isReadOnly       -> shown, disabled (Knob::setEnabled(false)), and
 *                        because it is disabled it is not draggable, but
 *                        it is NOT excluded from automation/controller
 *                        connection -- that is a property of the
 *                        underlying FloatModel and Vst3ParameterModel
 *                        itself already refuses to forward a write for a
 *                        read-only parameter (see Vst3ParameterModel.h),
 *                        so this widget does not need its own guard for
 *                        that; disabling is purely so the user isn't
 *                        invited to drag a knob that can't move.
 *   - isProgramChange  -> NOT skipped: the "firm rule" in
 *                        PRESTIGE-Phase-4c-State-Fix-and-Knobs.md section 1
 *                        ("every plugin parameter is exposed") and
 *                        Vst3ParameterWindow's own precedent (it shows
 *                        isProgramChange parameters, just labelled) both
 *                        argue against skipping it silently. Shown as a
 *                        normal stepped knob with a "(program)" suffix on
 *                        its label so the distinction from an ordinary
 *                        stepped parameter is visible without a tooltip.
 *   - isBypass         -> Task B's requirement 2 asks for a checkbox.
 *                        Vst3ParameterModel only exposes a FloatModel (see
 *                        this class's .cpp for the header read confirming
 *                        that), and the natural checkbox widget for a
 *                        bool-shaped control in LMMS, LedCheckBox
 *                        (AutomatableButton), binds to a BoolModel-family
 *                        model -- but AutomatableButton.h was not part of
 *                        this session's uploaded files (see
 *                        PRESTIGE-Phase-4c-State-Fix-and-Knobs.md's ground
 *                        rules: "ask for it by name before writing code
 *                        that depends on it"), so its setModel() contract
 *                        was not verified and no bridging model is built
 *                        against it here. isBypass parameters are shown as
 *                        a stepped Knob instead (VST3 bypass is normalised
 *                        [0,1], almost always stepCount 1, i.e. exactly two
 *                        positions), with a "(bypass)" label suffix, which
 *                        is fully functional -- click/drag toggles it, it
 *                        automates and connects like any other parameter
 *                        -- just not literally a checkbox widget. Swapping
 *                        in a real LedCheckBox is a follow-up once
 *                        AutomatableButton.h's model API is confirmed; not
 *                        done here rather than guessed.
 *   - stepCount > 0    -> the underlying FloatModel is already constructed
 *                        with a matching step size (see
 *                        Vst3ParameterModel.cpp's constructor), so a plain
 *                        Knob bound to it already snaps to the discrete
 *                        values on drag; no separate spin-box widget type
 *                        is used. This satisfies requirement 2's "use a
 *                        stepped control ... when stepCount > 0" with the
 *                        model doing the stepping rather than the widget.
 *
 * Threading (Task B requirement 6): every method here runs on the GUI
 * thread only, same as the models and Knob widgets it touches. Nothing
 * here is called from, or adds locking visible to, the audio path.
 *
 * Lifetime: this widget does not own the Vst3ParameterModel objects it
 * points at (Vst3EffectControls does, via its m_parameterModels vector,
 * per Vst3EffectControls.h's ownership comment) -- every reference is kept
 * as a QPointer for the same reason Vst3ParameterWindow uses QPointer, and
 * setParameters()/clear() is how the owning dialog tells this widget the
 * set changed or is about to be torn down.
 */
class Vst3ParameterGrid : public QWidget
{
	Q_OBJECT
public:
	explicit Vst3ParameterGrid(QWidget* parent = nullptr);
	~Vst3ParameterGrid() override;

	/// Replace the parameters shown. Safe to call with the same vector
	/// contents repeatedly (e.g. nothing to do until the plugin loads);
	/// safe to call with an empty vector, which shows @p emptyMessage
	/// instead of a grid.
	void setParameters(const std::vector<Vst3ParameterModel*>& models, const QString& emptyMessage);

	/// Equivalent to setParameters({}, emptyMessage) -- call this BEFORE
	/// the models are destroyed (matches Vst3ParameterWindow::
	/// clearParameters()'s documented ordering requirement, for the same
	/// reason: every Knob this widget owns must be destroyed, or at least
	/// unbound via unsetModel(), before the FloatModel it points at goes
	/// away).
	void clearParameters(const QString& emptyMessage);

private slots:
	void onFilterChanged();
	void onShowMoreClicked();
	void onScrollBarValueChanged(int value);

private:
	struct Row
	{
		QPointer<Vst3ParameterModel> model;
		int originalIndex = 0; // discovery order, for stable non-filtered layout
	};

	void rebuildVisibleRows();  // recompute m_filteredRows from m_allRows + filter text
	void relayoutPage();        // (re)create Knob widgets for the current page only
	void growPage();            // extend m_pageSize and call relayoutPage()
	bool matchesFilter(const Vst3Parameter& info, const QString& needleLower) const;
	QWidget* buildKnobCell(Vst3ParameterModel* model);

	std::vector<Row> m_allRows;       // every non-hidden parameter, discovery order
	std::vector<Row> m_filteredRows;  // m_allRows after the current search filter

	int m_pageSize = 0; // how many of m_filteredRows currently have a live Knob

	QLineEdit* m_searchEdit = nullptr;
	QLabel* m_countLabel = nullptr;
	QLabel* m_messageLabel = nullptr;
	QScrollArea* m_scrollArea = nullptr;
	QWidget* m_gridHost = nullptr;
	QGridLayout* m_gridLayout = nullptr;
	QPushButton* m_showMoreButton = nullptr;
};

} // namespace lmms::gui

#endif // LMMS_GUI_VST3_PARAMETER_GRID_H
