// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fuchsia/engine/browser/web_engine_browser_context.h"

#include <lib/fdio/namespace.h>
#include <memory>
#include <utility>

#include "base/base_paths_fuchsia.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/fuchsia/fuchsia_logging.h"
#include "base/path_service.h"
#include "base/threading/thread_restrictions.h"
#include "components/keyed_service/content/browser_context_dependency_manager.h"
#include "components/keyed_service/core/simple_key_map.h"
#include "components/profile_metrics/browser_profile_type.h"
#include "components/site_isolation/site_isolation_policy.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/resource_context.h"
#include "fuchsia/engine/browser/web_engine_net_log_observer.h"
#include "fuchsia/engine/switches.h"
#include "media/capabilities/in_memory_video_decode_stats_db_impl.h"
#include "media/mojo/services/video_decode_perf_history.h"
#include "services/network/public/cpp/network_switches.h"

namespace {

std::unique_ptr<WebEngineNetLogObserver> CreateNetLogObserver() {
  std::unique_ptr<WebEngineNetLogObserver> result;

  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch(network::switches::kLogNetLog)) {
    base::FilePath log_path =
        command_line->GetSwitchValuePath(network::switches::kLogNetLog);
    result = std::make_unique<WebEngineNetLogObserver>(log_path);
  }

  return result;
}

}  // namespace

class WebEngineBrowserContext::ResourceContext
    : public content::ResourceContext {
 public:
  ResourceContext() = default;
  ~ResourceContext() override = default;

 private:
  DISALLOW_COPY_AND_ASSIGN(ResourceContext);
};

// static
std::unique_ptr<WebEngineBrowserContext>
WebEngineBrowserContext::CreatePersistent(base::FilePath data_directory) {
  return base::WrapUnique(
      new WebEngineBrowserContext(std::move(data_directory)));
}

// static
std::unique_ptr<WebEngineBrowserContext>
WebEngineBrowserContext::CreateIncognito() {
  return base::WrapUnique(new WebEngineBrowserContext({}));
}

WebEngineBrowserContext::~WebEngineBrowserContext() {
  SimpleKeyMap::GetInstance()->Dissociate(this);
  NotifyWillBeDestroyed();

  if (resource_context_) {
    content::GetIOThreadTaskRunner({})->DeleteSoon(
        FROM_HERE, std::move(resource_context_));
  }

  BrowserContextDependencyManager::GetInstance()->DestroyBrowserContextServices(
      this);

  ShutdownStoragePartitions();
}

std::unique_ptr<content::ZoomLevelDelegate>
WebEngineBrowserContext::CreateZoomLevelDelegate(
    const base::FilePath& partition_path) {
  return nullptr;
}

base::FilePath WebEngineBrowserContext::GetPath() {
  return data_dir_path_;
}

bool WebEngineBrowserContext::IsOffTheRecord() {
  return data_dir_path_.empty();
}

content::ResourceContext* WebEngineBrowserContext::GetResourceContext() {
  return resource_context_.get();
}

content::DownloadManagerDelegate*
WebEngineBrowserContext::GetDownloadManagerDelegate() {
  NOTIMPLEMENTED();
  return nullptr;
}

content::BrowserPluginGuestManager* WebEngineBrowserContext::GetGuestManager() {
  return nullptr;
}

storage::SpecialStoragePolicy*
WebEngineBrowserContext::GetSpecialStoragePolicy() {
  return nullptr;
}

content::PushMessagingService*
WebEngineBrowserContext::GetPushMessagingService() {
  return nullptr;
}

content::StorageNotificationService*
WebEngineBrowserContext::GetStorageNotificationService() {
  return nullptr;
}

content::SSLHostStateDelegate*
WebEngineBrowserContext::GetSSLHostStateDelegate() {
  return nullptr;
}

content::PermissionControllerDelegate*
WebEngineBrowserContext::GetPermissionControllerDelegate() {
  return &permission_delegate_;
}

content::ClientHintsControllerDelegate*
WebEngineBrowserContext::GetClientHintsControllerDelegate() {
  return nullptr;
}

content::BackgroundFetchDelegate*
WebEngineBrowserContext::GetBackgroundFetchDelegate() {
  return nullptr;
}

content::BackgroundSyncController*
WebEngineBrowserContext::GetBackgroundSyncController() {
  return nullptr;
}

content::BrowsingDataRemoverDelegate*
WebEngineBrowserContext::GetBrowsingDataRemoverDelegate() {
  return nullptr;
}

std::unique_ptr<media::VideoDecodePerfHistory>
WebEngineBrowserContext::CreateVideoDecodePerfHistory() {
  if (!IsOffTheRecord()) {
    // Delegate to the base class for stateful VideoDecodePerfHistory DB
    // creation.
    return BrowserContext::CreateVideoDecodePerfHistory();
  }

  // Return in-memory VideoDecodePerfHistory.
  return std::make_unique<media::VideoDecodePerfHistory>(
      std::make_unique<media::InMemoryVideoDecodeStatsDBImpl>(
          nullptr /* seed_db_provider */),
      media::learning::FeatureProviderFactoryCB());
}

WebEngineBrowserContext::WebEngineBrowserContext(base::FilePath data_directory)
    : data_dir_path_(std::move(data_directory)),
      net_log_observer_(CreateNetLogObserver()),
      simple_factory_key_(GetPath(), IsOffTheRecord()),
      resource_context_(std::make_unique<ResourceContext>()) {
  SimpleKeyMap::GetInstance()->Associate(this, &simple_factory_key_);

  profile_metrics::SetBrowserProfileType(
      this, IsOffTheRecord() ? profile_metrics::BrowserProfileType::kIncognito
                             : profile_metrics::BrowserProfileType::kRegular);

  // TODO(crbug.com/1181156): Should apply any persisted isolated origins here.
  // However, since WebEngine does not persist any, that would currently be a
  // no-op.
}
