// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FUCHSIA_ENGINE_BROWSER_FRAME_IMPL_H_
#define FUCHSIA_ENGINE_BROWSER_FRAME_IMPL_H_

#include <fuchsia/logger/cpp/fidl.h>
#include <fuchsia/web/cpp/fidl.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/inspect/cpp/vmo/types.h>
#include <lib/syslog/logger.h>
#include <lib/ui/scenic/cpp/view_ref_pair.h>
#include <lib/zx/channel.h>

#include <list>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/fuchsia/scoped_fx_logger.h"
#include "base/macros.h"
#include "base/memory/read_only_shared_memory_region.h"
#include "components/media_control/browser/media_blocker.h"
#include "components/on_load_script_injector/browser/on_load_script_injector_host.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/browser/web_contents_observer.h"
#include "fuchsia/engine/browser/accessibility_bridge.h"
#include "fuchsia/engine/browser/event_filter.h"
#include "fuchsia/engine/browser/frame_permission_controller.h"
#include "fuchsia/engine/browser/navigation_controller_impl.h"
#include "fuchsia/engine/browser/theme_manager.h"
#include "fuchsia/engine/browser/url_request_rewrite_rules_manager.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "ui/aura/window_tree_host.h"
#include "ui/wm/core/focus_controller.h"
#include "url/gurl.h"

namespace content {
class FromRenderFrameHost;
}  // namespace content

class ReceiverSessionClient;
class ContextImpl;
class FrameWindowTreeHost;
class FrameLayoutManager;
class MediaPlayerImpl;
class NavigationPolicyHandler;

// Implementation of fuchsia.web.Frame based on content::WebContents.
class FrameImpl : public fuchsia::web::Frame,
                  public content::WebContentsObserver,
                  public content::WebContentsDelegate {
 public:
  // Returns FrameImpl that owns the |web_contents| or nullptr if the
  // |web_contents| is nullptr.
  static FrameImpl* FromWebContents(content::WebContents* web_contents);

  // Returns FrameImpl that owns the |render_frame_host| or nullptr if the
  // |render_frame_host| is nullptr.
  static FrameImpl* FromRenderFrameHost(
      content::RenderFrameHost* render_frame_host);

  // |context| must out-live |this|.
  // |params| apply both to this Frame, and also to any popup Frames it creates.
  // |inspect_node| will be populated with diagnostic data for this Frame.
  // DestroyFrame() is automatically called on |context| if the |frame_request|
  // channel disconnects.
  FrameImpl(std::unique_ptr<content::WebContents> web_contents,
            ContextImpl* context,
            fuchsia::web::CreateFrameParams params,
            inspect::Node inspect_node,
            fidl::InterfaceRequest<fuchsia::web::Frame> frame_request);
  ~FrameImpl() override;

  FrameImpl(const FrameImpl&) = delete;
  FrameImpl& operator=(const FrameImpl&) = delete;

  uint64_t media_session_id() const { return media_session_id_; }

  FramePermissionController* permission_controller() {
    return &permission_controller_;
  }

  UrlRequestRewriteRulesManager* url_request_rewrite_rules_manager() {
    return &url_request_rewrite_rules_manager_;
  }

  NavigationPolicyHandler* navigation_policy_handler() {
    return navigation_policy_handler_.get();
  }

  // Enables explicit sites filtering and set the error page. If |error_page| is
  // empty, the default error page will be used.
  void EnableExplicitSitesFilter(std::string error_page);

  const absl::optional<std::string>& explicit_sites_filter_error_page() const {
    return explicit_sites_filter_error_page_;
  }

  // Accessors required by tests.
  zx::unowned_channel GetBindingChannelForTest() const;
  content::WebContents* web_contents_for_test() const {
    return web_contents_.get();
  }
  bool has_view_for_test() const { return window_tree_host_ != nullptr; }
  AccessibilityBridge* accessibility_bridge_for_test() const {
    return accessibility_bridge_.get();
  }
  void set_semantics_manager_for_test(
      fuchsia::accessibility::semantics::SemanticsManager* semantics_manager) {
    semantics_manager_for_test_ = semantics_manager;
  }
  FrameWindowTreeHost* window_tree_host_for_test() {
    return window_tree_host_.get();
  }

 private:
  FRIEND_TEST_ALL_PREFIXES(FrameImplTest, DelayedNavigationEventAck);
  FRIEND_TEST_ALL_PREFIXES(FrameImplTest, NavigationObserverDisconnected);
  FRIEND_TEST_ALL_PREFIXES(FrameImplTest, NoNavigationObserverAttached);
  FRIEND_TEST_ALL_PREFIXES(FrameImplTest, ReloadFrame);
  FRIEND_TEST_ALL_PREFIXES(FrameImplTest, Stop);

  aura::Window* root_window() const;

  // Shared implementation for the ExecuteJavaScript[NoResult]() APIs.
  void ExecuteJavaScriptInternal(std::vector<std::string> origins,
                                 fuchsia::mem::Buffer script,
                                 ExecuteJavaScriptCallback callback,
                                 bool need_result);

  // Sends the next entry in |pending_popups_| to |popup_listener_|.
  void MaybeSendPopup();

  void OnPopupListenerDisconnected(zx_status_t status);

  // Cleans up the MediaPlayerImpl on disconnect.
  void OnMediaPlayerDisconnect();

  // An error handler for |accessibility_bridge_|.
  void OnAccessibilityError(zx_status_t error);

  // Initializes WindowTreeHost for the view with the specified |view_token|.
  // |view_token| may be uninitialized in headless mode.
  void InitWindowTreeHost(fuchsia::ui::views::ViewToken view_token,
                          scenic::ViewRefPair view_ref_pair);

  // Destroys the WindowTreeHost along with its view or other associated
  // resources.
  void DestroyWindowTreeHost();

  // Destroys |this| and sends the FIDL |error| to the client.
  void CloseAndDestroyFrame(zx_status_t error);

  // Determines whether |message| is a Cast Streaming message and if so, handles
  // it. Returns whether it handled the message, regardless of whether that was
  // successful. If true is returned, |callback| has been called. Returns false
  // immediately if Cast Streaming support is not enabled. Called by
  // PostMessage().
  bool MaybeHandleCastStreamingMessage(std::string* origin,
                                       fuchsia::web::WebMessage* message,
                                       PostMessageCallback* callback);

  void MaybeStartCastStreaming(content::NavigationHandle* navigation_handle);

  // Updates zoom level for the specified |render_view_host|.
  void UpdateRenderViewZoomLevel(content::RenderViewHost* render_view_host);

  // fuchsia::web::Frame implementation.
  void CreateView(fuchsia::ui::views::ViewToken view_token) override;
  void CreateViewWithViewRef(fuchsia::ui::views::ViewToken view_token,
                             fuchsia::ui::views::ViewRefControl control_ref,
                             fuchsia::ui::views::ViewRef view_ref) override;
  void GetMediaPlayer(fidl::InterfaceRequest<fuchsia::media::sessions2::Player>
                          player) override;
  void GetNavigationController(
      fidl::InterfaceRequest<fuchsia::web::NavigationController> controller)
      override;
  void ExecuteJavaScript(std::vector<std::string> origins,
                         fuchsia::mem::Buffer script,
                         ExecuteJavaScriptCallback callback) override;
  void ExecuteJavaScriptNoResult(
      std::vector<std::string> origins,
      fuchsia::mem::Buffer script,
      ExecuteJavaScriptNoResultCallback callback) override;
  void AddBeforeLoadJavaScript(
      uint64_t id,
      std::vector<std::string> origins,
      fuchsia::mem::Buffer script,
      AddBeforeLoadJavaScriptCallback callback) override;
  void RemoveBeforeLoadJavaScript(uint64_t id) override;
  void PostMessage(std::string origin,
                   fuchsia::web::WebMessage message,
                   fuchsia::web::Frame::PostMessageCallback callback) override;
  void SetNavigationEventListener(
      fidl::InterfaceHandle<fuchsia::web::NavigationEventListener> listener)
      override;
  void SetNavigationEventListener2(
      fidl::InterfaceHandle<fuchsia::web::NavigationEventListener> listener,
      fuchsia::web::NavigationEventListenerFlags flags) override;
  void SetJavaScriptLogLevel(fuchsia::web::ConsoleLogLevel level) override;
  void SetConsoleLogSink(fuchsia::logger::LogSinkHandle sink) override;
  void ConfigureInputTypes(fuchsia::web::InputTypes types,
                           fuchsia::web::AllowInputState allow) override;
  void SetPopupFrameCreationListener(
      fidl::InterfaceHandle<fuchsia::web::PopupFrameCreationListener> listener)
      override;
  void SetUrlRequestRewriteRules(
      std::vector<fuchsia::web::UrlRequestRewriteRule> rules,
      SetUrlRequestRewriteRulesCallback callback) override;
  void EnableHeadlessRendering() override;
  void DisableHeadlessRendering() override;
  void SetMediaSessionId(uint64_t session_id) override;
  void ForceContentDimensions(
      std::unique_ptr<fuchsia::ui::gfx::vec2> web_dips) override;
  void SetPermissionState(fuchsia::web::PermissionDescriptor permission,
                          std::string web_origin,
                          fuchsia::web::PermissionState state) override;
  void SetBlockMediaLoading(bool blocked) override;
  void MediaStartedPlaying(const MediaPlayerInfo& video_type,
                           const content::MediaPlayerId& id) override;
  void MediaStoppedPlaying(
      const MediaPlayerInfo& video_type,
      const content::MediaPlayerId& id,
      WebContentsObserver::MediaStoppedReason reason) override;
  void GetPrivateMemorySize(GetPrivateMemorySizeCallback callback) override;
  void SetNavigationPolicyProvider(
      fuchsia::web::NavigationPolicyProviderParams params,
      fidl::InterfaceHandle<fuchsia::web::NavigationPolicyProvider> provider)
      override;
  void SetPreferredTheme(fuchsia::settings::ThemeType theme) override;
  void SetPageScale(float scale) override;

  // content::WebContentsDelegate implementation.
  void CloseContents(content::WebContents* source) override;
  bool DidAddMessageToConsole(content::WebContents* source,
                              blink::mojom::ConsoleMessageLevel log_level,
                              const std::u16string& message,
                              int32_t line_no,
                              const std::u16string& source_id) override;
  bool IsWebContentsCreationOverridden(
      content::SiteInstance* source_site_instance,
      content::mojom::WindowContainerType window_container_type,
      const GURL& opener_url,
      const std::string& frame_name,
      const GURL& target_url) override;
  void WebContentsCreated(content::WebContents* source_contents,
                          int opener_render_process_id,
                          int opener_render_frame_id,
                          const std::string& frame_name,
                          const GURL& target_url,
                          content::WebContents* new_contents) override;
  void AddNewContents(content::WebContents* source,
                      std::unique_ptr<content::WebContents> new_contents,
                      const GURL& target_url,
                      WindowOpenDisposition disposition,
                      const gfx::Rect& initial_rect,
                      bool user_gesture,
                      bool* was_blocked) override;
  void RequestMediaAccessPermission(
      content::WebContents* web_contents,
      const content::MediaStreamRequest& request,
      content::MediaResponseCallback callback) override;
  bool CheckMediaAccessPermission(content::RenderFrameHost* render_frame_host,
                                  const GURL& security_origin,
                                  blink::mojom::MediaStreamType type) override;
  bool CanOverscrollContent() override;

  // content::WebContentsObserver implementation.
  void ReadyToCommitNavigation(
      content::NavigationHandle* navigation_handle) override;
  void DidFinishLoad(content::RenderFrameHost* render_frame_host,
                     const GURL& validated_url) override;
  void RenderFrameCreated(content::RenderFrameHost* frame_host) override;
  void RenderViewHostChanged(content::RenderViewHost* old_host,
                             content::RenderViewHost* new_host) override;
  void DidFirstVisuallyNonEmptyPaint() override;
  void ResourceLoadComplete(
      content::RenderFrameHost* render_frame_host,
      const content::GlobalRequestID& request_id,
      const blink::mojom::ResourceLoadInfo& resource_load_info) override;

  const std::unique_ptr<content::WebContents> web_contents_;
  ContextImpl* const context_;

  // Optional tag to apply when emitting web console logs.
  const std::string console_log_tag_;

  // Logger used for console messages from content, depending on |log_level_|.
  base::ScopedFxLogger console_logger_;
  fx_log_severity_t log_level_ = FX_LOG_NONE;

  // Parameters applied to popups created by content running in this Frame.
  const fuchsia::web::CreateFrameParams params_for_popups_;

  std::unique_ptr<FrameWindowTreeHost> window_tree_host_;

  std::unique_ptr<wm::FocusController> focus_controller_;

  // Owned via |window_tree_host_|.
  FrameLayoutManager* layout_manager_ = nullptr;

  std::unique_ptr<AccessibilityBridge> accessibility_bridge_;
  fuchsia::accessibility::semantics::SemanticsManager*
      semantics_manager_for_test_ = nullptr;

  EventFilter event_filter_;
  NavigationControllerImpl navigation_controller_;
  UrlRequestRewriteRulesManager url_request_rewrite_rules_manager_;
  FramePermissionController permission_controller_;
  std::unique_ptr<NavigationPolicyHandler> navigation_policy_handler_;

  // Current page scale. Updated by calling SetPageScale().
  float page_scale_ = 1.0;

  // Session ID to use for fuchsia.media.AudioConsumer. Set with
  // SetMediaSessionId().
  uint64_t media_session_id_ = 0;

  // Used for receiving and dispatching popup created by this Frame.
  fuchsia::web::PopupFrameCreationListenerPtr popup_listener_;
  std::list<std::unique_ptr<content::WebContents>> pending_popups_;
  bool popup_ack_outstanding_ = false;
  gfx::Size render_size_override_;

  std::unique_ptr<MediaPlayerImpl> media_player_;
  std::unique_ptr<ReceiverSessionClient> receiver_session_client_;
  on_load_script_injector::OnLoadScriptInjectorHost<uint64_t> script_injector_;

  fidl::Binding<fuchsia::web::Frame> binding_;
  media_control::MediaBlocker media_blocker_;

  ThemeManager theme_manager_;

  // The error page to be displayed when a navigation to an explicit site is
  // filtered. Explicit sites are filtered if it has a value. If set to the
  // empty string, the default error page will be displayed.
  absl::optional<std::string> explicit_sites_filter_error_page_;

  // Used to publish Frame details to Inspect.
  inspect::Node inspect_node_;
  const inspect::StringProperty inspect_name_property_;

  base::WeakPtrFactory<FrameImpl> weak_factory_{this};
};

#endif  // FUCHSIA_ENGINE_BROWSER_FRAME_IMPL_H_
