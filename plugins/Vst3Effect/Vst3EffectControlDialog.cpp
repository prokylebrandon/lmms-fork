/*
 * Vst3EffectControlDialog.cpp - dialog for displaying VST3 effect controls
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

#include "Vst3EffectControlDialog.h"

#include <vector>

#include <QEvent>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "GuiApplication.h"
#include "MainWindow.h"
#include "SubWindow.h"
#include "Vst3Effect.h"
#include "Vst3EffectControls.h"
#include "Vst3ParameterGrid.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace lmms::gui
{

namespace
{

// Duplicated from gui::PrestigeView (Prestige.cpp), not promoted, per
// Task 3's decision documented in this file's header: these depend on
// SubWindow, core LMMS GUI infrastructure Vst3Base deliberately has no
// dependency on (see Vst3ParameterModel.h's own comment on the Task 2
// promotion it DID make, for contrast). Small, self-contained, and
// Prestige-independent, so duplicating ~50 lines here is the more honest
// choice than reaching a plugin-hosting library into LMMS's window
// management, or inventing a new shared header nobody asked for.

/// Watches a SubWindow for its Close event so a callback can run BEFORE the
/// window goes away (detach the plugin view first). Never consumes the
/// event: normal close handling still runs afterwards.
class EditorCloseFilter : public QObject
{
public:
	EditorCloseFilter(QObject* parent, std::function<void()> onClose) :
		QObject(parent),
		m_onClose(std::move(onClose))
	{
	}

protected:
	bool eventFilter(QObject* watched, QEvent* event) override
	{
		if (event->type() == QEvent::Close && m_onClose)
		{
			m_onClose();
		}
		return QObject::eventFilter(watched, event);
	}

private:
	std::function<void()> m_onClose;
};

/// Works around a resize-cursor-gets-stuck bug specific to the editor
/// window -- see gui::PrestigeView's identical class for the full
/// explanation (native HWND boundary intercepting Qt's hover-leave
/// tracking on the SubWindow). Fix: watch for QEvent::Enter on the native
/// host and force the SubWindow's cursor back to normal.
class EditorHostCursorFilter : public QObject
{
public:
	EditorHostCursorFilter(QObject* parent, QPointer<SubWindow> window) :
		QObject(parent),
		m_window(std::move(window))
	{
	}

protected:
	bool eventFilter(QObject* watched, QEvent* event) override
	{
		if (event->type() == QEvent::Enter && m_window)
		{
			m_window->unsetCursor();
		}
		return QObject::eventFilter(watched, event);
	}

private:
	QPointer<SubWindow> m_window;
};

/// IPlugView sizes are physical pixels on Windows/Linux but logical units
/// on macOS. Without this, on a scaled Windows display the plugin would
/// draw into only part of an oversized frame.
QSize toWidgetSize(const QWidget* reference, int viewWidth, int viewHeight)
{
#ifdef __APPLE__
	Q_UNUSED(reference)
	return QSize(viewWidth, viewHeight);
#else
	const qreal ratio = reference->devicePixelRatioF();
	return QSize(qRound(viewWidth / ratio), qRound(viewHeight / ratio));
#endif
}

} // namespace

Vst3EffectControlDialog::Vst3EffectControlDialog(Vst3EffectControls* controls) :
	EffectControlDialog(controls),
	m_controls(controls)
{
	auto* layout = new QVBoxLayout(this);

	Vst3Effect* effect = controls != nullptr ? controls->vst3Effect() : nullptr;
	const bool loaded = effect != nullptr && effect->pluginInstance() != nullptr;
	const QString title = loaded
		? tr("%1 (%2)").arg(effect->pluginInstance()->name(), effect->pluginInstance()->vendor())
		: tr("VST3 effect (not loaded)");

	m_infoLabel = new QLabel(title, this);
	m_infoLabel->setWordWrap(true);
	layout->addWidget(m_infoLabel);

	// Fixed compact sizing borrowed from VstEffectControlDialog's own
	// toggle-button convention (Task 3, batch 2) -- see this file's
	// header comment: only the button's presentation is reused, not its
	// underlying showUI()/hideUI() mechanism, which doesn't apply to a
	// VST3 IPlugView.
	m_editorButton = new QPushButton(tr("Show editor"), this);
	m_editorButton->setEnabled(loaded);
	m_editorButton->setMinimumWidth(78);
	m_editorButton->setMaximumWidth(78);
	m_editorButton->setMinimumHeight(24);
	m_editorButton->setMaximumHeight(24);
	connect(m_editorButton, &QPushButton::clicked, this, &Vst3EffectControlDialog::toggleEditor);
	layout->addWidget(m_editorButton);

	// Task B: the per-parameter knob grid, populated once from the
	// models Vst3EffectControls::buildParameterModels() already built.
	// Unlike Prestige's Vst3ParameterWindow (which is shown/hidden as a
	// separate window across a plugin that can be replaced in place),
	// this dialog and its plugin are 1:1 for the dialog's whole lifetime
	// (see Vst3EffectControls.h's ownership comment), so the grid is
	// filled once here rather than through a setParameters()/
	// parameterModelsChanged() signal pair reacting to a later plugin
	// swap that can't happen for Vst3Effect.
	m_grid = new Vst3ParameterGrid(this);
	layout->addWidget(m_grid, 1);

	if (loaded)
	{
		std::vector<Vst3ParameterModel*> models;
		models.reserve(m_controls->parameterModels().size());
		for (const auto& model : m_controls->parameterModels())
		{
			models.push_back(model.get());
		}
		m_grid->setParameters(models, QString());
	}
	else
	{
		m_grid->setParameters({}, tr("VST3 effect failed to load; no parameters available."));
	}

	// See Vst3EffectControls.h's parameterModelsAboutToClear() doc
	// comment: this is the connection that makes the signal actually
	// protect a dialog that is open at teardown time. Queued would risk
	// the grid clearing itself after the models it's about to touch are
	// already gone (teardownParameterModels() runs synchronously right
	// before destroying them), so this is a direct (default,
	// same-thread) connection deliberately, not queued.
	if (m_controls)
	{
		connect(m_controls, &Vst3EffectControls::parameterModelsAboutToClear,
			this, [this]() { m_grid->clearParameters(tr("VST3 effect failed to load; no parameters available.")); });
	}

	resize(560, 480);
}


Vst3EffectControlDialog::~Vst3EffectControlDialog()
{
	closeEditorWindow();
}


void Vst3EffectControlDialog::toggleEditor()
{
	if (m_editorWindow)
	{
		closeEditorWindow();
	}
	else
	{
		openEditorWindow(true);
	}
}


void Vst3EffectControlDialog::openEditorWindow(bool userInitiated)
{
	Vst3Effect* effect = m_controls != nullptr ? m_controls->vst3Effect() : nullptr;
	if (!effect || !effect->pluginInstance())
	{
		return;
	}

	// Already open: just bring it to the front.
	if (m_editorWindow)
	{
		m_editorWindow->show();
		m_editorWindow->raise();
		return;
	}

	// Step 1: ask the plugin for a view and its initial size. A plugin
	// with no editor lands here, and that is a normal, non-error
	// situation.
	int width = 0;
	int height = 0;
	QString error;
	const bool created = effect->createEditor(&width, &height,
		[this](int w, int h)
		{
			// Plugin asked to resize its editor (IPlugFrame::resizeView).
			if (m_editorHost) { m_editorHost->setFixedSize(toWidgetSize(m_editorHost, w, h)); }
			if (m_editorWindow) { m_editorWindow->adjustSize(); }
		},
		&error);

	if (!created)
	{
		if (userInitiated)
		{
			QMessageBox::information(this, tr("VST3 effect"),
				error.isEmpty() ? tr("Plugin editor is unavailable") : error);
		}
		return;
	}

	// Step 2: a native host widget sized to the plugin's view. The plugin
	// attaches its own child window to this widget's native handle, so
	// the widget must be a real native window (WA_NativeWindow).
	auto* host = new QWidget;
	host->setAttribute(Qt::WA_NativeWindow);
	// WA_PaintOnScreen: tell Qt not to use its backing store for this
	// widget, avoiding overdraw artefacts against the embedded native
	// child window on repaint.
	host->setAttribute(Qt::WA_PaintOnScreen);
	host->setFixedSize(toWidgetSize(host, width, height));

	// Same wrapper Prestige uses for its native editor windows. Closing
	// must not delete the widget out from under the plugin, so
	// DeleteOnClose is off and we destroy the window ourselves, after the
	// view is detached.
	SubWindow* window = getGUI()->mainWindow()->addWindowedWidget(host);
	window->setAttribute(Qt::WA_DeleteOnClose, false);
	window->setWindowTitle(effect->pluginInstance()->name());

	m_editorHost = host;
	m_editorWindow = window;

	// See EditorHostCursorFilter's comment: host is a native HWND sitting
	// right at the SubWindow's top resize margin. Installed on host, not
	// window, since it needs QEvent::Enter for host itself.
	host->installEventFilter(new EditorHostCursorFilter(host, window));

	// Step 3: attach. winId() forces creation of the native HWND. On
	// Windows we must also set WS_CLIPCHILDREN on the host HWND and
	// WS_CLIPSIBLINGS on the plugin's own HWND so GDI clips their
	// painting to their own window rectangles.
	const WId hostWinId = host->winId();
#ifdef Q_OS_WIN
	{
		const HWND hwnd = reinterpret_cast<HWND>(hostWinId);
		LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
		SetWindowLongPtr(hwnd, GWL_STYLE, style | WS_CLIPCHILDREN | WS_CLIPSIBLINGS);
	}
#endif
	if (!effect->attachEditor(reinterpret_cast<void*>(hostWinId), &error))
	{
		closeEditorWindow();
		if (userInitiated)
		{
			QMessageBox::warning(this, tr("VST3 editor"),
				error.isEmpty() ? tr("Plugin editor is unavailable") : error);
		}
		return;
	}

	// If the user closes the window with its own close button, detach the
	// plugin view first.
	window->installEventFilter(new EditorCloseFilter(window, [this]()
	{
		closeEditorWindow();
	}));

	window->show();
	m_editorButton->setText(tr("Hide editor"));
}


void Vst3EffectControlDialog::closeEditorWindow()
{
	// Plugin view FIRST: it has to detach from (and destroy its child of)
	// the native parent window before that window is destroyed. Closing
	// the editor never touches processor or plugin state; reopening
	// builds a fresh view against the same controller.
	Vst3Effect* effect = m_controls != nullptr ? m_controls->vst3Effect() : nullptr;
	if (effect)
	{
		effect->closeEditor();
	}

	if (m_editorWindow)
	{
		// deleteLater(): this may be running inside the window's own
		// close event, so it must not be deleted synchronously.
		m_editorWindow->hide();
		m_editorWindow->deleteLater();
		m_editorWindow.clear();
	}
	m_editorHost.clear();

	if (m_editorButton)
	{
		m_editorButton->setText(tr("Show editor"));
	}
}

} // namespace lmms::gui
