/*
 * Vst3ParameterGrid.cpp - scrollable grid of per-parameter knobs for the
 *                          VST3 effect control dialog
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

#include "Vst3ParameterGrid.h"

#include <algorithm>

#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>

#include "Knob.h"
#include "Vst3ParameterModel.h"

namespace lmms::gui
{

namespace
{

// See Vst3ParameterGrid.h's class comment for the reasoning behind these
// two numbers. kInitialPageSize: large enough that a small-to-medium
// plugin (the common case -- tens of parameters) shows everything at once
// with no extra click, small enough that even a slow debug build creating
// kInitialPageSize real Knob widgets does not create a visible stall when
// the dialog first opens. kPageGrowth: how many more Knobs "Show more" (or
// scrolling near the bottom) creates at a time for a plugin above that
// size -- deliberately the same order of magnitude as the initial page so
// growing doesn't itself become a second stall.
constexpr int kInitialPageSize = 64;
constexpr int kPageGrowth = 64;

QString trText(const char* text)
{
	return QObject::tr(text);
}

QString parameterLabel(const Vst3Parameter& info)
{
	QString base = info.title.isEmpty()
		? trText("Parameter %1").arg(static_cast<qulonglong>(info.id))
		: info.title;

	// See the header comment on isBypass/isProgramChange handling: both
	// are shown as a normal (stepped) Knob, not a different widget type,
	// so the label suffix is what actually distinguishes them at a
	// glance.
	if (info.isBypass) { base += trText(" (bypass)"); }
	if (info.isProgramChange) { base += trText(" (program)"); }
	return base;
}

QString idText(Vst3ParamID id)
{
	const auto value = static_cast<qulonglong>(id);
	return QStringLiteral("%1 (0x%2)").arg(value).arg(QString::number(value, 16).toUpper());
}

} // namespace

// -----------------------------------------------------------------------
// A Knob whose floating value text is the plugin's own formatted string
// ("-6.0 dB", "440 Hz") instead of the bare normalised 0..1 number --
// satisfies Task B requirement 3. Identical in spirit to
// Vst3ParameterWindow's own Vst3ParameterKnob (Prestige side); not shared,
// per this file's header comment on why the two sides duplicate small
// helpers rather than promoting them.
// -----------------------------------------------------------------------
class Vst3GridKnob : public Knob
{
public:
	Vst3GridKnob(QWidget* parent, Vst3ParameterModel* parameter) :
		Knob(KnobType::Bright26, parameterLabel(parameter->info()), parent,
		     Knob::LabelRendering::WidgetFont),
		m_parameter(parameter)
	{
		setHintText(trText("Value:"), QString());
		setToolTip(trText("%1\nID %2").arg(parameterLabel(parameter->info()), idText(parameter->info().id)));
	}

protected:
	QString currentValueToText() override
	{
		if (!m_parameter) { return {}; }
		const QString text = m_parameter->formattedValue();
		return text.isEmpty() ? Knob::currentValueToText() : text;
	}

private:
	QPointer<Vst3ParameterModel> m_parameter;
};

Vst3ParameterGrid::Vst3ParameterGrid(QWidget* parent) :
	QWidget(parent)
{
	auto* outer = new QVBoxLayout(this);
	outer->setContentsMargins(4, 4, 4, 4);

	// --- search row ---
	auto* topRow = new QHBoxLayout;
	m_searchEdit = new QLineEdit(this);
	m_searchEdit->setPlaceholderText(trText("Search parameter name, unit or ID"));
	m_searchEdit->setClearButtonEnabled(true);
	connect(m_searchEdit, &QLineEdit::textChanged, this, &Vst3ParameterGrid::onFilterChanged);

	m_countLabel = new QLabel(this);
	topRow->addWidget(m_searchEdit, 1);
	topRow->addWidget(m_countLabel);
	outer->addLayout(topRow);

	m_messageLabel = new QLabel(this);
	m_messageLabel->setWordWrap(true);
	m_messageLabel->hide();
	outer->addWidget(m_messageLabel);

	// --- scrollable grid ---
	m_gridHost = new QWidget;
	m_gridLayout = new QGridLayout(m_gridHost);
	m_gridLayout->setAlignment(Qt::AlignTop | Qt::AlignLeft);

	m_scrollArea = new QScrollArea(this);
	m_scrollArea->setWidget(m_gridHost);
	m_scrollArea->setWidgetResizable(true);
	m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	outer->addWidget(m_scrollArea, 1);

	connect(m_scrollArea->verticalScrollBar(), &QScrollBar::valueChanged,
	        this, &Vst3ParameterGrid::onScrollBarValueChanged);

	// --- "show more" affordance for a page smaller than the filtered set ---
	m_showMoreButton = new QPushButton(this);
	m_showMoreButton->hide();
	connect(m_showMoreButton, &QPushButton::clicked, this, &Vst3ParameterGrid::onShowMoreClicked);
	outer->addWidget(m_showMoreButton);

	clearParameters(trText("No VST3 effect loaded."));
}

Vst3ParameterGrid::~Vst3ParameterGrid() = default;

void Vst3ParameterGrid::setParameters(const std::vector<Vst3ParameterModel*>& models, const QString& emptyMessage)
{
	m_allRows.clear();
	m_allRows.reserve(models.size());
	int idx = 0;
	for (auto* m : models)
	{
		if (!m) { continue; }
		// Task B requirement 2: isHidden parameters are skipped entirely,
		// not even reachable through the search filter -- see the header
		// comment for why this differs from Prestige's "show behind a
		// toggle" choice.
		if (m->info().isHidden) { ++idx; continue; }
		m_allRows.push_back(Row{ QPointer<Vst3ParameterModel>(m), idx });
		++idx;
	}

	m_messageLabel->setVisible(m_allRows.empty());
	if (m_allRows.empty())
	{
		m_messageLabel->setText(emptyMessage);
	}

	m_pageSize = 0;
	rebuildVisibleRows();
}

void Vst3ParameterGrid::clearParameters(const QString& emptyMessage)
{
	setParameters({}, emptyMessage);
}

void Vst3ParameterGrid::onFilterChanged()
{
	m_pageSize = 0;
	rebuildVisibleRows();
}

bool Vst3ParameterGrid::matchesFilter(const Vst3Parameter& info, const QString& needleLower) const
{
	if (needleLower.isEmpty()) { return true; }
	if (info.title.toLower().contains(needleLower)) { return true; }
	if (info.units.toLower().contains(needleLower)) { return true; }

	const auto idValue = static_cast<qulonglong>(info.id);
	if (QString::number(idValue).contains(needleLower)) { return true; }
	if (QString::number(idValue, 16).contains(needleLower, Qt::CaseInsensitive)) { return true; }

	return false;
}

void Vst3ParameterGrid::rebuildVisibleRows()
{
	const QString needleLower = m_searchEdit->text().trimmed().toLower();

	m_filteredRows.clear();
	m_filteredRows.reserve(m_allRows.size());
	for (const auto& row : m_allRows)
	{
		if (!row.model) { continue; } // model destroyed out from under us; skip defensively
		if (matchesFilter(row.model->info(), needleLower))
		{
			m_filteredRows.push_back(row);
		}
	}

	m_countLabel->setText(trText("%1 of %2").arg(m_filteredRows.size()).arg(m_allRows.size()));

	if (m_pageSize <= 0)
	{
		m_pageSize = std::min<int>(kInitialPageSize, static_cast<int>(m_filteredRows.size()));
	}
	m_pageSize = std::min<int>(m_pageSize, static_cast<int>(m_filteredRows.size()));

	relayoutPage();
}

void Vst3ParameterGrid::relayoutPage()
{
	// Clear every existing cell widget. Deleting a Knob here is safe
	// regardless of the model's own lifetime -- Qt widget destruction
	// only disconnects the Knob's own signal/slot connections and does
	// not reach into the model it was pointed at (see this class's
	// header comment on lifetime/ownership).
	QLayoutItem* item = nullptr;
	while ((item = m_gridLayout->takeAt(0)) != nullptr)
	{
		delete item->widget();
		delete item;
	}

	// A 4-columns-per-row layout keeps individual cells a readable width
	// without the grid becoming a single very tall column for a plugin
	// with, say, 20 parameters -- matches the general shape of
	// LadspaControlDialog's own grid (columns chosen from the control
	// count there; fixed here since knob cells are a consistent size and
	// a fixed column count makes the "Show more" growth predictable).
	constexpr int kColumns = 4;

	const int shown = std::min<int>(m_pageSize, static_cast<int>(m_filteredRows.size()));
	for (int i = 0; i < shown; ++i)
	{
		Vst3ParameterModel* model = m_filteredRows[static_cast<std::size_t>(i)].model;
		if (!model) { continue; }
		QWidget* cell = buildKnobCell(model);
		m_gridLayout->addWidget(cell, i / kColumns, i % kColumns);
	}

	const bool hasMore = shown < static_cast<int>(m_filteredRows.size());
	m_showMoreButton->setVisible(hasMore);
	if (hasMore)
	{
		m_showMoreButton->setText(
			trText("Show %1 more (of %2 remaining)")
				.arg(std::min<int>(kPageGrowth, static_cast<int>(m_filteredRows.size()) - shown))
				.arg(static_cast<int>(m_filteredRows.size()) - shown));
	}
}

QWidget* Vst3ParameterGrid::buildKnobCell(Vst3ParameterModel* model)
{
	const Vst3Parameter& info = model->info();

	auto* cell = new QWidget(m_gridHost);
	auto* layout = new QVBoxLayout(cell);
	layout->setContentsMargins(2, 2, 2, 2);
	layout->setAlignment(Qt::AlignHCenter);

	auto* knob = new Vst3GridKnob(cell, model);
	knob->setModel(model->valueModel());

	// Task B requirement 2, read-only: shown, disabled, so the user isn't
	// invited to drag a knob that can't move. Disabling also blocks the
	// context menu's automate/connect entries, which is the outcome
	// requirement 2 wants for read-only anyway.
	//
	// Task B requirement 2, isAutomatable == false ("not automatable"):
	// this is NOT separately enforced here, and that is a real gap, not
	// an oversight papered over -- checked AutomatableModel.h and Model.h
	// directly (Task B requirement 1's "first read ... to see what
	// accessor to bind to" extended to this question too) and neither
	// has a per-model "refuse automation/controller connection" flag;
	// isAutomated() means "currently has an automation clip", not "is
	// allowed to have one". So a Knob bound to a FloatModel always offers
	// the normal automate/connect context menu regardless of
	// info.isAutomatable, and nothing in Vst3ParameterModel enforces the
	// flag either (grepped for isAutomatable there: it is stored in
	// Vst3Parameter and never read). Actually enforcing this needs either
	// a new AutomatableModel-level capability (affects every model in
	// LMMS, well beyond this task's scope) or a Vst3Effect-local
	// subclass of Knob that overrides the context-menu construction,
	// which AutomatableModelView does not currently expose a seam for
	// either (not confirmed without reading AutomatableModelView.cpp,
	// which was not part of this session's files). Left unenforced and
	// reported in the handoff note rather than silently assumed away.
	knob->setEnabled(!info.isReadOnly);

	layout->addWidget(knob, 0, Qt::AlignHCenter);
	cell->setLayout(layout);
	return cell;
}

void Vst3ParameterGrid::growPage()
{
	if (m_pageSize >= static_cast<int>(m_filteredRows.size())) { return; }
	m_pageSize = std::min<int>(m_pageSize + kPageGrowth, static_cast<int>(m_filteredRows.size()));
	relayoutPage();
}

void Vst3ParameterGrid::onShowMoreClicked()
{
	growPage();
}

void Vst3ParameterGrid::onScrollBarValueChanged(int value)
{
	// Auto-grow the page once the user scrolls within one step of the
	// bottom, so "Show more" is a fallback for someone who prefers
	// clicking, not the only way to reach the rest of a long filtered
	// list.
	QScrollBar* bar = m_scrollArea->verticalScrollBar();
	if (bar && value >= bar->maximum() - bar->singleStep())
	{
		growPage();
	}
}

} // namespace lmms::gui
