#include "ec_systemcore/adapter_identity.hpp"
#include "ec_systemcore/client.hpp"
#include "ec_systemcore/dc_sync.hpp"
#include "ec_systemcore/mini_json.hpp"
#include "ec_systemcore/protocol.hpp"
#include "ec_systemcore/realtime.hpp"
#include "ec_systemcore/safety.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using ec_systemcore::protocol::AckStatus;
using ec_systemcore::protocol::MessageType;

void Require(bool condition, const char* expression, int line) {
  if (!condition) {
    throw std::runtime_error("check failed at line " +
                             std::to_string(line) + ": " + expression);
  }
}

#define CHECK(expression) Require((expression), #expression, __LINE__)

template <typename Callable>
void CheckThrows(Callable&& callable) {
  bool threw = false;
  try {
    callable();
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
}

std::string Hex(const std::vector<std::uint8_t>& bytes) {
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const std::uint8_t byte : bytes) {
    output << std::setw(2) << static_cast<unsigned int>(byte);
  }
  return output.str();
}

void TestProtocolFraming() {
  using namespace ec_systemcore::protocol;
  const HelloPayload hello{
      .role = ClientRole::Controller,
      .subscribeInputs = true,
      .inputPeriodMs = 100,
      .lastSeenEpoch = 0x0102030405060708ULL,
  };
  const std::vector<std::uint8_t> encoded = EncodeFrame(
      MessageType::Hello, 0, EncodeHelloPayload(hello));
  CHECK(Hex(encoded) ==
        "18000000454353430101000000000000010164000807060504030201");

  FrameDecoder fragmented;
  std::vector<Frame> frames;
  for (const std::uint8_t byte : encoded) {
    CHECK(fragmented.Feed(std::span<const std::uint8_t>{&byte, 1},
                          &frames));
  }
  CHECK(frames.size() == 1);
  HelloPayload decoded;
  CHECK(DecodeHelloPayload(frames[0].payload, &decoded));
  CHECK(decoded.lastSeenEpoch == hello.lastSeenEpoch);

  const std::vector<std::uint8_t> maximumPayload(
      kMaximumFrameLength - kEnvelopeSize, 0xaa);
  const std::vector<std::uint8_t> maximumFrame =
      EncodeFrame(MessageType::Error, 7, maximumPayload);
  CHECK(maximumFrame.size() ==
        kLengthPrefixSize + kMaximumFrameLength);
  std::vector<std::uint8_t> coalesced = maximumFrame;
  coalesced.insert(coalesced.end(), maximumFrame.begin(),
                   maximumFrame.end());
  CHECK(coalesced.size() == kMaximumBufferedInput);
  FrameDecoder coalescedDecoder;
  frames.clear();
  CHECK(coalescedDecoder.Feed(coalesced, &frames));
  CHECK(frames.size() == 2);

  std::array<std::uint8_t, 4> oversized{
      0x01, 0x20, 0x00, 0x00};
  FrameDecoder oversizedDecoder;
  frames.clear();
  CHECK(!oversizedDecoder.Feed(oversized, &frames));
  CHECK(oversizedDecoder.error() ==
        FrameDecoder::Error::InvalidLength);

  std::vector<std::uint8_t> unknown = encoded;
  unknown[9] = 0x7f;
  FrameDecoder unknownDecoder;
  frames.clear();
  CHECK(!unknownDecoder.Feed(unknown, &frames));
  CHECK(unknownDecoder.error() ==
        FrameDecoder::Error::InvalidMessageType);

  std::vector<std::uint8_t> invalidAck =
      EncodeAckPayload(static_cast<AckStatus>(99), 0);
  AckStatus status = AckStatus::Ok;
  std::uint32_t detail = 0;
  CHECK(!DecodeAckPayload(invalidAck, &status, &detail));
}

void TestGoldenStatusAndBusInfo() {
  using namespace ec_systemcore::protocol;
  StatusPayload status{
      .aggregateState = static_cast<std::uint8_t>(BusState::Operational),
      .flags = 0x7f,
      .activeAdapters = 2,
      .subDevices = 3,
      .activeFaults = 1,
      .maximumJitterUs = 0x11223344U,
      .currentJitterUs = 0x55667788U,
      .lostFrames = 0x0102030405060708ULL,
      .cycleOverruns = 0x1112131415161718ULL,
      .epoch = 0x2122232425262728ULL,
      .controllerPid = 0x31323334U,
      .configuredBuses = 2,
      .schedulingMode = SchedulingMode::Fifo,
      .realtimeErrorBits = 0x41,
      .monotonicTimestampUs = 0x4142434445464748ULL,
  };
  const auto statusFrame =
      EncodeFrame(MessageType::Status, 0, EncodeStatusPayload(status));
  CHECK(Hex(statusFrame) ==
        "44000000454353430181000000000000047f020003000100443322118877"
        "665508070605040302011817161514131211282726252423222134333231"
        "020001414847464544434241");

  BusInfoPayload information{
      .busIndex = 1,
      .state = BusState::Operational,
      .linkUp = true,
      .lockState = AdapterLockState::Matched,
      .lockReason = AdapterLockReason::None,
      .subDeviceCount = 2,
      .logicalName = "drive",
      .physicalInterface = "eth9",
      .idPath = "port-A",
      .permanentMac = "02:11:22:33:44:55",
      .usbSerial = "S1",
      .usbVendorId = "1d6b",
      .usbProductId = "0002",
      .epoch = 0x0102030405060708ULL,
  };
  const auto busFrame =
      EncodeFrame(MessageType::BusInfo, 0,
                  EncodeBusInfoPayload(information));
  CHECK(Hex(busFrame) ==
        "540000004543534301840000000000000100040101000200050004000600"
        "11000200040004000807060504030201647269766565746839706f72742d"
        "4130323a31313a32323a33333a34343a353553313164366230303032");
  FrameDecoder decoder;
  std::vector<Frame> decodedFrames;
  CHECK(decoder.Feed(busFrame, &decodedFrames));
  CHECK(decodedFrames.size() == 1);
  BusInfoPayload decoded;
  CHECK(DecodeBusInfoPayload(decodedFrames[0].payload, &decoded));
  CHECK(decoded.idPath == "port-A");
  CHECK(decoded.permanentMac == "02:11:22:33:44:55");

  std::vector<std::uint8_t> malformed =
      EncodeStatusPayload(status);
  malformed[0] = 0xff;
  StatusPayload rejected;
  CHECK(!DecodeStatusPayload(malformed, &rejected));
  malformed = EncodeStatusPayload(status);
  std::fill(malformed.begin() + 32, malformed.begin() + 40, 0);
  CHECK(!DecodeStatusPayload(malformed, &rejected));
  malformed = EncodeStatusPayload(status);
  malformed[46] = 0xff;
  CHECK(!DecodeStatusPayload(malformed, &rejected));
  malformed = EncodeStatusPayload(status);
  malformed[47] = 0x80;
  CHECK(!DecodeStatusPayload(malformed, &rejected));

  StatusPayload invalid = status;
  invalid.aggregateState = 0xff;
  CHECK(EncodeStatusPayload(invalid).empty());
  invalid = status;
  invalid.epoch = 0;
  CHECK(EncodeStatusPayload(invalid).empty());
  invalid = status;
  invalid.schedulingMode = static_cast<SchedulingMode>(0xff);
  CHECK(EncodeStatusPayload(invalid).empty());
  invalid = status;
  invalid.realtimeErrorBits = 0x80;
  CHECK(EncodeStatusPayload(invalid).empty());
  CHECK(EncodeAckPayload(static_cast<AckStatus>(0xffff)).empty());
}

void TestPdoBounds() {
  using namespace ec_systemcore::protocol;
  PdoInputPayload input{
      .busIndex = 0,
      .subDeviceIndex = 1,
      .offset = 1020,
      .totalSize = 1024,
      .epoch = 9,
      .cycleSequence = 10,
      .data = {1, 2, 3, 4},
  };
  const auto encoded = EncodePdoInputPayload(input);
  CHECK(!encoded.empty());
  PdoInputPayload decoded;
  CHECK(DecodePdoInputPayload(encoded, &decoded, 1024));
  CHECK(decoded.data == input.data);

  input.offset = 1023;
  CHECK(EncodePdoInputPayload(input).empty());
  input.offset = 0;
  input.epoch = 0;
  CHECK(EncodePdoInputPayload(input).empty());
  input.epoch = 9;
  input.cycleSequence = 0;
  CHECK(EncodePdoInputPayload(input).empty());
  input.cycleSequence = 10;
  input.data.clear();
  CHECK(EncodePdoInputPayload(input).empty());
}

void TestJsonStrictness() {
  using ec_systemcore::Json;
  const Json exact = Json::Parse(
      R"({"large":9007199254740993,"maximum":9223372036854775807})");
  CHECK(exact.integerValue("large") == 9007199254740993LL);
  CHECK(exact.integerValue("maximum") ==
        std::numeric_limits<std::int64_t>::max());
  CheckThrows([] {
    static_cast<void>(
        Json::Parse(R"({"value":1e2})").integerValue("value"));
  });
  CheckThrows([] { Json::Parse(std::string{"[\v]"}); });
  CheckThrows([] {
    Json::Parse(R"("\u20ac")",
                Json::Limits{.maximumBytes = 32,
                             .maximumDepth = 2,
                             .maximumNodes = 2,
                             .maximumStringBytes = 1});
  });
  CheckThrows([] { Json::Parse(R"({"a":1,"a":2})"); });
  CheckThrows([] {
    Json::Parse(std::string{"\"\xc0\xaf\""});
  });
  CheckThrows([] {
    static_cast<void>(
        Json::Parse(R"({"x":"wrong"})").integerValue("x"));
  });
}

void TestSafetyGate() {
  using ec_systemcore::SafetyGate;
  SafetyGate gate{77, 250ms, 100ms};
  const auto start = SafetyGate::TimePoint{} + 1s;
  CHECK(gate.Acquire(1, 42, start).status == AckStatus::Ok);
  CHECK(gate.SetOutputEnabled(1, 77, true, true, start).status ==
        AckStatus::Ok);
  CHECK(gate.RecordOutputWrite(1, 77, start + 10ms).status ==
        AckStatus::Ok);
  CHECK(!gate.ExpireOutputCommands(start + 100ms).epochChanged);
  CHECK(gate.ExpireOutputCommands(start + 111ms).epochChanged);
  CHECK(gate.epoch() == 78);
  CHECK(!gate.outputsEnabled());
  CHECK(gate.AuthorizeWrite(1, 77).status ==
        AckStatus::StaleEpoch);

  CHECK(gate.Acquire(2, 43, start + 120ms).status == AckStatus::Ok);
  CHECK(gate.SetOutputEnabled(2, 78, true, true, start + 120ms).status ==
        AckStatus::Ok);
  const auto disabled =
      gate.SetOutputEnabled(2, 78, false, true, start + 121ms);
  CHECK(disabled.epochChanged);
  CHECK(gate.epoch() == 79);
  CHECK(!gate.hasController());
  CHECK(gate.AuthorizeWrite(2, 78).status ==
        AckStatus::StaleEpoch);

  CHECK(gate.Acquire(3, 44, start + 130ms).status == AckStatus::Ok);
  CHECK(gate.Expire(start + 381ms).epochChanged);
  CHECK(gate.epoch() == 80);
}

void TestWholeImageOutputSafety() {
  using ec_systemcore::HasCanonicalPdoPadding;
  using ec_systemcore::IsValidSoemPdoMetadata;
  using ec_systemcore::PdoImageByteLength;
  using ec_systemcore::PdoMappedByteLength;
  using ec_systemcore::ReadPackedPdoChunk;
  using ec_systemcore::SafetyGate;
  using ec_systemcore::ValidateWholeOutputWrite;
  using ec_systemcore::WritePackedPdoImage;
  using ec_systemcore::ZeroPackedPdoImage;

  const ec_systemcore::ControllerRateLimit typicalRate =
      ec_systemcore::CalculateControllerRateLimit(199, 100ms);
  CHECK(typicalRate.burstFrames >= 199);
  CHECK(typicalRate.framesPerSecond >= 1990);
  const ec_systemcore::ControllerRateLimit maximumRate =
      ec_systemcore::CalculateControllerRateLimit(8U * 199U, 20ms);
  CHECK(maximumRate.burstFrames >= 8U * 199U);
  CHECK(maximumRate.framesPerSecond >= 8U * 199U * 50U);
  CHECK(maximumRate.framesPerSecond <= 200000);

  CHECK(PdoImageByteLength(7) == 1);
  CHECK(PdoMappedByteLength(7, 7) == 2);
  CHECK(IsValidSoemPdoMetadata(7, 0, 7));
  CHECK(IsValidSoemPdoMetadata(9, 2, 0));
  CHECK(IsValidSoemPdoMetadata(0, 0, 0));
  CHECK(!IsValidSoemPdoMetadata(0, 0, 1));
  CHECK(!IsValidSoemPdoMetadata(9, 2, 1));
  CHECK(!IsValidSoemPdoMetadata(7, 1, 0));

  // A sub-byte output may share both boundary bytes with other SubDevices.
  // Writes and zeroing must alter only its seven mapped bits.
  std::array<std::uint8_t, 2> mapped{0xaa, 0x55};
  const std::array<std::uint8_t, 1> packed{0x55};
  CHECK(HasCanonicalPdoPadding(7, packed));
  CHECK(!HasCanonicalPdoPadding(
      7, std::array<std::uint8_t, 1>{0xd5}));
  CHECK(WritePackedPdoImage(mapped, 7, 7, packed));
  CHECK(mapped[0] == 0xaa);
  CHECK(mapped[1] == 0x6a);
  std::array<std::uint8_t, 1> roundTrip{};
  CHECK(ReadPackedPdoChunk(mapped, 7, 7, 0, roundTrip));
  CHECK(roundTrip == packed);
  CHECK(ZeroPackedPdoImage(mapped, 7, 7));
  CHECK(mapped[0] == 0x2a);
  CHECK(mapped[1] == 0x40);

  // The byte-oriented fast path canonicalizes unused high input bits.
  const std::array<std::uint8_t, 2> nineBitMapped{0x5a, 0xff};
  std::array<std::uint8_t, 2> nineBitPacked{};
  CHECK(ReadPackedPdoChunk(nineBitMapped, 0, 9, 0, nineBitPacked));
  CHECK(nineBitPacked[0] == 0x5a);
  CHECK(nineBitPacked[1] == 0x01);

  std::array<std::uint8_t, 4> output{};
  SafetyGate gate{100, 250ms, 100ms};
  const auto start = SafetyGate::TimePoint{} + 1s;
  CHECK(gate.Acquire(1, 42, start).status == AckStatus::Ok);
  CHECK(gate.SetOutputEnabled(1, 100, true, true, start).status ==
        AckStatus::Ok);
  CHECK(ValidateWholeOutputWrite(output.size(), 0, output.size()) ==
        AckStatus::Ok);
  output.fill(0xa5);
  CHECK(gate.RecordOutputWrite(1, 100, start).status ==
        AckStatus::Ok);

  // Repeated one-byte updates are rejected and therefore cannot keep stale
  // bytes alive by refreshing the output-command watchdog.
  for (int elapsed = 20; elapsed <= 100; elapsed += 20) {
    CHECK(ValidateWholeOutputWrite(output.size(), 0, 1) ==
          AckStatus::Bounds);
    CHECK(!gate.ExpireOutputCommands(
                   start + std::chrono::milliseconds{elapsed})
               .epochChanged);
  }
  const auto expired =
      gate.ExpireOutputCommands(start + 101ms);
  CHECK(expired.epochChanged);
  if (expired.epochChanged) {
    output.fill(0);
  }
  CHECK(std::all_of(output.begin(), output.end(),
                    [](std::uint8_t byte) { return byte == 0; }));
}

void TestRealtimeFailures() {
  using namespace ec_systemcore;
  RealtimeConfig configuration{
      .requestMemoryLock = true,
      .requestFifo = true,
      .fifoPriority = 55,
      .cpuAffinity = 4,
  };
  RealtimeOperations operations{
      .lockMemory = [] { return -1; },
      .setFifoScheduler = [](int) { return -1; },
      .setCpuAffinity = [](unsigned int) { return -1; },
      .availableCpuCount = [] { return 4U; },
      .currentSchedulingPolicy = [] { return 0; },
  };
  const RealtimeResult result =
      ApplyRealtimeConfiguration(configuration, operations);
  CHECK((result.errorBits & RealtimeMlockFailed) != 0);
  CHECK((result.errorBits & RealtimeSchedulerFailed) != 0);
  CHECK((result.errorBits &
         RealtimeAffinityConfigurationInvalid) != 0);
  CHECK(!result.fullyApplied(configuration));
}

void TestDistributedClock() {
  using ec_systemcore::DistributedClockController;
  DistributedClockController controller{5000000};
  ec_systemcore::DistributedClockAdjustment adjustment;
  for (std::int64_t cycle = 1; cycle <= 25; ++cycle) {
    adjustment = controller.Update(cycle * 5000000);
  }
  CHECK(adjustment.synchronized);
  CHECK(!adjustment.fault);
  CHECK(std::abs(adjustment.correctionNs) <= 500000);

  controller.Reset();
  CHECK(!controller.Update(5000000).fault);
  CHECK(!controller.Update(5000000).fault);
  CHECK(controller.Update(5000000).fault);

  controller.Reset();
  static_cast<void>(controller.Update(5000000));
  static_cast<void>(controller.Update(10000000));
  static_cast<void>(controller.Update(9000000));
  static_cast<void>(controller.Update(9000000));
  CHECK(controller.Update(9000000).fault);

  DistributedClockController extreme{
      std::numeric_limits<std::int64_t>::max()};
  CHECK(!extreme.Update(
             std::numeric_limits<std::int64_t>::max() - 1)
             .synchronized);
}

void TestIdentityRules() {
  using namespace ec_systemcore;
  CHECK(NormalizeMac("02-11-22-33-44-55") ==
        std::optional<std::string>{"02:11:22:33:44:55"});
  CHECK(!NormalizeMac("01:11:22:33:44:55").has_value());
  CHECK(!NormalizeMac("00:00:00:00:00:00").has_value());
  CHECK(!NormalizeMac("ff:ff:ff:ff:ff:ff").has_value());
  AdapterIdentityLock invalidLock;
  invalidLock.idPath = "bad\npath";
  CHECK(!ValidateAdapterLock(invalidLock, nullptr));

  AdapterIdentity first;
  first.interfaceName = "eth1";
  first.idPath = "duplicate";
  first.permanentMac = "02:11:22:33:44:55";
  first.ethernet = true;
  AdapterIdentity second = first;
  second.interfaceName = "eth2";
  AdapterIdentityLock duplicateLock;
  duplicateLock.idPath = "duplicate";
  const AdapterResolution ambiguous =
      ResolveAdapter({first, second}, "eth1", duplicateLock);
  CHECK(ambiguous.state ==
        ec_systemcore::protocol::AdapterLockState::Mismatch);
  CHECK(!ambiguous.identity.has_value());
}

}  // namespace

int main() {
  try {
    TestProtocolFraming();
    TestGoldenStatusAndBusInfo();
    TestPdoBounds();
    TestJsonStrictness();
    TestSafetyGate();
    TestWholeImageOutputSafety();
    TestRealtimeFailures();
    TestDistributedClock();
    TestIdentityRules();
    std::cout << "all ec-systemcore unit tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
