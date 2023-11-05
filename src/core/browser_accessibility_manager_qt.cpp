/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtWebEngine module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "browser_accessibility_manager_qt.h"

#include "content/browser/accessibility/browser_accessibility.h"
#include "ui/accessibility/ax_enums.mojom.h"

#include "browser_accessibility_qt.h"
#include "render_widget_host_view_qt.h"

#include <QtGui/qaccessible.h>

using namespace blink;

namespace content {

// static
BrowserAccessibilityManager *BrowserAccessibilityManager::Create(
        const ui::AXTreeUpdate &initialTree,
        BrowserAccessibilityDelegate *delegate)
{
#if QT_CONFIG(accessibility)
    Q_ASSERT(delegate);
    QtWebEngineCore::WebContentsAccessibilityQt *access = nullptr;
    access = static_cast<QtWebEngineCore::WebContentsAccessibilityQt *>(delegate->AccessibilityGetWebContentsAccessibility());
    return new BrowserAccessibilityManagerQt(access, initialTree, delegate);
#else
    return nullptr;
#endif // QT_CONFIG(accessibility)
}

// static
BrowserAccessibilityManager *BrowserAccessibilityManager::Create(
        BrowserAccessibilityDelegate *delegate)
{
#if QT_CONFIG(accessibility)
    return BrowserAccessibilityManager::Create(BrowserAccessibilityManagerQt::GetEmptyDocument(), delegate);
#else
    return nullptr;
#endif
}

#if QT_CONFIG(accessibility)
BrowserAccessibilityManagerQt::BrowserAccessibilityManagerQt(
    QtWebEngineCore::WebContentsAccessibilityQt *webContentsAccessibility,
    const ui::AXTreeUpdate &initialTree,
    BrowserAccessibilityDelegate* delegate)
      : BrowserAccessibilityManager(delegate)
      , m_webContentsAccessibility(webContentsAccessibility)
{
    Initialize(initialTree);
    m_valid = true; // BrowserAccessibilityQt can start using the AXTree
}

BrowserAccessibilityManagerQt::~BrowserAccessibilityManagerQt()
{
    m_valid = false; // BrowserAccessibilityQt should stop using the AXTree
}

QAccessibleInterface *BrowserAccessibilityManagerQt::rootParentAccessible()
{
    content::BrowserAccessibility *parent_node = GetParentNodeFromParentTree();
    if (!parent_node) {
        Q_ASSERT(m_webContentsAccessibility);
        return QAccessible::queryAccessibleInterface(m_webContentsAccessibility->accessibilityParentObject());
    }

    auto *parent_manager =
            static_cast<BrowserAccessibilityManagerQt *>(parent_node->manager());
    return parent_manager->rootParentAccessible();
}

void BrowserAccessibilityManagerQt::FireBlinkEvent(ax::mojom::Event event_type,
                                                   BrowserAccessibility* node)
{
    auto *iface = toQAccessibleInterface(node);

    switch (event_type) {
    case ax::mojom::Event::kFocus: {
        QAccessibleEvent event(iface, QAccessible::Focus);
        QAccessible::updateAccessibility(&event);
        break;
    }
    case ax::mojom::Event::kCheckedStateChanged: {
        QAccessible::State change;
        change.checked = true;
        QAccessibleStateChangeEvent event(iface, change);
        QAccessible::updateAccessibility(&event);
        break;
    }
    case ax::mojom::Event::kValueChanged: {
        QVariant value;
        if (QAccessibleValueInterface *valueIface = iface->valueInterface())
            value = valueIface->currentValue();
        QAccessibleValueChangeEvent event(iface, value);
        QAccessible::updateAccessibility(&event);
        break;
    }
    case ax::mojom::Event::kChildrenChanged:
        break;
    case ax::mojom::Event::kLayoutComplete:
        break;
    case ax::mojom::Event::kLoadComplete:
        break;
    case ax::mojom::Event::kTextChanged: {
        QAccessibleTextUpdateEvent event(iface, -1, QString(), QString());
        QAccessible::updateAccessibility(&event);
        break;
    }
    case ax::mojom::Event::kTextSelectionChanged: {
        QAccessibleTextInterface *textIface = iface->textInterface();
        if (textIface) {
            int start = 0;
            int end = 0;
            textIface->selection(0, &start, &end);
            if (start == end) {
                QAccessibleTextCursorEvent event(iface, start);
                QAccessible::updateAccessibility(&event);
            } else {
                QAccessibleTextSelectionEvent event(iface, start, end);
                QAccessible::updateAccessibility(&event);
            }
        }
        break;
    }
    default:
        break;
    }
}

void BrowserAccessibilityManagerQt::FireGeneratedEvent(ui::AXEventGenerator::Event event_type,
                                                       BrowserAccessibility* node)
{
    auto *iface = toQAccessibleInterface(node);

    switch (event_type) {
    case ui::AXEventGenerator::Event::VALUE_IN_TEXT_FIELD_CHANGED:
        if (iface->role() == QAccessible::EditableText) {
            QAccessibleTextUpdateEvent event(iface, -1, QString(), QString());
            QAccessible::updateAccessibility(&event);
        }
        break;
    default:
        break;
    }
}

#endif // QT_CONFIG(accessibility)

}
