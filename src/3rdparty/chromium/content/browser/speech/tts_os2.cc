// Copyright (c) 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/speech/tts_platform_impl.h"

#include "base/no_destructor.h"

namespace content {

// Dummy implementation to prevent a browser crash, see crbug.com/1019511
// TODO : Provide an implementation for OS/2
class TtsPlatformImplOS2 : public TtsPlatformImpl {
 public:
  TtsPlatformImplOS2() = default;
  TtsPlatformImplOS2(const TtsPlatformImplOS2&) = delete;
  TtsPlatformImplOS2& operator=(const TtsPlatformImplOS2&) = delete;

  // TtsPlatform implementation.
  bool PlatformImplSupported() override { return false; }
  bool PlatformImplInitialized() override { return false; }
  void Speak(int utterance_id,
             const std::string& utterance,
             const std::string& lang,
             const VoiceData& voice,
             const UtteranceContinuousParameters& params,
             base::OnceCallback<void(bool)> on_speak_finished) override {
    std::move(on_speak_finished).Run(false);
  }
  bool StopSpeaking() override { return false; }
  bool IsSpeaking() override { return false; }
  void GetVoices(std::vector<VoiceData>* out_voices) override {}
  void Pause() override {}
  void Resume() override {}

  // Get the single instance of this class.
  static TtsPlatformImplOS2* GetInstance() {
    static base::NoDestructor<TtsPlatformImplOS2> tts_platform;
    return tts_platform.get();
  }
};

// static
TtsPlatformImpl* TtsPlatformImpl::GetInstance() {
  return TtsPlatformImplOS2::GetInstance();
}

}  // namespace content
