/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 */

#include "quic/state/QuicStreamManager.h"

#include <quic/state/QuicStreamUtilities.h>

namespace quic {

/**
 * Updates the head of line blocked time for the stream. This should be called
 * on new data received or even data being read from the stream.
 * There are 2 cases when you can become head of line blocked:
 * 1. You're not previously holb. You receive new data which cannot be read.
 * 2. You are not head of line blocked. You read data from the stream, but you
 *    discover a hole.
 *
 * You can become not head of line blocked if the following conditions happen:
 * 1. You were head of line blocked, and you receive something that allows you
 *    to read from the stream.
 * 2. You were head of line blocked, but you receive a reset from the peer.
 */
static void updateHolBlockedTime(QuicStreamState& stream) {
  // No data has arrived, or the current stream offset matches
  // the stream offset that has been read so far. Stream is not HOL-blocked
  // (although may be blocked on missing data).
  // If there is no more data to read, or if the current read offset
  // matches the read offset in the front queue, a potential HOL block
  // becomes unblocked.
  if (stream.readBuffer.empty() ||
      (stream.currentReadOffset == stream.readBuffer.front().offset)) {
    // If we were previously HOL blocked, we're not any more.
    // Update the total HOLB time and reset the latch.
    if (stream.lastHolbTime) {
      stream.totalHolbTime +=
          std::chrono::duration_cast<std::chrono::microseconds>(
              Clock::now() - *stream.lastHolbTime);
      stream.lastHolbTime = folly::none;
    }
    return;
  }

  // No HOL unblocking event has occured. If we are already HOL bloked,
  // we remain HOL blocked.
  if (stream.lastHolbTime) {
    return;
  }
  // If we were previously not HOL blocked, we are now.
  stream.lastHolbTime = Clock::now();
  stream.holbCount++;
}

static bool isStreamUnopened(
    StreamId streamId,
    StreamId nextAcceptableStreamId) {
  return streamId >= nextAcceptableStreamId;
}

// If a stream is un-opened, these automatically creates all lower streams.
// Returns false if the stream is closed or already opened.
static LocalErrorCode openPeerStreamIfNotClosed(
    StreamId streamId,
    folly::F14FastSet<StreamId>& openStreams,
    StreamId& nextAcceptableStreamId,
    StreamId maxStreamId,
    std::vector<StreamId>& newStreams) {
  if (streamId < nextAcceptableStreamId) {
    return LocalErrorCode::CREATING_EXISTING_STREAM;
  }
  if (streamId >= maxStreamId) {
    return LocalErrorCode::STREAM_LIMIT_EXCEEDED;
  }

  StreamId start = nextAcceptableStreamId;
  auto numNewStreams = (streamId - start) / detail::kStreamIncrement;
  openStreams.reserve(openStreams.size() + numNewStreams);
  newStreams.reserve(newStreams.size() + numNewStreams);
  while (start <= streamId) {
    openStreams.emplace(start);
    newStreams.push_back(start);
    start += detail::kStreamIncrement;
  }

  if (streamId >= nextAcceptableStreamId) {
    nextAcceptableStreamId = streamId + detail::kStreamIncrement;
  }
  return LocalErrorCode::NO_ERROR;
}

static LocalErrorCode openLocalStreamIfNotClosed(
    StreamId streamId,
    folly::F14FastSet<StreamId>& openStreams,
    StreamId& nextAcceptableStreamId,
    StreamId maxStreamId) {
  if (streamId < nextAcceptableStreamId) {
    return LocalErrorCode::CREATING_EXISTING_STREAM;
  }
  if (streamId >= maxStreamId) {
    return LocalErrorCode::STREAM_LIMIT_EXCEEDED;
  }

  StreamId start = nextAcceptableStreamId;
  auto numNewStreams = (streamId - start) / detail::kStreamIncrement;
  openStreams.reserve(openStreams.size() + numNewStreams);
  while (start <= streamId) {
    openStreams.emplace(start);
    start += detail::kStreamIncrement;
  }

  if (streamId >= nextAcceptableStreamId) {
    nextAcceptableStreamId = streamId + detail::kStreamIncrement;
  }
  return LocalErrorCode::NO_ERROR;
}

bool QuicStreamManager::streamExists(StreamId streamId) {
  if (isLocalStream(nodeType_, streamId)) {
    if (isUnidirectionalStream(streamId)) {
      return openUnidirectionalLocalStreams_.count(streamId) > 0;
    } else {
      return openBidirectionalLocalStreams_.count(streamId) > 0;
    }
  } else {
    if (isUnidirectionalStream(streamId)) {
      return openUnidirectionalPeerStreams_.count(streamId) > 0;
    } else {
      return openBidirectionalPeerStreams_.count(streamId) > 0;
    }
  }
}

QuicStreamState* QuicStreamManager::findStream(StreamId streamId) {
  auto lookup = streams_.find(streamId);
  if (lookup == streams_.end()) {
    return nullptr;
  } else {
    return &lookup->second;
  }
}

void QuicStreamManager::setMaxLocalBidirectionalStreams(
    uint64_t maxStreams,
    bool force) {
  if (maxStreams > kMaxMaxStreams) {
    throw QuicTransportException(
        "Attempt to set maxStreams beyond the max allowed.",
        TransportErrorCode::STREAM_LIMIT_ERROR);
  }
  StreamId maxStreamId = maxStreams * detail::kStreamIncrement +
      initialLocalBidirectionalStreamId_;
  if (force || maxStreamId > maxLocalBidirectionalStreamId_) {
    maxLocalBidirectionalStreamId_ = maxStreamId;
    maxLocalBidirectionalStreamIdIncreased_ = true;
  }
}

void QuicStreamManager::setMaxLocalUnidirectionalStreams(
    uint64_t maxStreams,
    bool force) {
  if (maxStreams > kMaxMaxStreams) {
    throw QuicTransportException(
        "Attempt to set maxStreams beyond the max allowed.",
        TransportErrorCode::STREAM_LIMIT_ERROR);
  }
  StreamId maxStreamId = maxStreams * detail::kStreamIncrement +
      initialLocalUnidirectionalStreamId_;
  if (force || maxStreamId > maxLocalUnidirectionalStreamId_) {
    maxLocalUnidirectionalStreamId_ = maxStreamId;
    maxLocalUnidirectionalStreamIdIncreased_ = true;
  }
}

void QuicStreamManager::setMaxRemoteBidirectionalStreams(uint64_t maxStreams) {
  setMaxRemoteBidirectionalStreamsInternal(maxStreams, false);
}

void QuicStreamManager::setMaxRemoteUnidirectionalStreams(uint64_t maxStreams) {
  setMaxRemoteUnidirectionalStreamsInternal(maxStreams, false);
}

void QuicStreamManager::setMaxRemoteBidirectionalStreamsInternal(
    uint64_t maxStreams,
    bool force) {
  if (maxStreams > kMaxMaxStreams) {
    throw QuicTransportException(
        "Attempt to set maxStreams beyond the max allowed.",
        TransportErrorCode::STREAM_LIMIT_ERROR);
  }
  StreamId maxStreamId = maxStreams * detail::kStreamIncrement +
      initialRemoteBidirectionalStreamId_;
  if (force || maxStreamId > maxRemoteBidirectionalStreamId_) {
    maxRemoteBidirectionalStreamId_ = maxStreamId;
  }
}

void QuicStreamManager::setMaxRemoteUnidirectionalStreamsInternal(
    uint64_t maxStreams,
    bool force) {
  if (maxStreams > kMaxMaxStreams) {
    throw QuicTransportException(
        "Attempt to set maxStreams beyond the max allowed.",
        TransportErrorCode::STREAM_LIMIT_ERROR);
  }
  StreamId maxStreamId = maxStreams * detail::kStreamIncrement +
      initialRemoteUnidirectionalStreamId_;
  if (force || maxStreamId > maxRemoteUnidirectionalStreamId_) {
    maxRemoteUnidirectionalStreamId_ = maxStreamId;
  }
}

bool QuicStreamManager::consumeMaxLocalBidirectionalStreamIdIncreased() {
  bool res = maxLocalBidirectionalStreamIdIncreased_;
  maxLocalBidirectionalStreamIdIncreased_ = false;
  return res;
}

bool QuicStreamManager::consumeMaxLocalUnidirectionalStreamIdIncreased() {
  bool res = maxLocalUnidirectionalStreamIdIncreased_;
  maxLocalUnidirectionalStreamIdIncreased_ = false;
  return res;
}

bool QuicStreamManager::setStreamPriority(
    StreamId id,
    PriorityLevel level,
    bool incremental) {
  auto stream = findStream(id);
  if (stream) {
    Priority newPriority(level, incremental);
    if (stream->priority == newPriority) {
      return false;
    }
    stream->priority = newPriority;
    auto priorityMapEntry = streamPriorityLevels_.find(id);
    if (priorityMapEntry == streamPriorityLevels_.end()) {
      throw QuicTransportException(
          "Active stream not in stream priority map",
          TransportErrorCode::STREAM_STATE_ERROR);
    } else {
      priorityMapEntry->second = newPriority.level;
    }
    notifyStreamPriorityChanges();
    // If this stream is already in the writable or loss queus, update the
    // priority there.
    writableStreams_.updateIfExist(id, stream->priority);
    writableDSRStreams_.updateIfExist(id, stream->priority);
    return true;
  }
  return false;
}

void QuicStreamManager::refreshTransportSettings(
    const TransportSettings& settings) {
  transportSettings_ = &settings;
  setMaxRemoteBidirectionalStreamsInternal(
      transportSettings_->advertisedInitialMaxStreamsBidi, true);
  setMaxRemoteUnidirectionalStreamsInternal(
      transportSettings_->advertisedInitialMaxStreamsUni, true);
}

// We create local streams lazily. If a local stream was created
// but not allocated yet, this will allocate a stream.
// This will return nullptr if a stream is closed or un-opened.
QuicStreamState* FOLLY_NULLABLE
QuicStreamManager::getOrCreateOpenedLocalStream(StreamId streamId) {
  auto& openLocalStreams = isUnidirectionalStream(streamId)
      ? openUnidirectionalLocalStreams_
      : openBidirectionalLocalStreams_;
  if (openLocalStreams.count(streamId)) {
    // Open a lazily created stream.
    auto it = streams_.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(streamId),
        std::forward_as_tuple(streamId, conn_));
    QUIC_STATS(conn_.statsCallback, onNewQuicStream);
    if (!it.second) {
      throw QuicTransportException(
          "Creating an active stream", TransportErrorCode::STREAM_STATE_ERROR);
    }
    addToStreamPriorityMap(it.first->second);
    return &it.first->second;
  }
  return nullptr;
}

QuicStreamState* QuicStreamManager::getStream(StreamId streamId) {
  if (isRemoteStream(nodeType_, streamId)) {
    auto stream = getOrCreatePeerStream(streamId);
    updateAppIdleState();
    return stream;
  }
  auto it = streams_.find(streamId);
  if (it != streams_.end()) {
    return &it->second;
  }
  auto stream = getOrCreateOpenedLocalStream(streamId);
  auto nextAcceptableStreamId = isUnidirectionalStream(streamId)
      ? nextAcceptableLocalUnidirectionalStreamId_
      : nextAcceptableLocalBidirectionalStreamId_;
  if (!stream && isStreamUnopened(streamId, nextAcceptableStreamId)) {
    throw QuicTransportException(
        "Trying to get unopened local stream",
        TransportErrorCode::STREAM_STATE_ERROR);
  }
  updateAppIdleState();
  return stream;
}

folly::Expected<QuicStreamState*, LocalErrorCode>
QuicStreamManager::createNextBidirectionalStream() {
  auto stream = createStream(nextBidirectionalStreamId_);
  if (stream.hasValue()) {
    nextBidirectionalStreamId_ += detail::kStreamIncrement;
  }
  return stream;
}

folly::Expected<QuicStreamState*, LocalErrorCode>
QuicStreamManager::createNextUnidirectionalStream() {
  auto stream = createStream(nextUnidirectionalStreamId_);
  if (stream.hasValue()) {
    nextUnidirectionalStreamId_ += detail::kStreamIncrement;
  }
  return stream;
}

QuicStreamState* FOLLY_NULLABLE
QuicStreamManager::getOrCreatePeerStream(StreamId streamId) {
  // This function maintains 3 invariants:
  // 1. Streams below nextAcceptableStreamId are streams that have been
  //    seen before. Everything above can be opened.
  // 2. Streams that have been seen before, always have an entry in
  //    openPeerStreams. If a stream below nextAcceptableStreamId does not
  //    have an entry in openPeerStreams, then it is closed.
  // 3. If streamId n is open all streams < n will be seen.
  // It also tries to create the entire state for a stream in a lazy manner.

  // Validate the stream id is correct
  if (nodeType_ == QuicNodeType::Client && isClientStream(streamId)) {
    throw QuicTransportException(
        "Attempted getting client peer stream on client",
        TransportErrorCode::STREAM_STATE_ERROR);
  } else if (nodeType_ == QuicNodeType::Server && isServerStream(streamId)) {
    throw QuicTransportException(
        "Attempted getting server peer stream on server",
        TransportErrorCode::STREAM_STATE_ERROR);
  } else if (!isClientStream(streamId) && !isServerStream(streamId)) {
    throw QuicTransportException(
        "Invalid stream", TransportErrorCode::STREAM_STATE_ERROR);
  }

  // TODO when we can rely on C++17, this is a good candidate for try_emplace.
  auto peerStream = streams_.find(streamId);
  if (peerStream != streams_.end()) {
    return &peerStream->second;
  }
  auto& openPeerStreams = isUnidirectionalStream(streamId)
      ? openUnidirectionalPeerStreams_
      : openBidirectionalPeerStreams_;
  if (openPeerStreams.count(streamId)) {
    // Stream was already open, create the state for it lazily.
    auto it = streams_.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(streamId),
        std::forward_as_tuple(streamId, conn_));
    addToStreamPriorityMap(it.first->second);
    QUIC_STATS(conn_.statsCallback, onNewQuicStream);
    return &it.first->second;
  }

  auto& nextAcceptableStreamId = isUnidirectionalStream(streamId)
      ? nextAcceptablePeerUnidirectionalStreamId_
      : nextAcceptablePeerBidirectionalStreamId_;
  auto maxStreamId = isUnidirectionalStream(streamId)
      ? maxRemoteUnidirectionalStreamId_
      : maxRemoteBidirectionalStreamId_;
  auto openedResult = openPeerStreamIfNotClosed(
      streamId,
      openPeerStreams,
      nextAcceptableStreamId,
      maxStreamId,
      newPeerStreams_);
  if (openedResult == LocalErrorCode::CREATING_EXISTING_STREAM) {
    // Stream could be closed here.
    return nullptr;
  } else if (openedResult == LocalErrorCode::STREAM_LIMIT_EXCEEDED) {
    throw QuicTransportException(
        "Exceeded stream limit.", TransportErrorCode::STREAM_LIMIT_ERROR);
  }

  auto it = streams_.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(streamId),
      std::forward_as_tuple(streamId, conn_));
  addToStreamPriorityMap(it.first->second);
  QUIC_STATS(conn_.statsCallback, onNewQuicStream);
  return &it.first->second;
}

folly::Expected<QuicStreamState*, LocalErrorCode>
QuicStreamManager::createStream(StreamId streamId) {
  if (nodeType_ == QuicNodeType::Client && !isClientStream(streamId)) {
    throw QuicTransportException(
        "Attempted creating non-client stream on client",
        TransportErrorCode::STREAM_STATE_ERROR);
  } else if (nodeType_ == QuicNodeType::Server && !isServerStream(streamId)) {
    throw QuicTransportException(
        "Attempted creating non-server stream on server",
        TransportErrorCode::STREAM_STATE_ERROR);
  }
  auto existingStream = getOrCreateOpenedLocalStream(streamId);
  if (existingStream) {
    return existingStream;
  }
  bool isUni = isUnidirectionalStream(streamId);
  auto& nextAcceptableStreamId = isUni
      ? nextAcceptableLocalUnidirectionalStreamId_
      : nextAcceptableLocalBidirectionalStreamId_;
  auto maxStreamId =
      isUni ? maxLocalUnidirectionalStreamId_ : maxLocalBidirectionalStreamId_;

  auto& openLocalStreams =
      isUni ? openUnidirectionalLocalStreams_ : openBidirectionalLocalStreams_;
  auto openedResult = openLocalStreamIfNotClosed(
      streamId, openLocalStreams, nextAcceptableStreamId, maxStreamId);
  if (openedResult != LocalErrorCode::NO_ERROR) {
    return folly::makeUnexpected(openedResult);
  }
  auto it = streams_.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(streamId),
      std::forward_as_tuple(streamId, conn_));
  addToStreamPriorityMap(it.first->second);
  QUIC_STATS(conn_.statsCallback, onNewQuicStream);
  updateAppIdleState();
  return &it.first->second;
}

void QuicStreamManager::removeClosedStream(StreamId streamId) {
  auto it = streams_.find(streamId);
  if (it == streams_.end()) {
    VLOG(10) << "Trying to remove already closed stream=" << streamId;
    return;
  }
  VLOG(10) << "Removing closed stream=" << streamId;
  DCHECK(it->second.inTerminalStates());
  readableStreams_.erase(streamId);
  peekableStreams_.erase(streamId);
  writableStreams_.erase(streamId);
  writableDSRStreams_.erase(streamId);
  writableControlStreams_.erase(streamId);
  removeLoss(streamId);
  blockedStreams_.erase(streamId);
  deliverableStreams_.erase(streamId);
  txStreams_.erase(streamId);
  windowUpdates_.erase(streamId);
  stopSendingStreams_.erase(streamId);
  flowControlUpdated_.erase(streamId);
  {
    const auto streamPriorityIt = streamPriorityLevels_.find(streamId);
    if (streamPriorityIt == streamPriorityLevels_.end()) {
      throw QuicTransportException(
          "Removed stream is not in the priority map",
          TransportErrorCode::STREAM_STATE_ERROR);
    }
    streamPriorityLevels_.erase(streamPriorityIt);
  }
  if (it->second.isControl) {
    DCHECK_GT(numControlStreams_, 0);
    numControlStreams_--;
  }
  streams_.erase(it);
  QUIC_STATS(conn_.statsCallback, onQuicStreamClosed);
  if (isRemoteStream(nodeType_, streamId)) {
    auto& openPeerStreams = isUnidirectionalStream(streamId)
        ? openUnidirectionalPeerStreams_
        : openBidirectionalPeerStreams_;
    openPeerStreams.erase(streamId);
    // Check if we should send a stream limit update. We need to send an
    // update every time we've closed a number of streams >= the set windowing
    // fraction.
    uint64_t initialStreamLimit = isUnidirectionalStream(streamId)
        ? transportSettings_->advertisedInitialMaxStreamsUni
        : transportSettings_->advertisedInitialMaxStreamsBidi;
    uint64_t streamWindow = initialStreamLimit / streamLimitWindowingFraction_;
    uint64_t openableRemoteStreams = isUnidirectionalStream(streamId)
        ? openableRemoteUnidirectionalStreams()
        : openableRemoteBidirectionalStreams();
    // The "credit" here is how much available stream space we have based on
    // what the initial stream limit was set to.
    uint64_t streamCredit =
        initialStreamLimit - openableRemoteStreams - openPeerStreams.size();
    if (streamCredit >= streamWindow) {
      if (isUnidirectionalStream(streamId)) {
        uint64_t maxStreams = (maxRemoteUnidirectionalStreamId_ -
                               initialRemoteUnidirectionalStreamId_) /
            detail::kStreamIncrement;
        setMaxRemoteUnidirectionalStreams(maxStreams + streamCredit);
        remoteUnidirectionalStreamLimitUpdate_ = maxStreams + streamCredit;
      } else {
        uint64_t maxStreams = (maxRemoteBidirectionalStreamId_ -
                               initialRemoteBidirectionalStreamId_) /
            detail::kStreamIncrement;
        setMaxRemoteBidirectionalStreams(maxStreams + streamCredit);
        remoteBidirectionalStreamLimitUpdate_ = maxStreams + streamCredit;
      }
    }
  } else {
    auto& openLocalStreams = isUnidirectionalStream(streamId)
        ? openUnidirectionalLocalStreams_
        : openBidirectionalLocalStreams_;
    openLocalStreams.erase(streamId);
  }
  updateAppIdleState();
  notifyStreamPriorityChanges();
}

void QuicStreamManager::updateReadableStreams(QuicStreamState& stream) {
  updateHolBlockedTime(stream);
  if (stream.hasReadableData() || stream.streamReadError.has_value()) {
    readableStreams_.emplace(stream.id);
  } else {
    readableStreams_.erase(stream.id);
  }
}

void QuicStreamManager::updateWritableStreams(QuicStreamState& stream) {
  if (stream.streamWriteError.has_value()) {
    CHECK(stream.lossBuffer.empty());
    CHECK(stream.lossBufMetas.empty());
    removeWritable(stream);
    removeDSRWritable(stream);
    return;
  }
  if (stream.hasWritableData() || !stream.lossBuffer.empty()) {
    addWritable(stream);
  } else {
    removeWritable(stream);
  }
  if (stream.isControl) {
    return;
  }
  if (stream.dsrSender &&
      (stream.hasWritableBufMeta() || !stream.lossBufMetas.empty())) {
    addDSRWritable(stream);
  } else {
    removeDSRWritable(stream);
  }
}

void QuicStreamManager::updatePeekableStreams(QuicStreamState& stream) {
  // In the PeekCallback, the API peekError() is added, so change the condition
  // and allow streamReadError in the peekableStreams
  if (stream.hasPeekableData() || stream.streamReadError.has_value()) {
    peekableStreams_.emplace(stream.id);
  } else {
    peekableStreams_.erase(stream.id);
  }
}

void QuicStreamManager::updateAppIdleState() {
  bool currentNonCtrlStreams = hasNonCtrlStreams();
  if (isAppIdle_ && !currentNonCtrlStreams) {
    // We were app limited, and we continue to be app limited.
    return;
  } else if (!isAppIdle_ && currentNonCtrlStreams) {
    // We were not app limited, and we continue to be not app limited.
    return;
  }
  isAppIdle_ = !currentNonCtrlStreams;
  if (conn_.congestionController) {
    conn_.congestionController->setAppIdle(isAppIdle_, Clock::now());
  }
}

void QuicStreamManager::setStreamAsControl(QuicStreamState& stream) {
  if (!stream.isControl) {
    stream.isControl = true;
    numControlStreams_++;
  }
  updateAppIdleState();
}

bool QuicStreamManager::isAppIdle() const {
  return isAppIdle_;
}

PriorityLevel QuicStreamManager::getHighestPriorityLevel() const {
  // Highest priority is minimum value
  auto min = kDefaultMaxPriority;
  for (auto& entry : streamPriorityLevels_) {
    if (entry.second < min) {
      min = entry.second;
    }
    if (min == 0) {
      break;
    }
  }
  return min;
}

void QuicStreamManager::setPriorityChangesObserver(
    QuicStreamPrioritiesObserver* observer) {
  priorityChangesObserver_ = observer;
}

void QuicStreamManager::resetPriorityChangesObserver() {
  if (!priorityChangesObserver_) {
    return;
  }
  priorityChangesObserver_ = nullptr;
}

void QuicStreamManager::notifyStreamPriorityChanges() {
  if (priorityChangesObserver_) {
    priorityChangesObserver_->onStreamPrioritiesChange();
  }
}

void QuicStreamManager::addToStreamPriorityMap(
    const QuicStreamState& streamState) {
  auto entry = streamPriorityLevels_.emplace(
      streamState.id, PriorityLevel(streamState.priority.level));

  // Verify stream didn't already exist in streamPriorityLevels_
  if (!entry.second) {
    throw QuicTransportException(
        "Attempted to add stream already in priority map",
        TransportErrorCode::STREAM_STATE_ERROR);
  }

  // Verify inserted item
  if (entry.first->second != PriorityLevel(streamState.priority.level)) {
    throw QuicTransportException(
        "Failed to add stream to priority map",
        TransportErrorCode::STREAM_STATE_ERROR);
  }

  // Notify observer (if set)
  notifyStreamPriorityChanges();
}

} // namespace quic
