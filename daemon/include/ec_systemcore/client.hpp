#pragma once

#include "ec_systemcore/protocol.hpp"

#include <chrono>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ec_systemcore {

enum class ClientError {
  None,
  InvalidArgument,
  NotConnected,
  ControllerDenied,
  SocketError,
  ProtocolError,
  Timeout,
  ServerRejected,
  OutOfMemory,
};

struct ClientOptions {
  std::string socketPath{"/run/ec-systemcore/ec-systemcore.sock"};
  protocol::ClientRole role = protocol::ClientRole::Controller;
  bool subscribeInputs = false;
  std::uint16_t inputPeriodMs = 100;
  std::chrono::milliseconds connectTimeout{500};
  std::chrono::milliseconds requestTimeout{250};
  std::chrono::milliseconds initialReconnectBackoff{50};
  std::chrono::milliseconds maximumReconnectBackoff{1000};
  std::chrono::milliseconds heartbeatPeriod{50};
  bool backgroundReconnect = true;
};

struct ClientErrorSnapshot {
  ClientError error = ClientError::None;
  protocol::AckStatus serverStatus = protocol::AckStatus::Ok;
  std::array<char, 256> message{};
};

struct PdoImage {
  std::uint16_t busIndex = 0;
  std::uint16_t subDeviceIndex = 0;
  std::uint64_t epoch = 0;
  std::uint64_t cycleSequence = 0;
  std::vector<std::uint8_t> data;
};

// Thread-safe, no-throw client. With backgroundReconnect=true it owns one
// worker thread that reconnects, polls, and heartbeats. No user callback is
// invoked by that thread; callers drain bounded queues. Writes are never
// retained across a disconnect or epoch change. Destroying or Disconnecting
// joins the worker and best-effort disables/releases control first.
class Client final {
 public:
  explicit Client(ClientOptions options = {}) noexcept;
  ~Client();

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  Client(Client&& other) noexcept;
  Client& operator=(Client&& other) noexcept;

  [[nodiscard]] bool Connect() noexcept;
  [[nodiscard]] bool MaintainConnection() noexcept;
  [[nodiscard]] bool Poll(std::chrono::milliseconds timeout =
                              std::chrono::milliseconds{0}) noexcept;
  void Disconnect() noexcept;

  [[nodiscard]] bool Heartbeat() noexcept;
  [[nodiscard]] bool EnableOutputs(bool enabled) noexcept;
  [[nodiscard]] bool WriteOutput(
      std::uint16_t busIndex, std::uint16_t subDeviceIndex,
      std::uint32_t offset, std::span<const std::uint8_t> data) noexcept;
  [[nodiscard]] bool ReleaseControl() noexcept;
  [[nodiscard]] bool SubscribeInputs(bool enabled,
                                     std::uint16_t periodMs) noexcept;
  [[nodiscard]] bool ClearCounters() noexcept;
  [[nodiscard]] bool RescanAdapters() noexcept;
  [[nodiscard]] bool RuntimeUnlockAdapter(
      std::uint16_t busIndex) noexcept;

  [[nodiscard]] bool PopInput(
      protocol::PdoInputPayload* input) noexcept;
  [[nodiscard]] bool PopInputImage(PdoImage* image) noexcept;
  [[nodiscard]] bool PopBusInfo(
      protocol::BusInfoPayload* information) noexcept;

  [[nodiscard]] bool connected() const noexcept;
  [[nodiscard]] bool controllerGranted() const noexcept;
  [[nodiscard]] bool outputsEnabled() const noexcept;
  [[nodiscard]] std::uint64_t epoch() const noexcept;
  [[nodiscard]] protocol::StatusPayload status() const noexcept;
  [[nodiscard]] ClientError lastError() const noexcept;
  [[nodiscard]] protocol::AckStatus lastServerStatus() const noexcept;
  [[nodiscard]] ClientErrorSnapshot errorSnapshot() const noexcept;

 private:
  class Implementation;
  Implementation* implementation_ = nullptr;
};

}  // namespace ec_systemcore
