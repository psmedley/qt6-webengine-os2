// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MOJO_PUBLIC_CPP_PLATFORM_PLATFORM_HANDLE_H_
#define MOJO_PUBLIC_CPP_PLATFORM_PLATFORM_HANDLE_H_

#include "base/check_op.h"
#include "base/component_export.h"
#include "base/files/platform_file.h"
#include "build/build_config.h"
#include "mojo/public/c/system/platform_handle.h"

#if BUILDFLAG(IS_WIN)
#include "base/win/scoped_handle.h"
#elif BUILDFLAG(IS_FUCHSIA)
#include <lib/zx/handle.h>
#elif BUILDFLAG(IS_MAC)
#include "base/mac/scoped_mach_port.h"
#elif BUILDFLAG(IS_OS2)
#include "base/os2/scoped_shmem_handle.h"
#endif

#if BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)
#include "base/files/scoped_file.h"
#endif

namespace mojo {

// A PlatformHandle is a generic wrapper around a platform-specific system
// handle type, e.g. a POSIX file descriptor, Windows HANDLE, or macOS Mach
// port. This can wrap any of various such types depending on the host platform
// for which it's compiled.
//
// This is useful primarily for two reasons:
//
// - Interacting with the Mojo invitation API, which use OS primitives to
//   bootstrap Mojo IPC connections.
// - Interacting with Mojo platform handle wrapping and unwrapping API, which
//   allows handles to OS primitives to be transmitted over Mojo IPC with a
//   stable wire representation via Mojo handles.
//
// NOTE: This assumes ownership if the handle it represents.
class COMPONENT_EXPORT(MOJO_CPP_PLATFORM) PlatformHandle {
 public:
  enum class Type {
    kNone,
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_FUCHSIA)
    kHandle,
#elif BUILDFLAG(IS_MAC)
    kMachSend,
    kMachReceive,
#elif BUILDFLAG(IS_OS2)
    kShmemHandle,
#endif
#if BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)
    kFd,
#endif
  };

  PlatformHandle();
  PlatformHandle(PlatformHandle&& other);

#if BUILDFLAG(IS_WIN)
  explicit PlatformHandle(base::win::ScopedHandle handle);
#elif BUILDFLAG(IS_FUCHSIA)
  explicit PlatformHandle(zx::handle handle);
#elif BUILDFLAG(IS_MAC)
  explicit PlatformHandle(base::mac::ScopedMachSendRight mach_port);
  explicit PlatformHandle(base::mac::ScopedMachReceiveRight mach_port);
#elif BUILDFLAG(IS_OS2)
  explicit PlatformHandle(base::os2::ScopedShmemHandle handle);
#endif

#if BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)
  explicit PlatformHandle(base::ScopedFD fd);
#endif

  ~PlatformHandle();

  PlatformHandle& operator=(PlatformHandle&& other);

  // Takes ownership of |handle|'s underlying platform handle and fills in
  // |mojo_handle| with a representation of it. The caller assumes ownership of
  // the platform handle.
  static void ToMojoPlatformHandle(PlatformHandle handle,
                                   MojoPlatformHandle* mojo_handle);

  // Closes the underlying platform handle.
  // Assumes ownership of the platform handle described by |handle|, and returns
  // it as a new PlatformHandle.
  static PlatformHandle FromMojoPlatformHandle(
      const MojoPlatformHandle* handle);

  Type type() const { return type_; }

  void reset();

  // Relinquishes ownership of the underlying handle, regardless of type, and
  // discards its value. To release and obtain the underlying handle value, use
  // one of the specific |Release*()| methods below.
  void release();

  // Duplicates the underlying platform handle, returning a new PlatformHandle
  // which owns it.
  PlatformHandle Clone() const;

#if BUILDFLAG(IS_WIN)
  bool is_valid() const { return is_valid_handle(); }
  bool is_valid_handle() const { return handle_.IsValid(); }
  bool is_handle() const { return type_ == Type::kHandle; }
  const base::win::ScopedHandle& GetHandle() const { return handle_; }
  base::win::ScopedHandle TakeHandle() {
    DCHECK_EQ(type_, Type::kHandle);
    type_ = Type::kNone;
    return std::move(handle_);
  }
  [[nodiscard]] HANDLE ReleaseHandle() {
    DCHECK_EQ(type_, Type::kHandle);
    type_ = Type::kNone;
    return handle_.Take();
  }
#elif BUILDFLAG(IS_FUCHSIA)
  bool is_valid() const { return is_valid_fd() || is_valid_handle(); }
  bool is_valid_handle() const { return handle_.is_valid(); }
  bool is_handle() const { return type_ == Type::kHandle; }
  const zx::handle& GetHandle() const { return handle_; }
  zx::handle TakeHandle() {
    if (type_ == Type::kHandle)
      type_ = Type::kNone;
    return std::move(handle_);
  }
  [[nodiscard]] zx_handle_t ReleaseHandle() {
    if (type_ == Type::kHandle)
      type_ = Type::kNone;
    return handle_.release();
  }
#elif BUILDFLAG(IS_MAC)
  bool is_valid() const { return is_valid_fd() || is_valid_mach_port(); }
  bool is_valid_mach_port() const {
    return is_valid_mach_send() || is_valid_mach_receive();
  }

  bool is_valid_mach_send() const { return mach_send_.is_valid(); }
  bool is_mach_send() const { return type_ == Type::kMachSend; }
  const base::mac::ScopedMachSendRight& GetMachSendRight() const {
    return mach_send_;
  }
  base::mac::ScopedMachSendRight TakeMachSendRight() {
    if (type_ == Type::kMachSend)
      type_ = Type::kNone;
    return std::move(mach_send_);
  }
  [[nodiscard]] mach_port_t ReleaseMachSendRight() {
    return TakeMachSendRight().release();
  }

  bool is_valid_mach_receive() const { return mach_receive_.is_valid(); }
  bool is_mach_receive() const { return type_ == Type::kMachReceive; }
  const base::mac::ScopedMachReceiveRight& GetMachReceiveRight() const {
    return mach_receive_;
  }
  base::mac::ScopedMachReceiveRight TakeMachReceiveRight() {
    if (type_ == Type::kMachReceive)
      type_ = Type::kNone;
    return std::move(mach_receive_);
  }
  mach_port_t ReleaseMachReceiveRight() WARN_UNUSED_RESULT {
    return TakeMachReceiveRight().release();
  }
#elif BUILDFLAG(IS_OS2)
  bool is_valid() const { return is_valid_fd() || is_valid_shmem_handle(); }
  bool is_valid_shmem_handle() const { return handle_.is_valid(); }
  bool is_shmem_handle() const { return type_ == Type::kShmemHandle; }
  const base::os2::ScopedShmemHandle& GetShmemHandle() const {
    return handle_;
  }
  base::os2::ScopedShmemHandle TakeShmemHandle() {
    if (type_ == Type::kShmemHandle)
      type_ = Type::kNone;
    return std::move(handle_);
  }
  SHMEM ReleaseShmemHandle() WARN_UNUSED_RESULT {
    if (type_ == Type::kShmemHandle)
      type_ = Type::kNone;
    return handle_.release();
  }
#elif BUILDFLAG(IS_POSIX)
  bool is_valid() const { return is_valid_fd(); }
#else
#error "Unsupported platform."
#endif

#if BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)
  bool is_valid_fd() const { return fd_.is_valid(); }
  bool is_fd() const { return type_ == Type::kFd; }
  const base::ScopedFD& GetFD() const { return fd_; }
  base::ScopedFD TakeFD() {
    if (type_ == Type::kFd)
      type_ = Type::kNone;
    return std::move(fd_);
  }
  [[nodiscard]] int ReleaseFD() {
    if (type_ == Type::kFd)
      type_ = Type::kNone;
    return fd_.release();
  }
#endif

  bool is_valid_platform_file() const {
#if BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)
    return is_valid_fd();
#elif BUILDFLAG(IS_WIN)
    return is_valid_handle();
#else
#error "Unsupported platform"
#endif
  }
  base::ScopedPlatformFile TakePlatformFile() {
#if BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)
    return TakeFD();
#elif BUILDFLAG(IS_WIN)
    return TakeHandle();
#else
#error "Unsupported platform"
#endif
  }
  [[nodiscard]] base::PlatformFile ReleasePlatformFile() {
#if BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)
    return ReleaseFD();
#elif BUILDFLAG(IS_WIN)
    return ReleaseHandle();
#else
#error "Unsupported platform"
#endif
  }

 private:
  Type type_ = Type::kNone;

#if BUILDFLAG(IS_WIN)
  base::win::ScopedHandle handle_;
#elif BUILDFLAG(IS_FUCHSIA)
  zx::handle handle_;
#elif BUILDFLAG(IS_MAC)
  base::mac::ScopedMachSendRight mach_send_;
  base::mac::ScopedMachReceiveRight mach_receive_;
#elif BUILDFLAG(IS_OS2)
  base::os2::ScopedShmemHandle handle_;
#endif

#if BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)
  base::ScopedFD fd_;
#endif

  DISALLOW_COPY_AND_ASSIGN(PlatformHandle);
};

}  // namespace mojo

#endif  // MOJO_PUBLIC_CPP_PLATFORM_PLATFORM_HANDLE_H_
