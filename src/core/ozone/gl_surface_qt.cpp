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

// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.Chromium file.

#if !defined(OS_MAC) && !defined(OS_OS2)

#include "gl_surface_qt.h"
#include "qtwebenginecoreglobal_p.h"

#include "base/logging.h"

#if defined(OS_WIN)
#include "web_engine_context.h"
#include "ozone/gl_surface_wgl_qt.h"
#include "ozone/gl_surface_egl_qt.h"

#include "gpu/ipc/service/image_transport_surface.h"
#include "ui/gl/gl_implementation.h"
#include "ui/gl/direct_composition_surface_win.h"
#include "ui/gl/vsync_provider_win.h"
#endif


namespace gl {

void *GLSurfaceQt::g_display = nullptr;
void *GLSurfaceQt::g_config = nullptr;
std::string GLSurfaceQt::g_client_extensions;
std::string GLSurfaceQt::g_extensions;

GLSurfaceQt::~GLSurfaceQt()
{
}

GLSurfaceQt::GLSurfaceQt()
{
}

GLSurfaceQt::GLSurfaceQt(const gfx::Size& size)
    : m_size(size)
{
    // Some implementations of Pbuffer do not support having a 0 size. For such
    // cases use a (1, 1) surface.
    if (m_size.GetArea() == 0)
        m_size.SetSize(1, 1);
}

bool GLSurfaceQt::HasEGLExtension(const char* name)
{
    return ExtensionsContain(g_extensions.c_str(), name);
}

bool GLSurfaceQt::IsOffscreen()
{
    return true;
}

gfx::SwapResult GLSurfaceQt::SwapBuffers(PresentationCallback callback)
{
    LOG(ERROR) << "Attempted to call SwapBuffers on a pbuffer.";
    Q_UNREACHABLE();
    return gfx::SwapResult::SWAP_FAILED;
}

gfx::Size GLSurfaceQt::GetSize()
{
    return m_size;
}

GLSurfaceFormat GLSurfaceQt::GetFormat()
{
    return m_format;
}

void* GLSurfaceQt::GetDisplay()
{
    return g_display;
}

void* GLSurfaceQt::GetConfig()
{
    return g_config;
}

#if defined(OS_WIN)
namespace init {
bool InitializeGLOneOffPlatform()
{
    VSyncProviderWin::InitializeOneOff();

    if (GetGLImplementation() == kGLImplementationEGLGLES2 || GetGLImplementation() == kGLImplementationEGLANGLE)
        return GLSurfaceEGLQt::InitializeOneOff();

    if (GetGLImplementation() == kGLImplementationDesktopGL)
        return GLSurfaceWGLQt::InitializeOneOff();

    return false;
}

bool usingSoftwareDynamicGL()
{
    return QtWebEngineCore::usingSoftwareDynamicGL();
}

scoped_refptr<GLSurface>
CreateOffscreenGLSurfaceWithFormat(const gfx::Size& size, GLSurfaceFormat format)
{
    scoped_refptr<GLSurface> surface;
    switch (GetGLImplementation()) {
    case kGLImplementationDesktopGLCoreProfile:
    case kGLImplementationDesktopGL: {
        surface = new GLSurfaceWGLQt(size);
        if (surface->Initialize(format))
            return surface;
        break;
    }
    case kGLImplementationEGLANGLE:
    case kGLImplementationEGLGLES2: {
        surface = new GLSurfaceEGLQt(size);
        if (surface->Initialize(format))
            return surface;

        // Surfaceless context will be used ONLY if pseudo surfaceless context
        // is not available since some implementations of surfaceless context
        // have problems. (e.g. QTBUG-57290)
        if (GLSurfaceEGLQt::g_egl_surfaceless_context_supported) {
            surface = new GLSurfacelessQtEGL(size);
            if (surface->Initialize(format))
                return surface;
        }
        LOG(ERROR) << "eglCreatePbufferSurface failed and surfaceless context not available";
        LOG(WARNING) << "Failed to create offscreen GL surface";
        break;
    }
    default:
        break;
    }
    LOG(ERROR) << "Requested OpenGL implementation is not supported. Implementation: " << GetGLImplementation();
    Q_UNREACHABLE();
    return nullptr;
}

scoped_refptr<GLSurface>
CreateViewGLSurface(gfx::AcceleratedWidget window)
{
    QT_NOT_USED
    return nullptr;
}

} // namespace init
#endif  // defined(OS_WIN)
} // namespace gl

#if defined(OS_WIN)
namespace gpu {
class GpuCommandBufferStub;
class GpuChannelManager;
scoped_refptr<gl::GLSurface> ImageTransportSurface::CreateNativeSurface(base::WeakPtr<ImageTransportSurfaceDelegate>,
                                                                        SurfaceHandle, gl::GLSurfaceFormat)
{
    QT_NOT_USED
    return scoped_refptr<gl::GLSurface>();
}
} // namespace gpu

namespace gl {

bool DirectCompositionSurfaceWin::IsDirectCompositionSupported()
{
    return false;
}

bool DirectCompositionSurfaceWin::IsDecodeSwapChainSupported()
{
    return false;
}

bool DirectCompositionSurfaceWin::IsHDRSupported()
{
    return false;
}

bool DirectCompositionSurfaceWin::IsSwapChainTearingSupported()
{
    return false;
}

bool DirectCompositionSurfaceWin::AreOverlaysSupported()
{
    return false;
}

UINT DirectCompositionSurfaceWin::GetOverlaySupportFlags(DXGI_FORMAT format)
{
    Q_UNUSED(format);
    return 0;
}

void DirectCompositionSurfaceWin::DisableDecodeSwapChain()
{
}

void DirectCompositionSurfaceWin::DisableSoftwareOverlays()
{
}
} // namespace gl
#endif
#endif // !defined(OS_MAC) && !defined(OS_OS2)
