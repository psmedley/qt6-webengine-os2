/****************************************************************************
**
** Copyright (C) 2017 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com, author Tobias König <tobias.koenig@kdab.com>
** Copyright (C) 2020 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtPDF module of the Qt Toolkit.
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

#ifndef QPDFPAGERENDERER_H
#define QPDFPAGERENDERER_H

#include <QtPdf/qtpdfglobal.h>

#include <QtCore/qobject.h>
#include <QtCore/qsize.h>
#include <QtPdf/qpdfdocument.h>
#include <QtPdf/qpdfdocumentrenderoptions.h>

QT_BEGIN_NAMESPACE

class QPdfPageRendererPrivate;

class Q_PDF_EXPORT QPdfPageRenderer : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QPdfDocument* document READ document WRITE setDocument NOTIFY documentChanged)
    Q_PROPERTY(RenderMode renderMode READ renderMode WRITE setRenderMode NOTIFY renderModeChanged)

public:
    enum class RenderMode
    {
        MultiThreaded,
        SingleThreaded
    };
    Q_ENUM(RenderMode)

    QPdfPageRenderer() : QPdfPageRenderer(nullptr) {}
    explicit QPdfPageRenderer(QObject *parent);
    ~QPdfPageRenderer() override;

    RenderMode renderMode() const;
    void setRenderMode(RenderMode mode);

    QPdfDocument* document() const;
    void setDocument(QPdfDocument *document);

    quint64 requestPage(int pageNumber, QSize imageSize,
                        QPdfDocumentRenderOptions options = QPdfDocumentRenderOptions());

Q_SIGNALS:
    void documentChanged(QPdfDocument *document);
    void renderModeChanged(RenderMode renderMode);

    void pageRendered(int pageNumber, QSize imageSize, const QImage &image,
                      QPdfDocumentRenderOptions options, quint64 requestId);

private:
    QScopedPointer<QPdfPageRendererPrivate> d_ptr;
};

QT_END_NAMESPACE

#endif
