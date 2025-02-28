/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 */

#include <quic/state/QuicStateFunctions.h>
#include <quic/state/QuicStreamFunctions.h>

#include <quic/common/TimeUtil.h>

namespace {
std::deque<quic::OutstandingPacket>::reverse_iterator
getPreviousOutstandingPacket(
    quic::QuicConnectionStateBase& conn,
    quic::PacketNumberSpace packetNumberSpace,
    std::deque<quic::OutstandingPacket>::reverse_iterator from) {
  return std::find_if(
      from, conn.outstandings.packets.rend(), [=](const auto& op) {
        return !op.declaredLost &&
            packetNumberSpace == op.packet.header.getPacketNumberSpace();
      });
}
std::deque<quic::OutstandingPacket>::reverse_iterator
getPreviousOutstandingPacketIncludingLost(
    quic::QuicConnectionStateBase& conn,
    quic::PacketNumberSpace packetNumberSpace,
    std::deque<quic::OutstandingPacket>::reverse_iterator from) {
  return std::find_if(
      from, conn.outstandings.packets.rend(), [=](const auto& op) {
        return packetNumberSpace == op.packet.header.getPacketNumberSpace();
      });
}

} // namespace

namespace quic {

void updateRtt(
    QuicConnectionStateBase& conn,
    std::chrono::microseconds rttSample,
    std::chrono::microseconds ackDelay) {
  std::chrono::microseconds minRtt = timeMin(conn.lossState.mrtt, rttSample);
  conn.lossState.maxAckDelay = timeMax(conn.lossState.maxAckDelay, ackDelay);
  bool shouldUseAckDelay = (rttSample > ackDelay) &&
      (rttSample > minRtt + ackDelay || conn.lossState.mrtt == kDefaultMinRtt);
  if (shouldUseAckDelay) {
    rttSample -= ackDelay;
  }
  // mrtt ignores ack delay. This is the same in the current recovery draft
  // section A.6.
  conn.lossState.mrtt = minRtt;
  // We use the original minRtt without the ack delay included here
  // explicitly. We might want to change this by including ackDelay
  // as well.
  conn.lossState.lrtt = rttSample;
  if (conn.lossState.srtt == 0us) {
    conn.lossState.srtt = rttSample;
    conn.lossState.rttvar = rttSample / 2;
  } else {
    conn.lossState.rttvar = conn.lossState.rttvar * (kRttBeta - 1) / kRttBeta +
        (conn.lossState.srtt > rttSample ? conn.lossState.srtt - rttSample
                                         : rttSample - conn.lossState.srtt) /
            kRttBeta;
    conn.lossState.srtt = conn.lossState.srtt * (kRttAlpha - 1) / kRttAlpha +
        rttSample / kRttAlpha;
  }
  if (conn.qLogger) {
    conn.qLogger->addMetricUpdate(
        rttSample, conn.lossState.mrtt, conn.lossState.srtt, ackDelay);
  }
}

void updateAckSendStateOnRecvPacket(
    QuicConnectionStateBase& conn,
    AckState& ackState,
    bool pktOutOfOrder,
    bool pktHasRetransmittableData,
    bool pktHasCryptoData) {
  DCHECK(!pktHasCryptoData || pktHasRetransmittableData);
  auto thresh = kNonRtxRxPacketsPendingBeforeAck;
  if (pktHasRetransmittableData || ackState.numRxPacketsRecvd) {
    if (ackState.tolerance.hasValue()) {
      thresh = ackState.tolerance.value();
    } else {
      thresh = ackState.largestReceivedPacketNum.value_or(0) >
              conn.transportSettings.rxPacketsBeforeAckInitThreshold
          ? conn.transportSettings.rxPacketsBeforeAckAfterInit
          : conn.transportSettings.rxPacketsBeforeAckBeforeInit;
    }
  }
  if (ackState.ignoreReorder) {
    pktOutOfOrder = false;
  }
  if (pktHasRetransmittableData) {
    if (pktHasCryptoData || pktOutOfOrder ||
        ++ackState.numRxPacketsRecvd + ackState.numNonRxPacketsRecvd >=
            thresh) {
      VLOG(10) << conn
               << " ack immediately because packet threshold pktHasCryptoData="
               << pktHasCryptoData << " pktHasRetransmittableData="
               << static_cast<int>(pktHasRetransmittableData)
               << " numRxPacketsRecvd="
               << static_cast<int>(ackState.numRxPacketsRecvd)
               << " numNonRxPacketsRecvd="
               << static_cast<int>(ackState.numNonRxPacketsRecvd);
      conn.pendingEvents.scheduleAckTimeout = false;
      ackState.needsToSendAckImmediately = true;
    } else if (!ackState.needsToSendAckImmediately) {
      VLOG(10) << conn << " scheduling ack timeout pktHasCryptoData="
               << pktHasCryptoData << " pktHasRetransmittableData="
               << static_cast<int>(pktHasRetransmittableData)
               << " numRxPacketsRecvd="
               << static_cast<int>(ackState.numRxPacketsRecvd)
               << " numNonRxPacketsRecvd="
               << static_cast<int>(ackState.numNonRxPacketsRecvd);
      conn.pendingEvents.scheduleAckTimeout = true;
    }
  } else if (
      ++ackState.numNonRxPacketsRecvd + ackState.numRxPacketsRecvd >= thresh) {
    VLOG(10)
        << conn
        << " ack immediately because exceeds nonrx threshold numNonRxPacketsRecvd="
        << static_cast<int>(ackState.numNonRxPacketsRecvd)
        << " numRxPacketsRecvd="
        << static_cast<int>(ackState.numRxPacketsRecvd);
    conn.pendingEvents.scheduleAckTimeout = false;
    ackState.needsToSendAckImmediately = true;
  }
  if (ackState.needsToSendAckImmediately) {
    ackState.numRxPacketsRecvd = 0;
    ackState.numNonRxPacketsRecvd = 0;
  }
}

void updateAckStateOnAckTimeout(QuicConnectionStateBase& conn) {
  VLOG(10) << conn << " ack immediately due to ack timeout";
  conn.ackStates.appDataAckState.needsToSendAckImmediately = true;
  conn.ackStates.appDataAckState.numRxPacketsRecvd = 0;
  conn.ackStates.appDataAckState.numNonRxPacketsRecvd = 0;
  conn.pendingEvents.scheduleAckTimeout = false;
}

void updateAckSendStateOnSentPacketWithAcks(
    QuicConnectionStateBase& conn,
    AckState& ackState,
    PacketNum largestAckScheduled) {
  VLOG(10) << conn << " unset ack immediately due to sending packet with acks";
  conn.pendingEvents.scheduleAckTimeout = false;
  ackState.needsToSendAckImmediately = false;
  // When we send an ack we're most likely going to ack the largest received
  // packet, so reset the counters for numRxPacketsRecvd and
  // numNonRxPacketsRecvd. Since our ack threshold is quite small, we make the
  // critical assumtion here that that all the needed acks can fit into one
  // packet if needed. If this is not the case, then some packets may not get
  // acked as a result and the receiver might retransmit them.
  ackState.numRxPacketsRecvd = 0;
  ackState.numNonRxPacketsRecvd = 0;
  ackState.largestAckScheduled = largestAckScheduled;
}

bool isConnectionPaced(const QuicConnectionStateBase& conn) noexcept {
  return (
      conn.transportSettings.pacingEnabled && conn.canBePaced && conn.pacer);
}

AckState& getAckState(
    QuicConnectionStateBase& conn,
    PacketNumberSpace pnSpace) noexcept {
  switch (pnSpace) {
    case PacketNumberSpace::Initial:
      return conn.ackStates.initialAckState;
    case PacketNumberSpace::Handshake:
      return conn.ackStates.handshakeAckState;
    case PacketNumberSpace::AppData:
      return conn.ackStates.appDataAckState;
  }
  folly::assume_unreachable();
}

const AckState& getAckState(
    const QuicConnectionStateBase& conn,
    PacketNumberSpace pnSpace) noexcept {
  switch (pnSpace) {
    case PacketNumberSpace::Initial:
      return conn.ackStates.initialAckState;
    case PacketNumberSpace::Handshake:
      return conn.ackStates.handshakeAckState;
    case PacketNumberSpace::AppData:
      return conn.ackStates.appDataAckState;
  }
  folly::assume_unreachable();
}

AckStateVersion currentAckStateVersion(
    const QuicConnectionStateBase& conn) noexcept {
  return AckStateVersion(
      conn.ackStates.initialAckState.acks.insertVersion(),
      conn.ackStates.handshakeAckState.acks.insertVersion(),
      conn.ackStates.appDataAckState.acks.insertVersion());
}

PacketNum getNextPacketNum(
    const QuicConnectionStateBase& conn,
    PacketNumberSpace pnSpace) noexcept {
  return getAckState(conn, pnSpace).nextPacketNum;
}

void increaseNextPacketNum(
    QuicConnectionStateBase& conn,
    PacketNumberSpace pnSpace) noexcept {
  getAckState(conn, pnSpace).nextPacketNum++;
  if (getAckState(conn, pnSpace).nextPacketNum == kMaxPacketNumber - 1) {
    conn.pendingEvents.closeTransport = true;
  }
}

std::deque<OutstandingPacket>::iterator getFirstOutstandingPacket(
    QuicConnectionStateBase& conn,
    PacketNumberSpace packetNumberSpace) {
  return getNextOutstandingPacket(
      conn, packetNumberSpace, conn.outstandings.packets.begin());
}

std::deque<OutstandingPacket>::reverse_iterator getLastOutstandingPacket(
    QuicConnectionStateBase& conn,
    PacketNumberSpace packetNumberSpace) {
  return getPreviousOutstandingPacket(
      conn, packetNumberSpace, conn.outstandings.packets.rbegin());
}

std::deque<OutstandingPacket>::reverse_iterator
getLastOutstandingPacketIncludingLost(
    QuicConnectionStateBase& conn,
    PacketNumberSpace packetNumberSpace) {
  return getPreviousOutstandingPacketIncludingLost(
      conn, packetNumberSpace, conn.outstandings.packets.rbegin());
}

std::deque<OutstandingPacket>::iterator getNextOutstandingPacket(
    QuicConnectionStateBase& conn,
    PacketNumberSpace packetNumberSpace,
    std::deque<OutstandingPacket>::iterator from) {
  return std::find_if(
      from, conn.outstandings.packets.end(), [=](const auto& op) {
        return !op.declaredLost &&
            packetNumberSpace == op.packet.header.getPacketNumberSpace();
      });
}

bool hasReceivedPacketsAtLastCloseSent(
    const QuicConnectionStateBase& conn) noexcept {
  return conn.ackStates.initialAckState.largestReceivedAtLastCloseSent ||
      conn.ackStates.handshakeAckState.largestReceivedAtLastCloseSent ||
      conn.ackStates.appDataAckState.largestReceivedAtLastCloseSent;
}

bool hasNotReceivedNewPacketsSinceLastCloseSent(
    const QuicConnectionStateBase& conn) noexcept {
  DCHECK(
      !conn.ackStates.initialAckState.largestReceivedAtLastCloseSent ||
      *conn.ackStates.initialAckState.largestReceivedAtLastCloseSent <=
          *conn.ackStates.initialAckState.largestReceivedPacketNum);
  DCHECK(
      !conn.ackStates.handshakeAckState.largestReceivedAtLastCloseSent ||
      *conn.ackStates.handshakeAckState.largestReceivedAtLastCloseSent <=
          *conn.ackStates.handshakeAckState.largestReceivedPacketNum);
  DCHECK(
      !conn.ackStates.appDataAckState.largestReceivedAtLastCloseSent ||
      *conn.ackStates.appDataAckState.largestReceivedAtLastCloseSent <=
          *conn.ackStates.appDataAckState.largestReceivedPacketNum);
  return conn.ackStates.initialAckState.largestReceivedAtLastCloseSent ==
      conn.ackStates.initialAckState.largestReceivedPacketNum &&
      conn.ackStates.handshakeAckState.largestReceivedAtLastCloseSent ==
      conn.ackStates.handshakeAckState.largestReceivedPacketNum &&
      conn.ackStates.appDataAckState.largestReceivedAtLastCloseSent ==
      conn.ackStates.appDataAckState.largestReceivedPacketNum;
}

void updateLargestReceivedPacketsAtLastCloseSent(
    QuicConnectionStateBase& conn) noexcept {
  conn.ackStates.initialAckState.largestReceivedAtLastCloseSent =
      conn.ackStates.initialAckState.largestReceivedPacketNum;
  conn.ackStates.handshakeAckState.largestReceivedAtLastCloseSent =
      conn.ackStates.handshakeAckState.largestReceivedPacketNum;
  conn.ackStates.appDataAckState.largestReceivedAtLastCloseSent =
      conn.ackStates.appDataAckState.largestReceivedPacketNum;
}

bool hasReceivedPackets(const QuicConnectionStateBase& conn) noexcept {
  return conn.ackStates.initialAckState.largestReceivedPacketNum ||
      conn.ackStates.handshakeAckState.largestReceivedPacketNum ||
      conn.ackStates.appDataAckState.largestReceivedPacketNum;
}

folly::Optional<TimePoint>& getLossTime(
    QuicConnectionStateBase& conn,
    PacketNumberSpace pnSpace) noexcept {
  return conn.lossState.lossTimes[pnSpace];
}

bool canSetLossTimerForAppData(const QuicConnectionStateBase& conn) noexcept {
  return conn.oneRttWriteCipher != nullptr;
}

std::pair<folly::Optional<TimePoint>, PacketNumberSpace> earliestLossTimer(
    const QuicConnectionStateBase& conn) noexcept {
  bool considerAppData = canSetLossTimerForAppData(conn);
  return earliestTimeAndSpace(conn.lossState.lossTimes, considerAppData);
}

std::pair<folly::Optional<TimePoint>, PacketNumberSpace> earliestTimeAndSpace(
    const EnumArray<PacketNumberSpace, folly::Optional<TimePoint>>& times,
    bool considerAppData) noexcept {
  std::pair<folly::Optional<TimePoint>, PacketNumberSpace> res = {
      folly::none, PacketNumberSpace::Initial};
  for (PacketNumberSpace pns : times.keys()) {
    if (!times[pns]) {
      continue;
    }
    if (pns == PacketNumberSpace::AppData && !considerAppData) {
      continue;
    }
    if (!res.first || *res.first > *times[pns]) {
      res.first = times[pns];
      res.second = pns;
    }
  }
  return res;
}

} // namespace quic
