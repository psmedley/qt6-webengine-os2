// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/web/cpp/fidl.h>
#include <lib/fidl/cpp/binding.h>

#include "base/barrier_closure.h"
#include "base/base_paths_fuchsia.h"
#include "base/files/file_util.h"
#include "base/path_service.h"
#include "base/test/bind.h"
#include "components/cast/message_port/fuchsia/message_port_fuchsia.h"
#include "content/public/test/browser_test.h"
#include "fuchsia/base/mem_buffer_util.h"
#include "fuchsia/base/test/fit_adapter.h"
#include "fuchsia/base/test/frame_test_util.h"
#include "fuchsia/base/test/result_receiver.h"
#include "fuchsia/base/test/test_navigation_listener.h"
#include "fuchsia/engine/test/web_engine_browser_test.h"
#include "fuchsia/runners/cast/api_bindings_client.h"
#include "fuchsia/runners/cast/create_web_message.h"
#include "fuchsia/runners/cast/fake_api_bindings.h"
#include "fuchsia/runners/cast/named_message_port_connector_fuchsia.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

class ApiBindingsClientTest : public cr_fuchsia::WebEngineBrowserTest {
 public:
  ApiBindingsClientTest() : api_service_binding_(&api_service_) {
    set_test_server_root(base::FilePath("fuchsia/runners/cast/testdata"));
  }

  ~ApiBindingsClientTest() override = default;

  void SetUp() override { cr_fuchsia::WebEngineBrowserTest::SetUp(); }

 protected:
  void StartClient(bool disconnect_before_attach,
                   base::OnceClosure on_error_closure) {
    base::ScopedAllowBlockingForTesting allow_blocking;

    // Get the bindings from |api_service_|.
    base::RunLoop run_loop;
    client_ = std::make_unique<ApiBindingsClient>(
        api_service_binding_.NewBinding(), run_loop.QuitClosure());
    ASSERT_FALSE(client_->HasBindings());
    run_loop.Run();
    ASSERT_TRUE(client_->HasBindings());

    frame_ = WebEngineBrowserTest::CreateFrame(&navigation_listener_);
    frame_->GetNavigationController(controller_.NewRequest());
    connector_ =
        std::make_unique<NamedMessagePortConnectorFuchsia>(frame_.get());

    if (disconnect_before_attach)
      api_service_binding_.Unbind();

    base::RunLoop().RunUntilIdle();

    client_->AttachToFrame(frame_.get(), connector_.get(),
                           std::move(on_error_closure));
  }

  void SetUpOnMainThread() override {
    cr_fuchsia::WebEngineBrowserTest::SetUpOnMainThread();
    ASSERT_TRUE(embedded_test_server()->Start());
  }

  void TearDownOnMainThread() override {
    // Destroy |client_| before the MessageLoop is destroyed.
    client_.reset();
  }

  fuchsia::web::FramePtr frame_;
  std::unique_ptr<NamedMessagePortConnectorFuchsia> connector_;
  FakeApiBindingsImpl api_service_;
  fidl::Binding<chromium::cast::ApiBindings> api_service_binding_;
  std::unique_ptr<ApiBindingsClient> client_;
  cr_fuchsia::TestNavigationListener navigation_listener_;
  fuchsia::web::NavigationControllerPtr controller_;

  DISALLOW_COPY_AND_ASSIGN(ApiBindingsClientTest);
};

// Tests API registration, injection, and message IPC.
// Registers a port that echoes messages received over a MessagePort back to the
// sender.
IN_PROC_BROWSER_TEST_F(ApiBindingsClientTest, EndToEnd) {
  // Define the injected bindings.
  std::vector<chromium::cast::ApiBinding> binding_list;
  chromium::cast::ApiBinding echo_binding;
  echo_binding.set_before_load_script(cr_fuchsia::MemBufferFromString(
      "window.echo = cast.__platform__.PortConnector.bind('echoService');",
      "test"));
  binding_list.emplace_back(std::move(echo_binding));
  api_service_.set_bindings(std::move(binding_list));

  StartClient(false, base::MakeExpectedNotRunClosure(FROM_HERE));

  base::RunLoop post_message_responses_loop;
  base::RepeatingClosure post_message_response_closure =
      base::BarrierClosure(2, post_message_responses_loop.QuitClosure());

  // Navigate to a test page that makes use of the injected bindings.
  const GURL test_url = embedded_test_server()->GetURL("/echo.html");
  EXPECT_TRUE(cr_fuchsia::LoadUrlAndExpectResponse(
      controller_.get(), fuchsia::web::LoadUrlParams(), test_url.spec()));
  navigation_listener_.RunUntilUrlEquals(test_url);

  std::string connect_message;
  std::unique_ptr<cast_api_bindings::MessagePort> connect_port;
  connector_->GetConnectMessage(&connect_message, &connect_port);
  frame_->PostMessage(
      "*", CreateWebMessage(connect_message, std::move(connect_port)),
      [&post_message_response_closure](
          fuchsia::web::Frame_PostMessage_Result result) {
        ASSERT_TRUE(result.is_response());
        post_message_response_closure.Run();
      });

  // Connect to the echo service hosted by the page and send a ping to it.
  fuchsia::web::WebMessage message;
  message.set_data(cr_fuchsia::MemBufferFromString("ping", "ping-msg"));
  fuchsia::web::MessagePortPtr port =
      api_service_.RunAndReturnConnectedPort("echoService").Bind();
  port->PostMessage(std::move(message),
                    [&post_message_response_closure](
                        fuchsia::web::MessagePort_PostMessage_Result result) {
                      ASSERT_TRUE(result.is_response());
                      post_message_response_closure.Run();
                    });

  // Handle the ping response.
  base::RunLoop response_loop;
  cr_fuchsia::ResultReceiver<fuchsia::web::WebMessage> response(
      response_loop.QuitClosure());
  port->ReceiveMessage(
      cr_fuchsia::CallbackToFitFunction(response.GetReceiveCallback()));
  response_loop.Run();

  std::string response_string;
  EXPECT_TRUE(
      cr_fuchsia::StringFromMemBuffer(response->data(), &response_string));
  EXPECT_EQ("ack ping", response_string);

  // Ensure that we've received acks for all messages.
  post_message_responses_loop.Run();
}

IN_PROC_BROWSER_TEST_F(ApiBindingsClientTest,
                       ClientDisconnectsBeforeFrameAttached) {
  bool error_signaled = false;
  StartClient(
      true, base::BindOnce([](bool* error_signaled) { *error_signaled = true; },
                           base::Unretained(&error_signaled)));
  EXPECT_TRUE(error_signaled);
}

}  // namespace
