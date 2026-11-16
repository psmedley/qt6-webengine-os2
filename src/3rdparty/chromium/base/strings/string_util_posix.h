// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_STRINGS_STRING_UTIL_POSIX_H_
#define BASE_STRINGS_STRING_UTIL_POSIX_H_

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include <string_view>

#include "base/check.h"
#include "base/strings/string_piece.h"

namespace base {

// Chromium code style is to not use malloc'd strings; this is only for use
// for interaction with APIs that require it.
inline char* strdup(const char* str) {
  return ::strdup(str);
}

inline int vsnprintf(char* buffer, size_t size,
                     const char* format, va_list arguments) {
  return ::vsnprintf(buffer, size, format, arguments);
}

inline int vswprintf(wchar_t* buffer, size_t size,
                     const wchar_t* format, va_list arguments) {
  DCHECK(IsWprintfFormatPortable(format));
  return ::vswprintf(buffer, size, format, arguments);
}

#ifdef __OS2__
inline const char16_t* as_u16cstr(const wchar_t* str) {
  return reinterpret_cast<const char16_t*>(str);
}
inline const char16_t* as_u16cstr(std::wstring_view str) {
  return reinterpret_cast<const char16_t*>(str.data());
}
// The following section contains overloads of the cross-platform APIs for
// std::wstring and std::wstring_view. (Chromium removed base::WStringPiece;
// IsStringASCII(std::wstring_view) is defined for OS/2 in string_util.cc.)
BASE_EXPORT bool IsStringASCII(std::wstring_view str);
#endif

}  // namespace base

#endif  // BASE_STRINGS_STRING_UTIL_POSIX_H_
