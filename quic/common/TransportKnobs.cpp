// Copyright 2004-present Facebook. All Rights Reserved.

#include <folly/json.h>
#include <glog/logging.h>
#include <quic/QuicConstants.h>
#include <quic/common/TransportKnobs.h>

namespace quic {

namespace {

constexpr uint64_t kKnobFractionMax = 100;

bool compareTransportKnobParam(
    const TransportKnobParam& lhs,
    const TransportKnobParam& rhs) {
  // Sort param by id, then value
  if (lhs.id != rhs.id) {
    return lhs.id < rhs.id;
  }
  return lhs.val < rhs.val;
}

} // namespace

folly::Optional<TransportKnobParams> parseTransportKnobs(
    const std::string& serializedParams) {
  TransportKnobParams knobParams;
  try {
    folly::dynamic params = folly::parseJson(serializedParams);
    for (const auto& id : params.keys()) {
      auto paramId = folly::to<uint64_t>(id.asInt());
      auto val = params[id];
      switch (val.type()) {
        case folly::dynamic::Type::BOOL:
        case folly::dynamic::Type::INT64:
          knobParams.push_back({paramId, folly::to<uint64_t>(val.asInt())});
          continue;
        case folly::dynamic::Type::STRING: {
          /*
           * set cc algorithm
           * expected format: string, all lower case, name of cc algorithm
           */
          if (paramId ==
              static_cast<uint64_t>(TransportKnobParamId::CC_ALGORITHM_KNOB)) {
            folly::Optional<CongestionControlType> cctype =
                congestionControlStrToType(val.asString());
            if (cctype) {
              knobParams.push_back(
                  {paramId, folly::to<uint64_t>(cctype.value())});
            } else {
              LOG(ERROR) << "unknown cc type " << val;
              return folly::none;
            }
            /*
             * set rtt factor used in cc algs like bbr or copa
             * expressed as a fraction (see
             * quic/congestion_control/TokenlessPacer.cpp) expected format:
             * string, "{numerator}/{denominator}" numerator and denominator
             * must both be in the range (0,MAX]
             */
          } else if (
              paramId ==
                  static_cast<uint64_t>(
                      TransportKnobParamId::STARTUP_RTT_FACTOR_KNOB) ||
              paramId ==
                  static_cast<uint64_t>(
                      TransportKnobParamId::DEFAULT_RTT_FACTOR_KNOB)) {
            auto s = val.asString();
            uint64_t factor = 0;
            auto pos = s.find('/');
            if (pos == std::string::npos) {
              LOG(ERROR)
                  << "rtt factor knob expected format {numerator}/{denominator}";
              return folly::none;
            }
            uint64_t numerator =
                folly::tryTo<int>(s.substr(0, pos)).value_or(kKnobFractionMax);
            uint64_t denominator =
                folly::tryTo<int>(s.substr(pos + 1, s.length()))
                    .value_or(kKnobFractionMax);
            if (numerator <= 0 || denominator <= 0 ||
                numerator >= kKnobFractionMax ||
                denominator >= kKnobFractionMax) {
              LOG(ERROR)
                  << "rtt factor knob numerator and denominator must be ints in range (0,"
                  << kKnobFractionMax << "]";
              return folly::none;
            }
            // transport knobs must be a single int, so we pack numerator and
            // denominator into a single int here and unpack in the handler
            factor = numerator * kKnobFractionMax + denominator;
            knobParams.push_back({paramId, folly::to<uint64_t>(factor)});
          } else if (
              paramId ==
              static_cast<uint64_t>(
                  TransportKnobParamId::AUTO_BACKGROUND_MODE)) {
            /*
             * set the auto background mode parameters for the transport
             * expected format: string
             * "{priority_threshold},{percent_utilization}" priority_threshold:
             * integer value [0-7] percent_utilization: integer value [25-100]
             */
            uint64_t combinedKnobVal = 0;
            std::string priorityThresholdStr, utilizationPercentStr;
            if (!folly::split(
                    ',',
                    val.asString(),
                    priorityThresholdStr,
                    utilizationPercentStr)) {
              LOG(ERROR)
                  << "auto background mode knob value is not in expected format: "
                  << "{priority_threshold},{percent_utilization}";
              return folly::none;
            }
            uint64_t priorityThreshold =
                folly::tryTo<int>(priorityThresholdStr).value_or(-1);
            uint64_t utilizationPercent =
                folly::tryTo<int>(utilizationPercentStr).value_or(-1);
            if (priorityThreshold < 0 ||
                priorityThreshold > kDefaultMaxPriority ||
                utilizationPercent < 25 || utilizationPercent > 100) {
              LOG(ERROR) << "invalid auto background mode parameters."
                         << "priority_threshold must be int [0-7]. "
                         << "percent_utilization must be int [25-100]";
              return folly::none;
            }
            // pack the values into one integer that will be unpacked in the
            // handler
            combinedKnobVal =
                (priorityThreshold * kPriorityThresholdKnobMultiplier) +
                utilizationPercent;
            knobParams.push_back(
                {paramId, folly::to<uint64_t>(combinedKnobVal)});
          } else {
            LOG(ERROR) << "string param type is not valid for this knob";
            return folly::none;
          }
          continue;
        }
        default:
          // Quic transport knob param values cannot be of type ARRAY, NULLT or
          // OBJECT
          LOG(ERROR) << "Invalid transport knob param value type" << val.type();
          return folly::none;
      }
    }
  } catch (const std::exception& e) {
    LOG(ERROR) << "fail to parse knobs: " << e.what();
    return folly::none;
  }

  std::sort(knobParams.begin(), knobParams.end(), compareTransportKnobParam);
  return knobParams;
}

} // namespace quic
