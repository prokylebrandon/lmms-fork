/*
 * Vst3ParameterWindow.cpp - searchable VST3 parameter list + inspector for PRESTIGE
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

#include "Vst3ParameterWindow.h"

#include <QAbstractTableModel>
#include <QCheckBox>
#include <QComboBox>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QShowEvent>
#include <QSortFilterProxyModel>
#include <QStringList>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include "AutomatableModel.h"
#include "Knob.h"
#include "Vst3ParameterModel.h"

namespace lmms::gui
{

namespace
{

// These helpers are shared by the table and the inspector so both always
// describe a parameter the same way. Two different numbers are shown on
// purpose and never merged: the plugin-FORMATTED value ("440 Hz", produced
// by the plugin's own controller) and the NORMALISED value (the 0..1 number
// that VST3 actually transports and that LMMS automation edits).

QString trText(const char* text)
{
	return QObject::tr(text);
}

QString parameterTitle(const Vst3Parameter& info)
{
	return info.title.isEmpty()
		? trText("Parameter %1").arg(static_cast<qulonglong>(info.id))
		: info.title;
}

QString idText(Vst3ParamID id)
{
	const auto value = static_cast<qulonglong>(id);
	return QStringLiteral("%1 (0x%2)").arg(value).arg(QString::number(value, 16).toUpper());
}

QString typeText(const Vst3Parameter& info)
{
	QStringList parts;

	// VST3 stepCount: 0 = continuous, 1 = on/off toggle, N = N+1 discrete
	// values. Stepped parameters are labelled as such and the inspector knob
	// is quantised to the same steps (see Vst3ParameterModel), so they are
	// never presented as continuous.
	if (info.stepCount <= 0)
	{
		parts << trText("Continuous");
	}
	else if (info.stepCount == 1)
	{
		parts << trText("Toggle");
	}
	else
	{
		parts << trText("Stepped (%1 values)").arg(info.stepCount + 1);
	}

	if (info.isReadOnly) { parts << trText("read-only"); }
	if (!info.isAutomatable) { parts << trText("not automatable"); }
	if (info.isBypass) { parts << trText("bypass"); }
	if (info.isProgramChange) { parts << trText("program change"); }

	return parts.join(QStringLiteral(", "));
}

QString normalisedText(const Vst3ParameterModel& parameter)
{
	return QString::number(parameter.valueModel()->value(), 'f', 3);
}

QString formattedText(const Vst3ParameterModel& parameter)
{
	const QString text = parameter.formattedValue();
	return text.isEmpty() ? normalisedText(parameter) : text;
}

// Reuses the convention LMMS already has: a model is "automated" when an
// automation clip drives it, and "controlled" when a controller connection
// (LFO, MIDI CC, peak controller, ...) is attached.
QString automationText(const Vst3ParameterModel& parameter)
{
	const FloatModel* model = parameter.valueModel();
	const bool automated = model->isAutomated();
	const bool controlled = model->controllerConnection() != nullptr;

	if (automated && controlled) { return trText("Automation clip + controller"); }
	if (automated) { return trText("Automation clip"); }
	if (controlled) { return trText("Controller"); }
	return trText("None");
}

} // namespace


// ---------------------------------------------------------------------------
// Table model
// ---------------------------------------------------------------------------

/// One row per parameter, in the plugin's own discovery order. Holds
/// QPointers, never owning pointers: PrestigeInstrument owns the models.
class Vst3ParameterTableModel : public QAbstractTableModel
{
public:
	enum Column
	{
		ColName,
		ColValue,
		ColNormalised,
		ColId,
		ColType,
		ColAutomation,
		ColumnCount
	};

	using QAbstractTableModel::QAbstractTableModel;

	void setParameters(std::vector<QPointer<Vst3ParameterModel>> parameters)
	{
		beginResetModel();
		m_parameters = std::move(parameters);
		endResetModel();
	}

	Vst3ParameterModel* parameterAt(int row) const
	{
		if (row < 0 || row >= static_cast<int>(m_parameters.size()))
		{
			return nullptr;
		}
		return m_parameters[static_cast<std::size_t>(row)].data();
	}

	/// Re-read everything that can change without the table being told:
	/// formatted/normalised values (automation, plugin-side edits) and the
	/// automation status. Called from a slow timer while the window is
	/// visible; the view only re-queries the rows actually on screen.
	void refreshValues()
	{
		if (m_parameters.empty())
		{
			return;
		}
		const int last = static_cast<int>(m_parameters.size()) - 1;
		emit dataChanged(index(0, ColValue), index(last, ColAutomation), {Qt::DisplayRole, Qt::UserRole});
	}

	int rowCount(const QModelIndex& parent = QModelIndex()) const override
	{
		return parent.isValid() ? 0 : static_cast<int>(m_parameters.size());
	}

	int columnCount(const QModelIndex& parent = QModelIndex()) const override
	{
		return parent.isValid() ? 0 : ColumnCount;
	}

	QVariant headerData(int section, Qt::Orientation orientation, int role) const override
	{
		if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
		{
			return {};
		}
		switch (section)
		{
			case ColName: return trText("Name");
			case ColValue: return trText("Value");
			case ColNormalised: return trText("Normalised");
			case ColId: return trText("ID");
			case ColType: return trText("Type");
			case ColAutomation: return trText("Automation");
			default: break;
		}
		return {};
	}

	QVariant data(const QModelIndex& index, int role) const override
	{
		if (!index.isValid())
		{
			return {};
		}
		const Vst3ParameterModel* parameter = parameterAt(index.row());
		if (!parameter)
		{
			return {};
		}
		const Vst3Parameter& info = parameter->info();

		switch (role)
		{
			case Qt::DisplayRole:
				switch (index.column())
				{
					case ColName: return parameterTitle(info);
					case ColValue: return formattedText(*parameter);
					case ColNormalised: return normalisedText(*parameter);
					case ColId: return idText(info.id);
					case ColType: return typeText(info);
					case ColAutomation: return automationText(*parameter);
					default: break;
				}
				break;

			case Qt::ToolTipRole:
				if (index.column() == ColName)
				{
					QString tip = parameterTitle(info);
					if (!info.shortTitle.isEmpty() && info.shortTitle != info.title)
					{
						tip += QStringLiteral("\n") + trText("Short name: %1").arg(info.shortTitle);
					}
					if (!info.units.isEmpty())
					{
						tip += QStringLiteral("\n") + trText("Units: %1").arg(info.units);
					}
					return tip;
				}
				if (index.column() == ColNormalised)
				{
					return trText("The 0..1 value VST3 transports and LMMS automation edits. "
						"The plugin's own display of the value is in the Value column.");
				}
				if (index.column() == ColId)
				{
					return trText("VST3 parameter ID (stable identity, used in saved projects)");
				}
				break;

			case Qt::TextAlignmentRole:
				if (index.column() == ColNormalised || index.column() == ColId)
				{
					return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
				}
				return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);

			case Qt::UserRole: // sort key: numeric columns sort as numbers, not text
				switch (index.column())
				{
					case ColName: return parameterTitle(info);
					case ColValue:
					case ColNormalised: return static_cast<double>(parameter->valueModel()->value());
					case ColId: return static_cast<qulonglong>(info.id);
					case ColType: return typeText(info);
					case ColAutomation: return automationText(*parameter);
					default: break;
				}
				break;

			default:
				break;
		}
		return {};
	}

private:
	std::vector<QPointer<Vst3ParameterModel>> m_parameters;
};


// ---------------------------------------------------------------------------
// Filter proxy
// ---------------------------------------------------------------------------

/// Search + scope filter. The search text is split on whitespace and every
/// word must match somewhere in the parameter's name, short name, units,
/// or ID (decimal or 0x-hex), case-insensitively.
class Vst3ParameterFilterProxy : public QSortFilterProxyModel
{
public:
	enum class Scope
	{
		All = 0,
		Writable = 1,
		Automated = 2
	};

	using QSortFilterProxyModel::QSortFilterProxyModel;

	void setSearchText(const QString& text)
	{
		m_words = text.toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
		invalidateFilter();
	}

	void setScope(Scope scope)
	{
		m_scope = scope;
		invalidateFilter();
	}

	/// Hidden parameters (Vst3Parameter::isHidden, from VST3's kIsHidden
	/// flag) are excluded unless this is true. Off by default, per the
	/// Part 2 prompt's "hide such parameters by default with a 'show
	/// hidden' option".
	void setShowHidden(bool show)
	{
		m_showHidden = show;
		invalidateFilter();
	}

	/// Re-evaluate the filter (automation status can change without the
	/// filter being touched). Only ever called on user actions / show, never
	/// from the refresh timer, because the Automated scope is not free.
	///
	/// Two earlier fixes both tried to force this by driving the PROXY's
	/// own reset/invalidation primitives directly: first invalidateFilter()
	/// alone, then invalidateFilter() wrapped in this proxy's own
	/// beginResetModel()/endResetModel(). Both were confirmed (by the user,
	/// against the real binary) NOT to fix the table failing to update on
	/// browse or unload.
	///
	/// The likely reason: beginResetModel()/endResetModel() called directly
	/// on a QSortFilterProxyModel -- rather than being triggered reactively
	/// off the SOURCE model's own reset, which is the path Qt's proxy
	/// implementation actually wires up to rebuild its internal
	/// row-to-source mapping -- only emit the reset signals to attached
	/// views; they do not by themselves rebuild the proxy's own mapping.
	/// invalidateFilter() does rebuild that mapping, but nesting it *inside*
	/// an explicit beginResetModel()/endResetModel() pair on the same model
	/// (as the second attempt did) is not a normal, well-tested combination
	/// either, and evidently did not reliably produce a working rebuild.
	///
	/// This instead detaches and reattaches the source model.
	/// QAbstractProxyModel::setSourceModel() is the ordinary, heavily-used
	/// path Qt itself relies on whenever a proxy's data source is swapped:
	/// it emits the correct reset signals to every attached view AND
	/// unconditionally rebuilds the row mapping from scratch, so correctness
	/// here does not depend on the interaction between invalidateFilter()
	/// and manually-driven reset calls that the two earlier attempts ran
	/// into.
	void refresh()
	{
		QAbstractItemModel* source = sourceModel();
		setSourceModel(nullptr);
		setSourceModel(source);
	}

protected:
	bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override
	{
		Q_UNUSED(sourceParent)

		const auto* source = static_cast<const Vst3ParameterTableModel*>(sourceModel());
		const Vst3ParameterModel* parameter = source->parameterAt(sourceRow);
		if (!parameter)
		{
			return false;
		}
		const Vst3Parameter& info = parameter->info();

		if (info.isHidden && !m_showHidden)
		{
			return false;
		}
		if (m_scope == Scope::Writable && info.isReadOnly)
		{
			return false;
		}
		if (m_scope == Scope::Automated && !parameter->valueModel()->isAutomatedOrControlled())
		{
			return false;
		}

		if (m_words.isEmpty())
		{
			return true;
		}

		const auto id = static_cast<qulonglong>(info.id);
		const QString haystack = (info.title + QLatin1Char(' ') + info.shortTitle + QLatin1Char(' ')
			+ info.units + QLatin1Char(' ') + QString::number(id) + QLatin1Char(' ')
			+ QStringLiteral("0x") + QString::number(id, 16)).toLower();

		for (const QString& word : m_words)
		{
			if (!haystack.contains(word))
			{
				return false;
			}
		}
		return true;
	}

private:
	QStringList m_words;
	Scope m_scope = Scope::All;
	bool m_showHidden = false; // hidden parameters excluded by default
};


// ---------------------------------------------------------------------------
// Inspector knob
// ---------------------------------------------------------------------------

/// A normal LMMS Knob whose floating value text shows the plugin's own
/// formatted string ("440 Hz") instead of a bare 0..1 number. Everything
/// else (drag, wheel, context menu, controller connection, automation) is
/// stock LMMS behaviour on the parameter's FloatModel.
class Vst3ParameterKnob : public Knob
{
public:
	explicit Vst3ParameterKnob(QWidget* parent) :
		Knob(KnobType::Bright26, parent)
	{
	}

	void setParameter(Vst3ParameterModel* parameter) { m_parameter = parameter; }

protected:
	QString currentValueToText() override
	{
		if (!m_parameter)
		{
			return {};
		}
		return formattedText(*m_parameter);
	}

private:
	QPointer<Vst3ParameterModel> m_parameter;
};


// ---------------------------------------------------------------------------
// Vst3ParameterWindow
// ---------------------------------------------------------------------------

Vst3ParameterWindow::Vst3ParameterWindow(QWidget* parent) :
	QWidget(parent)
{
	setMinimumSize(460, 380);
	resize(720, 540);

	m_placeholder = std::make_unique<FloatModel>(0.0f, 0.0f, 1.0f, 0.001f);

	m_tableModel = new Vst3ParameterTableModel(this);
	m_proxy = new Vst3ParameterFilterProxy(this);
	m_proxy->setSourceModel(m_tableModel);
	m_proxy->setSortRole(Qt::UserRole);
	m_proxy->setSortCaseSensitivity(Qt::CaseInsensitive);
	// The table refreshes values on a timer; re-sorting/re-filtering on
	// every one of those would make rows jump under the user's cursor.
	m_proxy->setDynamicSortFilter(false);

	auto* layout = new QVBoxLayout(this);

	// --- search / filter row ---
	auto* topRow = new QHBoxLayout;
	m_searchEdit = new QLineEdit(this);
	m_searchEdit->setPlaceholderText(tr("Search name, unit or ID"));
	m_searchEdit->setClearButtonEnabled(true);

	m_scopeCombo = new QComboBox(this);
	m_scopeCombo->addItem(tr("All parameters"));
	m_scopeCombo->addItem(tr("Writable only"));
	m_scopeCombo->addItem(tr("Automated or controlled only"));

	m_showHiddenCheck = new QCheckBox(tr("Show hidden"), this);
	m_showHiddenCheck->setToolTip(
		tr("Also list parameters the plugin marked as hidden from a generic host UI."));

	m_countLabel = new QLabel(this);

	topRow->addWidget(m_searchEdit, 1);
	topRow->addWidget(m_scopeCombo);
	topRow->addWidget(m_showHiddenCheck);
	topRow->addWidget(m_countLabel);
	layout->addLayout(topRow);

	m_messageLabel = new QLabel(this);
	m_messageLabel->setWordWrap(true);
	m_messageLabel->hide();
	layout->addWidget(m_messageLabel);

	// --- table ---
	m_view = new QTableView(this);
	m_view->setModel(m_proxy);
	m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_view->setSelectionMode(QAbstractItemView::SingleSelection);
	m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
	m_view->setAlternatingRowColors(true);
	m_view->setWordWrap(false);
	m_view->verticalHeader()->hide();
	m_view->setSortingEnabled(true);
	// Start in the plugin's own order; clicking a header sorts.
	m_view->sortByColumn(-1, Qt::AscendingOrder);

	auto* header = m_view->horizontalHeader();
	header->setSectionResizeMode(Vst3ParameterTableModel::ColName, QHeaderView::Stretch);
	header->setSectionResizeMode(Vst3ParameterTableModel::ColValue, QHeaderView::Interactive);
	header->setSectionResizeMode(Vst3ParameterTableModel::ColNormalised, QHeaderView::Interactive);
	header->setSectionResizeMode(Vst3ParameterTableModel::ColId, QHeaderView::Interactive);
	header->setSectionResizeMode(Vst3ParameterTableModel::ColType, QHeaderView::Interactive);
	header->setSectionResizeMode(Vst3ParameterTableModel::ColAutomation, QHeaderView::Interactive);
	m_view->setColumnWidth(Vst3ParameterTableModel::ColValue, 110);
	m_view->setColumnWidth(Vst3ParameterTableModel::ColNormalised, 80);
	m_view->setColumnWidth(Vst3ParameterTableModel::ColId, 130);
	m_view->setColumnWidth(Vst3ParameterTableModel::ColType, 170);
	m_view->setColumnWidth(Vst3ParameterTableModel::ColAutomation, 130);

	layout->addWidget(m_view, 1);

	// --- inspector ---
	auto* inspector = new QHBoxLayout;

	m_knob = new Vst3ParameterKnob(this);
	m_knob->setModel(m_placeholder.get());
	m_knob->setEnabled(false);
	inspector->addWidget(m_knob, 0, Qt::AlignTop);

	auto* text = new QVBoxLayout;
	m_titleLabel = new QLabel(this);
	QFont titleFont = m_titleLabel->font();
	titleFont.setBold(true);
	m_titleLabel->setFont(titleFont);
	m_detailLabel = new QLabel(this);
	m_valueLabel = new QLabel(this);
	m_automationLabel = new QLabel(this);
	auto* hint = new QLabel(tr("Right-click the knob for LMMS controller connection options."), this);
	hint->setWordWrap(true);
	text->addWidget(m_titleLabel);
	text->addWidget(m_detailLabel);
	text->addWidget(m_valueLabel);
	text->addWidget(m_automationLabel);
	text->addWidget(hint);
	inspector->addLayout(text, 1);

	m_resetButton = new QPushButton(tr("Reset to default"), this);
	inspector->addWidget(m_resetButton, 0, Qt::AlignTop);
	layout->addLayout(inspector);

	// --- wiring ---
	connect(m_searchEdit, &QLineEdit::textChanged, this, &Vst3ParameterWindow::onFilterChanged);
	connect(m_scopeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
		this, &Vst3ParameterWindow::onFilterChanged);
	connect(m_showHiddenCheck, &QCheckBox::toggled, this, &Vst3ParameterWindow::onFilterChanged);
	connect(m_view->selectionModel(), &QItemSelectionModel::currentRowChanged,
		this, &Vst3ParameterWindow::onCurrentRowChanged);
	connect(m_resetButton, &QPushButton::clicked, this, &Vst3ParameterWindow::resetSelected);

	m_timer = new QTimer(this);
	m_timer->setInterval(250);
	connect(m_timer, &QTimer::timeout, this, &Vst3ParameterWindow::onTick);

	setSelected(nullptr);
	updateCount();
}

Vst3ParameterWindow::~Vst3ParameterWindow()
{
	if (m_valueConnection)
	{
		QObject::disconnect(m_valueConnection);
	}

	// Destroy the knob while the placeholder model (a member, destroyed
	// after this body) is still alive, so the knob is never bound to a
	// model that is already gone.
	delete m_knob;
	m_knob = nullptr;
}

void Vst3ParameterWindow::setParameters(const std::vector<Vst3ParameterModel*>& models, const QString& emptyMessage)
{
	// Unbind the knob from the old selection FIRST: the models it points at
	// may be about to be destroyed by whoever called us.
	setSelected(nullptr);

	std::vector<QPointer<Vst3ParameterModel>> rows;
	rows.reserve(models.size());
	for (auto* model : models)
	{
		rows.emplace_back(model);
	}
	m_tableModel->setParameters(std::move(rows));

	// The proxy has setDynamicSortFilter(false), which stops it from
	// automatically re-running filterAcceptsRow() when the source model
	// emits modelReset, so its row mapping does not update on its own just
	// because the source above changed. refresh() forces that rebuild -- see
	// its own comment for why this now goes through detaching/reattaching
	// the source model rather than driving the proxy's reset/invalidation
	// primitives directly (the two things tried before this, in that order,
	// were confirmed against the real binary NOT to fix the table failing
	// to update on browse or unload).
	m_proxy->refresh();

	m_messageLabel->setText(emptyMessage);
	m_messageLabel->setVisible(models.empty() && !emptyMessage.isEmpty());

	updateCount();
}

void Vst3ParameterWindow::clearParameters(const QString& emptyMessage)
{
	setParameters(std::vector<Vst3ParameterModel*>{}, emptyMessage);
}

void Vst3ParameterWindow::showEvent(QShowEvent* event)
{
	QWidget::showEvent(event);

	// Values and automation status may have changed while hidden.
	m_proxy->refresh();
	m_tableModel->refreshValues();
	updateInspector();
	updateCount();
	m_timer->start();
}

void Vst3ParameterWindow::hideEvent(QHideEvent* event)
{
	QWidget::hideEvent(event);
	m_timer->stop();
}

void Vst3ParameterWindow::onFilterChanged()
{
	m_proxy->setSearchText(m_searchEdit->text());
	m_proxy->setScope(static_cast<Vst3ParameterFilterProxy::Scope>(m_scopeCombo->currentIndex()));
	m_proxy->setShowHidden(m_showHiddenCheck->isChecked());
	updateCount();
}

void Vst3ParameterWindow::onCurrentRowChanged(const QModelIndex& current)
{
	Vst3ParameterModel* parameter = nullptr;
	if (current.isValid())
	{
		parameter = m_tableModel->parameterAt(m_proxy->mapToSource(current).row());
	}
	setSelected(parameter);
}

void Vst3ParameterWindow::onTick()
{
	m_tableModel->refreshValues();
	updateInspector();
}

void Vst3ParameterWindow::resetSelected()
{
	if (m_selected && !m_selected->info().isReadOnly)
	{
		m_selected->valueModel()->reset();
	}
}

void Vst3ParameterWindow::setSelected(Vst3ParameterModel* parameter)
{
	if (m_valueConnection)
	{
		QObject::disconnect(m_valueConnection);
		m_valueConnection = QMetaObject::Connection();
	}

	m_selected = parameter;

	if (parameter)
	{
		const Vst3Parameter& info = parameter->info();
		m_knob->setParameter(parameter);
		m_knob->setModel(parameter->valueModel());
		m_knob->setDescription(parameterTitle(info));
		// Read-only parameters (meters, mostly) are shown but not editable;
		// Vst3ParameterModel also refuses to forward writes to them.
		m_knob->setEnabled(!info.isReadOnly);
		m_resetButton->setEnabled(!info.isReadOnly);

		m_valueConnection = connect(parameter->valueModel(), &FloatModel::dataChanged,
			this, &Vst3ParameterWindow::updateInspector);
	}
	else
	{
		m_knob->setParameter(nullptr);
		m_knob->setModel(m_placeholder.get());
		m_knob->setEnabled(false);
		m_resetButton->setEnabled(false);
	}

	updateInspector();
}

void Vst3ParameterWindow::updateInspector()
{
	if (!m_selected)
	{
		m_titleLabel->setText(tr("No parameter selected"));
		m_detailLabel->clear();
		m_valueLabel->clear();
		m_automationLabel->clear();
		return;
	}

	const Vst3Parameter& info = m_selected->info();

	m_titleLabel->setText(info.units.isEmpty()
		? parameterTitle(info)
		: tr("%1 (%2)").arg(parameterTitle(info), info.units));
	m_detailLabel->setText(tr("ID %1 - %2").arg(idText(info.id), typeText(info)));
	m_valueLabel->setText(tr("Value: %1    Normalised: %2")
		.arg(formattedText(*m_selected), normalisedText(*m_selected)));
	m_automationLabel->setText(tr("Automation: %1").arg(automationText(*m_selected)));
}

void Vst3ParameterWindow::updateCount()
{
	m_countLabel->setText(tr("%1 of %2").arg(m_proxy->rowCount()).arg(m_tableModel->rowCount()));
}

} // namespace lmms::gui