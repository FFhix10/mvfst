/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 */

#include <quic/server/state/ServerStateMachine.h>

#include <quic/api/QuicTransportFunctions.h>
#include <quic/common/BufUtil.h>
#include <quic/congestion_control/CongestionControllerFactory.h>
#include <quic/congestion_control/TokenlessPacer.h>
#include <quic/fizz/handshake/FizzCryptoFactory.h>
#include <quic/flowcontrol/QuicFlowController.h>
#include <quic/handshake/TransportParameters.h>
#include <quic/logging/QLoggerConstants.h>
#include <quic/state/DatagramHandlers.h>
#include <quic/state/QuicPacingFunctions.h>
#include <quic/state/QuicStreamFunctions.h>
#include <quic/state/QuicTransportStatsCallback.h>
#include <quic/state/SimpleFrameFunctions.h>
#include <quic/state/stream/StreamReceiveHandlers.h>
#include <quic/state/stream/StreamSendHandlers.h>

namespace quic {
using namespace std::chrono_literals;

namespace {
using PacketDropReason = QuicTransportStatsCallback::PacketDropReason;
constexpr size_t kConnIdEncodingRetryLimit = 16;

bool maybeNATRebinding(
    const folly::SocketAddress& newPeerAddress,
    const folly::SocketAddress& oldPeerAddress) {
  auto& newIPAddr = newPeerAddress.getIPAddress();
  auto& oldIPAddr = oldPeerAddress.getIPAddress();

  // Port changed
  if (newIPAddr == oldIPAddr) {
    return true;
  }

  return newIPAddr.isV4() && oldIPAddr.isV4() &&
      newIPAddr.inSubnet(oldIPAddr, 24);
}

CongestionAndRttState moveCurrentCongestionAndRttState(
    QuicServerConnectionState& conn) {
  CongestionAndRttState state;
  state.peerAddress = conn.peerAddress;
  state.recordTime = Clock::now();
  state.congestionController = std::move(conn.congestionController);
  state.srtt = conn.lossState.srtt;
  state.lrtt = conn.lossState.lrtt;
  state.rttvar = conn.lossState.rttvar;
  state.mrtt = conn.lossState.mrtt;
  return state;
}

void resetCongestionAndRttState(QuicServerConnectionState& conn) {
  CHECK(conn.congestionControllerFactory)
      << "CongestionControllerFactory is not set.";
  conn.congestionController =
      conn.congestionControllerFactory->makeCongestionController(
          conn, conn.transportSettings.defaultCongestionController);
  conn.lossState.srtt = 0us;
  conn.lossState.lrtt = 0us;
  conn.lossState.rttvar = 0us;
  conn.lossState.mrtt = kDefaultMinRtt;
}

void recoverOrResetCongestionAndRttState(
    QuicServerConnectionState& conn,
    const folly::SocketAddress& peerAddress) {
  auto& lastState = conn.migrationState.lastCongestionAndRtt;
  if (lastState && lastState->peerAddress == peerAddress &&
      (Clock::now() - lastState->recordTime <=
       kTimeToRetainLastCongestionAndRttState)) {
    // recover from matched non-stale state
    conn.congestionController = std::move(lastState->congestionController);
    conn.lossState.srtt = lastState->srtt;
    conn.lossState.lrtt = lastState->lrtt;
    conn.lossState.rttvar = lastState->rttvar;
    conn.lossState.mrtt = lastState->mrtt;
    conn.migrationState.lastCongestionAndRtt = folly::none;
  } else {
    resetCongestionAndRttState(conn);
  }
}

void setExperimentalSettings(QuicServerConnectionState& /*conn*/) {
  // MVFST_EXPERIMENTAL is currently using initCwnd=30 set in
  // QuicServerWorker.cpp before CC is initialized.
}
} // namespace

void processClientInitialParams(
    QuicServerConnectionState& conn,
    const ClientTransportParameters& clientParams) {
  auto preferredAddress = getIntegerParameter(
      TransportParameterId::preferred_address, clientParams.parameters);
  auto origConnId = getIntegerParameter(
      TransportParameterId::original_destination_connection_id,
      clientParams.parameters);
  auto statelessResetToken = getIntegerParameter(
      TransportParameterId::stateless_reset_token, clientParams.parameters);
  auto retrySourceConnId = getIntegerParameter(
      TransportParameterId::retry_source_connection_id,
      clientParams.parameters);

  auto maxData = getIntegerParameter(
      TransportParameterId::initial_max_data, clientParams.parameters);
  auto maxStreamDataBidiLocal = getIntegerParameter(
      TransportParameterId::initial_max_stream_data_bidi_local,
      clientParams.parameters);
  auto maxStreamDataBidiRemote = getIntegerParameter(
      TransportParameterId::initial_max_stream_data_bidi_remote,
      clientParams.parameters);
  auto maxStreamDataUni = getIntegerParameter(
      TransportParameterId::initial_max_stream_data_uni,
      clientParams.parameters);
  auto maxStreamsBidi = getIntegerParameter(
      TransportParameterId::initial_max_streams_bidi, clientParams.parameters);
  auto maxStreamsUni = getIntegerParameter(
      TransportParameterId::initial_max_streams_uni, clientParams.parameters);
  auto idleTimeout = getIntegerParameter(
      TransportParameterId::idle_timeout, clientParams.parameters);
  auto ackDelayExponent = getIntegerParameter(
      TransportParameterId::ack_delay_exponent, clientParams.parameters);
  auto packetSize = getIntegerParameter(
      TransportParameterId::max_packet_size, clientParams.parameters);
  auto activeConnectionIdLimit = getIntegerParameter(
      TransportParameterId::active_connection_id_limit,
      clientParams.parameters);
  auto d6dBasePMTU = getIntegerParameter(
      static_cast<TransportParameterId>(kD6DBasePMTUParameterId),
      clientParams.parameters);
  auto d6dRaiseTimeout = getIntegerParameter(
      static_cast<TransportParameterId>(kD6DRaiseTimeoutParameterId),
      clientParams.parameters);
  auto d6dProbeTimeout = getIntegerParameter(
      static_cast<TransportParameterId>(kD6DProbeTimeoutParameterId),
      clientParams.parameters);
  auto minAckDelay = getIntegerParameter(
      TransportParameterId::min_ack_delay, clientParams.parameters);
  auto maxAckDelay = getIntegerParameter(
      TransportParameterId::max_ack_delay, clientParams.parameters);
  auto maxDatagramFrameSize = getIntegerParameter(
      TransportParameterId::max_datagram_frame_size, clientParams.parameters);

  if (conn.version == QuicVersion::QUIC_DRAFT ||
      conn.version == QuicVersion::QUIC_V1) {
    auto initialSourceConnId = getConnIdParameter(
        TransportParameterId::initial_source_connection_id,
        clientParams.parameters);
    if (!initialSourceConnId ||
        initialSourceConnId.value() !=
            conn.readCodec->getClientConnectionId()) {
      throw QuicTransportException(
          "Initial CID does not match.",
          TransportErrorCode::TRANSPORT_PARAMETER_ERROR);
    }
  }

  // validate that we didn't receive original connection ID, stateless
  // reset token, or preferred address.
  if (preferredAddress && *preferredAddress != 0) {
    throw QuicTransportException(
        "Preferred Address is received by server",
        TransportErrorCode::TRANSPORT_PARAMETER_ERROR);
  }

  if (origConnId && *origConnId != 0) {
    throw QuicTransportException(
        "OriginalDestinationConnectionId is received by server",
        TransportErrorCode::TRANSPORT_PARAMETER_ERROR);
  }

  if (statelessResetToken && statelessResetToken.value() != 0) {
    throw QuicTransportException(
        "Stateless Reset Token is received by server",
        TransportErrorCode::TRANSPORT_PARAMETER_ERROR);
  }

  if (retrySourceConnId && retrySourceConnId.value() != 0) {
    throw QuicTransportException(
        "Retry Source Connection ID is received by server",
        TransportErrorCode::TRANSPORT_PARAMETER_ERROR);
  }

  if (maxAckDelay && *maxAckDelay >= kMaxAckDelay) {
    throw QuicTransportException(
        "Max Ack Delay is greater than 2^14 ",
        TransportErrorCode::TRANSPORT_PARAMETER_ERROR);
  }

  // TODO Validate active_connection_id_limit
  if (packetSize && *packetSize < kMinMaxUDPPayload) {
    throw QuicTransportException(
        folly::to<std::string>(
            "Max packet size too small. received max_packetSize = ",
            *packetSize),
        TransportErrorCode::TRANSPORT_PARAMETER_ERROR);
  }

  VLOG(10) << "Client advertised flow control ";
  VLOG(10) << "conn=" << maxData.value_or(0);
  VLOG(10) << " stream bidi local=" << maxStreamDataBidiLocal.value_or(0)
           << " ";
  VLOG(10) << " stream bidi remote=" << maxStreamDataBidiRemote.value_or(0)
           << " ";
  VLOG(10) << " stream uni=" << maxStreamDataUni.value_or(0) << " ";
  VLOG(10) << conn;
  conn.flowControlState.peerAdvertisedMaxOffset = maxData.value_or(0);
  conn.flowControlState.peerAdvertisedInitialMaxStreamOffsetBidiLocal =
      maxStreamDataBidiLocal.value_or(0);
  conn.flowControlState.peerAdvertisedInitialMaxStreamOffsetBidiRemote =
      maxStreamDataBidiRemote.value_or(0);
  conn.flowControlState.peerAdvertisedInitialMaxStreamOffsetUni =
      maxStreamDataUni.value_or(0);
  conn.streamManager->setMaxLocalBidirectionalStreams(
      maxStreamsBidi.value_or(0));
  conn.streamManager->setMaxLocalUnidirectionalStreams(
      maxStreamsUni.value_or(0));
  conn.peerIdleTimeout = std::chrono::milliseconds(idleTimeout.value_or(0));
  conn.peerIdleTimeout = timeMin(conn.peerIdleTimeout, kMaxIdleTimeout);
  if (ackDelayExponent && *ackDelayExponent > kMaxAckDelayExponent) {
    throw QuicTransportException(
        "ack_delay_exponent too large",
        TransportErrorCode::TRANSPORT_PARAMETER_ERROR);
  }
  conn.peerAckDelayExponent =
      ackDelayExponent.value_or(kDefaultAckDelayExponent);
  if (minAckDelay.hasValue()) {
    conn.peerMinAckDelay = std::chrono::microseconds(minAckDelay.value());
  }
  if (maxDatagramFrameSize.hasValue()) {
    if (maxDatagramFrameSize.value() > 0 &&
        maxDatagramFrameSize.value() <= kMaxDatagramPacketOverhead) {
      throw QuicTransportException(
          "max_datagram_frame_size too small",
          TransportErrorCode::TRANSPORT_PARAMETER_ERROR);
    }
    conn.datagramState.maxWriteFrameSize = maxDatagramFrameSize.value();
  }

  // Default to max because we can probe PMTU now, and this will be the upper
  // limit
  uint64_t maxUdpPayloadSize = kDefaultMaxUDPPayload;
  if (packetSize) {
    maxUdpPayloadSize = std::min(*packetSize, maxUdpPayloadSize);
    conn.peerMaxUdpPayloadSize = maxUdpPayloadSize;
    if (conn.transportSettings.canIgnorePathMTU) {
      if (*packetSize > kDefaultMaxUDPPayload) {
        // A good peer should never set oversized limit, so to be safe we
        // fallback to default
        conn.udpSendPacketLen = kDefaultUDPSendPacketLen;
      } else {
        // Otherwise, canIgnorePathMTU forces us to immediately set
        // udpSendPacketLen
        // TODO: rename "canIgnorePathMTU" to "forciblySetPathMTU"
        conn.udpSendPacketLen = maxUdpPayloadSize;
      }
    }
  }

  conn.peerActiveConnectionIdLimit =
      activeConnectionIdLimit.value_or(kDefaultActiveConnectionIdLimit);

  if (conn.transportSettings.d6dConfig.enabled) {
    // Sanity check
    if (d6dBasePMTU) {
      if (*d6dBasePMTU >= kMinMaxUDPPayload &&
          *d6dBasePMTU <= kDefaultMaxUDPPayload) {
        // The reason to take the max is because we don't want d6d to send
        // probes with a smaller packet size than udpSendPacketLen, which would
        // be useless and cause meaningless delay on finding the upper bound.
        conn.d6d.basePMTU = std::max(*d6dBasePMTU, conn.udpSendPacketLen);
        conn.d6d.maxPMTU = maxUdpPayloadSize;
        VLOG(10) << "conn.d6d.basePMTU=" << conn.d6d.basePMTU;

        // Start from base
        conn.d6d.state = D6DMachineState::BASE;
        conn.d6d.meta.lastNonSearchState = D6DMachineState::DISABLED;
        conn.d6d.meta.timeLastNonSearchState = Clock::now();

        // Temporary, should be removed after transport knob pipeline works
        conn.d6d.noBlackholeDetection = true;
      } else {
        LOG(ERROR) << "client d6dBasePMTU fails sanity check: " << *d6dBasePMTU;
        // We treat base pmtu transport param as client's swich to activate d6d,
        // so not receiving that means there's no need to configure the rest d6d
        // params
        return;
      }
    }

    if (d6dRaiseTimeout) {
      if (*d6dRaiseTimeout >= kMinD6DRaiseTimeout.count()) {
        conn.d6d.raiseTimeout = std::chrono::seconds(*d6dRaiseTimeout);
        VLOG(10) << "conn.d6d.raiseTimeout=" << conn.d6d.raiseTimeout.count();
      } else {
        LOG(ERROR) << "client d6dRaiseTimeout fails sanity check: "
                   << *d6dRaiseTimeout;
      }
    }

    if (d6dProbeTimeout) {
      if (*d6dProbeTimeout >= kMinD6DProbeTimeout.count()) {
        conn.d6d.probeTimeout = std::chrono::seconds(*d6dProbeTimeout);
        VLOG(10) << "conn.d6d.probeTimeout=" << conn.d6d.probeTimeout.count();
      } else {
        LOG(ERROR) << "client d6dProbeTimeout fails sanity check: "
                   << *d6dProbeTimeout;
      }
    }
  }
}

void updateHandshakeState(QuicServerConnectionState& conn) {
  // Zero RTT read cipher is available after chlo is processed with the
  // condition that early data attempt is accepted.
  auto handshakeLayer = conn.serverHandshakeLayer;
  auto zeroRttReadCipher = handshakeLayer->getZeroRttReadCipher();
  auto zeroRttHeaderCipher = handshakeLayer->getZeroRttReadHeaderCipher();
  // One RTT write cipher is available at Fizz layer after chlo is processed.
  // However, the cipher is only exported to QUIC if early data attempt is
  // accepted. Otherwise, the cipher will be available after cfin is
  // processed.
  auto oneRttWriteCipher = handshakeLayer->getOneRttWriteCipher();
  // One RTT read cipher is available after cfin is processed.
  auto oneRttReadCipher = handshakeLayer->getOneRttReadCipher();

  auto oneRttWriteHeaderCipher = handshakeLayer->getOneRttWriteHeaderCipher();
  auto oneRttReadHeaderCipher = handshakeLayer->getOneRttReadHeaderCipher();

  if (zeroRttReadCipher) {
    conn.usedZeroRtt = true;
    if (conn.qLogger) {
      conn.qLogger->addTransportStateUpdate(kDerivedZeroRttReadCipher);
    }
    conn.readCodec->setZeroRttReadCipher(std::move(zeroRttReadCipher));
  }
  if (zeroRttHeaderCipher) {
    conn.readCodec->setZeroRttHeaderCipher(std::move(zeroRttHeaderCipher));
  }
  if (oneRttWriteHeaderCipher) {
    conn.oneRttWriteHeaderCipher = std::move(oneRttWriteHeaderCipher);
  }
  if (oneRttReadHeaderCipher) {
    conn.readCodec->setOneRttHeaderCipher(std::move(oneRttReadHeaderCipher));
  }

  if (oneRttWriteCipher) {
    if (conn.qLogger) {
      conn.qLogger->addTransportStateUpdate(kDerivedOneRttWriteCipher);
    }
    if (conn.oneRttWriteCipher) {
      throw QuicTransportException(
          "Duplicate 1-rtt write cipher", TransportErrorCode::CRYPTO_ERROR);
    }
    conn.oneRttWriteCipher = std::move(oneRttWriteCipher);

    updatePacingOnKeyEstablished(conn);

    // We negotiate the transport parameters whenever we have the 1-RTT write
    // keys available.
    auto clientParams = handshakeLayer->getClientTransportParams();
    if (!clientParams) {
      throw QuicTransportException(
          "No client transport params",
          TransportErrorCode::TRANSPORT_PARAMETER_ERROR);
    }
    processClientInitialParams(conn, std::move(*clientParams));
  }
  if (oneRttReadCipher) {
    if (conn.qLogger) {
      conn.qLogger->addTransportStateUpdate(kDerivedOneRttReadCipher);
    }
    // Clear limit because CFIN is received at this point
    conn.writableBytesLimit = folly::none;
    conn.readCodec->setOneRttReadCipher(std::move(oneRttReadCipher));
  }
  auto handshakeReadCipher = handshakeLayer->getHandshakeReadCipher();
  auto handshakeReadHeaderCipher =
      handshakeLayer->getHandshakeReadHeaderCipher();
  if (handshakeReadCipher) {
    CHECK(handshakeReadHeaderCipher);
    conn.readCodec->setHandshakeReadCipher(std::move(handshakeReadCipher));
    conn.readCodec->setHandshakeHeaderCipher(
        std::move(handshakeReadHeaderCipher));
  }
  if (handshakeLayer->isHandshakeDone()) {
    CHECK(conn.oneRttWriteCipher);
    if (!conn.sentHandshakeDone) {
      sendSimpleFrame(conn, HandshakeDoneFrame());
      conn.sentHandshakeDone = true;
    }
  }
}

bool validateAndUpdateSourceToken(
    QuicServerConnectionState& conn,
    std::vector<folly::IPAddress> sourceAddresses) {
  DCHECK(conn.peerAddress.isInitialized());
  bool foundMatch = false;
  for (int ii = sourceAddresses.size() - 1; ii >= 0; --ii) {
    // TODO T33014230 subnet matching
    if (conn.peerAddress.getIPAddress() == sourceAddresses[ii]) {
      foundMatch = true;
      // If peer address is found in the token, move the element to the end
      // of vector to increase its favorability.
      sourceAddresses.erase(sourceAddresses.begin() + ii);
      sourceAddresses.push_back(conn.peerAddress.getIPAddress());
    }
  }
  conn.sourceTokenMatching = foundMatch;
  bool acceptZeroRtt =
      (conn.transportSettings.zeroRttSourceTokenMatchingPolicy !=
       ZeroRttSourceTokenMatchingPolicy::ALWAYS_REJECT) &&
      foundMatch;

  if (!foundMatch) {
    // Add peer address to token for next resumption
    if (sourceAddresses.size() >= kMaxNumTokenSourceAddresses) {
      sourceAddresses.erase(sourceAddresses.begin());
    }
    sourceAddresses.push_back(conn.peerAddress.getIPAddress());

    switch (conn.transportSettings.zeroRttSourceTokenMatchingPolicy) {
      case ZeroRttSourceTokenMatchingPolicy::ALWAYS_REJECT:
      case ZeroRttSourceTokenMatchingPolicy::REJECT_IF_NO_EXACT_MATCH:
        acceptZeroRtt = false;
        break;
      case ZeroRttSourceTokenMatchingPolicy::LIMIT_IF_NO_EXACT_MATCH:
        acceptZeroRtt = true;
        conn.writableBytesLimit =
            conn.transportSettings.limitedCwndInMss * conn.udpSendPacketLen;
        break;
    }
  }
  // Save the source token so that it can be written to client via NST later
  conn.tokenSourceAddresses = std::move(sourceAddresses);

  return acceptZeroRtt;
}

void updateWritableByteLimitOnRecvPacket(QuicServerConnectionState& conn) {
  // When we receive a packet we increase the limit again. The reasoning this is
  // that a peer can do the same by opening a new connection.
  if (conn.writableBytesLimit) {
    conn.writableBytesLimit = *conn.writableBytesLimit +
        conn.transportSettings.limitedCwndInMss * conn.udpSendPacketLen;
  }
}

void updateTransportParamsFromTicket(
    QuicServerConnectionState& conn,
    uint64_t idleTimeout,
    uint64_t maxRecvPacketSize,
    uint64_t initialMaxData,
    uint64_t initialMaxStreamDataBidiLocal,
    uint64_t initialMaxStreamDataBidiRemote,
    uint64_t initialMaxStreamDataUni,
    uint64_t initialMaxStreamsBidi,
    uint64_t initialMaxStreamsUni) {
  conn.transportSettings.idleTimeout = std::chrono::milliseconds(idleTimeout);
  conn.transportSettings.maxRecvPacketSize = maxRecvPacketSize;

  conn.transportSettings.advertisedInitialConnectionWindowSize = initialMaxData;
  conn.transportSettings.advertisedInitialBidiLocalStreamWindowSize =
      initialMaxStreamDataBidiLocal;
  conn.transportSettings.advertisedInitialBidiRemoteStreamWindowSize =
      initialMaxStreamDataBidiRemote;
  conn.transportSettings.advertisedInitialUniStreamWindowSize =
      initialMaxStreamDataUni;
  updateFlowControlStateWithSettings(
      conn.flowControlState, conn.transportSettings);

  conn.transportSettings.advertisedInitialMaxStreamsBidi =
      initialMaxStreamsBidi;
  conn.transportSettings.advertisedInitialMaxStreamsUni = initialMaxStreamsUni;
}

void onConnectionMigration(
    QuicServerConnectionState& conn,
    const folly::SocketAddress& newPeerAddress,
    bool isIntentional) {
  if (conn.migrationState.numMigrations >= kMaxNumMigrationsAllowed) {
    if (conn.qLogger) {
      conn.qLogger->addPacketDrop(
          0,
          QuicTransportStatsCallback::toString(
              PacketDropReason::PEER_ADDRESS_CHANGE));
    }
    QUIC_STATS(
        conn.statsCallback,
        onPacketDropped,
        PacketDropReason::PEER_ADDRESS_CHANGE);
    throw QuicTransportException(
        "Too many migrations", TransportErrorCode::INVALID_MIGRATION);
  }
  ++conn.migrationState.numMigrations;

  bool hasPendingPathChallenge = conn.pendingEvents.pathChallenge.has_value();
  // Clear any pending path challenge frame that is not sent
  conn.pendingEvents.pathChallenge = folly::none;

  auto& previousPeerAddresses = conn.migrationState.previousPeerAddresses;
  auto it = std::find(
      previousPeerAddresses.begin(),
      previousPeerAddresses.end(),
      newPeerAddress);
  if (it == previousPeerAddresses.end()) {
    // Send new path challenge
    uint64_t pathData;
    folly::Random::secureRandom(&pathData, sizeof(pathData));
    conn.pendingEvents.pathChallenge = PathChallengeFrame(pathData);

    // If we are already in the middle of a migration reset
    // the available bytes in the rate-limited window, but keep the
    // window.
    conn.pathValidationLimiter =
        std::make_unique<PendingPathRateLimiter>(conn.udpSendPacketLen);
  } else {
    previousPeerAddresses.erase(it);
  }

  // At this point, path validation scheduled, writable bytes limit set
  // However if this is NAT rebinding, keep congestion state unchanged
  bool isNATRebinding = maybeNATRebinding(newPeerAddress, conn.peerAddress);

  // Cancel current path validation if any
  if (hasPendingPathChallenge || conn.outstandingPathValidation) {
    conn.pendingEvents.schedulePathValidationTimeout = false;
    conn.outstandingPathValidation = folly::none;

    // Only change congestion & rtt state if not NAT rebinding
    if (!isNATRebinding) {
      recoverOrResetCongestionAndRttState(conn, newPeerAddress);
    }
  } else {
    // Only add validated addresses to previousPeerAddresses
    conn.migrationState.previousPeerAddresses.push_back(conn.peerAddress);

    // Only change congestion & rtt state if not NAT rebinding
    if (!isNATRebinding) {
      // Current peer address is validated,
      // remember its congestion state and rtt stats
      CongestionAndRttState state = moveCurrentCongestionAndRttState(conn);
      recoverOrResetCongestionAndRttState(conn, newPeerAddress);
      conn.migrationState.lastCongestionAndRtt = std::move(state);
    }
  }

  if (conn.qLogger) {
    conn.qLogger->addConnectionMigrationUpdate(isIntentional);
  }
  conn.peerAddress = newPeerAddress;
}

void onServerReadData(
    QuicServerConnectionState& conn,
    ServerEvents::ReadData& readData) {
  switch (conn.state) {
    case ServerState::Open:
      onServerReadDataFromOpen(conn, readData);
      return;
    case ServerState::Closed:
      onServerReadDataFromClosed(conn, readData);
      return;
  }
}

void handleCipherUnavailable(
    CipherUnavailable* originalData,
    QuicServerConnectionState& conn,
    size_t packetSize,
    ServerEvents::ReadData& readData) {
  if (!originalData->packet || originalData->packet->empty()) {
    VLOG(10) << "drop because no data " << conn;
    if (conn.qLogger) {
      conn.qLogger->addPacketDrop(packetSize, kNoData);
    }
    return;
  }
  if (originalData->protectionType != ProtectionType::ZeroRtt &&
      originalData->protectionType != ProtectionType::KeyPhaseZero) {
    VLOG(10) << "drop because unexpected protection level " << conn;
    if (conn.qLogger) {
      conn.qLogger->addPacketDrop(packetSize, kUnexpectedProtectionLevel);
    }
    return;
  }

  size_t combinedSize =
      (conn.pendingZeroRttData ? conn.pendingZeroRttData->size() : 0) +
      (conn.pendingOneRttData ? conn.pendingOneRttData->size() : 0);
  if (combinedSize >= conn.transportSettings.maxPacketsToBuffer) {
    VLOG(10) << "drop because max buffered " << conn;
    if (conn.qLogger) {
      conn.qLogger->addPacketDrop(packetSize, kMaxBuffered);
    }
    return;
  }

  auto& pendingData = originalData->protectionType == ProtectionType::ZeroRtt
      ? conn.pendingZeroRttData
      : conn.pendingOneRttData;
  if (pendingData) {
    if (conn.qLogger) {
      conn.qLogger->addPacketBuffered(originalData->protectionType, packetSize);
    }
    ServerEvents::ReadData pendingReadData;
    pendingReadData.peer = readData.peer;
    pendingReadData.networkData = NetworkDataSingle(
        std::move(originalData->packet), readData.networkData.receiveTimePoint);
    pendingData->emplace_back(std::move(pendingReadData));
    VLOG(10) << "Adding pending data to "
             << toString(originalData->protectionType)
             << " buffer size=" << pendingData->size() << " " << conn;
  } else {
    VLOG(10) << "drop because " << toString(originalData->protectionType)
             << " buffer no longer available " << conn;
    if (conn.qLogger) {
      conn.qLogger->addPacketDrop(packetSize, kBufferUnavailable);
    }
  }
}

void onServerReadDataFromOpen(
    QuicServerConnectionState& conn,
    ServerEvents::ReadData& readData) {
  CHECK_EQ(conn.state, ServerState::Open);
  // Don't bother parsing if the data is empty.
  if (!readData.networkData.data ||
      readData.networkData.data->computeChainDataLength() == 0) {
    return;
  }
  if (!conn.readCodec) {
    // First packet from the peer
    folly::io::Cursor cursor(readData.networkData.data.get());
    auto initialByte = cursor.readBE<uint8_t>();
    auto parsedLongHeader = parseLongHeaderInvariant(initialByte, cursor);
    if (!parsedLongHeader) {
      VLOG(4) << "Could not parse initial packet header";
      if (conn.qLogger) {
        conn.qLogger->addPacketDrop(
            0,
            QuicTransportStatsCallback::toString(
                PacketDropReason::PARSE_ERROR));
      }
      QUIC_STATS(
          conn.statsCallback, onPacketDropped, PacketDropReason::PARSE_ERROR);
      return;
    }
    QuicVersion version = parsedLongHeader->invariant.version;
    if (version == QuicVersion::VERSION_NEGOTIATION) {
      VLOG(4) << "Server droppiong VN packet";
      if (conn.qLogger) {
        conn.qLogger->addPacketDrop(
            0,
            QuicTransportStatsCallback::toString(
                PacketDropReason::INVALID_PACKET));
      }
      QUIC_STATS(
          conn.statsCallback,
          onPacketDropped,
          PacketDropReason::INVALID_PACKET);
      return;
    }

    const auto& clientConnectionId = parsedLongHeader->invariant.srcConnId;
    const auto& initialDestinationConnectionId =
        parsedLongHeader->invariant.dstConnId;

    if (initialDestinationConnectionId.size() < kDefaultConnectionIdSize) {
      VLOG(4) << "Initial connectionid too small";
      if (conn.qLogger) {
        conn.qLogger->addPacketDrop(
            0,
            QuicTransportStatsCallback::toString(
                PacketDropReason::INITIAL_CONNID_SMALL));
      }
      QUIC_STATS(
          conn.statsCallback,
          onPacketDropped,
          PacketDropReason::INITIAL_CONNID_SMALL);
      return;
    }

    CHECK(conn.connIdAlgo) << "ConnectionIdAlgo is not set.";
    CHECK(!conn.serverConnectionId.has_value());
    // serverConnIdParams must be set by the QuicServerTransport
    CHECK(conn.serverConnIdParams);

    auto newServerConnIdData = conn.createAndAddNewSelfConnId();
    CHECK(newServerConnIdData.has_value());
    conn.serverConnectionId = newServerConnIdData->connId;

    auto customTransportParams = setSupportedExtensionTransportParameters(conn);

    QUIC_STATS(conn.statsCallback, onStatelessReset);
    conn.serverHandshakeLayer->accept(
        std::make_shared<ServerTransportParametersExtension>(
            version,
            conn.transportSettings.advertisedInitialConnectionWindowSize,
            conn.transportSettings.advertisedInitialBidiLocalStreamWindowSize,
            conn.transportSettings.advertisedInitialBidiRemoteStreamWindowSize,
            conn.transportSettings.advertisedInitialUniStreamWindowSize,
            conn.transportSettings.advertisedInitialMaxStreamsBidi,
            conn.transportSettings.advertisedInitialMaxStreamsUni,
            conn.transportSettings.idleTimeout,
            conn.transportSettings.ackDelayExponent,
            conn.transportSettings.maxRecvPacketSize,
            *newServerConnIdData->token,
            conn.serverConnectionId.value(),
            initialDestinationConnectionId,
            customTransportParams));
    conn.transportParametersEncoded = true;
    const CryptoFactory& cryptoFactory =
        conn.serverHandshakeLayer->getCryptoFactory();
    conn.readCodec = std::make_unique<QuicReadCodec>(QuicNodeType::Server);
    conn.readCodec->setInitialReadCipher(cryptoFactory.getClientInitialCipher(
        initialDestinationConnectionId, version));
    conn.readCodec->setClientConnectionId(clientConnectionId);
    conn.readCodec->setServerConnectionId(*conn.serverConnectionId);
    if (conn.qLogger) {
      conn.qLogger->setScid(conn.serverConnectionId);
      conn.qLogger->setDcid(initialDestinationConnectionId);
    }
    conn.readCodec->setCodecParameters(
        CodecParameters(conn.peerAckDelayExponent, version));
    conn.initialWriteCipher = cryptoFactory.getServerInitialCipher(
        initialDestinationConnectionId, version);

    conn.readCodec->setInitialHeaderCipher(
        cryptoFactory.makeClientInitialHeaderCipher(
            initialDestinationConnectionId, version));
    conn.initialHeaderCipher = cryptoFactory.makeServerInitialHeaderCipher(
        initialDestinationConnectionId, version);
    conn.peerAddress = conn.originalPeerAddress;
  }
  BufQueue udpData;
  udpData.append(std::move(readData.networkData.data));
  for (uint16_t processedPackets = 0;
       !udpData.empty() && processedPackets < kMaxNumCoalescedPackets;
       processedPackets++) {
    size_t dataSize = udpData.chainLength();
    auto parsedPacket = conn.readCodec->parsePacket(udpData, conn.ackStates);
    size_t packetSize = dataSize - udpData.chainLength();

    switch (parsedPacket.type()) {
      case CodecResult::Type::CIPHER_UNAVAILABLE: {
        handleCipherUnavailable(
            parsedPacket.cipherUnavailable(), conn, packetSize, readData);
        break;
      }
      case CodecResult::Type::RETRY: {
        VLOG(10) << "drop because the server is not supposed to "
                 << "receive a retry " << conn;
        if (conn.qLogger) {
          conn.qLogger->addPacketDrop(packetSize, kRetry);
        }
        break;
      }
      case CodecResult::Type::STATELESS_RESET: {
        VLOG(10) << "drop because reset " << conn;
        if (conn.qLogger) {
          conn.qLogger->addPacketDrop(packetSize, kReset);
        }
        break;
      }
      case CodecResult::Type::NOTHING: {
        VLOG(10) << "drop cipher unavailable, no data " << conn;
        if (conn.qLogger) {
          conn.qLogger->addPacketDrop(packetSize, kCipherUnavailable);
        }
        break;
      }
      case CodecResult::Type::REGULAR_PACKET:
        break;
    }

    RegularQuicPacket* regularOptional = parsedPacket.regularPacket();
    if (!regularOptional) {
      // We were unable to parse the packet, drop for now. All the drop reasons
      // should have already been logged into QLogger and QuicTrace inside the
      // previous switch-case block. We just need to update QUIC_STATS here.
      VLOG(10) << "Not able to parse QUIC packet " << conn;
      QUIC_STATS(
          conn.statsCallback, onPacketDropped, PacketDropReason::PARSE_ERROR);
      continue;
    }
    if (regularOptional->frames.empty()) {
      // This packet had a pareseable header (most probably short header)
      // but no data. This is a protocol violation so we throw an exception.
      // This drop has not been recorded in the switch-case block above
      // so we record it here.
      if (conn.qLogger) {
        conn.qLogger->addPacketDrop(
            packetSize,
            QuicTransportStatsCallback::toString(
                PacketDropReason::PROTOCOL_VIOLATION));
      }
      QUIC_STATS(
          conn.statsCallback,
          onPacketDropped,
          PacketDropReason::PROTOCOL_VIOLATION);
      throw QuicTransportException(
          "Packet has no frames", TransportErrorCode::PROTOCOL_VIOLATION);
    }

    auto protectionLevel = regularOptional->header.getProtectionType();
    auto encryptionLevel = protectionTypeToEncryptionLevel(protectionLevel);

    auto packetNum = regularOptional->header.getPacketSequenceNum();
    auto packetNumberSpace = regularOptional->header.getPacketNumberSpace();

    auto& regularPacket = *regularOptional;

    bool isProtectedPacket = protectionLevel == ProtectionType::ZeroRtt ||
        protectionLevel == ProtectionType::KeyPhaseZero ||
        protectionLevel == ProtectionType::KeyPhaseOne;

    if (!isProtectedPacket) {
      for (auto& quicFrame : regularPacket.frames) {
        auto isPadding = quicFrame.asPaddingFrame();
        auto isAck = quicFrame.asReadAckFrame();
        auto isClose = quicFrame.asConnectionCloseFrame();
        auto isCrypto = quicFrame.asReadCryptoFrame();
        auto isPing = quicFrame.asPingFrame();
        // TODO: add path challenge and response
        if (!isPadding && !isAck && !isClose && !isCrypto && !isPing) {
          QUIC_STATS(
              conn.statsCallback,
              onPacketDropped,
              PacketDropReason::PROTOCOL_VIOLATION);
          if (conn.qLogger) {
            conn.qLogger->addPacketDrop(
                packetSize,
                QuicTransportStatsCallback::toString(
                    PacketDropReason::PROTOCOL_VIOLATION));
          }
          throw QuicTransportException(
              "Invalid frame", TransportErrorCode::PROTOCOL_VIOLATION);
        }
      }
    }

    CHECK(conn.clientConnectionId);
    if (conn.qLogger) {
      conn.qLogger->addPacket(regularPacket, packetSize);
    }
    // We assume that the higher layer takes care of validating that the version
    // is supported.
    if (!conn.version) {
      LongHeader* longHeader = regularPacket.header.asLong();
      if (!longHeader) {
        throw QuicTransportException(
            "Invalid packet type", TransportErrorCode::PROTOCOL_VIOLATION);
      }
      conn.version = longHeader->getVersion();
      if (conn.version == QuicVersion::MVFST_EXPERIMENTAL) {
        setExperimentalSettings(conn);
      }
    }

    if (conn.peerAddress != readData.peer) {
      if (encryptionLevel != EncryptionLevel::AppData) {
        if (conn.qLogger) {
          conn.qLogger->addPacketDrop(
              packetSize,
              QuicTransportStatsCallback::toString(
                  PacketDropReason::PEER_ADDRESS_CHANGE));
        }
        QUIC_STATS(
            conn.statsCallback,
            onPacketDropped,
            PacketDropReason::PEER_ADDRESS_CHANGE);
        throw QuicTransportException(
            "Migration not allowed during handshake",
            TransportErrorCode::INVALID_MIGRATION);
      }

      if (conn.transportSettings.disableMigration) {
        if (conn.qLogger) {
          conn.qLogger->addPacketDrop(
              packetSize,
              QuicTransportStatsCallback::toString(
                  PacketDropReason::PEER_ADDRESS_CHANGE));
        }
        QUIC_STATS(
            conn.statsCallback,
            onPacketDropped,
            PacketDropReason::PEER_ADDRESS_CHANGE);
        throw QuicTransportException(
            "Migration disabled", TransportErrorCode::INVALID_MIGRATION);
      }
    }

    auto& ackState = getAckState(conn, packetNumberSpace);
    bool outOfOrder = updateLargestReceivedPacketNum(
        ackState, packetNum, readData.networkData.receiveTimePoint);
    if (outOfOrder) {
      QUIC_STATS(conn.statsCallback, onOutOfOrderPacketReceived);
    }
    DCHECK(hasReceivedPackets(conn));

    bool pktHasRetransmittableData = false;
    bool pktHasCryptoData = false;
    bool isNonProbingPacket = false;
    bool handshakeConfirmedThisLoop = false;

    for (auto& quicFrame : regularPacket.frames) {
      switch (quicFrame.type()) {
        case QuicFrame::Type::ReadAckFrame: {
          VLOG(10) << "Server received ack frame packet=" << packetNum << " "
                   << conn;
          isNonProbingPacket = true;
          ReadAckFrame& ackFrame = *quicFrame.asReadAckFrame();
          processAckFrame(
              conn,
              packetNumberSpace,
              ackFrame,
              [&](const OutstandingPacket& packet,
                  const QuicWriteFrame& packetFrame,
                  const ReadAckFrame&) {
                switch (packetFrame.type()) {
                  case QuicWriteFrame::Type::WriteStreamFrame: {
                    const WriteStreamFrame& frame =
                        *packetFrame.asWriteStreamFrame();
                    VLOG(4)
                        << "Server received ack for stream=" << frame.streamId
                        << " offset=" << frame.offset << " fin=" << frame.fin
                        << " len=" << frame.len << " " << conn;
                    auto ackedStream =
                        conn.streamManager->getStream(frame.streamId);
                    if (ackedStream) {
                      sendAckSMHandler(*ackedStream, frame);
                    }
                    break;
                  }
                  case QuicWriteFrame::Type::WriteCryptoFrame: {
                    const WriteCryptoFrame& frame =
                        *packetFrame.asWriteCryptoFrame();
                    auto cryptoStream =
                        getCryptoStream(*conn.cryptoState, encryptionLevel);
                    processCryptoStreamAck(
                        *cryptoStream, frame.offset, frame.len);
                    break;
                  }
                  case QuicWriteFrame::Type::RstStreamFrame: {
                    const RstStreamFrame& frame =
                        *packetFrame.asRstStreamFrame();
                    VLOG(4) << "Server received ack for reset stream="
                            << frame.streamId << " " << conn;
                    auto stream = conn.streamManager->getStream(frame.streamId);
                    if (stream) {
                      sendRstAckSMHandler(*stream);
                    }
                    break;
                  }
                  case QuicWriteFrame::Type::WriteAckFrame: {
                    const WriteAckFrame& frame = *packetFrame.asWriteAckFrame();
                    DCHECK(!frame.ackBlocks.empty());
                    VLOG(4) << "Server received ack for largestAcked="
                            << frame.ackBlocks.front().end << " " << conn;
                    commonAckVisitorForAckFrame(ackState, frame);
                    break;
                  }
                  case QuicWriteFrame::Type::PingFrame:
                    if (!packet.metadata.isD6DProbe) {
                      conn.pendingEvents.cancelPingTimeout = true;
                    }
                    return;
                  case QuicWriteFrame::Type::QuicSimpleFrame: {
                    const QuicSimpleFrame& frame =
                        *packetFrame.asQuicSimpleFrame();
                    // ACK of HandshakeDone is a server-specific behavior.
                    if (frame.asHandshakeDoneFrame()) {
                      // Call handshakeConfirmed outside of the packet
                      // processing loop to avoid a re-entrancy.
                      handshakeConfirmedThisLoop = true;
                    }
                    break;
                  }
                  default: {
                    break;
                  }
                }
              },
              markPacketLoss,
              readData.networkData.receiveTimePoint);
          break;
        }
        case QuicFrame::Type::RstStreamFrame: {
          RstStreamFrame& frame = *quicFrame.asRstStreamFrame();
          VLOG(10) << "Server received reset stream=" << frame.streamId << " "
                   << conn;
          pktHasRetransmittableData = true;
          isNonProbingPacket = true;
          auto stream = conn.streamManager->getStream(frame.streamId);
          if (!stream) {
            break;
          }
          receiveRstStreamSMHandler(*stream, frame);
          break;
        }
        case QuicFrame::Type::ReadCryptoFrame: {
          pktHasRetransmittableData = true;
          pktHasCryptoData = true;
          isNonProbingPacket = true;
          ReadCryptoFrame& cryptoFrame = *quicFrame.asReadCryptoFrame();
          VLOG(10) << "Server received crypto data offset="
                   << cryptoFrame.offset
                   << " len=" << cryptoFrame.data->computeChainDataLength()
                   << " currentReadOffset="
                   << getCryptoStream(*conn.cryptoState, encryptionLevel)
                          ->currentReadOffset
                   << " " << conn;
          appendDataToReadBuffer(
              *getCryptoStream(*conn.cryptoState, encryptionLevel),
              StreamBuffer(
                  std::move(cryptoFrame.data), cryptoFrame.offset, false));
          break;
        }
        case QuicFrame::Type::ReadStreamFrame: {
          ReadStreamFrame& frame = *quicFrame.asReadStreamFrame();
          VLOG(10) << "Server received stream data for stream="
                   << frame.streamId << ", offset=" << frame.offset
                   << " len=" << frame.data->computeChainDataLength()
                   << " fin=" << frame.fin << " " << conn;
          pktHasRetransmittableData = true;
          isNonProbingPacket = true;
          auto stream = conn.streamManager->getStream(frame.streamId);
          // Ignore data from closed streams that we don't have the
          // state for any more.
          if (stream) {
            receiveReadStreamFrameSMHandler(*stream, std::move(frame));
          }
          break;
        }
        case QuicFrame::Type::MaxDataFrame: {
          MaxDataFrame& connWindowUpdate = *quicFrame.asMaxDataFrame();
          VLOG(10) << "Server received max data offset="
                   << connWindowUpdate.maximumData << " " << conn;
          pktHasRetransmittableData = true;
          isNonProbingPacket = true;
          handleConnWindowUpdate(conn, connWindowUpdate, packetNum);
          break;
        }
        case QuicFrame::Type::MaxStreamDataFrame: {
          MaxStreamDataFrame& streamWindowUpdate =
              *quicFrame.asMaxStreamDataFrame();
          VLOG(10) << "Server received max stream data stream="
                   << streamWindowUpdate.streamId
                   << " offset=" << streamWindowUpdate.maximumData << " "
                   << conn;
          if (isReceivingStream(conn.nodeType, streamWindowUpdate.streamId)) {
            throw QuicTransportException(
                "Received MaxStreamDataFrame for receiving stream.",
                TransportErrorCode::STREAM_STATE_ERROR);
          }
          pktHasRetransmittableData = true;
          isNonProbingPacket = true;
          auto stream =
              conn.streamManager->getStream(streamWindowUpdate.streamId);
          if (stream) {
            handleStreamWindowUpdate(
                *stream, streamWindowUpdate.maximumData, packetNum);
          }
          break;
        }
        case QuicFrame::Type::DataBlockedFrame: {
          VLOG(10) << "Server received blocked " << conn;
          pktHasRetransmittableData = true;
          isNonProbingPacket = true;
          handleConnBlocked(conn);
          break;
        }
        case QuicFrame::Type::StreamDataBlockedFrame: {
          StreamDataBlockedFrame& blocked =
              *quicFrame.asStreamDataBlockedFrame();
          VLOG(10) << "Server received blocked stream=" << blocked.streamId
                   << " " << conn;
          pktHasRetransmittableData = true;
          isNonProbingPacket = true;
          auto stream = conn.streamManager->getStream(blocked.streamId);
          if (stream) {
            handleStreamBlocked(*stream);
          }
          break;
        }
        case QuicFrame::Type::StreamsBlockedFrame: {
          StreamsBlockedFrame& blocked = *quicFrame.asStreamsBlockedFrame();
          // peer wishes to open a stream, but is unable to due to the maximum
          // stream limit set by us
          // TODO implement the handler
          isNonProbingPacket = true;
          VLOG(10) << "Server received streams blocked limit="
                   << blocked.streamLimit << ", " << conn;
          break;
        }
        case QuicFrame::Type::ConnectionCloseFrame: {
          isNonProbingPacket = true;
          ConnectionCloseFrame& connFrame = *quicFrame.asConnectionCloseFrame();
          auto errMsg = folly::to<std::string>(
              "Server closed by peer reason=", connFrame.reasonPhrase);
          VLOG(4) << errMsg << " " << conn;
          // we want to deliver app callbacks with the peer supplied error,
          // but send a NO_ERROR to the peer.
          if (conn.qLogger) {
            conn.qLogger->addTransportStateUpdate(getPeerClose(errMsg));
          }
          conn.peerConnectionError = std::make_pair(
              QuicErrorCode(connFrame.errorCode), std::move(errMsg));
          if (getSendConnFlowControlBytesWire(conn) == 0 &&
              conn.flowControlState.sumCurStreamBufferLen) {
            VLOG(2) << "Client gives up a flow control blocked connection";
          }
          throw QuicTransportException(
              "Peer closed", TransportErrorCode::NO_ERROR);
          break;
        }
        case QuicFrame::Type::PingFrame:
          isNonProbingPacket = true;
          // Ping isn't retransmittable data. But we would like to ack them
          // early.
          pktHasRetransmittableData = true;
          break;
        case QuicFrame::Type::PaddingFrame:
          break;
        case QuicFrame::Type::QuicSimpleFrame: {
          pktHasRetransmittableData = true;
          QuicSimpleFrame& simpleFrame = *quicFrame.asQuicSimpleFrame();
          isNonProbingPacket |= updateSimpleFrameOnPacketReceived(
              conn, simpleFrame, packetNum, readData.peer != conn.peerAddress);
          break;
        }
        case QuicFrame::Type::DatagramFrame: {
          DatagramFrame& frame = *quicFrame.asDatagramFrame();
          VLOG(10) << "Server received datagram data: "
                   << " len=" << frame.length;
          // Datagram isn't retransmittable. But we would like to ack them
          // early. So, make Datagram frames count towards ack policy
          pktHasRetransmittableData = true;
          handleDatagram(conn, frame);
          break;
        }
        default: {
          break;
        }
      }
    }

    if (handshakeConfirmedThisLoop) {
      handshakeConfirmed(conn);
    }

    // Update writable limit before processing the handshake data. This is so
    // that if we haven't decided whether or not to validate the peer, we won't
    // increase the limit.
    updateWritableByteLimitOnRecvPacket(conn);

    if (conn.peerAddress != readData.peer) {
      // TODO use new conn id, make sure the other endpoint has new conn id
      if (isNonProbingPacket) {
        if (packetNum == ackState.largestReceivedPacketNum) {
          ShortHeader* shortHeader = regularPacket.header.asShort();
          bool intentionalMigration = false;
          if (shortHeader &&
              shortHeader->getConnectionId() != conn.serverConnectionId) {
            intentionalMigration = true;
          }
          onConnectionMigration(conn, readData.peer, intentionalMigration);
        }
      } else {
        // Server will need to response with PathResponse to the new address
        // while not updating peerAddress to new address
        if (conn.qLogger) {
          conn.qLogger->addPacketDrop(
              packetSize,
              QuicTransportStatsCallback::toString(
                  PacketDropReason::PEER_ADDRESS_CHANGE));
        }
        QUIC_STATS(
            conn.statsCallback,
            onPacketDropped,
            PacketDropReason::PEER_ADDRESS_CHANGE);
        throw QuicTransportException(
            "Probing not supported yet", TransportErrorCode::INVALID_MIGRATION);
      }
    }

    // Try reading bytes off of crypto, and performing a handshake.
    auto data = readDataFromCryptoStream(
        *getCryptoStream(*conn.cryptoState, encryptionLevel));
    if (data) {
      conn.serverHandshakeLayer->doHandshake(std::move(data), encryptionLevel);

      try {
        updateHandshakeState(conn);
      } catch (...) {
        if (conn.qLogger) {
          conn.qLogger->addPacketDrop(
              packetSize,
              QuicTransportStatsCallback::toString(
                  PacketDropReason::TRANSPORT_PARAMETER_ERROR));
        }
        QUIC_STATS(
            conn.statsCallback,
            onPacketDropped,
            QuicTransportStatsCallback::PacketDropReason::
                TRANSPORT_PARAMETER_ERROR);
        throw;
      }
    }
    updateAckSendStateOnRecvPacket(
        conn,
        ackState,
        outOfOrder,
        pktHasRetransmittableData,
        pktHasCryptoData);
    if (encryptionLevel == EncryptionLevel::Handshake &&
        conn.initialWriteCipher) {
      conn.initialWriteCipher.reset();
      conn.initialHeaderCipher.reset();
      conn.readCodec->setInitialReadCipher(nullptr);
      conn.readCodec->setInitialHeaderCipher(nullptr);
      implicitAckCryptoStream(conn, EncryptionLevel::Initial);
    }
    QUIC_STATS(conn.statsCallback, onPacketProcessed);
  }
  VLOG_IF(4, !udpData.empty())
      << "Leaving " << udpData.chainLength()
      << " bytes unprocessed after attempting to process "
      << kMaxNumCoalescedPackets << " packets.";
}

void onServerReadDataFromClosed(
    QuicServerConnectionState& conn,
    ServerEvents::ReadData& readData) {
  CHECK_EQ(conn.state, ServerState::Closed);
  BufQueue udpData;
  udpData.append(std::move(readData.networkData.data));
  auto packetSize = udpData.empty() ? 0 : udpData.chainLength();
  if (!conn.readCodec) {
    // drop data. We closed before we even got the first packet. This is
    // normally not possible but might as well.
    if (conn.qLogger) {
      conn.qLogger->addPacketDrop(
          packetSize,
          QuicTransportStatsCallback::toString(
              PacketDropReason::SERVER_STATE_CLOSED));
    }
    QUIC_STATS(
        conn.statsCallback,
        onPacketDropped,
        PacketDropReason::SERVER_STATE_CLOSED);
    return;
  }

  if (conn.peerConnectionError) {
    // We already got a peer error. We can ignore any futher peer errors.
    if (conn.qLogger) {
      conn.qLogger->addPacketDrop(
          packetSize,
          QuicTransportStatsCallback::toString(
              PacketDropReason::SERVER_STATE_CLOSED));
    }
    QUIC_STATS(
        conn.statsCallback,
        onPacketDropped,
        PacketDropReason::SERVER_STATE_CLOSED);
    return;
  }
  auto parsedPacket = conn.readCodec->parsePacket(udpData, conn.ackStates);
  switch (parsedPacket.type()) {
    case CodecResult::Type::CIPHER_UNAVAILABLE: {
      VLOG(10) << "drop cipher unavailable " << conn;
      if (conn.qLogger) {
        conn.qLogger->addPacketDrop(packetSize, kCipherUnavailable);
      }
      break;
    }
    case CodecResult::Type::RETRY: {
      VLOG(10) << "drop because the server is not supposed to "
               << "receive a retry " << conn;
      if (conn.qLogger) {
        conn.qLogger->addPacketDrop(packetSize, kRetry);
      }
      break;
    }
    case CodecResult::Type::STATELESS_RESET: {
      VLOG(10) << "drop because reset " << conn;
      if (conn.qLogger) {
        conn.qLogger->addPacketDrop(packetSize, kReset);
      }
      break;
    }
    case CodecResult::Type::NOTHING: {
      VLOG(10) << "drop cipher unavailable, no data " << conn;
      if (conn.qLogger) {
        conn.qLogger->addPacketDrop(packetSize, kCipherUnavailable);
      }
      break;
    }
    case CodecResult::Type::REGULAR_PACKET:
      break;
  }
  auto regularOptional = parsedPacket.regularPacket();
  if (!regularOptional) {
    // We were unable to parse the packet, drop for now.
    VLOG(10) << "Not able to parse QUIC packet " << conn;
    if (conn.qLogger) {
      conn.qLogger->addPacketDrop(
          packetSize,
          QuicTransportStatsCallback::toString(PacketDropReason::PARSE_ERROR));
    }
    QUIC_STATS(
        conn.statsCallback, onPacketDropped, PacketDropReason::PARSE_ERROR);
    return;
  }
  if (regularOptional->frames.empty()) {
    // This packet had a pareseable header (most probably short header)
    // but no data. This is a protocol violation so we throw an exception.
    // This drop has not been recorded in the switch-case block above
    // so we record it here.
    if (conn.qLogger) {
      conn.qLogger->addPacketDrop(
          packetSize,
          QuicTransportStatsCallback::toString(
              PacketDropReason::PROTOCOL_VIOLATION));
    }
    QUIC_STATS(
        conn.statsCallback,
        onPacketDropped,
        PacketDropReason::PROTOCOL_VIOLATION);
    throw QuicTransportException(
        "Packet has no frames", TransportErrorCode::PROTOCOL_VIOLATION);
  }

  auto& regularPacket = *regularOptional;
  auto packetNum = regularPacket.header.getPacketSequenceNum();
  auto pnSpace = regularPacket.header.getPacketNumberSpace();
  if (conn.qLogger) {
    conn.qLogger->addPacket(regularPacket, packetSize);
  }

  // Only process the close frames in the packet
  for (auto& quicFrame : regularPacket.frames) {
    switch (quicFrame.type()) {
      case QuicFrame::Type::ConnectionCloseFrame: {
        ConnectionCloseFrame& connFrame = *quicFrame.asConnectionCloseFrame();
        auto errMsg = folly::to<std::string>(
            "Server closed by peer reason=", connFrame.reasonPhrase);
        VLOG(4) << errMsg << " " << conn;
        if (conn.qLogger) {
          conn.qLogger->addTransportStateUpdate(getPeerClose(errMsg));
        }
        // we want to deliver app callbacks with the peer supplied error,
        // but send a NO_ERROR to the peer.
        conn.peerConnectionError = std::make_pair(
            QuicErrorCode(connFrame.errorCode), std::move(errMsg));
        break;
      }
      default:
        break;
    }
  }

  // We only need to set the largest received packet number in order to
  // determine whether or not we need to send a new close.
  auto& largestReceivedPacketNum =
      getAckState(conn, pnSpace).largestReceivedPacketNum;
  largestReceivedPacketNum = std::max<PacketNum>(
      largestReceivedPacketNum.value_or(packetNum), packetNum);
}

void onServerClose(QuicServerConnectionState& conn) {
  switch (conn.state) {
    case ServerState::Open:
      onServerCloseOpenState(conn);
      return;
    case ServerState::Closed:
      return;
  }
}

void onServerCloseOpenState(QuicServerConnectionState& conn) {
  conn.state = ServerState::Closed;
}

folly::Optional<ConnectionIdData>
QuicServerConnectionState::createAndAddNewSelfConnId() {
  // Should be set right after server transport construction.
  CHECK(connIdAlgo);
  CHECK(serverConnIdParams);

  CHECK(transportSettings.statelessResetTokenSecret);

  StatelessResetGenerator generator(
      transportSettings.statelessResetTokenSecret.value(),
      serverAddr.getFullyQualified());

  // The default connectionId algo has 36 bits of randomness.
  auto encodedCid = connIdAlgo->encodeConnectionId(*serverConnIdParams);
  size_t encodedTimes = 0;
  while (encodedCid && connIdRejector &&
         connIdRejector->rejectConnectionId(*encodedCid) &&
         ++encodedTimes < kConnIdEncodingRetryLimit) {
    encodedCid = connIdAlgo->encodeConnectionId(*serverConnIdParams);
  }
  LOG_IF(ERROR, encodedTimes == kConnIdEncodingRetryLimit)
      << "Quic CIDRejector rejected all conneectionIDs";
  if (encodedCid.hasError()) {
    return folly::none;
  }
  auto newConnIdData =
      ConnectionIdData{*encodedCid, nextSelfConnectionIdSequence++};
  newConnIdData.token = generator.generateToken(newConnIdData.connId);
  selfConnectionIds.push_back(newConnIdData);
  return newConnIdData;
}

std::vector<TransportParameter> setSupportedExtensionTransportParameters(
    QuicServerConnectionState& conn) {
  std::vector<TransportParameter> customTransportParams;
  if (conn.transportSettings.datagramConfig.enabled) {
    auto maxDatagramFrameSize =
        std::make_unique<CustomIntegralTransportParameter>(
            static_cast<uint64_t>(
                TransportParameterId::max_datagram_frame_size),
            conn.datagramState.maxReadFrameSize);
    customTransportParams.push_back(maxDatagramFrameSize->encode());
  }
  return customTransportParams;
}
} // namespace quic
