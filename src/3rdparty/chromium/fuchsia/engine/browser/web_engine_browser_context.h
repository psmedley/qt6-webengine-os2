// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FUCHSIA_ENGINE_BROWSER_WEB_ENGINE_BROWSER_CONTEXT_H_
#define FUCHSIA_ENGINE_BROWSER_WEB_ENGINE_BROWSER_CONTEXT_H_

#include <memory>

#include "base/files/file_path.h"
#include "components/keyed_service/core/simple_factory_key.h"
#include "content/public/browser/browser_context.h"
#include "fuchsia/engine/browser/web_engine_permission_delegate.h"

class WebEngineNetLogObserver;

class WebEngineBrowserContext final : public content::BrowserContext {
 public:
  // Creates a browser context that persists cookies, LocalStorage, etc, in
  // the specified |data_directory|.
  static std::unique_ptr<WebEngineBrowserContext> CreatePersistent(
      base::FilePath data_directory);

  // Creates a browser context with no support for persistent data.
  static std::unique_ptr<WebEngineBrowserContext> CreateIncognito();

  ~WebEngineBrowserContext() override;

  WebEngineBrowserContext(const WebEngineBrowserContext&) = delete;
  WebEngineBrowserContext& operator=(const WebEngineBrowserContext&) = delete;

  // BrowserContext implementation.
  std::unique_ptr<content::ZoomLevelDelegate> CreateZoomLevelDelegate(
      const base::FilePath& partition_path) override;
  base::FilePath GetPath() override;
  bool IsOffTheRecord() override;
  content::ResourceContext* GetResourceContext() override;
  content::DownloadManagerDelegate* GetDownloadManagerDelegate() override;
  content::BrowserPluginGuestManager* GetGuestManager() override;
  storage::SpecialStoragePolicy* GetSpecialStoragePolicy() override;
  content::PushMessagingService* GetPushMessagingService() override;
  content::StorageNotificationService* GetStorageNotificationService() override;
  content::SSLHostStateDelegate* GetSSLHostStateDelegate() override;
  content::PermissionControllerDelegate* GetPermissionControllerDelegate()
      override;
  content::ClientHintsControllerDelegate* GetClientHintsControllerDelegate()
      override;
  content::BackgroundFetchDelegate* GetBackgroundFetchDelegate() override;
  content::BackgroundSyncController* GetBackgroundSyncController() override;
  content::BrowsingDataRemoverDelegate* GetBrowsingDataRemoverDelegate()
      override;
  std::unique_ptr<media::VideoDecodePerfHistory> CreateVideoDecodePerfHistory()
      override;

 private:
  // Contains URLRequestContextGetter required for resource loading.
  class ResourceContext;

  explicit WebEngineBrowserContext(base::FilePath data_dir_path);

  const base::FilePath data_dir_path_;

  const std::unique_ptr<WebEngineNetLogObserver> net_log_observer_;
  SimpleFactoryKey simple_factory_key_;
  WebEnginePermissionDelegate permission_delegate_;
  std::unique_ptr<ResourceContext> resource_context_;
};

#endif  // FUCHSIA_ENGINE_BROWSER_WEB_ENGINE_BROWSER_CONTEXT_H_
