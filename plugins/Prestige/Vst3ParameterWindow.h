/*
 * Vst3ParameterWindow.h - searchable VST3 parameter list + inspector for PRESTIGE
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

#ifndef LMMS_GUI_VST3_PARAMETER_WINDOW_H
#define LMMS_GUI_VST3_PARAMETER_WINDOW_H

#include <QMetaObject>
#include <QModelIndex>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <memory>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableView;
class QTimer;

namespace lmms
{

class FloatModel;
class Vst3ParameterModel;

namespace gui
{

class Vst3ParameterFilterProxy;
class Vst3ParameterKnob;
class Vst3ParameterTableModel;

/**
 * @brief Generic VST3 parameter management window.
 *
 * A complement to the plugin's own native editor, not a replacement for it:
 * it exists for inspection (name, plugin-formatted value, normalised value,
 * VST3 parameter ID, type/flags, automation status), fallback control for
 * plugins with no usable editor, and as the place where a parameter's
 * FloatModel gets a widget, which is what LMMS needs before a model can be
 * automated or connected to a controller.
 *
 * Layout:
 *   - a search box + scope filter over a table of every parameter. The
 *     table is a plain model/view, so a plugin with thousands of
 *     parameters costs rows, not widgets;
 *   - an inspector for the ONE selected parameter, with a real LMMS Knob
 *     bound to that parameter's Vst3ParameterModel::valueModel(). Right-
 *     clicking that knob gives the standard LMMS controller/automation
 *     menu.
 *
 * Deliberately independent of PrestigeInstrument: it is handed a list of
 * Vst3ParameterModel pointers and nothing else. Lifetime rule, enforced by
 * PrestigeView: the owner calls clearParameters() BEFORE the models are
 * destroyed (PrestigeInstrument::pluginAboutToClose), and setParameters()
 * again once a new set exists (PrestigeInstrument::parameterModelsChanged).
 * As a safety net every model reference held here is a QPointer, so a
 * model that dies anyway shows up as an empty row instead of a dangling
 * pointer.
 *
 * GUI thread only.
 */
class Vst3ParameterWindow : public QWidget
{
	Q_OBJECT
public:
	explicit Vst3ParameterWindow(QWidget* parent = nullptr);
	~Vst3ParameterWindow() override;

	/// Replace the parameters shown. @p emptyMessage is displayed above the
	/// table when @p models is empty (e.g. "No plugin loaded."). Any
	/// current selection is dropped and the inspector knob is unbound from
	/// its old model first.
	void setParameters(const std::vector<Vst3ParameterModel*>& models, const QString& emptyMessage);

	/// Same as setParameters() with an empty list.
	void clearParameters(const QString& emptyMessage);

protected:
	void showEvent(QShowEvent* event) override;
	void hideEvent(QHideEvent* event) override;

private slots:
	void onFilterChanged();
	void onCurrentRowChanged(const QModelIndex& current);
	void onTick();
	void resetSelected();
	void updateInspector();

private:
	void setSelected(Vst3ParameterModel* parameter);
	void updateCount();

	Vst3ParameterTableModel* m_tableModel = nullptr;
	Vst3ParameterFilterProxy* m_proxy = nullptr;

	QLineEdit* m_searchEdit = nullptr;
	QComboBox* m_scopeCombo = nullptr;
	QLabel* m_countLabel = nullptr;
	QLabel* m_messageLabel = nullptr;
	QTableView* m_view = nullptr;

	// Inspector
	Vst3ParameterKnob* m_knob = nullptr;
	QLabel* m_titleLabel = nullptr;
	QLabel* m_detailLabel = nullptr;
	QLabel* m_valueLabel = nullptr;
	QLabel* m_automationLabel = nullptr;
	QPushButton* m_resetButton = nullptr;

	QTimer* m_timer = nullptr;

	// The knob is never left without a model (some LMMS widgets assume a
	// model is always present), so when nothing is selected it is bound to
	// this inert placeholder instead of being unset.
	std::unique_ptr<FloatModel> m_placeholder;

	QPointer<Vst3ParameterModel> m_selected;
	QMetaObject::Connection m_valueConnection;
};

} // namespace gui

} // namespace lmms

#endif // LMMS_GUI_VST3_PARAMETER_WINDOW_H
