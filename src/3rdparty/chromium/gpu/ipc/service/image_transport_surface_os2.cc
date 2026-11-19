// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gpu/ipc/service/image_transport_surface.h"

#include "base/check_op.h"
#include "ui/gl/gl_implementation.h"
#include "ui/gl/gl_surface.h"
#include "ui/gl/gl_surface_stub.h"
#include "ui/gl/init/gl_factory.h"

namespace gpu {

// static
scoped_refptr<gl::Presenter> ImageTransportSurface::CreatePresenter(
    gl::GLDisplay* display,
    base::WeakPtr<ImageTransportSurfaceDelegate> delegate,
    SurfaceHandle surface_handle) {
  // No surfaceless (overlay) presentation on OS/2; callers fall back to
  // CreateNativeGLSurface().
  return nullptr;
}

// static
scoped_refptr<gl::GLSurface> ImageTransportSurface::CreateNativeGLSurface(
    gl::GLDisplay* display,
    base::WeakPtr<ImageTransportSurfaceDelegate> delegate,
    SurfaceHandle surface_handle,
    gl::GLSurfaceFormat format) {
  if (gl::GetGLImplementation() == gl::kGLImplementationMockGL ||
      gl::GetGLImplementation() == gl::kGLImplementationStubGL) {
    return new gl::GLSurfaceStub;
  }

  DCHECK_NE(surface_handle, kNullSurfaceHandle);
  return gl::init::CreateViewGLSurface(display, surface_handle);
}

}  // namespace gpu
