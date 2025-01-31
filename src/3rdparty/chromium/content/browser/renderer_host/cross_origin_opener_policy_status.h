// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_RENDERER_HOST_CROSS_ORIGIN_OPENER_POLICY_STATUS_H_
#define CONTENT_BROWSER_RENDERER_HOST_CROSS_ORIGIN_OPENER_POLICY_STATUS_H_

#include <memory>

#include "services/network/public/cpp/cross_origin_opener_policy.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

namespace net {
class NetworkIsolationKey;
}  // namespace net

namespace content {
class CrossOriginOpenerPolicyReporter;
class FrameTreeNode;
class NavigationRequest;
class StoragePartition;

// Groups information used to apply COOP during navigations. This class will be
// used to trigger a number of mechanisms such as BrowsingInstance switch or
// reporting.
class CrossOriginOpenerPolicyStatus {
 public:
  explicit CrossOriginOpenerPolicyStatus(NavigationRequest* navigation_request);
  ~CrossOriginOpenerPolicyStatus();

  // Sanitize the COOP header from the `response`.
  // Return an error, and swap browsing context group when COOP is used on
  // sandboxed popups.
  absl::optional<network::mojom::BlockedByResponseReason> SanitizeResponse(
      network::mojom::URLResponseHead* response);

  // Called when receiving a redirect or the final response.
  void EnforceCOOP(const network::CrossOriginOpenerPolicy& response_coop,
                   const url::Origin& response_origin,
                   const net::NetworkIsolationKey& network_isolation_key);

  // Set to true whenever the Cross-Origin-Opener-Policy spec requires a
  // "BrowsingContext group" swap:
  // https://gist.github.com/annevk/6f2dd8c79c77123f39797f6bdac43f3e
  // This forces the new RenderFrameHost to use a different BrowsingInstance
  // than the current one. If other pages had JavaScript references to the
  // Window object for the frame (via window.opener, window.open(), et cetera),
  // those references will be broken; window.name will also be reset to an empty
  // string.
  bool require_browsing_instance_swap() const {
    return require_browsing_instance_swap_;
  }

  // The virtual browsing context group of the document to commit. Initially,
  // the navigation inherits the virtual browsing context group of the current
  // document. Updated when the report-only COOP of a response would result in
  // a browsing context group swap if enforced.
  int virtual_browsing_context_group() const {
    return virtual_browsing_context_group_;
  }

  // The COOP used when comparing to the COOP and origin of a response. At the
  // beginning of the navigation, it is the COOP of the current document. After
  // receiving any kind of response, including redirects, it is the COOP of the
  // last response.
  const network::CrossOriginOpenerPolicy& current_coop() const {
    return current_coop_;
  }

  std::unique_ptr<CrossOriginOpenerPolicyReporter> TakeCoopReporter();

  // Called when a RenderFrameHost has been created to use its process's
  // storage partition. Until then, the reporter uses the current process
  // storage partition.
  void UpdateReporterStoragePartition(StoragePartition* storage_partition);

 private:
  // Make sure COOP is relevant or clear the COOP headers.
  void SanitizeCoopHeaders(
      const GURL& response_url,
      network::mojom::URLResponseHead* response_head) const;

  // The NavigationRequest which owns this object.
  NavigationRequest* const navigation_request_;

  // Tracks the FrameTreeNode in which this navigation is taking place.
  const FrameTreeNode* frame_tree_node_;

  bool require_browsing_instance_swap_ = false;

  int virtual_browsing_context_group_;

  // Whether this is the first navigation happening in the browsing context.
  // TODO(https://crbug.com/1216244): This should be set to whether this
  // navigation happens on the initial empty document instead. Currently it's
  // set to FrameTreeNode's has_committed_real_load(), which does not consider
  // document.open(), etc.
  const bool is_initial_navigation_;

  network::CrossOriginOpenerPolicy current_coop_;

  // The origin used when comparing to the COOP and origin of a response. At
  // the beginning of the navigation, it is the origin of the current document.
  // After receiving any kind of response, including redirects, it is the origin
  // of the last response.
  url::Origin current_origin_;

  // The current URL, to use for reporting. At the beginning of the navigation,
  // it is the URL of the current document. After receiving any kind of
  // response, including redirects, it is the URL of the last response.
  GURL current_url_;

  // Indicates whether to use the reporter in the current RenderFrameHost to
  // send a report for a navigation away from a current response. If false, the
  // |coop_reporter| field from this CrossOriginOpenerPolicyStatus should be
  // used instead.
  bool use_current_document_coop_reporter_ = true;

  // The reporter currently in use by COOP.
  std::unique_ptr<CrossOriginOpenerPolicyReporter> coop_reporter_;

  // Whether the current context tracked by this CrossOriginOpenerPolicy is the
  // source of the current navigation. This is updated every time we receive a
  // redirect, as redirects are considered the source of the navigation to the
  // next URL.
  bool is_navigation_source_;
};

}  // namespace content

#endif  // CONTENT_BROWSER_RENDERER_HOST_CROSS_ORIGIN_OPENER_POLICY_STATUS_H_
