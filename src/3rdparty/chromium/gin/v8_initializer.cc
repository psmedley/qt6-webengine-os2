// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gin/v8_initializer.h"

#include <stddef.h>
#include <stdint.h>

#include <memory>

#include "base/allocator/partition_allocator/page_allocator.h"
#include "base/check.h"
#include "base/debug/alias.h"
#include "base/debug/crash_logging.h"
#include "base/feature_list.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/memory_mapped_file.h"
#include "base/lazy_instance.h"
#include "base/metrics/histogram_functions.h"
#include "base/metrics/histogram_macros.h"
#include "base/notreached.h"
#include "base/path_service.h"
#include "base/rand_util.h"
#include "base/strings/string_util.h"
#include "base/strings/sys_string_conversions.h"
#include "base/system/sys_info.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "base/win/windows_version.h"
#include "build/build_config.h"
#include "gin/gin_features.h"

#if defined(V8_USE_EXTERNAL_STARTUP_DATA) && defined(OS_ANDROID)
#include "base/android/apk_assets.h"
#endif  // V8_USE_EXTERNAL_STARTUP_DATA
#if defined(OS_MAC)
#include "base/mac/foundation_util.h"
#endif

namespace gin {

namespace {

// This global is never freed nor closed.
base::MemoryMappedFile* g_mapped_snapshot = nullptr;

bool GenerateEntropy(unsigned char* buffer, size_t amount) {
  base::RandBytes(buffer, amount);
  return true;
}

void GetMappedFileData(base::MemoryMappedFile* mapped_file,
                       v8::StartupData* data) {
  if (mapped_file) {
    data->data = reinterpret_cast<const char*>(mapped_file->data());
    data->raw_size = static_cast<int>(mapped_file->length());
  } else {
    data->data = nullptr;
    data->raw_size = 0;
  }
}

#if defined(V8_USE_EXTERNAL_STARTUP_DATA)

#if defined(OS_ANDROID)
const char kV8ContextSnapshotFileName64[] = "v8_context_snapshot_64.bin";
const char kV8ContextSnapshotFileName32[] = "v8_context_snapshot_32.bin";
const char kSnapshotFileName64[] = "snapshot_blob_64.bin";
const char kSnapshotFileName32[] = "snapshot_blob_32.bin";

#if defined(__LP64__)
#define kV8ContextSnapshotFileName kV8ContextSnapshotFileName64
#define kSnapshotFileName kSnapshotFileName64
#else
#define kV8ContextSnapshotFileName kV8ContextSnapshotFileName32
#define kSnapshotFileName kSnapshotFileName32
#endif

#else  // defined(OS_ANDROID)
#if defined(USE_V8_CONTEXT_SNAPSHOT)
const char kV8ContextSnapshotFileName[] = V8_CONTEXT_SNAPSHOT_FILENAME;
#endif
const char kSnapshotFileName[] = "snapshot_blob.bin";
#endif  // defined(OS_ANDROID)

const char* GetSnapshotFileName(
    const V8Initializer::V8SnapshotFileType file_type) {
  switch (file_type) {
    case V8Initializer::V8SnapshotFileType::kDefault:
      return kSnapshotFileName;
    case V8Initializer::V8SnapshotFileType::kWithAdditionalContext:
#if defined(USE_V8_CONTEXT_SNAPSHOT)
      return kV8ContextSnapshotFileName;
#else
      NOTREACHED();
      return nullptr;
#endif
  }
  NOTREACHED();
  return nullptr;
}

void GetV8FilePath(const char* file_name, base::FilePath* path_out) {
#if defined(OS_ANDROID)
  // This is the path within the .apk.
  *path_out =
      base::FilePath(FILE_PATH_LITERAL("assets")).AppendASCII(file_name);
#elif defined(OS_MAC)
  base::ScopedCFTypeRef<CFStringRef> bundle_resource(
      base::SysUTF8ToCFStringRef(file_name));
  *path_out = base::mac::PathForFrameworkBundleResource(bundle_resource);
#else
  base::FilePath data_path;
  bool r = base::PathService::Get(base::DIR_ASSETS, &data_path);
  DCHECK(r);
  *path_out = data_path.AppendASCII(file_name);
#endif
}

bool MapV8File(base::File file,
               base::MemoryMappedFile::Region region,
               base::MemoryMappedFile** mmapped_file_out) {
  DCHECK(*mmapped_file_out == NULL);
  std::unique_ptr<base::MemoryMappedFile> mmapped_file(
      new base::MemoryMappedFile());
  if (mmapped_file->Initialize(std::move(file), region)) {
    *mmapped_file_out = mmapped_file.release();
    return true;
  }
  return false;
}

base::File OpenV8File(const char* file_name,
                      base::MemoryMappedFile::Region* region_out) {
  // Re-try logic here is motivated by http://crbug.com/479537
  // for A/V on Windows (https://support.microsoft.com/en-us/kb/316609).

  // These match tools/metrics/histograms.xml
  enum OpenV8FileResult {
    OPENED = 0,
    OPENED_RETRY,
    FAILED_IN_USE,
    FAILED_OTHER,
    MAX_VALUE
  };
  base::FilePath path;
  GetV8FilePath(file_name, &path);

#if defined(OS_ANDROID)
  base::File file(base::android::OpenApkAsset(path.value(), region_out));
  OpenV8FileResult result = file.IsValid() ? OpenV8FileResult::OPENED
                                           : OpenV8FileResult::FAILED_OTHER;
#else
  // Re-try logic here is motivated by http://crbug.com/479537
  // for A/V on Windows (https://support.microsoft.com/en-us/kb/316609).
  const int kMaxOpenAttempts = 5;
  const int kOpenRetryDelayMillis = 250;

  OpenV8FileResult result = OpenV8FileResult::FAILED_IN_USE;
  int flags = base::File::FLAG_OPEN | base::File::FLAG_READ;
  base::File file;
  for (int attempt = 0; attempt < kMaxOpenAttempts; attempt++) {
    file.Initialize(path, flags);
    if (file.IsValid()) {
      *region_out = base::MemoryMappedFile::Region::kWholeFile;
      if (attempt == 0) {
        result = OpenV8FileResult::OPENED;
        break;
      } else {
        result = OpenV8FileResult::OPENED_RETRY;
        break;
      }
    } else if (file.error_details() != base::File::FILE_ERROR_IN_USE) {
      result = OpenV8FileResult::FAILED_OTHER;
      break;
    } else if (kMaxOpenAttempts - 1 != attempt) {
      base::PlatformThread::Sleep(
          base::TimeDelta::FromMilliseconds(kOpenRetryDelayMillis));
    }
  }
#endif  // defined(OS_ANDROID)

  UMA_HISTOGRAM_ENUMERATION("V8.Initializer.OpenV8File.Result", result,
                            OpenV8FileResult::MAX_VALUE);
  return file;
}

#endif  // defined(V8_USE_EXTERNAL_STARTUP_DATA)

template <int LENGTH>
void SetV8Flags(const char (&flag)[LENGTH]) {
  v8::V8::SetFlagsFromString(flag, LENGTH - 1);
}

void SetV8FlagsFormatted(const char* format, ...) {
  char buffer[128];
  va_list args;
  va_start(args, format);
  int length = base::vsnprintf(buffer, sizeof(buffer), format, args);
  if (length <= 0 || sizeof(buffer) <= static_cast<unsigned>(length)) {
    PLOG(ERROR) << "Invalid formatted V8 flag: " << format;
    return;
  }
  v8::V8::SetFlagsFromString(buffer, length - 1);
}

void RunArrayBufferCageReservationExperiment() {
  // TODO(1218005) remove this function and windows_version.h include once the
  // experiment has ended.
#if defined(ARCH_CPU_64_BITS)
  constexpr size_t kGigaBytes = 1024 * 1024 * 1024;
  constexpr size_t kTeraBytes = 1024 * kGigaBytes;

  constexpr size_t kCageMaxSize = 1 * kTeraBytes;
  constexpr size_t kCageMinSize = 8 * kGigaBytes;

#if defined(OS_WIN)
  // Windows prior to Win10 (or possibly Win8/8.1) appears to create page table
  // entries when reserving virtual memory, causing unacceptably high memory
  // consumption (e.g. ~2GB when reserving 1TB). As such, the experiment is
  // only enabled on Win10.
  if (base::win::GetVersion() < base::win::Version::WIN10) {
    return;
  }
#endif

  void* reservation = nullptr;
  size_t current_size = kCageMaxSize;
  while (!reservation && current_size >= kCageMinSize) {
    // The cage reservation will need to be 4GB aligned.
    reservation = base::AllocPages(nullptr, current_size, 4 * kGigaBytes,
                                   base::PageInaccessible, base::PageTag::kV8);
    if (!reservation) {
      current_size /= 2;
    }
  }

  int result = current_size / kGigaBytes;
  if (reservation) {
    base::FreePages(reservation, current_size);
  } else {
    result = 0;
  }

  base::UmaHistogramSparse("V8.MaxArrayBufferCageReservationSize", result);
#endif
}

template <size_t N, size_t M>
void SetV8FlagsIfOverridden(const base::Feature& feature,
                            const char (&enabling_flag)[N],
                            const char (&disabling_flag)[M]) {
  auto overridden_state = base::FeatureList::GetStateIfOverridden(feature);
  if (!overridden_state.has_value()) {
    return;
  }
  if (overridden_state.value()) {
    SetV8Flags(enabling_flag);
  } else {
    SetV8Flags(disabling_flag);
  }
}

}  // namespace

// static
void V8Initializer::Initialize(IsolateHolder::ScriptMode mode) {
  static bool v8_is_initialized = false;
  if (v8_is_initialized)
    return;

  if (base::FeatureList::IsEnabled(
          features::kV8ArrayBufferCageReservationExperiment)) {
    RunArrayBufferCageReservationExperiment();
  }

  v8::V8::InitializePlatform(V8Platform::Get());

  if (!base::FeatureList::IsEnabled(features::kV8OptimizeJavascript)) {
    // We avoid explicitly passing --opt if kV8OptimizeJavascript is enabled
    // since it is the default, and doing so would override flags passed
    // explicitly, e.g., via --js-flags=--no-opt.
    SetV8Flags("--no-opt");
  }

  if (!base::FeatureList::IsEnabled(features::kV8FlushBytecode)) {
    SetV8Flags("--no-flush-bytecode");
  }

  if (base::FeatureList::IsEnabled(features::kV8OffThreadFinalization)) {
    SetV8Flags("--finalize-streaming-on-background");
  }

  if (!base::FeatureList::IsEnabled(features::kV8LazyFeedbackAllocation)) {
    SetV8Flags("--no-lazy-feedback-allocation");
  }

  if (base::FeatureList::IsEnabled(features::kV8ConcurrentInlining)) {
    SetV8Flags("--concurrent_inlining");
  }

  if (base::FeatureList::IsEnabled(features::kV8PerContextMarkingWorklist)) {
    SetV8Flags("--stress-per-context-marking-worklist");
  }

  if (base::FeatureList::IsEnabled(features::kV8FlushEmbeddedBlobICache)) {
    SetV8Flags("--experimental-flush-embedded-blob-icache");
  }

  if (base::FeatureList::IsEnabled(features::kV8ReduceConcurrentMarkingTasks)) {
    SetV8Flags("--gc-experiment-reduce-concurrent-marking-tasks");
  }

  if (base::FeatureList::IsEnabled(features::kV8NoReclaimUnmodifiedWrappers)) {
    SetV8Flags("--no-reclaim-unmodified-wrappers");
  }

  if (!base::FeatureList::IsEnabled(features::kV8LocalHeaps)) {
    // The --local-heaps flag is enabled by default, so we need to explicitly
    // disable it if kV8LocalHeaps is disabled.
    // Also disable TurboFan's direct access if local heaps are not enabled.
    SetV8Flags("--no-local-heaps --no-turbo-direct-heap-access");
  }

  if (!base::FeatureList::IsEnabled(features::kV8TurboDirectHeapAccess)) {
    // The --turbo-direct-heap-access flag is enabled by default, so we need to
    // explicitly disable it if kV8TurboDirectHeapAccess is disabled.
    SetV8Flags("--no-turbo-direct-heap-access");
  }

  if (!base::FeatureList::IsEnabled(features::kV8ExperimentalRegexpEngine)) {
    // The --enable-experimental-regexp-engine-on-excessive-backtracks flag is
    // enabled by default, so we need to explicitly disable it if
    // kV8ExperimentalRegexpEngine is disabled.
    SetV8Flags(
        "--no-enable-experimental-regexp-engine-on-excessive-backtracks");
  }

  if (base::FeatureList::IsEnabled(features::kV8TurboFastApiCalls)) {
    SetV8Flags("--turbo-fast-api-calls");
  }

  if (base::FeatureList::IsEnabled(features::kV8Turboprop)) {
    SetV8Flags("--turboprop");
  }

  SetV8FlagsIfOverridden(features::kV8Sparkplug, "--sparkplug",
                         "--no-sparkplug");

  if (base::FeatureList::IsEnabled(
          features::kV8SparkplugNeedsShortBuiltinCalls)) {
    SetV8Flags("--sparkplug-needs-short-builtins");
  }

  if (base::FeatureList::IsEnabled(features::kV8UntrustedCodeMitigations)) {
    SetV8Flags("--untrusted-code-mitigations");
  } else {
    SetV8Flags("--no-untrusted-code-mitigations");
  }

  if (base::FeatureList::IsEnabled(features::kV8ScriptAblation)) {
    if (int delay = features::kV8ScriptDelayMs.Get()) {
      SetV8FlagsFormatted("--script-delay=%i", delay);
    }
    if (int delay = features::kV8ScriptDelayOnceMs.Get()) {
      SetV8FlagsFormatted("--script-delay-once=%i", delay);
    }
    if (double fraction = features::kV8ScriptDelayFraction.Get()) {
      SetV8FlagsFormatted("--script-delay-fraction=%f", fraction);
    }
  }

  if (!base::FeatureList::IsEnabled(features::kV8ShortBuiltinCalls)) {
    // The --short-builtin-calls flag is enabled by default on x64 and arm64
    // desktop configurations, so we need to explicitly disable it if
    // kV8ShortBuiltinCalls is disabled.
    // On other configurations it's not supported, so we don't try to enable
    // it if the feature flag is on.
    SetV8Flags("--no-short-builtin-calls");
  }

  SetV8FlagsIfOverridden(features::kV8SlowHistograms, "--slow-histograms",
                         "--no-slow-histograms");

  if (IsolateHolder::kStrictMode == mode) {
    SetV8Flags("--use_strict");
  }

#if defined(V8_USE_EXTERNAL_STARTUP_DATA)
  if (g_mapped_snapshot) {
    v8::StartupData snapshot;
    GetMappedFileData(g_mapped_snapshot, &snapshot);
    v8::V8::SetSnapshotDataBlob(&snapshot);
  }
#endif  // V8_USE_EXTERNAL_STARTUP_DATA

  v8::V8::SetEntropySource(&GenerateEntropy);
  v8::V8::Initialize();

  v8_is_initialized = true;
}

// static
void V8Initializer::GetV8ExternalSnapshotData(v8::StartupData* snapshot) {
  GetMappedFileData(g_mapped_snapshot, snapshot);
}

// static
void V8Initializer::GetV8ExternalSnapshotData(const char** snapshot_data_out,
                                              int* snapshot_size_out) {
  v8::StartupData snapshot;
  GetV8ExternalSnapshotData(&snapshot);
  *snapshot_data_out = snapshot.data;
  *snapshot_size_out = snapshot.raw_size;
}

#if defined(V8_USE_EXTERNAL_STARTUP_DATA)

// static
void V8Initializer::LoadV8Snapshot(V8SnapshotFileType snapshot_file_type) {
  if (g_mapped_snapshot) {
    // TODO(crbug.com/802962): Confirm not loading different type of snapshot
    // files in a process.
    return;
  }

  base::MemoryMappedFile::Region file_region;
  base::File file =
      OpenV8File(GetSnapshotFileName(snapshot_file_type), &file_region);
  LoadV8SnapshotFromFile(std::move(file), &file_region, snapshot_file_type);
}

// static
void V8Initializer::LoadV8SnapshotFromFile(
    base::File snapshot_file,
    base::MemoryMappedFile::Region* snapshot_file_region,
    V8SnapshotFileType snapshot_file_type) {
  if (g_mapped_snapshot)
    return;

  if (!snapshot_file.IsValid()) {
    LOG(FATAL) << "Error loading V8 startup snapshot file";
    return;
  }

  base::MemoryMappedFile::Region region =
      base::MemoryMappedFile::Region::kWholeFile;
  if (snapshot_file_region) {
    region = *snapshot_file_region;
  }

  if (!MapV8File(std::move(snapshot_file), region, &g_mapped_snapshot)) {
    LOG(FATAL) << "Error mapping V8 startup snapshot file";
    return;
  }
}

#if defined(OS_ANDROID)
// static
base::FilePath V8Initializer::GetSnapshotFilePath(
    bool abi_32_bit,
    V8SnapshotFileType snapshot_file_type) {
  base::FilePath path;
  const char* filename = nullptr;
  switch (snapshot_file_type) {
    case V8Initializer::V8SnapshotFileType::kDefault:
      filename = abi_32_bit ? kSnapshotFileName32 : kSnapshotFileName64;
      break;
    case V8Initializer::V8SnapshotFileType::kWithAdditionalContext:
      filename = abi_32_bit ? kV8ContextSnapshotFileName32
                            : kV8ContextSnapshotFileName64;
      break;
  }
  CHECK(filename);

  GetV8FilePath(filename, &path);
  return path;
}
#endif  // defined(OS_ANDROID)
#endif  // defined(V8_USE_EXTERNAL_STARTUP_DATA)

}  // namespace gin
