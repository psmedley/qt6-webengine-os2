/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef INCLUDE_PERFETTO_PUBLIC_ABI_EXPORT_H_
#define INCLUDE_PERFETTO_PUBLIC_ABI_EXPORT_H_

#ifdef _WIN32
#define PERFETTO_INTERNAL_DLL_EXPORT __declspec(dllexport)
#define PERFETTO_INTERNAL_DLL_IMPORT __declspec(dllimport)
#else
#define PERFETTO_INTERNAL_DLL_EXPORT __attribute__((visibility("default")))
#define PERFETTO_INTERNAL_DLL_IMPORT
#endif

// PERFETTO_SDK_EXPORT: Exports a symbol from the perfetto SDK shared library.
#if defined(PERFETTO_IMPLEMENTATION)
#define PERFETTO_SDK_EXPORT PERFETTO_INTERNAL_DLL_EXPORT
#else
#define PERFETTO_SDK_EXPORT PERFETTO_INTERNAL_DLL_IMPORT
#endif

#endif  // INCLUDE_PERFETTO_PUBLIC_ABI_EXPORT_H_
