/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <quic/fizz/server/handshake/FizzServerQuicHandshakeContext.h>
#include <quic/server/state/ServerStateMachine.h>
#include <quic/state/QuicPriorityQueue.h>
#include <quic/state/QuicStreamManager.h>
#include <quic/state/QuicStreamUtilities.h>
#include <quic/state/test/Mocks.h>

using namespace testing;

namespace quic {
namespace test {

class QuicStreamManagerTest : public Test {
 public:
  QuicStreamManagerTest()
      : conn(FizzServerQuicHandshakeContext::Builder().build()) {}
  void SetUp() override {
    conn.flowControlState.peerAdvertisedInitialMaxStreamOffsetBidiLocal =
        kDefaultStreamWindowSize;
    conn.flowControlState.peerAdvertisedInitialMaxStreamOffsetBidiRemote =
        kDefaultStreamWindowSize;
    conn.flowControlState.peerAdvertisedInitialMaxStreamOffsetUni =
        kDefaultStreamWindowSize;
    conn.flowControlState.peerAdvertisedMaxOffset =
        kDefaultConnectionWindowSize;
    conn.streamManager->setMaxLocalBidirectionalStreams(
        kDefaultMaxStreamsBidirectional);
    conn.streamManager->setMaxLocalUnidirectionalStreams(
        kDefaultMaxStreamsUnidirectional);
    auto congestionController =
        std::make_unique<NiceMock<MockCongestionController>>();
    mockController = congestionController.get();
    conn.congestionController = std::move(congestionController);
  }

  QuicServerConnectionState conn;
  MockCongestionController* mockController;
};

TEST_F(QuicStreamManagerTest, SkipRedundantPriorityUpdate) {
  auto& manager = *conn.streamManager;
  auto stream = manager.createNextBidirectionalStream();
  auto streamId = stream.value()->id;
  Priority currentPriority = stream.value()->priority;
  EXPECT_TRUE(manager.setStreamPriority(
      streamId,
      (currentPriority.level + 1) % (kDefaultMaxPriority + 1),
      ~currentPriority.incremental));
  EXPECT_FALSE(manager.setStreamPriority(
      streamId,
      (currentPriority.level + 1) % (kDefaultMaxPriority + 1),
      ~currentPriority.incremental));
}

TEST_F(QuicStreamManagerTest, TestAppIdleCreateBidiStream) {
  auto& manager = *conn.streamManager;
  EXPECT_FALSE(manager.isAppIdle());

  // The app limiited state did not change.
  EXPECT_CALL(*mockController, setAppIdle(false, _)).Times(0);
  auto stream = manager.createNextBidirectionalStream();
  StreamId id = stream.value()->id;
  EXPECT_FALSE(manager.isAppIdle());

  EXPECT_CALL(*mockController, setAppIdle(true, _));
  // Force transition to closed state
  stream.value()->sendState = StreamSendState::Closed;
  stream.value()->recvState = StreamRecvState::Closed;
  manager.removeClosedStream(stream.value()->id);
  EXPECT_TRUE(manager.isAppIdle());
  EXPECT_EQ(manager.getStream(id), nullptr);
}

TEST_F(QuicStreamManagerTest, TestAppIdleCreateUnidiStream) {
  auto& manager = *conn.streamManager;
  EXPECT_FALSE(manager.isAppIdle());
  EXPECT_CALL(*mockController, setAppIdle(false, _)).Times(0);
  auto stream = manager.createNextUnidirectionalStream();
  EXPECT_FALSE(manager.isAppIdle());

  // Force transition to closed state
  EXPECT_CALL(*mockController, setAppIdle(true, _));
  stream.value()->sendState = StreamSendState::Closed;
  stream.value()->recvState = StreamRecvState::Closed;
  manager.removeClosedStream(stream.value()->id);
  EXPECT_TRUE(manager.isAppIdle());
}

TEST_F(QuicStreamManagerTest, TestAppIdleExistingLocalStream) {
  auto& manager = *conn.streamManager;
  EXPECT_FALSE(manager.isAppIdle());
  EXPECT_CALL(*mockController, setAppIdle(false, _)).Times(0);

  auto stream = manager.createNextUnidirectionalStream();
  EXPECT_FALSE(manager.isAppIdle());

  EXPECT_CALL(*mockController, setAppIdle(true, _));
  manager.setStreamAsControl(*stream.value());
  EXPECT_TRUE(manager.isAppIdle());

  manager.getStream(stream.value()->id);
  EXPECT_TRUE(manager.isAppIdle());
}

TEST_F(QuicStreamManagerTest, TestAppIdleStreamAsControl) {
  auto& manager = *conn.streamManager;
  EXPECT_FALSE(manager.isAppIdle());

  auto stream = manager.createNextUnidirectionalStream();
  EXPECT_FALSE(manager.isAppIdle());

  EXPECT_CALL(*mockController, setAppIdle(true, _));
  manager.setStreamAsControl(*stream.value());
  EXPECT_TRUE(manager.isAppIdle());

  EXPECT_CALL(*mockController, setAppIdle(false, _));
  manager.createNextUnidirectionalStream();
  EXPECT_FALSE(manager.isAppIdle());
}

TEST_F(QuicStreamManagerTest, TestAppIdleCreatePeerStream) {
  auto& manager = *conn.streamManager;
  EXPECT_FALSE(manager.isAppIdle());
  StreamId id = 0;
  auto stream = manager.getStream(id);
  EXPECT_FALSE(manager.isAppIdle());

  EXPECT_CALL(*mockController, setAppIdle(true, _));
  manager.setStreamAsControl(*stream);
  EXPECT_TRUE(manager.isAppIdle());

  EXPECT_CALL(*mockController, setAppIdle(false, _));
  StreamId id2 = 4;
  manager.getStream(id2);
  EXPECT_FALSE(manager.isAppIdle());
}

TEST_F(QuicStreamManagerTest, TestAppIdleExistingPeerStream) {
  auto& manager = *conn.streamManager;
  EXPECT_FALSE(manager.isAppIdle());
  EXPECT_CALL(*mockController, setAppIdle(false, _)).Times(0);

  StreamId id = 0;
  auto stream = manager.getStream(id);
  EXPECT_FALSE(manager.isAppIdle());

  EXPECT_CALL(*mockController, setAppIdle(true, _));
  manager.setStreamAsControl(*stream);
  EXPECT_TRUE(manager.isAppIdle());

  manager.getStream(id);
  EXPECT_TRUE(manager.isAppIdle());
}

TEST_F(QuicStreamManagerTest, TestAppIdleClosePeerStream) {
  auto& manager = *conn.streamManager;
  EXPECT_FALSE(manager.isAppIdle());
  StreamId id = 0;
  auto stream = manager.getStream(id);
  EXPECT_FALSE(manager.isAppIdle());

  EXPECT_CALL(*mockController, setAppIdle(true, _));
  // Force transition to closed state
  stream->sendState = StreamSendState::Closed;
  stream->recvState = StreamRecvState::Closed;
  manager.removeClosedStream(stream->id);
  EXPECT_TRUE(manager.isAppIdle());
  EXPECT_EQ(manager.getStream(id), nullptr);
}

TEST_F(QuicStreamManagerTest, TestAppIdleCloseControlStream) {
  auto& manager = *conn.streamManager;
  EXPECT_FALSE(manager.isAppIdle());
  EXPECT_CALL(*mockController, setAppIdle(false, _)).Times(0);

  StreamId id = 0;
  auto stream = manager.getStream(id);
  EXPECT_FALSE(manager.isAppIdle());

  EXPECT_CALL(*mockController, setAppIdle(true, _));
  manager.setStreamAsControl(*stream);
  EXPECT_TRUE(manager.isAppIdle());

  // Force transition to closed state
  stream->sendState = StreamSendState::Closed;
  stream->recvState = StreamRecvState::Closed;
  manager.removeClosedStream(stream->id);
  EXPECT_TRUE(manager.isAppIdle());
}

TEST_F(QuicStreamManagerTest, StreamLimitWindowedUpdate) {
  auto& manager = *conn.streamManager;
  conn.transportSettings.advertisedInitialMaxStreamsBidi = 100;
  conn.transportSettings.advertisedInitialMaxStreamsUni = 100;
  manager.refreshTransportSettings(conn.transportSettings);
  manager.setStreamLimitWindowingFraction(4);
  for (int i = 0; i < 100; i++) {
    manager.getStream(i * detail::kStreamIncrement);
    manager.getStream(2 + i * detail::kStreamIncrement);
  }
  for (int i = 0; i < 25; i++) {
    auto stream = manager.getStream(i * detail::kStreamIncrement);
    stream->sendState = StreamSendState::Closed;
    stream->recvState = StreamRecvState::Closed;
    manager.removeClosedStream(stream->id);
    stream = manager.getStream(2 + i * detail::kStreamIncrement);
    stream->sendState = StreamSendState::Closed;
    stream->recvState = StreamRecvState::Closed;
    manager.removeClosedStream(stream->id);
  }
  auto update = manager.remoteBidirectionalStreamLimitUpdate();
  ASSERT_TRUE(update);
  EXPECT_EQ(update.value(), 125);
  EXPECT_FALSE(manager.remoteBidirectionalStreamLimitUpdate());

  update = manager.remoteUnidirectionalStreamLimitUpdate();
  ASSERT_TRUE(update);
  EXPECT_EQ(update.value(), 125);
  EXPECT_FALSE(manager.remoteUnidirectionalStreamLimitUpdate());
}

TEST_F(QuicStreamManagerTest, StreamLimitNoWindowedUpdate) {
  auto& manager = *conn.streamManager;
  conn.transportSettings.advertisedInitialMaxStreamsBidi = 100;
  manager.refreshTransportSettings(conn.transportSettings);
  manager.setStreamLimitWindowingFraction(4);
  for (int i = 0; i < 100; i++) {
    manager.getStream(i * detail::kStreamIncrement);
  }
  for (int i = 0; i < 24; i++) {
    auto stream = manager.getStream(i * detail::kStreamIncrement);
    stream->sendState = StreamSendState::Closed;
    stream->recvState = StreamRecvState::Closed;
    manager.removeClosedStream(stream->id);
  }
  auto update = manager.remoteBidirectionalStreamLimitUpdate();
  EXPECT_FALSE(update);
}

TEST_F(QuicStreamManagerTest, StreamLimitManyWindowedUpdate) {
  auto& manager = *conn.streamManager;
  conn.transportSettings.advertisedInitialMaxStreamsBidi = 100;
  manager.refreshTransportSettings(conn.transportSettings);
  manager.setStreamLimitWindowingFraction(4);
  for (int i = 0; i < 100; i++) {
    manager.getStream(i * detail::kStreamIncrement);
  }
  for (int i = 0; i < 50; i++) {
    auto stream = manager.getStream(i * detail::kStreamIncrement);
    stream->sendState = StreamSendState::Closed;
    stream->recvState = StreamRecvState::Closed;
    manager.removeClosedStream(stream->id);
  }
  auto update = manager.remoteBidirectionalStreamLimitUpdate();
  ASSERT_TRUE(update);
  EXPECT_EQ(update.value(), 150);
  EXPECT_FALSE(manager.remoteBidirectionalStreamLimitUpdate());
  EXPECT_FALSE(manager.remoteUnidirectionalStreamLimitUpdate());
}

TEST_F(QuicStreamManagerTest, StreamLimitIncrementBidi) {
  auto& manager = *conn.streamManager;
  manager.setMaxLocalBidirectionalStreams(100, true);
  manager.refreshTransportSettings(conn.transportSettings);
  StreamId max;
  for (int i = 0; i < 100; i++) {
    max = manager.createNextBidirectionalStream().value()->id;
  }
  EXPECT_TRUE(manager.createNextBidirectionalStream().hasError());
  manager.setMaxLocalBidirectionalStreams(200);
  auto s = manager.createNextBidirectionalStream();
  EXPECT_TRUE(s.hasValue());
  EXPECT_EQ(s.value()->id, max + detail::kStreamIncrement);
}

TEST_F(QuicStreamManagerTest, StreamLimitIncrementUni) {
  auto& manager = *conn.streamManager;
  manager.setMaxLocalUnidirectionalStreams(100, true);
  manager.refreshTransportSettings(conn.transportSettings);
  StreamId max;
  for (int i = 0; i < 100; i++) {
    max = manager.createNextUnidirectionalStream().value()->id;
  }
  EXPECT_TRUE(manager.createNextUnidirectionalStream().hasError());
  manager.setMaxLocalUnidirectionalStreams(200);
  auto s = manager.createNextUnidirectionalStream();
  EXPECT_TRUE(s.hasValue());
  EXPECT_EQ(s.value()->id, max + detail::kStreamIncrement);
}

TEST_F(QuicStreamManagerTest, TestClearActionable) {
  auto& manager = *conn.streamManager;

  StreamId id = 1;
  auto stream = manager.createNextUnidirectionalStream().value();
  stream->readBuffer.emplace_back(folly::IOBuf::copyBuffer("blah blah"), 0);
  manager.queueFlowControlUpdated(id);
  manager.addDeliverable(id);
  manager.updateReadableStreams(*stream);
  manager.updatePeekableStreams(*stream);
  EXPECT_TRUE(manager.flowControlUpdatedContains(id));
  EXPECT_TRUE(manager.deliverableContains(id));
  EXPECT_FALSE(manager.readableStreams().empty());
  EXPECT_FALSE(manager.peekableStreams().empty());
  manager.clearActionable();
  EXPECT_FALSE(manager.flowControlUpdatedContains(id));
  EXPECT_FALSE(manager.deliverableContains(id));
  EXPECT_TRUE(manager.readableStreams().empty());
  EXPECT_TRUE(manager.peekableStreams().empty());
}

TEST_F(QuicStreamManagerTest, WriteBufferMeta) {
  auto& manager = *conn.streamManager;
  auto stream = manager.createNextUnidirectionalStream().value();
  // Add some real data into write buffer
  writeDataToQuicStream(*stream, folly::IOBuf::copyBuffer("prefix"), false);
  // Artificially remove the stream from writable queue, so that any further
  // writable query is about the DSR state.
  manager.removeWritable(*stream);

  BufferMeta bufferMeta(200);
  writeBufMetaToQuicStream(*stream, bufferMeta, true);
  EXPECT_TRUE(stream->hasWritableBufMeta());
  EXPECT_TRUE(manager.hasWritable());

  stream->sendState = StreamSendState::Closed;
  stream->recvState = StreamRecvState::Closed;
  manager.removeClosedStream(stream->id);
  EXPECT_TRUE(manager.writableDSRStreams().empty());
}

TEST_F(QuicStreamManagerTest, NotifyOnStreamPriorityChanges) {
  // Verify that the StreamPriorityChanges callback function is called
  // upon stream creation, priority changes, stream removal.
  // For different steps try local (uni/bi)directional streams and remote
  // streams

  MockQuicStreamPrioritiesObserver mObserver;

  auto& manager = *conn.streamManager;
  manager.setPriorityChangesObserver(&mObserver);
  EXPECT_CALL(mObserver, onStreamPrioritiesChange())
      .Times(2); // On stream creation and on setting the priority
  auto stream = manager.createNextUnidirectionalStream().value();
  EXPECT_EQ(manager.getHighestPriorityLevel(), kDefaultPriority.level);

  manager.setStreamPriority(stream->id, 1, false);
  EXPECT_EQ(manager.getHighestPriorityLevel(), 1);

  EXPECT_CALL(mObserver, onStreamPrioritiesChange())
      .Times(1); // On removing a closed stream
  stream->sendState = StreamSendState::Closed;
  stream->recvState = StreamRecvState::Closed;
  manager.removeClosedStream(stream->id);
  // No active stream. Highest priority should return the max value (least
  // priority).
  EXPECT_EQ(manager.getHighestPriorityLevel(), kDefaultMaxPriority);

  EXPECT_CALL(mObserver, onStreamPrioritiesChange())
      .Times(2); // On stream creation - create two streams - one bidirectional
  auto stream2Id = manager.createNextUnidirectionalStream().value()->id;
  auto stream3 = manager.createNextBidirectionalStream().value();
  EXPECT_EQ(manager.getHighestPriorityLevel(), kDefaultPriority.level);

  EXPECT_CALL(mObserver, onStreamPrioritiesChange())
      .Times(1); // On increasing the priority of one of the streams
  manager.setStreamPriority(stream3->id, 0, false);
  EXPECT_EQ(manager.getHighestPriorityLevel(), 0);

  EXPECT_CALL(mObserver, onStreamPrioritiesChange())
      .Times(1); // On removing a closed stream;
  stream3->sendState = StreamSendState::Closed;
  stream3->recvState = StreamRecvState::Closed;
  manager.removeClosedStream(stream3->id);
  EXPECT_EQ(manager.getHighestPriorityLevel(), kDefaultPriority.level);

  EXPECT_CALL(mObserver, onStreamPrioritiesChange())
      .Times(1); // On stream creation - remote stream
  auto peerStreamId = 20;
  ASSERT_TRUE(isRemoteStream(conn.nodeType, peerStreamId));
  auto stream4 = manager.getStream(peerStreamId);
  EXPECT_NE(stream4, nullptr);
  EXPECT_EQ(manager.getHighestPriorityLevel(), kDefaultPriority.level);

  EXPECT_CALL(mObserver, onStreamPrioritiesChange())
      .Times(0); // Removing streams but observer removed
  manager.resetPriorityChangesObserver();
  stream4->sendState = StreamSendState::Closed;
  stream4->recvState = StreamRecvState::Closed;
  manager.removeClosedStream(stream4->id);
  CHECK_NOTNULL(manager.getStream(stream2Id))->sendState =
      StreamSendState::Closed;
  CHECK_NOTNULL(manager.getStream(stream2Id))->recvState =
      StreamRecvState::Closed;
  manager.removeClosedStream(stream2Id);
}

} // namespace test
} // namespace quic
