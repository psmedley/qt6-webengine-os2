/****************************************************************************
**
** Copyright (C) 2019 The Qt Company Ltd.
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

#ifndef QPDFIOHANDLER_H
#define QPDFIOHANDLER_H

//
//  W A R N I N G
//  -------------
//
// This file is not part of the Qt API.  It exists purely as an
// implementation detail.  This header file may change from version to
// version without notice, or even be removed.
//
// We mean it.
//

#include <QtGui/qimageiohandler.h>
#include <QtPdf/QPdfDocument>

QT_BEGIN_NAMESPACE

class QPdfIOHandler : public QImageIOHandler
{
public:
    QPdfIOHandler();
    bool canRead() const override;
    static bool canRead(QIODevice *device);
    int currentImageNumber() const override;
    QRect currentImageRect() const override;
    int imageCount() const override;
    bool read(QImage *image) override;
    QVariant option(ImageOption option) const override;
    void setOption(ImageOption option, const QVariant & value) override;
    bool supportsOption(ImageOption option) const override;
    bool jumpToImage(int frame) override;
    bool jumpToNextImage() override;

private:
    bool load(QIODevice *device);

private:
    QPdfDocument m_doc;
    int m_page = -1;

    QRect m_clipRect;
    QSize m_scaledSize;
    QRect m_scaledClipRect;
    QColor m_backColor = Qt::transparent;
    bool m_loaded = false;
};

QT_END_NAMESPACE

#endif // QPDFIOHANDLER_H
