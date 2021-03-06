// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "google/cloud/pubsub/subscriber_connection.h"
#include "google/cloud/pubsub/testing/mock_subscriber_stub.h"
#include "google/cloud/testing_util/assert_ok.h"
#include <gmock/gmock.h>
#include <atomic>

namespace google {
namespace cloud {
namespace pubsub {
inline namespace GOOGLE_CLOUD_CPP_PUBSUB_NS {
namespace {

using ::testing::_;
using ::testing::AtLeast;

TEST(SubscriberConnectionTest, Basic) {
  auto mock = std::make_shared<pubsub_testing::MockSubscriberStub>();
  Subscription const subscription("test-project", "test-subscription");

  EXPECT_CALL(*mock, Pull(_, _))
      .Times(AtLeast(1))
      .WillRepeatedly([&](grpc::ClientContext&,
                          google::pubsub::v1::PullRequest const& request) {
        EXPECT_EQ(subscription.FullName(), request.subscription());
        google::pubsub::v1::PullResponse response;
        auto& m = *response.add_received_messages();
        m.set_ack_id("test-ack-id-0");
        m.mutable_message()->set_message_id("test-message-id-0");
        return make_status_or(response);
      });
  EXPECT_CALL(*mock, Acknowledge(_, _))
      .Times(AtLeast(1))
      .WillRepeatedly(
          [&](grpc::ClientContext&,
              google::pubsub::v1::AcknowledgeRequest const& request) {
            EXPECT_EQ(subscription.FullName(), request.subscription());
            EXPECT_FALSE(request.ack_ids().empty());
            for (auto& id : request.ack_ids()) {
              EXPECT_EQ("test-ack-id-0", id);
            }
            return Status{};
          });

  auto subscriber = pubsub_internal::MakeSubscriberConnection(mock, {});
  std::atomic_flag received_one{false};
  promise<void> waiter;
  auto handler = [&](Message const& m, AckHandler h) {
    EXPECT_EQ("test-message-id-0", m.message_id());
    EXPECT_EQ("test-ack-id-0", h.ack_id());
    ASSERT_NO_FATAL_FAILURE(std::move(h).ack());
    if (received_one.test_and_set()) return;
    waiter.set_value();
  };
  auto response = subscriber->Subscribe({subscription.FullName(), handler});
  waiter.get_future().wait();
  response.cancel();
  ASSERT_STATUS_OK(response.get());
}

TEST(SubscriberConnectionTest, PullFailure) {
  auto mock = std::make_shared<pubsub_testing::MockSubscriberStub>();
  Subscription const subscription("test-project", "test-subscription");

  auto const expected = Status(StatusCode::kPermissionDenied, "uh-oh");
  EXPECT_CALL(*mock, Pull(_, _))
      .Times(AtLeast(1))
      .WillRepeatedly([&](grpc::ClientContext&,
                          google::pubsub::v1::PullRequest const& request) {
        EXPECT_EQ(subscription.FullName(), request.subscription());
        return StatusOr<google::pubsub::v1::PullResponse>(expected);
      });

  auto subscriber = pubsub_internal::MakeSubscriberConnection(mock, {});
  auto handler = [&](Message const&, AckHandler const&) {};
  auto response = subscriber->Subscribe({subscription.FullName(), handler});
  EXPECT_EQ(expected, response.get());
}

/// @test Verify callbacks are scheduled in the background threads.
TEST(SubscriberConnectionTest, ScheduleCallbacks) {
  auto mock = std::make_shared<pubsub_testing::MockSubscriberStub>();
  Subscription const subscription("test-project", "test-subscription");

  std::mutex mu;
  int count = 0;
  EXPECT_CALL(*mock, Pull(_, _))
      .Times(AtLeast(1))
      .WillRepeatedly([&](grpc::ClientContext&,
                          google::pubsub::v1::PullRequest const& request) {
        EXPECT_EQ(subscription.FullName(), request.subscription());
        google::pubsub::v1::PullResponse response;
        for (int i = 0; i != 2; ++i) {
          auto& m = *response.add_received_messages();
          std::lock_guard<std::mutex> lk(mu);
          m.set_ack_id("test-ack-id-" + std::to_string(count));
          m.mutable_message()->set_message_id("test-message-id-" +
                                              std::to_string(count));
          ++count;
        }
        return make_status_or(response);
      });

  std::atomic<int> expected_ack_id{0};
  EXPECT_CALL(*mock, Acknowledge(_, _))
      .Times(AtLeast(1))
      .WillRepeatedly(
          [&](grpc::ClientContext&,
              google::pubsub::v1::AcknowledgeRequest const& request) {
            EXPECT_EQ(subscription.FullName(), request.subscription());
            for (auto const& a : request.ack_ids()) {
              EXPECT_EQ("test-ack-id-" + std::to_string(expected_ack_id), a);
              ++expected_ack_id;
            }
            return Status{};
          });

  google::cloud::CompletionQueue cq;
  auto subscriber = pubsub_internal::MakeSubscriberConnection(
      mock, ConnectionOptions{grpc::InsecureChannelCredentials()}
                .DisableBackgroundThreads(cq));

  std::vector<std::thread> tasks;
  std::generate_n(std::back_inserter(tasks), 4,
                  [&] { return std::thread([&cq] { cq.Run(); }); });
  std::set<std::thread::id> ids;
  auto const main_id = std::this_thread::get_id();
  std::transform(tasks.begin(), tasks.end(), std::inserter(ids, ids.end()),
                 [](std::thread const& t) { return t.get_id(); });

  std::atomic<int> expected_message_id{0};
  auto handler = [&](Message const& m, AckHandler h) {
    EXPECT_EQ("test-message-id-" + std::to_string(expected_message_id),
              m.message_id());
    auto pos = ids.find(std::this_thread::get_id());
    EXPECT_NE(ids.end(), pos);
    EXPECT_NE(main_id, std::this_thread::get_id());
    std::move(h).ack();
    ++expected_message_id;
  };
  auto response = subscriber->Subscribe({subscription.FullName(), handler});

  while (expected_ack_id.load() < 100) {
    auto s = response.wait_for(std::chrono::milliseconds(5));
    if (s != std::future_status::timeout) break;
  }
  response.cancel();
  EXPECT_STATUS_OK(response.get());

  cq.Shutdown();
  for (auto& t : tasks) t.join();
}

}  // namespace
}  // namespace GOOGLE_CLOUD_CPP_PUBSUB_NS
}  // namespace pubsub
}  // namespace cloud
}  // namespace google
