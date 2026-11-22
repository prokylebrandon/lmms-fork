/*
 * Vst3EditorScale.h - editor scale menu for hosted VST3 editor windows
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

#ifndef LMMS_GUI_VST3_EDITOR_SCALE_H
#define LMMS_GUI_VST3_EDITOR_SCALE_H

#include <QAction>
#include <QContextMenuEvent>
#include <QEvent>
#include <QMdiArea>
#include <QMenu>
#include <QObject>
#include <QPointer>
#include <QScreen>
#include <QSize>
#include <QString>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <functional>

#include "SubWindow.h"

// Header-only on purpose: Prestige and Vst3Effect each host a native editor
// window the same way, and both need the same scale menu.  Everything that
// touches the plugin goes through Hooks, so this header knows nothing about
// either instrument; it must not include <windows.h> (min/max macros).

namespace lmms::gui::vst3editor
{

/// Scales offered in the menu, as a fraction of the editor's normal size.
inline constexpr float kScalePresets[] = {1.0f, 0.9f, 0.8f, 0.75f, 0.67f, 0.5f};

/// Smallest scale "Fit to window" will pick.
inline constexpr float kMinScale = 0.25f;

/// The largest scale (never above 100%) at which an editor that is
/// @p hostSize large at @p currentScale fits into @p available once the window
/// decoration (@p chrome) is added.  Rounded down so it really fits.
inline float fitScale(const QSize& hostSize, float currentScale, const QSize& chrome, const QSize& available)
{
    if (hostSize.isEmpty() || !(currentScale > 0.0f)) { return currentScale; }

    // Assume the plugin's size grows in proportion to the scale it was given,
    // so its size at 100% is the current size divided by the current scale.
    const double baseW = hostSize.width()  / static_cast<double>(currentScale);
    const double baseH = hostSize.height() / static_cast<double>(currentScale);
    const double fitW = (available.width()  - chrome.width())  / baseW;
    const double fitH = (available.height() - chrome.height()) / baseH;

    const double s = std::floor(std::min({1.0, fitW, fitH}) * 100.0) / 100.0;
    return static_cast<float>(std::max(static_cast<double>(kMinScale), s));
}

/// Event filter for the editor's SubWindow: right-clicking the title bar or
/// the frame (anything that is not the plugin's own native window) opens a
/// menu to scale the editor down.  Scaling uses the VST3
/// IPlugViewContentScaleSupport interface, so it only works for plugins that
/// implement it; for the others the menu says so.
class EditorScaler : public QObject
{
public:
    struct Hooks
    {
        std::function<bool()>           supported; //!< the plugin view implements content scale support
        std::function<bool(float)>      setFactor; //!< hand a content scale factor to the plugin view
        std::function<bool(int*, int*)> viewSize;  //!< the plugin view's current size (getSize())
        std::function<QSize(int, int)>  toWidget;  //!< plugin view size -> Qt widget size
        std::function<void(float)>      remember;  //!< scale picked by the user (kept for the next open)
        std::function<void()>           resync;    //!< re-check the real plugin window size afterwards
    };

    EditorScaler(QObject* parent, QPointer<QWidget> host, QPointer<SubWindow> window, float initialScale, Hooks hooks) :
        QObject(parent),
        m_host(std::move(host)),
        m_window(std::move(window)),
        m_scale(initialScale),
        m_hooks(std::move(hooks))
    {
    }

    /// Re-applies a scale the user chose earlier; call once the view is
    /// attached.  Does nothing at 100%, so a plugin is left alone until the
    /// user asks for a change.
    void applyInitial()
    {
        if (std::abs(m_scale - 1.0f) > 0.001f) { apply(m_scale); }
    }

    /// @return false when nothing changed (no plugin support, or declined)
    bool apply(float scale)
    {
        if (!m_host || !m_hooks.supported || !m_hooks.supported() || !m_hooks.setFactor) { return false; }

        // On Windows/Linux the plugin is told the display scale and the user's
        // scale as one factor; macOS views already work in logical units.
#ifdef __APPLE__
        const double base = 1.0;
#else
        const double base = m_host->devicePixelRatioF();
#endif
        if (!m_hooks.setFactor(static_cast<float>(base * scale))) { return false; }

        m_scale = scale;
        if (m_hooks.remember) { m_hooks.remember(scale); }

        // A plugin that honours the factor answers with resizeView(), which
        // the window has already followed.  Ask for the size again so a plugin
        // that only updates getSize() ends up right as well.
        int w = 0;
        int h = 0;
        if (m_hooks.viewSize && m_hooks.toWidget && m_hooks.viewSize(&w, &h) && m_host)
        {
            m_host->setFixedSize(m_hooks.toWidget(w, h));
            if (m_window) { m_window->adjustSize(); }
        }
        if (m_hooks.resync) { m_hooks.resync(); }
        return true;
    }

    /// Picks the largest scale at which the whole editor fits the workspace
    /// (or the screen, when the window is detached) and applies it.
    void fit()
    {
        if (!m_host || !m_window) { return; }

        QSize available;
        QSize chrome;
        if (m_window->isDetached())
        {
            // The host widget is its own top-level window here: use the screen
            // it is on, leaving room for the OS title bar and the taskbar.
            if (const QScreen* screen = m_host->screen())
            {
                available = screen->availableGeometry().size() - QSize(40, 80);
            }
        }
        else if (const QMdiArea* area = m_window->mdiArea())
        {
            available = area->viewport()->size();
            chrome = m_window->size() - m_host->size(); // title bar + frame
        }
        if (available.isEmpty()) { return; }

        apply(fitScale(m_host->size(), m_scale, chrome, available));
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::ContextMenu && m_host)
        {
            auto* ev = static_cast<QContextMenuEvent*>(event);
            // Only the window's own title bar / frame: anything over the
            // editor itself belongs to the plugin.
            if (ev->reason() == QContextMenuEvent::Mouse && !m_host->geometry().contains(ev->pos()))
            {
                showMenu(ev->globalPos());
                return true;
            }
        }
        return QObject::eventFilter(watched, event);
    }

private:
    void showMenu(const QPoint& globalPos)
    {
        QMenu menu;
        QAction* fitAction = nullptr;

        if (m_hooks.supported && m_hooks.supported())
        {
            menu.addSection(QObject::tr("Editor scale"));
            fitAction = menu.addAction(QObject::tr("Fit to window"));
            menu.addSeparator();
            for (const float preset : kScalePresets)
            {
                QAction* action = menu.addAction(QString::number(qRound(preset * 100)) + QStringLiteral("%"));
                action->setCheckable(true);
                action->setChecked(std::abs(preset - m_scale) < 0.005f);
                action->setData(preset);
            }
        }
        else
        {
            menu.addAction(QObject::tr("This plugin does not support scaling"))->setEnabled(false);
        }

        QAction* chosen = menu.exec(globalPos);
        if (!chosen) { return; }

        if (chosen == fitAction) { fit(); }
        else if (chosen->isCheckable()) { apply(chosen->data().toFloat()); }
    }

    QPointer<QWidget>   m_host;
    QPointer<SubWindow> m_window;
    float               m_scale;
    Hooks               m_hooks;
};

} // namespace lmms::gui::vst3editor

#endif // LMMS_GUI_VST3_EDITOR_SCALE_H
