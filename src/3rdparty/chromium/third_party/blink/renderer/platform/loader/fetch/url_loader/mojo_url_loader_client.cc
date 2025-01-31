// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/loader/fetch/url_loader/mojo_url_loader_client.h"

#include <iterator>

#include "base/bind.h"
#include "base/callback.h"
#include "base/containers/queue.h"
#include "base/feature_list.h"
#include "base/metrics/histogram_macros.h"
#include "base/single_thread_task_runner.h"
#include "base/trace_event/trace_event.h"
#include "mojo/public/cpp/system/data_pipe_drainer.h"
#include "net/url_request/redirect_info.h"
#include "services/network/public/cpp/features.h"
#include "services/network/public/mojom/early_hints.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "third_party/blink/public/common/features.h"
#include "third_party/blink/public/mojom/frame/back_forward_cache_controller.mojom.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/public/platform/web_back_forward_cache_loader_helper.h"
#include "third_party/blink/public/platform/web_resource_request_sender.h"
#include "third_party/blink/renderer/platform/back_forward_cache_utils.h"
#include "third_party/blink/renderer/platform/loader/fetch/back_forward_cache_loader_helper.h"
#include "third_party/blink/renderer/platform/loader/fetch/loader_freeze_mode.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"

namespace blink {
namespace {

constexpr size_t kDefaultMaxBufferedBodyBytesPerRequestX = 100 * 1000;
constexpr base::TimeDelta kGracePeriodToFinishLoadingWhileInBackForwardCache =
    base::TimeDelta::FromSeconds(60);

}  // namespace

class MojoURLLoaderClient::DeferredMessage {
 public:
  DeferredMessage() = default;
  DeferredMessage(const DeferredMessage&) = delete;
  DeferredMessage& operator=(const DeferredMessage&) = delete;
  virtual ~DeferredMessage() = default;

  virtual void HandleMessage(
      WebResourceRequestSender* resource_request_sender) = 0;
  virtual bool IsCompletionMessage() const = 0;
};

class MojoURLLoaderClient::DeferredOnReceiveResponse final
    : public DeferredMessage {
 public:
  explicit DeferredOnReceiveResponse(
      network::mojom::URLResponseHeadPtr response_head)
      : response_head_(std::move(response_head)) {}

  void HandleMessage(
      WebResourceRequestSender* resource_request_sender) override {
    resource_request_sender->OnReceivedResponse(std::move(response_head_));
  }
  bool IsCompletionMessage() const override { return false; }

 private:
  network::mojom::URLResponseHeadPtr response_head_;
};

class MojoURLLoaderClient::DeferredOnReceiveRedirect final
    : public DeferredMessage {
 public:
  DeferredOnReceiveRedirect(
      const net::RedirectInfo& redirect_info,
      network::mojom::URLResponseHeadPtr response_head,
      scoped_refptr<base::SingleThreadTaskRunner> task_runner)
      : redirect_info_(redirect_info),
        response_head_(std::move(response_head)),
        task_runner_(std::move(task_runner)) {}

  void HandleMessage(
      WebResourceRequestSender* resource_request_sender) override {
    resource_request_sender->OnReceivedRedirect(
        redirect_info_, std::move(response_head_), task_runner_);
  }
  bool IsCompletionMessage() const override { return false; }

 private:
  const net::RedirectInfo redirect_info_;
  network::mojom::URLResponseHeadPtr response_head_;
  scoped_refptr<base::SingleThreadTaskRunner> task_runner_;
};

class MojoURLLoaderClient::DeferredOnUploadProgress final
    : public DeferredMessage {
 public:
  DeferredOnUploadProgress(int64_t current, int64_t total)
      : current_(current), total_(total) {}

  void HandleMessage(
      WebResourceRequestSender* resource_request_sender) override {
    resource_request_sender->OnUploadProgress(current_, total_);
  }
  bool IsCompletionMessage() const override { return false; }

 private:
  const int64_t current_;
  const int64_t total_;
};

class MojoURLLoaderClient::DeferredOnReceiveCachedMetadata final
    : public DeferredMessage {
 public:
  explicit DeferredOnReceiveCachedMetadata(mojo_base::BigBuffer data)
      : data_(std::move(data)) {}

  void HandleMessage(
      WebResourceRequestSender* resource_request_sender) override {
    resource_request_sender->OnReceivedCachedMetadata(std::move(data_));
  }
  bool IsCompletionMessage() const override { return false; }

 private:
  mojo_base::BigBuffer data_;
};

class MojoURLLoaderClient::DeferredOnStartLoadingResponseBody final
    : public DeferredMessage {
 public:
  explicit DeferredOnStartLoadingResponseBody(
      mojo::ScopedDataPipeConsumerHandle body)
      : body_(std::move(body)) {}

  void HandleMessage(
      WebResourceRequestSender* resource_request_sender) override {
    resource_request_sender->OnStartLoadingResponseBody(std::move(body_));
  }
  bool IsCompletionMessage() const override { return false; }

 private:
  mojo::ScopedDataPipeConsumerHandle body_;
};

class MojoURLLoaderClient::DeferredOnComplete final : public DeferredMessage {
 public:
  explicit DeferredOnComplete(const network::URLLoaderCompletionStatus& status)
      : status_(status) {}

  void HandleMessage(
      WebResourceRequestSender* resource_request_sender) override {
    resource_request_sender->OnRequestComplete(status_);
  }
  bool IsCompletionMessage() const override { return true; }

 private:
  const network::URLLoaderCompletionStatus status_;
};

class MojoURLLoaderClient::BodyBuffer final
    : public mojo::DataPipeDrainer::Client {
 public:
  BodyBuffer(MojoURLLoaderClient* owner,
             mojo::ScopedDataPipeConsumerHandle readable,
             mojo::ScopedDataPipeProducerHandle writable,
             scoped_refptr<base::SingleThreadTaskRunner> task_runner)
      : owner_(owner),
        writable_(std::move(writable)),
        writable_watcher_(FROM_HERE,
                          mojo::SimpleWatcher::ArmingPolicy::MANUAL,
                          std::move(task_runner)),
        max_bytes_drained_(GetLoadingTasksUnfreezableParamAsInt(
            "max_buffered_bytes",
            kDefaultMaxBufferedBodyBytesPerRequestX)) {
    pipe_drainer_ =
        std::make_unique<mojo::DataPipeDrainer>(this, std::move(readable));
    writable_watcher_.Watch(
        writable_.get(),
        MOJO_HANDLE_SIGNAL_WRITABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
        base::BindRepeating(&BodyBuffer::WriteBufferedBody,
                            base::Unretained(this)));
  }

  bool active() const { return writable_watcher_.IsWatching(); }

  // mojo::DataPipeDrainer::Client
  void OnDataAvailable(const void* data, size_t num_bytes) override {
    DCHECK(draining_);
    SCOPED_CRASH_KEY_NUMBER("OnDataAvailable", "buffered_body_size",
                            buffered_body_.size());
    SCOPED_CRASH_KEY_NUMBER("OnDataAvailable", "data_bytes", num_bytes);
    SCOPED_CRASH_KEY_STRING256("OnDataAvailable", "last_loaded_url",
                               owner_->last_loaded_url().GetString().Utf8());

    total_bytes_drained_ += num_bytes;
    TRACE_EVENT2("loading", "MojoURLLoaderClient::BodyBuffer::OnDataAvailable",
                 "total_bytes_drained", static_cast<int>(total_bytes_drained_),
                 "added_bytes", static_cast<int>(num_bytes));

    if (owner_->freeze_mode() == WebLoaderFreezeMode::kBufferIncoming) {
      owner_->DidBufferLoadWhileInBackForwardCache(num_bytes);
      if (total_bytes_drained_ > max_bytes_drained_ ||
          !owner_->CanContinueBufferingWhileInBackForwardCache()) {
        owner_->EvictFromBackForwardCache(
            blink::mojom::RendererEvictionReason::kNetworkExceedsBufferLimit);
        return;
      }
    }
    buffered_body_.emplace(static_cast<const char*>(data),
                           static_cast<const char*>(data) + num_bytes);
    WriteBufferedBody(MOJO_RESULT_OK);
  }

  void OnDataComplete() override {
    DCHECK(draining_);
    draining_ = false;
    WriteBufferedBody(MOJO_RESULT_OK);
  }

 private:
  void WriteBufferedBody(MojoResult) {
    // Try to write all the remaining chunks in |buffered_body_|.
    while (!buffered_body_.empty()) {
      // Write the chunk at the front of |buffered_body_|.
      const std::vector<char>& current_chunk = buffered_body_.front();
      DCHECK_LE(offset_in_current_chunk_, current_chunk.size());
      uint32_t bytes_sent = base::saturated_cast<uint32_t>(
          current_chunk.size() - offset_in_current_chunk_);
      MojoResult result =
          writable_->WriteData(current_chunk.data() + offset_in_current_chunk_,
                               &bytes_sent, MOJO_WRITE_DATA_FLAG_NONE);
      switch (result) {
        case MOJO_RESULT_OK:
          break;
        case MOJO_RESULT_FAILED_PRECONDITION:
          // The pipe is closed unexpectedly, finish writing now.
          draining_ = false;
          Finish();
          return;
        case MOJO_RESULT_SHOULD_WAIT:
          writable_watcher_.ArmOrNotify();
          return;
        default:
          NOTREACHED();
          return;
      }
      // We've sent |bytes_sent| bytes, update the current offset in the
      // frontmost chunk.
      offset_in_current_chunk_ += bytes_sent;
      DCHECK_LE(offset_in_current_chunk_, current_chunk.size());
      if (offset_in_current_chunk_ == current_chunk.size()) {
        // We've finished writing the chunk at the front of the queue, pop it so
        // that we'll write the next chunk next time.
        buffered_body_.pop();
        offset_in_current_chunk_ = 0;
      }
    }
    // We're finished if we've drained the original pipe and sent all the
    // buffered body.
    if (!draining_)
      Finish();
  }

  void Finish() {
    DCHECK(!draining_);
    // We've read and written all the data from the original pipe.
    writable_watcher_.Cancel();
    writable_.reset();
    // There might be a deferred OnComplete message waiting for us to finish
    // draining the response body, so flush the deferred messages in
    // the owner MojoURLLoaderClient.
    owner_->FlushDeferredMessages();
  }

  MojoURLLoaderClient* const owner_;
  mojo::ScopedDataPipeProducerHandle writable_;
  mojo::SimpleWatcher writable_watcher_;
  std::unique_ptr<mojo::DataPipeDrainer> pipe_drainer_;
  // We save the received response body as a queue of chunks so that we can free
  // memory as soon as we finish sending a chunk completely.
  base::queue<std::vector<char>> buffered_body_;
  uint32_t offset_in_current_chunk_ = 0;
  size_t total_bytes_drained_ = 0;
  const size_t max_bytes_drained_;
  bool draining_ = true;
};

MojoURLLoaderClient::MojoURLLoaderClient(
    WebResourceRequestSender* resource_request_sender,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner,
    bool bypass_redirect_checks,
    const GURL& request_url,
    WebBackForwardCacheLoaderHelper back_forward_cache_loader_helper)
    : back_forward_cache_timeout_(
          base::TimeDelta::FromSeconds(GetLoadingTasksUnfreezableParamAsInt(
              "grace_period_to_finish_loading_in_seconds",
              static_cast<int>(
                  kGracePeriodToFinishLoadingWhileInBackForwardCache
                      .InSeconds())))),
      resource_request_sender_(resource_request_sender),
      task_runner_(std::move(task_runner)),
      bypass_redirect_checks_(bypass_redirect_checks),
      last_loaded_url_(request_url),
      back_forward_cache_loader_helper_(back_forward_cache_loader_helper) {}

MojoURLLoaderClient::~MojoURLLoaderClient() = default;

void MojoURLLoaderClient::Freeze(WebLoaderFreezeMode mode) {
  freeze_mode_ = mode;
  if (mode == WebLoaderFreezeMode::kNone) {
    StopBackForwardCacheEvictionTimer();
    task_runner_->PostTask(
        FROM_HERE, WTF::Bind(&MojoURLLoaderClient::FlushDeferredMessages,
                             weak_factory_.GetWeakPtr()));
  } else if (mode == WebLoaderFreezeMode::kBufferIncoming &&
             !has_received_complete_ &&
             !back_forward_cache_eviction_timer_.IsRunning()) {
    // We should evict the page associated with this load if the connection
    // takes too long until it either finished or failed.
    back_forward_cache_eviction_timer_.SetTaskRunner(task_runner_);
    back_forward_cache_eviction_timer_.Start(
        FROM_HERE, back_forward_cache_timeout_,
        WTF::Bind(&MojoURLLoaderClient::EvictFromBackForwardCacheDueToTimeout,
                  weak_factory_.GetWeakPtr()));
  }
}

void MojoURLLoaderClient::OnReceiveEarlyHints(
    network::mojom::EarlyHintsPtr early_hints) {}

void MojoURLLoaderClient::OnReceiveResponse(
    network::mojom::URLResponseHeadPtr response_head) {
  TRACE_EVENT1("loading", "MojoURLLoaderClient::OnReceiveResponse", "url",
               last_loaded_url_.GetString().Utf8());

  has_received_response_head_ = true;
  on_receive_response_time_ = base::TimeTicks::Now();

  if (NeedsStoringMessage()) {
    StoreAndDispatch(
        std::make_unique<DeferredOnReceiveResponse>(std::move(response_head)));
  } else {
    resource_request_sender_->OnReceivedResponse(std::move(response_head));
  }
}

BackForwardCacheLoaderHelper*
MojoURLLoaderClient::GetBackForwardCacheLoaderHelper() {
  return back_forward_cache_loader_helper_.GetBackForwardCacheLoaderHelper();
}

void MojoURLLoaderClient::EvictFromBackForwardCache(
    blink::mojom::RendererEvictionReason reason) {
  StopBackForwardCacheEvictionTimer();
  auto* back_forward_cache_loader_helper = GetBackForwardCacheLoaderHelper();
  if (!back_forward_cache_loader_helper)
    return;
  back_forward_cache_loader_helper->EvictFromBackForwardCache(reason);
}

void MojoURLLoaderClient::DidBufferLoadWhileInBackForwardCache(
    size_t num_bytes) {
  auto* back_forward_cache_loader_helper = GetBackForwardCacheLoaderHelper();
  if (!back_forward_cache_loader_helper)
    return;
  back_forward_cache_loader_helper->DidBufferLoadWhileInBackForwardCache(
      num_bytes);
}

bool MojoURLLoaderClient::CanContinueBufferingWhileInBackForwardCache() {
  auto* back_forward_cache_loader_helper = GetBackForwardCacheLoaderHelper();
  if (!back_forward_cache_loader_helper)
    return false;
  return back_forward_cache_loader_helper
      ->CanContinueBufferingWhileInBackForwardCache();
}

void MojoURLLoaderClient::EvictFromBackForwardCacheDueToTimeout() {
  EvictFromBackForwardCache(
      blink::mojom::RendererEvictionReason::kNetworkRequestTimeout);
}

void MojoURLLoaderClient::StopBackForwardCacheEvictionTimer() {
  back_forward_cache_eviction_timer_.Stop();
}

void MojoURLLoaderClient::OnReceiveRedirect(
    const net::RedirectInfo& redirect_info,
    network::mojom::URLResponseHeadPtr response_head) {
  DCHECK(!has_received_response_head_);
  if (freeze_mode_ == WebLoaderFreezeMode::kBufferIncoming) {
    EvictFromBackForwardCache(
        blink::mojom::RendererEvictionReason::kNetworkRequestRedirected);

    OnComplete(network::URLLoaderCompletionStatus(net::ERR_ABORTED));
    return;
  }
  if (!bypass_redirect_checks_ &&
      !Platform::Current()->IsRedirectSafe(last_loaded_url_,
                                           redirect_info.new_url)) {
    OnComplete(network::URLLoaderCompletionStatus(net::ERR_UNSAFE_REDIRECT));
    return;
  }

  last_loaded_url_ = KURL(redirect_info.new_url);
  if (NeedsStoringMessage()) {
    StoreAndDispatch(std::make_unique<DeferredOnReceiveRedirect>(
        redirect_info, std::move(response_head), task_runner_));
  } else {
    resource_request_sender_->OnReceivedRedirect(
        redirect_info, std::move(response_head), task_runner_);
  }
}

void MojoURLLoaderClient::OnUploadProgress(
    int64_t current_position,
    int64_t total_size,
    OnUploadProgressCallback ack_callback) {
  if (NeedsStoringMessage()) {
    StoreAndDispatch(std::make_unique<DeferredOnUploadProgress>(
        current_position, total_size));
  } else {
    resource_request_sender_->OnUploadProgress(current_position, total_size);
  }
  std::move(ack_callback).Run();
}

void MojoURLLoaderClient::OnReceiveCachedMetadata(mojo_base::BigBuffer data) {
  if (NeedsStoringMessage()) {
    StoreAndDispatch(
        std::make_unique<DeferredOnReceiveCachedMetadata>(std::move(data)));
  } else {
    resource_request_sender_->OnReceivedCachedMetadata(std::move(data));
  }
}

void MojoURLLoaderClient::OnTransferSizeUpdated(int32_t transfer_size_diff) {
  if (NeedsStoringMessage()) {
    accumulated_transfer_size_diff_during_deferred_ += transfer_size_diff;
  } else {
    resource_request_sender_->OnTransferSizeUpdated(transfer_size_diff);
  }
}

void MojoURLLoaderClient::OnStartLoadingResponseBody(
    mojo::ScopedDataPipeConsumerHandle body) {
  TRACE_EVENT1("loading", "MojoURLLoaderClient::OnStartLoadingResponseBody",
               "url", last_loaded_url_.GetString().Utf8());

  DCHECK(has_received_response_head_);
  DCHECK(!has_received_response_body_);
  has_received_response_body_ = true;

  if (!on_receive_response_time_.is_null()) {
    UMA_HISTOGRAM_TIMES(
        "Renderer.OnReceiveResponseToOnStartLoadingResponseBody",
        base::TimeTicks::Now() - on_receive_response_time_);
  }

  if (!NeedsStoringMessage()) {
    // Send the message immediately.
    resource_request_sender_->OnStartLoadingResponseBody(std::move(body));
    return;
  }

  if (freeze_mode_ != WebLoaderFreezeMode::kBufferIncoming) {
    // Defer the message, storing the original body pipe.
    StoreAndDispatch(
        std::make_unique<DeferredOnStartLoadingResponseBody>(std::move(body)));
    return;
  }

  DCHECK(IsInflightNetworkRequestBackForwardCacheSupportEnabled());
  // We want to run loading tasks while deferred (but without dispatching the
  // messages). Drain the original pipe containing the response body into a
  // new pipe so that we won't block the network service if we're deferred for
  // a long time.
  mojo::ScopedDataPipeProducerHandle new_body_producer;
  mojo::ScopedDataPipeConsumerHandle new_body_consumer;
  MojoResult result =
      mojo::CreateDataPipe(nullptr, new_body_producer, new_body_consumer);
  if (result != MOJO_RESULT_OK) {
    OnComplete(
        network::URLLoaderCompletionStatus(net::ERR_INSUFFICIENT_RESOURCES));
    return;
  }
  body_buffer_ = std::make_unique<BodyBuffer>(
      this, std::move(body), std::move(new_body_producer), task_runner_);

  StoreAndDispatch(std::make_unique<DeferredOnStartLoadingResponseBody>(
      std::move(new_body_consumer)));
}

void MojoURLLoaderClient::OnComplete(
    const network::URLLoaderCompletionStatus& status) {
  has_received_complete_ = true;
  StopBackForwardCacheEvictionTimer();

  // Dispatch completion status to the WebResourceRequestSender.
  // Except for errors, there must always be a response's body.
  DCHECK(has_received_response_body_ || status.error_code != net::OK);
  if (NeedsStoringMessage()) {
    StoreAndDispatch(std::make_unique<DeferredOnComplete>(status));
  } else {
    resource_request_sender_->OnRequestComplete(status);
  }
}

bool MojoURLLoaderClient::NeedsStoringMessage() const {
  return freeze_mode_ != WebLoaderFreezeMode::kNone ||
         deferred_messages_.size() > 0 ||
         accumulated_transfer_size_diff_during_deferred_ > 0;
}

void MojoURLLoaderClient::StoreAndDispatch(
    std::unique_ptr<DeferredMessage> message) {
  DCHECK(NeedsStoringMessage());
  if (freeze_mode_ != WebLoaderFreezeMode::kNone) {
    deferred_messages_.emplace_back(std::move(message));
  } else if (deferred_messages_.size() > 0 ||
             accumulated_transfer_size_diff_during_deferred_ > 0) {
    deferred_messages_.emplace_back(std::move(message));
    FlushDeferredMessages();
  } else {
    NOTREACHED();
  }
}

void MojoURLLoaderClient::OnConnectionClosed() {
  // If the connection aborts before the load completes, mark it as aborted.
  if (!has_received_complete_) {
    OnComplete(network::URLLoaderCompletionStatus(net::ERR_ABORTED));
    return;
  }
}

void MojoURLLoaderClient::FlushDeferredMessages() {
  if (freeze_mode_ != WebLoaderFreezeMode::kNone)
    return;
  WebVector<std::unique_ptr<DeferredMessage>> messages;
  messages.Swap(deferred_messages_);
  bool has_completion_message = false;
  base::WeakPtr<MojoURLLoaderClient> weak_this = weak_factory_.GetWeakPtr();
  // First, dispatch all messages excluding the followings:
  //  - transfer size change
  //  - completion
  // These two types of messages are dispatched later.
  for (size_t index = 0; index < messages.size(); ++index) {
    if (messages[index]->IsCompletionMessage()) {
      // The completion message arrives at the end of the message queue.
      DCHECK(!has_completion_message);
      DCHECK_EQ(index, messages.size() - 1);
      has_completion_message = true;
      break;
    }

    messages[index]->HandleMessage(resource_request_sender_);
    if (!weak_this)
      return;
    if (freeze_mode_ != WebLoaderFreezeMode::kNone) {
      deferred_messages_.reserve(messages.size() - index - 1);
      for (size_t i = index + 1; i < messages.size(); ++i)
        deferred_messages_.emplace_back(std::move(messages[i]));
      return;
    }
  }

  // Dispatch the transfer size update.
  if (accumulated_transfer_size_diff_during_deferred_ > 0) {
    auto transfer_size_diff = accumulated_transfer_size_diff_during_deferred_;
    accumulated_transfer_size_diff_during_deferred_ = 0;
    resource_request_sender_->OnTransferSizeUpdated(transfer_size_diff);
    if (!weak_this)
      return;
    if (freeze_mode_ != WebLoaderFreezeMode::kNone) {
      if (has_completion_message) {
        DCHECK_GT(messages.size(), 0u);
        DCHECK(messages.back()->IsCompletionMessage());
        deferred_messages_.emplace_back(std::move(messages.back()));
      }
      return;
    }
  }

  // Dispatch the completion message.
  if (has_completion_message) {
    DCHECK_GT(messages.size(), 0u);
    DCHECK(messages.back()->IsCompletionMessage());
    if (body_buffer_ && body_buffer_->active()) {
      // If we still have an active body buffer, it means we haven't drained all
      // of the contents of the response body yet. We shouldn't dispatch the
      // completion message now, so
      // put the message back into |deferred_messages_| to be sent later after
      // the body buffer is no longer active.
      deferred_messages_.emplace_back(std::move(messages.back()));
      return;
    }
    messages.back()->HandleMessage(resource_request_sender_);
  }
}

}  // namespace blink
