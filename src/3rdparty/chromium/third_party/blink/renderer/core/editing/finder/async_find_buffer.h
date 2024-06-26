// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_EDITING_FINDER_ASYNC_FIND_BUFFER_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_EDITING_FINDER_ASYNC_FIND_BUFFER_H_

#include "base/callback.h"
#include "third_party/blink/renderer/core/editing/finder/find_buffer_runner.h"
#include "third_party/blink/renderer/core/editing/finder/find_options.h"
#include "third_party/blink/renderer/core/editing/forward.h"
#include "third_party/blink/renderer/core/editing/range_in_flat_tree.h"
#include "third_party/blink/renderer/platform/scheduler/public/post_cancellable_task.h"

namespace blink {
// This is used as an asynchronous wrapper around FindBuffer to provide a
// callback-based interface.
class AsyncFindBuffer final : public FindBufferRunner {
 public:
  explicit AsyncFindBuffer(){};
  ~AsyncFindBuffer() = default;

  void FindMatchInRange(RangeInFlatTree* search_range,
                        String search_text,
                        FindOptions options,
                        Callback completeCallback) override;
  void Cancel() override;
  bool IsActive() override { return pending_find_match_task_.IsActive(); }

 private:
  void Run(RangeInFlatTree* search_range,
           String search_text,
           FindOptions options,
           Callback completeCallback);

  void NextIteration(RangeInFlatTree* search_range,
                     String search_text,
                     FindOptions options,
                     Callback completeCallback);

  TaskHandle pending_find_match_task_;

  // Number of iterations it took to complete |FindMatchInRange| request. Used
  // for recording UMA histograms.
  int iterations_ = 0;

  // |FindMatchInRange| start time. Used for recorsing UMA histograms.
  base::TimeTicks search_start_time_;
};
}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_EDITING_FINDER_ASYNC_FIND_BUFFER_H_
