#include "ec_systemcore/adapter_identity.hpp"
#include "ec_systemcore/dc_sync.hpp"
#include "ec_systemcore/mini_json.hpp"
#include "ec_systemcore/protocol.hpp"
#include "ec_systemcore/realtime.hpp"
#include "ec_systemcore/safety.hpp"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include <soem/soem.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace ec_systemcore {
namespace {

constexpr std::string_view kDefaultConfigPath =
    "/etc/ec-systemcore/ec-systemcore.json";
constexpr std::string_view kDefaultSocketPath =
    "/run/ec-systemcore/ec-systemcore.sock";
constexpr std::string_view kDefaultLogDirectory =
    "/var/log/ec-systemcore";
constexpr std::int64_t kMinimumCyclePeriodUs = 5000;
constexpr std::int64_t kMaximumCyclePeriodUs = 1000000;
constexpr std::size_t kMaximumBuses = 8;
constexpr std::size_t kMaximumClients = 16;
constexpr std::size_t kMaximumClientOutputBytes = 2U * 1024U * 1024U;
constexpr std::size_t kMaximumConfigurationBytes = 1024U * 1024U;
constexpr std::size_t kMaximumLogFileBytes = 10U * 1024U * 1024U;
constexpr std::size_t kMaximumOutputCommands = 512;
constexpr std::size_t kMaximumInputChunks = 256;
constexpr std::uint32_t kCapabilities =
    protocol::CapabilityOutputs | protocol::CapabilityInputs |
    protocol::CapabilityAdapterIdentity |
    protocol::CapabilityDistributedClock;

volatile sig_atomic_t gRunning = 1;

void SignalHandler(int) {
  gRunning = 0;
}

[[nodiscard]] std::uint64_t MonotonicMicros() {
  timespec now{};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 ||
      now.tv_nsec < 0) {
    return 0;
  }
  return static_cast<std::uint64_t>(now.tv_sec) * 1000000ULL +
         static_cast<std::uint64_t>(now.tv_nsec / 1000L);
}

[[nodiscard]] std::uint64_t InitialEpoch() {
  std::uint64_t randomValue = 0;
  const ssize_t randomBytes =
      getrandom(&randomValue, sizeof(randomValue), GRND_NONBLOCK);
  if (randomBytes != static_cast<ssize_t>(sizeof(randomValue))) {
    timespec bootTime{};
    clock_gettime(CLOCK_BOOTTIME, &bootTime);
    randomValue =
        (static_cast<std::uint64_t>(bootTime.tv_sec) << 32U) ^
        static_cast<std::uint64_t>(bootTime.tv_nsec) ^
        (static_cast<std::uint64_t>(getpid()) << 16U) ^
        reinterpret_cast<std::uintptr_t>(&randomValue);
  }
  return randomValue == 0 ? 1 : randomValue;
}

[[nodiscard]] std::string ReadBoundedFile(
    const std::filesystem::path& path,
    std::size_t maximumBytes = kMaximumConfigurationBytes) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("unable to open " + path.string());
  }
  std::string result;
  std::array<char, 8192> buffer{};
  while (stream) {
    stream.read(buffer.data(),
                static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = stream.gcount();
    if (count <= 0) {
      break;
    }
    if (static_cast<std::size_t>(count) >
        maximumBytes - std::min(result.size(), maximumBytes)) {
      throw std::runtime_error(path.string() + " exceeds the size limit");
    }
    result.append(buffer.data(), static_cast<std::size_t>(count));
  }
  if (!stream.eof() && stream.fail()) {
    throw std::runtime_error("unable to read " + path.string());
  }
  return result;
}

[[nodiscard]] bool IsSafeAbsolutePath(std::string_view path) {
  if (path.empty() || path.front() != '/' || path.size() > 4096 ||
      path.find('\0') != std::string_view::npos) {
    return false;
  }
  std::filesystem::path parsed{path};
  if (!parsed.is_absolute()) {
    return false;
  }
  for (const auto& component : parsed) {
    if (component == "..") {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool IsValidLogicalName(std::string_view name) {
  if (name.empty() || name.size() > 32) {
    return false;
  }
  return std::all_of(name.begin(), name.end(), [](char character) {
    const auto byte = static_cast<unsigned char>(character);
    return std::isalnum(byte) != 0 || character == '_' || character == '-';
  });
}

[[nodiscard]] bool IsValidInterfaceName(std::string_view name) {
  if (name.empty() || name.size() >= IFNAMSIZ) {
    return false;
  }
  return std::all_of(name.begin(), name.end(), [](char character) {
    const auto byte = static_cast<unsigned char>(character);
    return std::isalnum(byte) != 0 || character == '_' || character == '-' ||
           character == '.' || character == ':';
  });
}

[[nodiscard]] bool IsRestrictedInterface(std::string_view name) {
  return name == "eth0" || name == "wlan0" || name == "usb0";
}

struct ExpectedSubDevice {
  std::uint32_t vendorId = 0;
  std::uint32_t productCode = 0;
  std::optional<std::uint32_t> revision;
  std::optional<std::uint32_t> outputBytes;
  std::optional<std::uint32_t> inputBytes;
};

struct InterfaceConfig {
  std::string logicalName;
  std::string physicalInterface;
  bool enabled = true;
  std::optional<AdapterIdentityLock> lock;
  std::vector<ExpectedSubDevice> expectedSubDevices;
  std::size_t maximumIoMapBytes = 256U * 1024U;
  bool distributedClock = false;
  std::int32_t distributedClockShiftNs = 0;
  bool allowUnverifiedTopology = false;
};

struct DaemonConfig {
  std::vector<InterfaceConfig> interfaces;
  bool allowRestrictedInterfaces = false;
  std::chrono::microseconds cyclePeriod{5000};
  std::chrono::milliseconds heartbeatTimeout{250};
  std::chrono::milliseconds outputCommandTimeout{100};
  std::string logDirectory{std::string{kDefaultLogDirectory}};
  std::size_t logCountLimit = 10;
  std::uint64_t freeSpaceThresholdBytes = 50ULL * 1024ULL * 1024ULL;
  RealtimeConfig realtime;
  std::vector<uid_t> controllerUids{0};
  std::vector<gid_t> controllerGids;
  std::string controllerGroup{"ec-systemcore-controller"};
};

[[nodiscard]] std::uint32_t CheckedUint32(
    const Json& object, std::string_view key,
    std::optional<std::uint32_t> fallback = std::nullopt) {
  const Json* value = object.find(key);
  if (value == nullptr) {
    if (fallback.has_value()) {
      return *fallback;
    }
    throw std::runtime_error("missing required integer field '" +
                             std::string{key} + "'");
  }
  const std::int64_t integer = object.integerValue(key);
  if (integer < 0 ||
      integer > static_cast<std::int64_t>(
                    std::numeric_limits<std::uint32_t>::max())) {
    throw std::runtime_error("field '" + std::string{key} +
                             "' is outside the uint32 range");
  }
  return static_cast<std::uint32_t>(integer);
}

void RejectUnknownKeys(
    const Json& object,
    std::initializer_list<std::string_view> accepted,
    std::string_view context) {
  if (!object.isObject()) {
    throw std::runtime_error(std::string{context} +
                             " must be an object");
  }
  for (const auto& [key, value] : object.object()) {
    static_cast<void>(value);
    if (std::find(accepted.begin(), accepted.end(), key) ==
        accepted.end()) {
      throw std::runtime_error("unknown " + std::string{context} +
                               " field '" + key + "'");
    }
  }
}

[[nodiscard]] std::optional<std::string> OptionalString(
    const Json& object, std::string_view key) {
  const Json* value = object.find(key);
  if (value == nullptr) {
    return std::nullopt;
  }
  if (!value->isString()) {
    throw std::runtime_error("field '" + std::string{key} +
                             "' must be a string");
  }
  if (value->string().empty()) {
    throw std::runtime_error("field '" + std::string{key} +
                             "' cannot be empty");
  }
  return value->string();
}

[[nodiscard]] DaemonConfig LoadConfigText(std::string_view text) {
  const Json root =
      Json::Parse(std::string{text},
                  Json::Limits{.maximumBytes = kMaximumConfigurationBytes,
                               .maximumDepth = 32,
                               .maximumNodes = 8192,
                               .maximumStringBytes = 4096});
  if (!root.isObject()) {
    throw std::runtime_error("configuration root must be an object");
  }
  RejectUnknownKeys(
      root,
      {"allow_restricted_interfaces", "cycle_period_us",
       "heartbeat_timeout_ms", "output_command_timeout_ms",
       "log_directory", "log_count_limit",
       "free_space_threshold_mb", "realtime_memory_lock",
       "realtime_fifo", "realtime_priority", "realtime_cpu",
       "controller_group", "controller_uids", "controller_gids",
       "interface_mappings", "nt4_server", "nt4_team"},
      "configuration");
  if (root.find("nt4_server") != nullptr ||
      root.find("nt4_team") != nullptr) {
    throw std::runtime_error(
        "nt4_server/nt4_team are unsupported; use the "
        "ec-systemcore client IPC API");
  }

  DaemonConfig config;
  config.allowRestrictedInterfaces =
      root.boolValue("allow_restricted_interfaces", false);
  const std::int64_t cyclePeriod =
      root.integerValue("cycle_period_us", kMinimumCyclePeriodUs);
  if (cyclePeriod < kMinimumCyclePeriodUs ||
      cyclePeriod > kMaximumCyclePeriodUs) {
    throw std::runtime_error(
        "cycle_period_us must be between 5000 and 1000000");
  }
  config.cyclePeriod = std::chrono::microseconds{cyclePeriod};

  const std::int64_t heartbeat =
      root.integerValue("heartbeat_timeout_ms", 250);
  if (heartbeat < 100 || heartbeat > 5000) {
    throw std::runtime_error(
        "heartbeat_timeout_ms must be between 100 and 5000");
  }
  config.heartbeatTimeout = std::chrono::milliseconds{heartbeat};
  const std::int64_t outputCommandTimeout =
      root.integerValue("output_command_timeout_ms", 100);
  if (outputCommandTimeout < 20 ||
      outputCommandTimeout > heartbeat) {
    throw std::runtime_error(
        "output_command_timeout_ms must be between 20 and "
        "heartbeat_timeout_ms");
  }
  if (outputCommandTimeout * 1000 < cyclePeriod * 2) {
    throw std::runtime_error(
        "output_command_timeout_ms must cover at least two "
        "cycle_period_us intervals");
  }
  config.outputCommandTimeout =
      std::chrono::milliseconds{outputCommandTimeout};
  config.logDirectory =
      root.stringValue("log_directory", std::string{kDefaultLogDirectory});
  if (config.logDirectory != kDefaultLogDirectory) {
    throw std::runtime_error(
        "log_directory is fixed at /var/log/ec-systemcore");
  }
  const std::int64_t logCount = root.integerValue("log_count_limit", 10);
  if (logCount < 1 || logCount > 1000) {
    throw std::runtime_error("log_count_limit must be between 1 and 1000");
  }
  config.logCountLimit = static_cast<std::size_t>(logCount);
  const std::int64_t freeMegabytes =
      root.integerValue("free_space_threshold_mb", 50);
  if (freeMegabytes < 0 || freeMegabytes > 1024 * 1024) {
    throw std::runtime_error(
        "free_space_threshold_mb is outside the accepted range");
  }
  config.freeSpaceThresholdBytes =
      static_cast<std::uint64_t>(freeMegabytes) * 1024ULL * 1024ULL;

  config.realtime.requestMemoryLock =
      root.boolValue("realtime_memory_lock", true);
  config.realtime.requestFifo =
      root.boolValue("realtime_fifo", true);
  const std::int64_t priority =
      root.integerValue("realtime_priority", 55);
  if (priority < std::numeric_limits<int>::min() ||
      priority > std::numeric_limits<int>::max()) {
    throw std::runtime_error("realtime_priority is outside the int range");
  }
  config.realtime.fifoPriority = static_cast<int>(priority);
  if (root.find("realtime_cpu") != nullptr) {
    const std::int64_t cpu = root.integerValue("realtime_cpu");
    if (cpu < 0 ||
        cpu > static_cast<std::int64_t>(
                  std::numeric_limits<unsigned int>::max())) {
      throw std::runtime_error("realtime_cpu is outside the accepted range");
    }
    config.realtime.cpuAffinity = static_cast<unsigned int>(cpu);
  }
  config.controllerGroup =
      root.stringValue("controller_group", "ec-systemcore-controller");
  if (config.controllerGroup.empty() ||
      config.controllerGroup.size() > 255 ||
      !std::all_of(config.controllerGroup.begin(),
                   config.controllerGroup.end(), [](char character) {
                     const auto byte =
                         static_cast<unsigned char>(character);
                     return std::isalnum(byte) != 0 ||
                            character == '_' || character == '-';
                   })) {
    throw std::runtime_error("controller_group is invalid");
  }
  const auto parsePeerIds = [&root](std::string_view key,
                                    auto* destination) {
    const Json* values = root.find(key);
    if (values == nullptr) {
      return;
    }
    if (!values->isArray() || values->array().size() > 64) {
      throw std::runtime_error(std::string{key} +
                               " must be a bounded array");
    }
    for (const Json& value : values->array()) {
      const std::int64_t integer = value.integer();
      using Id = typename std::remove_reference_t<
          decltype(*destination)>::value_type;
      if (integer < 0 ||
          static_cast<std::uint64_t>(integer) >
              static_cast<std::uint64_t>(
                  std::numeric_limits<Id>::max())) {
        throw std::runtime_error(std::string{key} +
                                 " contains an invalid ID");
      }
      destination->push_back(static_cast<Id>(integer));
    }
    std::sort(destination->begin(), destination->end());
    destination->erase(
        std::unique(destination->begin(), destination->end()),
        destination->end());
  };
  parsePeerIds("controller_uids", &config.controllerUids);
  parsePeerIds("controller_gids", &config.controllerGids);

  const Json* interfaces = root.find("interface_mappings");
  if (interfaces == nullptr || !interfaces->isArray()) {
    throw std::runtime_error(
        "configuration must contain an interface_mappings array");
  }
  if (interfaces->array().size() > kMaximumBuses) {
    throw std::runtime_error("too many configured EtherCAT buses");
  }

  std::set<std::string> logicalNames;
  std::set<std::string> physicalNames;
  for (const Json& entry : interfaces->array()) {
    if (!entry.isObject()) {
      throw std::runtime_error("each interface mapping must be an object");
    }
    RejectUnknownKeys(
        entry,
        {"logical_name", "physical_interface", "enabled",
         "maximum_io_map_bytes", "distributed_clock",
         "distributed_clock_shift_ns", "allow_unverified_topology",
         "lock", "expected_subdevices"},
        "interface mapping");
    InterfaceConfig interface;
    interface.logicalName = entry.stringValue("logical_name");
    interface.physicalInterface =
        entry.stringValue("physical_interface");
    interface.enabled = entry.boolValue("enabled", true);
    if (!IsValidLogicalName(interface.logicalName)) {
      throw std::runtime_error("invalid logical_name '" +
                               interface.logicalName + "'");
    }
    if (!IsValidInterfaceName(interface.physicalInterface)) {
      throw std::runtime_error("invalid physical_interface '" +
                               interface.physicalInterface + "'");
    }
    if (!config.allowRestrictedInterfaces &&
        IsRestrictedInterface(interface.physicalInterface)) {
      throw std::runtime_error(
          "restricted interface requires allow_restricted_interfaces: " +
          interface.physicalInterface);
    }
    if (!logicalNames.insert(interface.logicalName).second ||
        !physicalNames.insert(interface.physicalInterface).second) {
      throw std::runtime_error(
          "logical and physical interface names must be unique");
    }

    const std::int64_t ioMapLimit =
        entry.integerValue("maximum_io_map_bytes", 256 * 1024);
    if (ioMapLimit < 1024 ||
        ioMapLimit >
            static_cast<std::int64_t>(protocol::kMaximumPdoImage)) {
      throw std::runtime_error(
          "maximum_io_map_bytes must be between 1024 and 1048576");
    }
    interface.maximumIoMapBytes = static_cast<std::size_t>(ioMapLimit);
    interface.distributedClock =
        entry.boolValue("distributed_clock", false);
    interface.allowUnverifiedTopology =
        entry.boolValue("allow_unverified_topology", false);
    const std::int64_t dcShift =
        entry.integerValue("distributed_clock_shift_ns", 0);
    if (dcShift < std::numeric_limits<std::int32_t>::min() ||
        dcShift > std::numeric_limits<std::int32_t>::max()) {
      throw std::runtime_error(
          "distributed_clock_shift_ns is outside the int32 range");
    }
    interface.distributedClockShiftNs =
        static_cast<std::int32_t>(dcShift);

    if (const Json* lock = entry.find("lock"); lock != nullptr) {
      if (!lock->isObject()) {
        throw std::runtime_error("adapter lock must be an object");
      }
      RejectUnknownKeys(
          *lock,
          {"id_path", "permanent_mac", "usb_serial",
           "usb_vendor_id", "usb_product_id"},
          "adapter lock");
      AdapterIdentityLock parsed;
      parsed.idPath = lock->stringValue("id_path");
      parsed.permanentMac = OptionalString(*lock, "permanent_mac");
      parsed.usbSerial = OptionalString(*lock, "usb_serial");
      parsed.usbVendorId = OptionalString(*lock, "usb_vendor_id");
      parsed.usbProductId = OptionalString(*lock, "usb_product_id");
      std::string lockError;
      if (!ValidateAdapterLock(parsed, &lockError)) {
        throw std::runtime_error(lockError);
      }
      if (parsed.permanentMac.has_value()) {
        parsed.permanentMac = NormalizeMac(*parsed.permanentMac);
      }
      if (parsed.usbVendorId.has_value()) {
        parsed.usbVendorId = NormalizeUsbId(*parsed.usbVendorId);
      }
      if (parsed.usbProductId.has_value()) {
        parsed.usbProductId = NormalizeUsbId(*parsed.usbProductId);
      }
      interface.lock = std::move(parsed);
    }

    if (const Json* topology = entry.find("expected_subdevices");
        topology != nullptr) {
      if (!topology->isArray() ||
          topology->array().size() >= EC_MAXSLAVE) {
        throw std::runtime_error(
            "expected_subdevices must be a bounded array");
      }
      for (const Json& expected : topology->array()) {
        if (!expected.isObject()) {
          throw std::runtime_error(
              "each expected_subdevices entry must be an object");
        }
        RejectUnknownKeys(
            expected,
            {"vendor_id", "product_code", "revision",
             "output_bytes", "input_bytes"},
            "expected SubDevice");
        ExpectedSubDevice subDevice;
        subDevice.vendorId = CheckedUint32(expected, "vendor_id");
        subDevice.productCode =
            CheckedUint32(expected, "product_code");
        if (expected.find("revision") != nullptr) {
          subDevice.revision = CheckedUint32(expected, "revision");
        }
        if (expected.find("output_bytes") != nullptr) {
          subDevice.outputBytes =
              CheckedUint32(expected, "output_bytes");
          if (*subDevice.outputBytes >
              protocol::kMaximumOutputWrite) {
            throw std::runtime_error(
                "expected SubDevice output_bytes exceeds the atomic "
                "1024-byte write bound");
          }
        }
        if (expected.find("input_bytes") != nullptr) {
          subDevice.inputBytes =
              CheckedUint32(expected, "input_bytes");
        }
        interface.expectedSubDevices.emplace_back(std::move(subDevice));
      }
    }
    if (interface.enabled) {
      config.interfaces.emplace_back(std::move(interface));
    }
  }
  return config;
}

[[nodiscard]] DaemonConfig LoadConfig(
    const std::filesystem::path& path, std::string* sourceText) {
  std::string text = ReadBoundedFile(path);
  DaemonConfig config = LoadConfigText(text);
  if (sourceText != nullptr) {
    *sourceText = std::move(text);
  }
  return config;
}

class EventLog final {
 public:
  EventLog(std::filesystem::path directory, std::size_t countLimit,
           std::uint64_t freeSpaceThreshold)
      : directory_(std::move(directory)),
        countLimit_(countLimit),
        freeSpaceThreshold_(freeSpaceThreshold) {
    std::error_code error;
    const std::filesystem::path normalized =
        directory_.lexically_normal();
    const std::filesystem::path normalizedParent =
        normalized.parent_path();
    const std::filesystem::path canonicalParent =
        std::filesystem::weakly_canonical(normalizedParent, error);
    if (error || canonicalParent != normalizedParent) {
      throw std::runtime_error(
          "log directory parent path must not contain symlinks");
    }
    std::filesystem::create_directories(directory_, error);
    if (error) {
      throw std::runtime_error("unable to create log directory: " +
                               error.message());
    }
    const auto status = std::filesystem::symlink_status(directory_, error);
    if (error || !std::filesystem::is_directory(status) ||
        std::filesystem::is_symlink(status)) {
      throw std::runtime_error(
          "log directory must be a real directory, not a symlink");
    }
    const std::filesystem::path canonicalDirectory =
        std::filesystem::canonical(directory_, error);
    if (error || canonicalDirectory != normalized) {
      throw std::runtime_error(
          "log directory path must not contain symlinks");
    }
    std::lock_guard lock(mutex_);
    RotateLocked();
  }

  ~EventLog() {
    std::lock_guard lock(mutex_);
    stream_.flush();
  }

  void Event(std::string message) {
    message = Sanitize(std::move(message));
    std::lock_guard lock(mutex_);
    const std::size_t required = message.size() + 1U;
    if (bytesWritten_ > kMaximumLogFileBytes -
                            std::min(required, kMaximumLogFileBytes)) {
      RotateLocked();
    } else if (std::chrono::steady_clock::now() >= nextSpaceCheck_) {
      PruneLocked(countLimit_ > 0 ? countLimit_ - 1U : 0U);
      nextSpaceCheck_ =
          std::chrono::steady_clock::now() + std::chrono::seconds{60};
    }
    std::cerr << message << '\n';
    if (stream_) {
      stream_ << message << '\n';
      stream_.flush();
      bytesWritten_ += required;
    }
  }

 private:
  struct LogFile {
    std::filesystem::path path;
    std::filesystem::file_time_type modified{};
  };

  [[nodiscard]] static std::string Sanitize(std::string message) {
    if (message.size() > 1024) {
      message.resize(1024);
    }
    for (char& character : message) {
      const auto byte = static_cast<unsigned char>(character);
      if (character == '\n' || character == '\r' || character == '\0' ||
          (byte < 0x20U && character != '\t')) {
        character = ' ';
      }
    }
    return message;
  }

  [[nodiscard]] std::optional<std::uint64_t> AvailableBytes() const {
    struct statvfs statistics {};
    if (statvfs(directory_.c_str(), &statistics) != 0) {
      return std::nullopt;
    }
    const std::uint64_t blocks =
        static_cast<std::uint64_t>(statistics.f_bavail);
    const std::uint64_t blockSize =
        static_cast<std::uint64_t>(statistics.f_frsize);
    if (blockSize != 0 &&
        blocks > std::numeric_limits<std::uint64_t>::max() / blockSize) {
      return std::numeric_limits<std::uint64_t>::max();
    }
    return blocks * blockSize;
  }

  [[nodiscard]] std::vector<LogFile> ExistingLogs() const {
    std::vector<LogFile> logs;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator{directory_, error};
         !error && iterator != std::filesystem::directory_iterator{};
         iterator.increment(error)) {
      if (logs.size() >= 4096) {
        break;
      }
      const auto status = iterator->symlink_status(error);
      if (error || !std::filesystem::is_regular_file(status)) {
        error.clear();
        continue;
      }
      const std::string name = iterator->path().filename().string();
      if (!name.starts_with("ec-systemcore-") || !name.ends_with(".log")) {
        continue;
      }
      if (!currentPath_.empty() && iterator->path() == currentPath_) {
        continue;
      }
      logs.push_back(
          LogFile{iterator->path(), iterator->last_write_time(error)});
      error.clear();
    }
    std::sort(logs.begin(), logs.end(),
              [](const LogFile& left, const LogFile& right) {
                if (left.modified != right.modified) {
                  return left.modified < right.modified;
                }
                return left.path < right.path;
              });
    return logs;
  }

  void PruneLocked(std::size_t maximumExisting) {
    std::vector<LogFile> logs = ExistingLogs();
    std::optional<std::uint64_t> available = AvailableBytes();
    while (!logs.empty() &&
           (logs.size() > maximumExisting ||
            (available.has_value() &&
             *available < freeSpaceThreshold_))) {
      std::error_code error;
      std::filesystem::remove(logs.front().path, error);
      if (error) {
        break;
      }
      logs.erase(logs.begin());
      available = AvailableBytes();
    }
  }

  void RotateLocked() {
    if (stream_) {
      stream_.flush();
      stream_.close();
    }
    currentPath_.clear();
    PruneLocked(countLimit_ > 0 ? countLimit_ - 1U : 0U);
    nextSpaceCheck_ =
        std::chrono::steady_clock::now() + std::chrono::seconds{60};
    bytesWritten_ = 0;
    const std::optional<std::uint64_t> available = AvailableBytes();
    if (available.has_value() &&
        *available < freeSpaceThreshold_) {
      return;
    }

    timespec realtime{};
    clock_gettime(CLOCK_REALTIME, &realtime);
    std::ostringstream name;
    name << "ec-systemcore-" << realtime.tv_sec << '-' << getpid() << '-'
         << sequence_++ << ".log";
    const std::filesystem::path path = directory_ / name.str();
    stream_.open(path, std::ios::out | std::ios::app);
    if (!stream_) {
      return;
    }
    currentPath_ = path;
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    bytesWritten_ = error ? 0 : static_cast<std::size_t>(size);
    PruneLocked(countLimit_ > 0 ? countLimit_ - 1U : 0U);
  }

  std::filesystem::path directory_;
  std::size_t countLimit_ = 10;
  std::uint64_t freeSpaceThreshold_ = 0;
  std::ofstream stream_;
  std::filesystem::path currentPath_;
  std::mutex mutex_;
  std::size_t bytesWritten_ = 0;
  std::uint64_t sequence_ = 0;
  std::chrono::steady_clock::time_point nextSpaceCheck_{};
};

template <typename Value, std::size_t Capacity>
class SpscQueue final {
 public:
  static_assert(Capacity >= 2);

  bool TryPush(const Value& value) {
    const std::size_t write = write_.load(std::memory_order_relaxed);
    const std::size_t next = Next(write);
    if (next == read_.load(std::memory_order_acquire)) {
      return false;
    }
    values_[write] = value;
    write_.store(next, std::memory_order_release);
    return true;
  }

  bool TryPop(Value* value) {
    if (value == nullptr) {
      return false;
    }
    const std::size_t read = read_.load(std::memory_order_relaxed);
    if (read == write_.load(std::memory_order_acquire)) {
      return false;
    }
    *value = values_[read];
    read_.store(Next(read), std::memory_order_release);
    return true;
  }

  [[nodiscard]] std::size_t AvailableToWrite() const {
    const std::size_t write = write_.load(std::memory_order_relaxed);
    const std::size_t read = read_.load(std::memory_order_acquire);
    if (read > write) {
      return read - write - 1U;
    }
    return Capacity - (write - read) - 1U;
  }

 private:
  [[nodiscard]] static constexpr std::size_t Next(std::size_t value) {
    return (value + 1U) % Capacity;
  }

  std::array<Value, Capacity> values_{};
  alignas(64) std::atomic<std::size_t> read_{0};
  alignas(64) std::atomic<std::size_t> write_{0};
};

struct OutputCommand {
  std::uint64_t epoch = 0;
  std::uint16_t subDeviceIndex = 0;
  std::uint32_t offset = 0;
  std::uint16_t length = 0;
  std::uint64_t acceptedMonotonicUs = 0;
  std::array<std::uint8_t, protocol::kMaximumOutputWrite> data{};
};

using SharedFrame =
    std::shared_ptr<const std::vector<std::uint8_t>>;

struct InputChunk {
  protocol::PdoInputPayload header;
  std::array<std::uint8_t, protocol::kMaximumPdoChunk> data{};
  std::uint16_t length = 0;
  std::uint16_t snapshotChunkIndex = 0;
  std::uint16_t snapshotChunkCount = 0;
};

struct InputPublicationAccumulator {
  std::uint64_t epoch = 0;
  std::uint64_t cycleSequence = 0;
  std::uint16_t expectedChunks = 0;
  std::uint16_t nextChunk = 0;
  std::size_t totalBytes = 0;
  std::vector<SharedFrame> frames;

  void Reset() {
    epoch = 0;
    cycleSequence = 0;
    expectedChunks = 0;
    nextChunk = 0;
    totalBytes = 0;
    frames.clear();
  }
};

struct BusSnapshot {
  protocol::BusState state = protocol::BusState::Starting;
  bool linkUp = false;
  bool reinitializing = false;
  std::uint16_t subDeviceCount = 0;
  std::uint16_t faults = 0;
  std::uint64_t lostFrames = 0;
  std::uint64_t cycleOverruns = 0;
  std::uint32_t currentJitterUs = 0;
  std::uint32_t maximumJitterUs = 0;
  std::uint64_t cycleSequence = 0;
  protocol::SchedulingMode schedulingMode =
      protocol::SchedulingMode::Other;
  std::uint8_t realtimeErrorBits = 0;
};

template <typename Integer>
void AtomicSaturatingIncrement(std::atomic<Integer>& value,
                               Integer amount = 1) {
  Integer current = value.load(std::memory_order_relaxed);
  while (current != std::numeric_limits<Integer>::max()) {
    const Integer available =
        std::numeric_limits<Integer>::max() - current;
    const Integer next =
        amount > available ? std::numeric_limits<Integer>::max()
                           : static_cast<Integer>(current + amount);
    if (value.compare_exchange_weak(current, next,
                                    std::memory_order_relaxed,
                                    std::memory_order_relaxed)) {
      return;
    }
  }
}

void AtomicMaximum(std::atomic<std::uint32_t>& destination,
                   std::uint32_t value) {
  std::uint32_t current = destination.load(std::memory_order_relaxed);
  while (current < value &&
         !destination.compare_exchange_weak(
             current, value, std::memory_order_relaxed,
             std::memory_order_relaxed)) {
  }
}

class EthercatBus final {
 public:
  EthercatBus(std::uint16_t index, InterfaceConfig configuration,
              const DaemonConfig& daemonConfiguration, EventLog& log,
              std::atomic<std::uint64_t>& globalFaultedEpoch)
      : index_(index),
        configuration_(std::move(configuration)),
        cyclePeriod_(daemonConfiguration.cyclePeriod),
        outputCommandTimeout_(daemonConfiguration.outputCommandTimeout),
        realtimeConfiguration_(daemonConfiguration.realtime),
        log_(log),
        globalFaultedEpoch_(globalFaultedEpoch),
        ioMap_(AbsoluteIoMapAllocationSize(), 0),
        distributedClockController_(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                cyclePeriod_)
                .count()) {
    for (auto& size : outputSizes_) {
      size.store(0, std::memory_order_relaxed);
    }
    for (auto& bits : outputBits_) {
      bits.store(0, std::memory_order_relaxed);
    }
  }

  ~EthercatBus() { Stop(); }

  EthercatBus(const EthercatBus&) = delete;
  EthercatBus& operator=(const EthercatBus&) = delete;

  void Start(std::string interfaceName, bool linkUp, std::uint64_t epoch) {
    Stop();
    interfaceName_ = std::move(interfaceName);
    linkUp_.store(linkUp, std::memory_order_release);
    safetyEpoch_.store(epoch, std::memory_order_release);
    safetyEpochApplied_.store(0, std::memory_order_release);
    observedSafetyEpoch_ = 0;
    outputsPermitted_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    worker_ = std::thread([this] { Run(); });
  }

  void Stop() {
    outputsPermitted_.store(false, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  void SetLinkUp(bool linkUp) {
    if (!linkUp) {
      outputsPermitted_.store(false, std::memory_order_release);
    }
    linkUp_.store(linkUp, std::memory_order_release);
  }

  void SetSafety(std::uint64_t epoch, bool outputsPermitted) {
    safetyEpoch_.exchange(epoch, std::memory_order_acq_rel);
    const bool faultLatched =
        faultedEpoch_.load(std::memory_order_acquire) == epoch ||
        globalFaultedEpoch_.load(std::memory_order_acquire) == epoch;
    outputsPermitted_.store(outputsPermitted && !faultLatched,
                            std::memory_order_release);
  }

  void SetInputPublicationPeriod(std::chrono::milliseconds period) {
    const std::uint64_t microseconds =
        period.count() <= 0
            ? 0
            : static_cast<std::uint64_t>(period.count()) * 1000ULL;
    inputPublicationPeriodUs_.store(microseconds,
                                    std::memory_order_release);
  }

  [[nodiscard]] bool SafetyBarrierComplete(
      std::uint64_t epoch) const {
    if (state_.load(std::memory_order_acquire) !=
        protocol::BusState::Operational) {
      return true;
    }
    return safetyEpochApplied_.load(std::memory_order_acquire) == epoch;
  }

  [[nodiscard]] bool QueueOutput(const OutputCommand& command) {
    return outputCommands_.TryPush(command);
  }

  [[nodiscard]] protocol::AckStatus ValidateOutput(
      std::uint16_t subDevice, std::uint32_t offset,
      std::span<const std::uint8_t> image) const {
    if (state_.load(std::memory_order_acquire) !=
        protocol::BusState::Operational) {
      return protocol::AckStatus::Disabled;
    }
    const std::uint16_t count =
        subDeviceCount_.load(std::memory_order_acquire);
    if (subDevice == 0 || subDevice > count ||
        subDevice >= outputSizes_.size()) {
      return protocol::AckStatus::BadTarget;
    }
    const std::uint32_t size =
        outputSizes_[subDevice].load(std::memory_order_acquire);
    const protocol::AckStatus bounds =
        ValidateWholeOutputWrite(size, offset, image.size());
    if (bounds != protocol::AckStatus::Ok) {
      return bounds;
    }
    const std::uint16_t bits =
        outputBits_[subDevice].load(std::memory_order_acquire);
    return HasCanonicalPdoPadding(bits, image)
               ? protocol::AckStatus::Ok
               : protocol::AckStatus::Malformed;
  }

  bool PopInput(InputChunk* chunk) { return inputChunks_.TryPop(chunk); }

  void ResetCounters() {
    resetCounterSequence_.fetch_add(1, std::memory_order_release);
  }

  [[nodiscard]] std::uint64_t faultSequence() const {
    return faultSequence_.load(std::memory_order_acquire);
  }

  [[nodiscard]] protocol::DisableReason faultReason() const {
    return static_cast<protocol::DisableReason>(
        faultReason_.load(std::memory_order_acquire));
  }

  [[nodiscard]] BusSnapshot Snapshot() const {
    const std::uint16_t processFaults =
        faults_.load(std::memory_order_acquire);
    const std::uint16_t staleFaults =
        staleOutputFaults_.load(std::memory_order_acquire);
    return {
        .state = state_.load(std::memory_order_acquire),
        .linkUp = linkUp_.load(std::memory_order_acquire),
        .reinitializing =
            reinitializing_.load(std::memory_order_acquire),
        .subDeviceCount =
            subDeviceCount_.load(std::memory_order_acquire),
        .faults = static_cast<std::uint16_t>(
            std::min<std::uint32_t>(
                std::numeric_limits<std::uint16_t>::max(),
                static_cast<std::uint32_t>(processFaults) +
                    staleFaults)),
        .lostFrames = lostFrames_.load(std::memory_order_acquire),
        .cycleOverruns =
            cycleOverruns_.load(std::memory_order_acquire),
        .currentJitterUs =
            currentJitterUs_.load(std::memory_order_acquire),
        .maximumJitterUs =
            maximumJitterUs_.load(std::memory_order_acquire),
        .cycleSequence =
            cycleSequence_.load(std::memory_order_acquire),
        .schedulingMode = static_cast<protocol::SchedulingMode>(
            schedulingMode_.load(std::memory_order_acquire)),
        .realtimeErrorBits =
            realtimeErrorBits_.load(std::memory_order_acquire),
    };
  }

  [[nodiscard]] const InterfaceConfig& configuration() const {
    return configuration_;
  }

  [[nodiscard]] bool outputTopologyVerified() const {
    return outputTopologyVerified_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool distributedClockHealthy() const {
    return !configuration_.distributedClock ||
           distributedClockSynchronized_.load(
               std::memory_order_acquire);
  }

 private:
  [[nodiscard]] static constexpr std::size_t
  AbsoluteIoMapAllocationSize() {
    constexpr std::size_t maximumBytesPerDirection =
        (static_cast<std::size_t>(
             std::numeric_limits<std::uint16_t>::max()) +
         7U) /
        8U;
    return (static_cast<std::size_t>(EC_MAXSLAVE) + 1U) * 2U *
               maximumBytesPerDirection +
           64U * 1024U;
  }

  [[nodiscard]] bool PointerRangeInIoMap(
      const std::uint8_t* pointer, std::uint32_t length) const {
    const std::uintptr_t begin =
        reinterpret_cast<std::uintptr_t>(ioMap_.data());
    const std::uintptr_t end = begin + ioMap_.size();
    const std::uintptr_t candidate =
        reinterpret_cast<std::uintptr_t>(pointer);
    if (length == 0) {
      return pointer == nullptr ||
             (candidate >= begin && candidate <= end);
    }
    if (pointer == nullptr) {
      return false;
    }
    return candidate >= begin && candidate <= end &&
           static_cast<std::size_t>(length) <= end - candidate;
  }

  [[nodiscard]] bool ValidateTopology(std::string* error) {
    if (!configuration_.expectedSubDevices.empty() &&
        configuration_.expectedSubDevices.size() !=
            static_cast<std::size_t>(context_.slavecount)) {
      *error = "expected " +
               std::to_string(
                   configuration_.expectedSubDevices.size()) +
               " SubDevices but found " +
               std::to_string(context_.slavecount);
      return false;
    }

    bool outputCapable = false;
    bool allPdoSizesVerified =
        !configuration_.expectedSubDevices.empty();
    std::size_t totalInputChunks = 0;
    for (int index = 1; index <= context_.slavecount; ++index) {
      const ec_slavet& actual = context_.slavelist[index];
      const std::uint32_t outputBytes =
          PdoImageByteLength(actual.Obits);
      const std::uint32_t inputBytes =
          PdoImageByteLength(actual.Ibits);
      if (!IsValidSoemPdoMetadata(actual.Obits, actual.Obytes,
                                  actual.Ostartbit) ||
          !IsValidSoemPdoMetadata(actual.Ibits, actual.Ibytes,
                                  actual.Istartbit)) {
        *error = "SubDevice " + std::to_string(index) +
                 " exposes inconsistent SOEM PDO metadata";
        return false;
      }
      outputCapable = outputCapable || outputBytes != 0;
      if (outputBytes > protocol::kMaximumOutputWrite) {
        *error = "SubDevice " + std::to_string(index) +
                 " output image exceeds the atomic 1024-byte write bound";
        return false;
      }
      totalInputChunks +=
          (static_cast<std::size_t>(inputBytes) +
           protocol::kMaximumPdoChunk - 1U) /
          protocol::kMaximumPdoChunk;
      if (!PointerRangeInIoMap(
              actual.outputs,
              PdoMappedByteLength(actual.Obits, actual.Ostartbit)) ||
          !PointerRangeInIoMap(
              actual.inputs,
              PdoMappedByteLength(actual.Ibits, actual.Istartbit))) {
        *error = "SubDevice " + std::to_string(index) +
                 " exposes PDO pointers outside the bounded IO map";
        return false;
      }
      if (configuration_.expectedSubDevices.empty() &&
          configuration_.allowUnverifiedTopology) {
        outputSizes_[static_cast<std::size_t>(index)].store(
            0, std::memory_order_release);
        outputBits_[static_cast<std::size_t>(index)].store(
            0, std::memory_order_release);
      } else {
        outputSizes_[static_cast<std::size_t>(index)].store(
            outputBytes, std::memory_order_release);
        outputBits_[static_cast<std::size_t>(index)].store(
            actual.Obits, std::memory_order_release);
      }

      if (configuration_.expectedSubDevices.empty()) {
        continue;
      }
      const ExpectedSubDevice& expected =
          configuration_.expectedSubDevices[
              static_cast<std::size_t>(index - 1)];
      if ((outputBytes != 0 &&
           !expected.outputBytes.has_value()) ||
          (inputBytes != 0 && !expected.inputBytes.has_value())) {
        *error = "SubDevice " + std::to_string(index) +
                 " requires explicit output_bytes/input_bytes for every "
                 "mapped PDO direction";
        return false;
      }
      if (actual.eep_man != expected.vendorId ||
          actual.eep_id != expected.productCode ||
          (expected.revision.has_value() &&
           actual.eep_rev != *expected.revision) ||
          (expected.outputBytes.has_value() &&
           outputBytes != *expected.outputBytes) ||
          (expected.inputBytes.has_value() &&
           inputBytes != *expected.inputBytes)) {
        *error = "SubDevice " + std::to_string(index) +
                 " does not match expected vendor/product/revision/PDO sizes";
        return false;
      }
    }
    if (outputCapable &&
        configuration_.expectedSubDevices.empty() &&
        !configuration_.allowUnverifiedTopology) {
      *error =
          "output-capable bus requires expected_subdevices; "
          "allow_unverified_topology is input-only and keeps outputs disabled";
      return false;
    }
    if (totalInputChunks > 128) {
      *error =
          "input PDO image exceeds the coherent 128-chunk publication bound";
      return false;
    }
    outputTopologyVerified_.store(
        outputCapable &&
            !configuration_.expectedSubDevices.empty() &&
            allPdoSizesVerified,
        std::memory_order_release);
    return true;
  }

  void LogDiscoveredTopology() {
    std::ostringstream signature;
    signature << context_.slavecount << ';';
    std::vector<std::string> lines;
    lines.reserve(static_cast<std::size_t>(context_.slavecount));
    for (int index = 1; index <= context_.slavecount; ++index) {
      const ec_slavet& subDevice = context_.slavelist[index];
      const std::uint32_t outputBytes =
          PdoImageByteLength(subDevice.Obits);
      const std::uint32_t inputBytes =
          PdoImageByteLength(subDevice.Ibits);
      signature << subDevice.eep_man << ',' << subDevice.eep_id << ','
                << subDevice.eep_rev << ',' << outputBytes << ','
                << inputBytes << ',' << subDevice.Obits << ','
                << subDevice.Ibits << ',' << unsigned{subDevice.Ostartbit}
                << ',' << unsigned{subDevice.Istartbit} << ';';
      std::ostringstream line;
      line << configuration_.logicalName << ": discovered SubDevice "
           << index << " vendor_id=" << subDevice.eep_man
           << " product_code=" << subDevice.eep_id
           << " revision=" << subDevice.eep_rev
           << " output_bytes=" << outputBytes
           << " input_bytes=" << inputBytes
           << " output_bits=" << subDevice.Obits
           << " input_bits=" << subDevice.Ibits
           << " output_start_bit=" << unsigned{subDevice.Ostartbit}
           << " input_start_bit=" << unsigned{subDevice.Istartbit};
      lines.emplace_back(line.str());
    }
    if (signature.str() == lastLoggedTopology_) {
      return;
    }
    lastLoggedTopology_ = signature.str();
    for (std::string& line : lines) {
      log_.Event(std::move(line));
    }
  }

  [[nodiscard]] RealtimeOperations LinuxRealtimeOperations() {
    return {
        .lockMemory = [] { return mlockall(MCL_CURRENT | MCL_FUTURE); },
        .setFifoScheduler = [](int priority) {
          sched_param parameters{};
          parameters.sched_priority = priority;
          return pthread_setschedparam(pthread_self(), SCHED_FIFO,
                                       &parameters);
        },
        .setCpuAffinity = [](unsigned int cpu) {
          cpu_set_t set;
          CPU_ZERO(&set);
          CPU_SET(cpu, &set);
          return pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
        },
        .availableCpuCount = [] {
          const long count = sysconf(_SC_NPROCESSORS_CONF);
          return count <= 0 ? 0U : static_cast<unsigned int>(count);
        },
        .currentSchedulingPolicy = [] {
          int policy = SCHED_OTHER;
          sched_param parameters{};
          if (pthread_getschedparam(pthread_self(), &policy, &parameters) !=
              0) {
            return SCHED_OTHER;
          }
          return policy;
        },
    };
  }

  void Run() {
    const RealtimeResult realtime = ApplyRealtimeConfiguration(
        realtimeConfiguration_, LinuxRealtimeOperations());
    schedulingMode_.store(
        static_cast<std::uint8_t>(realtime.schedulingMode),
        std::memory_order_release);
    realtimeErrorBits_.store(realtime.errorBits,
                             std::memory_order_release);
    bool attemptedInitialization = false;

    while (running_.load(std::memory_order_acquire)) {
      if (!linkUp_.load(std::memory_order_acquire)) {
        state_.store(protocol::BusState::WaitingForLink,
                     std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        continue;
      }

      if (attemptedInitialization) {
        RaiseFault(protocol::DisableReason::Reinitialize);
      }
      attemptedInitialization = true;
      reinitializing_.store(true, std::memory_order_release);
      if (!Initialize()) {
        reinitializing_.store(false, std::memory_order_release);
        state_.store(protocol::BusState::Fault,
                     std::memory_order_release);
        for (int count = 0;
             count < 100 &&
             running_.load(std::memory_order_acquire) &&
             linkUp_.load(std::memory_order_acquire);
             ++count) {
          std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        continue;
      }
      reinitializing_.store(false, std::memory_order_release);
      RunCyclic();
      CloseContext(true);
    }
    state_.store(protocol::BusState::Stopping,
                 std::memory_order_release);
    CloseContext(true);
  }

  [[nodiscard]] bool Initialize() {
    CloseContext(false);
    state_.store(protocol::BusState::Initializing,
                 std::memory_order_release);
    std::fill(ioMap_.begin(), ioMap_.end(), 0);
    for (auto& size : outputSizes_) {
      size.store(0, std::memory_order_relaxed);
    }
    for (auto& bits : outputBits_) {
      bits.store(0, std::memory_order_relaxed);
    }
    std::memset(&context_, 0, sizeof(context_));
    contextAttempted_ = true;
    if (ecx_init(&context_, interfaceName_.c_str()) <= 0) {
      log_.Event(configuration_.logicalName +
                 ": SOEM could not open " + interfaceName_);
      CloseContext(false);
      return false;
    }
    contextOpen_ = true;

    const int discovered = ecx_config_init(&context_);
    if (discovered <= 0 || context_.slavecount <= 0 ||
        context_.slavecount >= EC_MAXSLAVE) {
      log_.Event(configuration_.logicalName +
                 ": no valid EtherCAT SubDevices discovered");
      CloseContext(false);
      return false;
    }

    const int mapped =
        ecx_config_map_group(&context_, ioMap_.data(), 0);
    if (mapped < 0 ||
        static_cast<std::size_t>(mapped) >
            configuration_.maximumIoMapBytes ||
        static_cast<std::size_t>(mapped) > ioMap_.size()) {
      log_.Event(configuration_.logicalName +
                 ": PDO IO map exceeds the configured bound");
      CloseContext(false);
      return false;
    }

    LogDiscoveredTopology();
    std::string topologyError;
    if (!ValidateTopology(&topologyError)) {
      log_.Event(configuration_.logicalName + ": " + topologyError);
      CloseContext(false);
      return false;
    }

    ec_groupt& group = context_.grouplist[0];
    expectedWorkingCounter_ =
        (static_cast<int>(group.outputsWKC) * 2) +
        static_cast<int>(group.inputsWKC);
    if (expectedWorkingCounter_ <= 0) {
      log_.Event(configuration_.logicalName +
                 ": process-data working counter is zero");
      CloseContext(false);
      return false;
    }
    if (!PointerRangeInIoMap(group.outputs, group.Obytes) ||
        !PointerRangeInIoMap(group.inputs, group.Ibytes)) {
      log_.Event(configuration_.logicalName +
                 ": group PDO range is outside the IO map");
      CloseContext(false);
      return false;
    }

    if (configuration_.distributedClock) {
      if (ecx_configdc(&context_) == FALSE || group.hasdc == FALSE) {
        log_.Event(configuration_.logicalName +
                   ": distributed clock requested but unavailable");
        CloseContext(false);
        return false;
      }
      const auto cycleNanoseconds =
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              cyclePeriod_)
              .count();
      if (cycleNanoseconds <= 0 ||
          cycleNanoseconds >
              std::numeric_limits<std::uint32_t>::max()) {
        log_.Event(configuration_.logicalName +
                   ": distributed clock period is invalid");
        CloseContext(false);
        return false;
      }
      for (int index = 1; index <= context_.slavecount; ++index) {
        if (context_.slavelist[index].hasdc != FALSE) {
          ecx_dcsync0(
              &context_, static_cast<std::uint16_t>(index), TRUE,
              static_cast<std::uint32_t>(cycleNanoseconds),
              configuration_.distributedClockShiftNs);
        }
      }
      dcConfigured_ = true;
      distributedClockController_.Reset();
      distributedClockSynchronized_.store(false,
                                          std::memory_order_release);
      realtimeErrorBits_.fetch_or(
          RealtimeDistributedClockUnlocked,
          std::memory_order_release);
    }

    state_.store(protocol::BusState::SafeOperational,
                 std::memory_order_release);
    context_.slavelist[0].state = EC_STATE_SAFE_OP;
    ecx_writestate(&context_, 0);
    ecx_statecheck(&context_, 0, EC_STATE_SAFE_OP,
                   EC_TIMEOUTSTATE * 4);
    ZeroOutputs();
    ecx_send_processdata(&context_);
    ecx_receive_processdata(&context_, ReceiveTimeoutUs());

    context_.slavelist[0].state = EC_STATE_OPERATIONAL;
    ecx_writestate(&context_, 0);
    bool operational = false;
    for (int attempt = 0; attempt < 10; ++attempt) {
      ZeroOutputs();
      ecx_send_processdata(&context_);
      ecx_receive_processdata(&context_, ReceiveTimeoutUs());
      const std::uint16_t state =
          ecx_statecheck(&context_, 0, EC_STATE_OPERATIONAL,
                         ReceiveTimeoutUs());
      if (state == EC_STATE_OPERATIONAL) {
        operational = true;
        break;
      }
    }
    if (!operational) {
      log_.Event(configuration_.logicalName +
                 ": EtherCAT topology did not reach OP");
      CloseContext(false);
      return false;
    }

    subDeviceCount_.store(
        static_cast<std::uint16_t>(context_.slavecount),
        std::memory_order_release);
    faults_.store(0, std::memory_order_release);
    state_.store(protocol::BusState::Operational,
                 std::memory_order_release);
    log_.Event(configuration_.logicalName + ": OP on " + interfaceName_ +
               " with " + std::to_string(context_.slavecount) +
               " SubDevices");
    return true;
  }

  [[nodiscard]] int ReceiveTimeoutUs() const {
    const auto halfPeriod =
        std::chrono::duration_cast<std::chrono::microseconds>(
            cyclePeriod_ / 2)
            .count();
    return static_cast<int>(
        std::clamp<std::int64_t>(halfPeriod, 500, EC_TIMEOUTRET));
  }

  void RunCyclic() {
    timespec next{};
    if (clock_gettime(CLOCK_MONOTONIC, &next) != 0) {
      realtimeErrorBits_.fetch_or(RealtimeClockError,
                                  std::memory_order_release);
      RaiseFault(protocol::DisableReason::ProcessDataFault);
      return;
    }
    AddToTimespec(&next, cyclePeriod_);
    std::uint64_t observedReset =
        resetCounterSequence_.load(std::memory_order_acquire);
    unsigned int consecutiveWorkingCounterMisses = 0;
    std::uint64_t observedInputPublicationPeriod = 0;
    std::uint64_t nextInputPublication = 0;

    while (running_.load(std::memory_order_acquire) &&
           linkUp_.load(std::memory_order_acquire) &&
           state_.load(std::memory_order_acquire) ==
               protocol::BusState::Operational) {
      int sleepResult = 0;
      do {
        sleepResult =
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
      } while (sleepResult == EINTR &&
               running_.load(std::memory_order_acquire));
      if (sleepResult != 0) {
        realtimeErrorBits_.fetch_or(RealtimeClockError,
                                    std::memory_order_release);
        RaiseFault(protocol::DisableReason::ProcessDataFault);
        return;
      }

      timespec wake{};
      if (clock_gettime(CLOCK_MONOTONIC, &wake) != 0) {
        realtimeErrorBits_.fetch_or(RealtimeClockError,
                                    std::memory_order_release);
        RaiseFault(protocol::DisableReason::ProcessDataFault);
        return;
      }
      const std::uint32_t jitter = DifferenceMicros(wake, next);
      currentJitterUs_.store(jitter, std::memory_order_relaxed);
      AtomicMaximum(maximumJitterUs_, jitter);

      const std::uint64_t reset =
          resetCounterSequence_.load(std::memory_order_acquire);
      if (reset != observedReset) {
        observedReset = reset;
        lostFrames_.store(0, std::memory_order_relaxed);
        cycleOverruns_.store(0, std::memory_order_relaxed);
        maximumJitterUs_.store(0, std::memory_order_relaxed);
      }

      const std::uint64_t activeEpoch =
          safetyEpoch_.load(std::memory_order_acquire);
      bool epochBarrier =
          activeEpoch != observedSafetyEpoch_;
      if (epochBarrier) {
        observedSafetyEpoch_ = activeEpoch;
        ZeroOutputs();
        outputsWerePermitted_ = false;
      } else {
        ApplyOutputCommands();
        EnforcePerSubDeviceFreshness(MonotonicMicros());
      }
      if (epochBarrier ||
          !outputsPermitted_.load(std::memory_order_acquire)) {
        ZeroOutputs();
        outputsWerePermitted_ = false;
      }

      // SetSafety() is asynchronous to this worker. Recheck immediately before
      // transmission so an epoch change that raced command application cannot
      // send a command from the previous controller generation. Enabling a new
      // epoch is separately gated on safetyEpochApplied_, which is published
      // only after this zero barrier has been transmitted.
      const std::uint64_t transmitEpoch =
          safetyEpoch_.load(std::memory_order_acquire);
      if (transmitEpoch != observedSafetyEpoch_) {
        observedSafetyEpoch_ = transmitEpoch;
        epochBarrier = true;
        ZeroOutputs();
        outputsWerePermitted_ = false;
      }
      if (!outputsPermitted_.load(std::memory_order_acquire)) {
        ZeroOutputs();
        outputsWerePermitted_ = false;
      }
      if (globalFaultedEpoch_.load(std::memory_order_acquire) ==
          observedSafetyEpoch_) {
        ZeroOutputs();
        outputsWerePermitted_ = false;
      }

      const int sent = ecx_send_processdata(&context_);
      const int workingCounter =
          sent > 0 ? ecx_receive_processdata(&context_, ReceiveTimeoutUs())
                   : -1;
      if (epochBarrier && sent > 0 &&
          workingCounter >= expectedWorkingCounter_ &&
          safetyEpoch_.load(std::memory_order_acquire) ==
              observedSafetyEpoch_) {
        safetyEpochApplied_.store(observedSafetyEpoch_,
                                  std::memory_order_release);
      }
      AtomicSaturatingIncrement(cycleSequence_);
      if (workingCounter < expectedWorkingCounter_) {
        AtomicSaturatingIncrement(lostFrames_);
        ++consecutiveWorkingCounterMisses;
        if (consecutiveWorkingCounterMisses >= 3) {
          faults_.store(1, std::memory_order_release);
          outputsPermitted_.store(false, std::memory_order_release);
          ZeroOutputs();
          ecx_send_processdata(&context_);
          RaiseFault(protocol::DisableReason::ProcessDataFault);
          state_.store(protocol::BusState::Fault,
                       std::memory_order_release);
          return;
        }
      } else {
        consecutiveWorkingCounterMisses = 0;
      }

      std::int64_t distributedClockCorrectionNs = 0;
      if (workingCounter >= expectedWorkingCounter_ && dcConfigured_) {
        const DistributedClockAdjustment adjustment =
            distributedClockController_.Update(context_.DCtime);
        distributedClockCorrectionNs = adjustment.correctionNs;
        distributedClockSynchronized_.store(
            adjustment.synchronized, std::memory_order_release);
        if (adjustment.synchronized) {
          realtimeErrorBits_.fetch_and(
              static_cast<std::uint8_t>(
                  ~RealtimeDistributedClockUnlocked),
              std::memory_order_release);
        } else {
          realtimeErrorBits_.fetch_or(
              RealtimeDistributedClockUnlocked,
              std::memory_order_release);
        }
        if (adjustment.fault) {
          faults_.store(1, std::memory_order_release);
          outputsPermitted_.store(false, std::memory_order_release);
          ZeroOutputs();
          ecx_send_processdata(&context_);
          RaiseFault(
              protocol::DisableReason::ProcessDataFault);
          state_.store(protocol::BusState::Fault,
                       std::memory_order_release);
          return;
        }
      }

      const std::uint64_t nowMicros = MonotonicMicros();
      const std::uint64_t publicationPeriod =
          inputPublicationPeriodUs_.load(std::memory_order_acquire);
      if (publicationPeriod != observedInputPublicationPeriod) {
        observedInputPublicationPeriod = publicationPeriod;
        nextInputPublication =
            publicationPeriod == 0
                ? 0
                : nowMicros + publicationPeriod;
      }
      if (workingCounter >= expectedWorkingCounter_ &&
          publicationPeriod != 0 &&
          nowMicros >= nextInputPublication) {
        PublishInputs();
        nextInputPublication = nowMicros + publicationPeriod;
      }

      timespec after{};
      if (clock_gettime(CLOCK_MONOTONIC, &after) != 0) {
        realtimeErrorBits_.fetch_or(RealtimeClockError,
                                    std::memory_order_release);
        RaiseFault(protocol::DisableReason::ProcessDataFault);
        return;
      }
      AddNanoseconds(
          &next,
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              cyclePeriod_)
                  .count() +
              distributedClockCorrectionNs);
      if (TimespecLess(next, after)) {
        const std::uint64_t lateUs = DifferenceMicros(after, next);
        const std::uint64_t periodUs =
            static_cast<std::uint64_t>(cyclePeriod_.count());
        const std::uint64_t missed = (lateUs / periodUs) + 1U;
        AtomicSaturatingIncrement(
            cycleOverruns_,
            missed > std::numeric_limits<std::uint64_t>::max()
                         ? std::numeric_limits<std::uint64_t>::max()
                         : missed);
        AddToTimespec(
            &next,
            std::chrono::microseconds{
                static_cast<std::int64_t>(
                    std::min<std::uint64_t>(
                        missed,
                        static_cast<std::uint64_t>(
                            std::numeric_limits<std::int64_t>::max() /
                            periodUs)) *
                    periodUs)});
      }
    }
    if (!linkUp_.load(std::memory_order_acquire)) {
      RaiseFault(protocol::DisableReason::LinkLoss);
    }
  }

  void ApplyOutputCommands() {
    OutputCommand command;
    const std::uint64_t epoch =
        safetyEpoch_.load(std::memory_order_acquire);
    const bool permitted =
        outputsPermitted_.load(std::memory_order_acquire);
    const std::uint64_t now = MonotonicMicros();
    if (permitted && !outputsWerePermitted_) {
      ZeroOutputs();
      for (int index = 1; index <= context_.slavecount; ++index) {
        lastOutputCommandUs_[static_cast<std::size_t>(index)] = now;
        staleOutput_[static_cast<std::size_t>(index)] = false;
      }
      outputsWerePermitted_ = true;
    }
    while (outputCommands_.TryPop(&command)) {
      if (!permitted || command.epoch != epoch ||
          command.subDeviceIndex == 0 ||
          command.subDeviceIndex > context_.slavecount) {
        continue;
      }
      const std::uint64_t timeoutUs =
          static_cast<std::uint64_t>(
              outputCommandTimeout_.count()) *
          1000ULL;
      if (command.acceptedMonotonicUs == 0 ||
          now < command.acceptedMonotonicUs ||
          now - command.acceptedMonotonicUs > timeoutUs) {
        continue;
      }
      ec_slavet& subDevice =
          context_.slavelist[command.subDeviceIndex];
      const std::uint32_t outputBytes =
          PdoImageByteLength(subDevice.Obits);
      const std::uint32_t mappedBytes =
          PdoMappedByteLength(subDevice.Obits, subDevice.Ostartbit);
      if (subDevice.outputs == nullptr ||
          !PointerRangeInIoMap(subDevice.outputs, mappedBytes) ||
          ValidateWholeOutputWrite(outputBytes, command.offset,
                                   command.length) !=
              protocol::AckStatus::Ok ||
          !WritePackedPdoImage(
              std::span<std::uint8_t>{subDevice.outputs, mappedBytes},
              subDevice.Ostartbit, subDevice.Obits,
              std::span<const std::uint8_t>{
                  command.data.data(), command.length})) {
        continue;
      }
      lastOutputCommandUs_[command.subDeviceIndex] =
          command.acceptedMonotonicUs;
      staleOutput_[command.subDeviceIndex] = false;
    }
  }

  void EnforcePerSubDeviceFreshness(std::uint64_t now) {
    if (!outputsPermitted_.load(std::memory_order_acquire)) {
      return;
    }
    const std::uint64_t timeoutUs =
        static_cast<std::uint64_t>(
            outputCommandTimeout_.count()) *
        1000ULL;
    std::uint16_t staleCount = 0;
    for (int index = 1; index <= context_.slavecount; ++index) {
      ec_slavet& subDevice = context_.slavelist[index];
      const std::uint32_t bytes =
          PdoImageByteLength(subDevice.Obits);
      if (bytes == 0 || subDevice.outputs == nullptr) {
        continue;
      }
      const std::uint64_t last =
          lastOutputCommandUs_[static_cast<std::size_t>(index)];
      if (last == 0 || now < last || now - last > timeoutUs) {
        const std::uint32_t mappedBytes =
            PdoMappedByteLength(subDevice.Obits, subDevice.Ostartbit);
        if (PointerRangeInIoMap(subDevice.outputs, mappedBytes)) {
          static_cast<void>(ZeroPackedPdoImage(
              std::span<std::uint8_t>{subDevice.outputs, mappedBytes},
              subDevice.Ostartbit, subDevice.Obits));
        }
        staleOutput_[static_cast<std::size_t>(index)] = true;
      }
      if (staleOutput_[static_cast<std::size_t>(index)] &&
          staleCount != std::numeric_limits<std::uint16_t>::max()) {
        ++staleCount;
      }
    }
    staleOutputFaults_.store(staleCount, std::memory_order_release);
  }

  void PublishInputs() {
    constexpr std::size_t kMaximumChunksPerPublication = 128;
    struct InputRange {
      std::uint16_t subDevice = 0;
      const std::uint8_t* data = nullptr;
      std::uint32_t size = 0;
      std::uint16_t bitCount = 0;
      std::uint8_t startBit = 0;
      std::uint32_t mappedBytes = 0;
      std::size_t firstOrdinal = 0;
      std::size_t chunkCount = 0;
    };
    std::array<InputRange, EC_MAXSLAVE> ranges{};
    std::size_t rangeCount = 0;
    std::size_t totalChunks = 0;
    for (int index = 1; index <= context_.slavecount; ++index) {
      const ec_slavet& subDevice = context_.slavelist[index];
      const std::uint32_t inputBytes =
          PdoImageByteLength(subDevice.Ibits);
      const std::uint32_t mappedBytes =
          PdoMappedByteLength(subDevice.Ibits, subDevice.Istartbit);
      if (inputBytes == 0 || subDevice.inputs == nullptr ||
          !PointerRangeInIoMap(subDevice.inputs, mappedBytes)) {
        continue;
      }
      const std::size_t chunks =
          (static_cast<std::size_t>(inputBytes) +
           protocol::kMaximumPdoChunk - 1U) /
          protocol::kMaximumPdoChunk;
      ranges[rangeCount++] = {
          .subDevice = static_cast<std::uint16_t>(index),
          .data = subDevice.inputs,
          .size = inputBytes,
          .bitCount = subDevice.Ibits,
          .startBit = subDevice.Istartbit,
          .mappedBytes = mappedBytes,
          .firstOrdinal = totalChunks,
          .chunkCount = chunks,
      };
      totalChunks += chunks;
    }
    if (totalChunks == 0) {
      inputChunkCursor_ = 0;
      return;
    }
    if (totalChunks > kMaximumChunksPerPublication ||
        inputChunks_.AvailableToWrite() < totalChunks) {
      return;
    }

    std::size_t publishedChunks = 0;
    const std::uint64_t epoch =
        safetyEpoch_.load(std::memory_order_acquire);
    const std::uint64_t sequence =
        cycleSequence_.load(std::memory_order_acquire);
    const std::size_t publishLimit =
        std::min(totalChunks, kMaximumChunksPerPublication);
    for (std::size_t count = 0; count < publishLimit; ++count) {
      const std::size_t ordinal =
          (inputChunkCursor_ + count) % totalChunks;
      const auto range =
          std::find_if(ranges.begin(), ranges.begin() + rangeCount,
                       [ordinal](const InputRange& candidate) {
                         return ordinal >= candidate.firstOrdinal &&
                                ordinal < candidate.firstOrdinal +
                                              candidate.chunkCount;
                       });
      if (range == ranges.begin() + rangeCount) {
        break;
      }
      const std::size_t chunkWithinRange =
          ordinal - range->firstOrdinal;
      const std::uint32_t offset = static_cast<std::uint32_t>(
          chunkWithinRange * protocol::kMaximumPdoChunk);
      InputChunk chunk;
      chunk.length = static_cast<std::uint16_t>(
          std::min<std::uint32_t>(
              static_cast<std::uint32_t>(protocol::kMaximumPdoChunk),
              range->size - offset));
      chunk.header.busIndex = index_;
      chunk.header.subDeviceIndex = range->subDevice;
      chunk.header.offset = offset;
      chunk.header.totalSize = range->size;
      chunk.header.epoch = epoch;
      chunk.header.cycleSequence = sequence;
      chunk.snapshotChunkIndex =
          static_cast<std::uint16_t>(count);
      chunk.snapshotChunkCount =
          static_cast<std::uint16_t>(totalChunks);
      if (!ReadPackedPdoChunk(
              std::span<const std::uint8_t>{
                  range->data, range->mappedBytes},
              range->startBit, range->bitCount, offset,
              std::span<std::uint8_t>{chunk.data.data(), chunk.length})) {
        break;
      }
      if (!inputChunks_.TryPush(chunk)) {
        break;
      }
      ++publishedChunks;
    }
    inputChunkCursor_ =
        (inputChunkCursor_ + publishedChunks) % totalChunks;
  }

  void ZeroOutputs() {
    if (!contextOpen_) {
      return;
    }
    for (int index = 1; index <= context_.slavecount; ++index) {
      ec_slavet& subDevice = context_.slavelist[index];
      const std::uint32_t mappedBytes =
          PdoMappedByteLength(subDevice.Obits, subDevice.Ostartbit);
      if (subDevice.Obits != 0 && subDevice.outputs != nullptr &&
          PointerRangeInIoMap(subDevice.outputs, mappedBytes)) {
        static_cast<void>(ZeroPackedPdoImage(
            std::span<std::uint8_t>{subDevice.outputs, mappedBytes},
            subDevice.Ostartbit, subDevice.Obits));
      }
    }
  }

  void CloseContext(bool transmitSafeFrame) {
    outputsPermitted_.store(false, std::memory_order_release);
    subDeviceCount_.store(0, std::memory_order_release);
    for (auto& size : outputSizes_) {
      size.store(0, std::memory_order_relaxed);
    }
    for (auto& bits : outputBits_) {
      bits.store(0, std::memory_order_relaxed);
    }
    outputTopologyVerified_.store(false, std::memory_order_release);
    outputsWerePermitted_ = false;
    staleOutputFaults_.store(0, std::memory_order_release);
    lastOutputCommandUs_.fill(0);
    staleOutput_.fill(false);
    if (!contextAttempted_) {
      return;
    }
    if (contextOpen_) {
      ZeroOutputs();
      if (transmitSafeFrame) {
        ecx_send_processdata(&context_);
        ecx_receive_processdata(&context_, ReceiveTimeoutUs());
      }
      if (dcConfigured_) {
        for (int index = 1; index <= context_.slavecount; ++index) {
          if (context_.slavelist[index].hasdc != FALSE) {
            ecx_dcsync0(&context_, static_cast<std::uint16_t>(index),
                        FALSE, 0, 0);
          }
        }
      }
      context_.slavelist[0].state = EC_STATE_INIT;
      ecx_writestate(&context_, 0);
    }
    ecx_close(&context_);
    pthread_mutex_destroy(&context_.port.getindex_mutex);
    pthread_mutex_destroy(&context_.port.tx_mutex);
    pthread_mutex_destroy(&context_.port.rx_mutex);
    std::memset(&context_, 0, sizeof(context_));
    contextAttempted_ = false;
    contextOpen_ = false;
    dcConfigured_ = false;
    distributedClockSynchronized_.store(false,
                                        std::memory_order_release);
    expectedWorkingCounter_ = 0;
  }

  void RaiseFault(protocol::DisableReason reason) {
    const std::uint64_t epoch =
        safetyEpoch_.load(std::memory_order_acquire);
    faultedEpoch_.store(epoch, std::memory_order_release);
    globalFaultedEpoch_.store(epoch, std::memory_order_release);
    outputsPermitted_.store(false, std::memory_order_release);
    faultReason_.store(static_cast<std::uint16_t>(reason),
                       std::memory_order_release);
    AtomicSaturatingIncrement(faultSequence_);
  }

  static void AddToTimespec(timespec* value,
                            std::chrono::microseconds duration) {
    const std::int64_t microseconds = duration.count();
    value->tv_sec +=
        static_cast<time_t>(microseconds / 1000000);
    value->tv_nsec +=
        static_cast<long>((microseconds % 1000000) * 1000);
    while (value->tv_nsec >= 1000000000L) {
      value->tv_nsec -= 1000000000L;
      ++value->tv_sec;
    }
  }

  static void AddNanoseconds(timespec* value,
                             std::int64_t nanoseconds) {
    value->tv_sec += static_cast<time_t>(
        nanoseconds / 1000000000LL);
    value->tv_nsec +=
        static_cast<long>(nanoseconds % 1000000000LL);
    while (value->tv_nsec >= 1000000000L) {
      value->tv_nsec -= 1000000000L;
      ++value->tv_sec;
    }
    while (value->tv_nsec < 0) {
      value->tv_nsec += 1000000000L;
      --value->tv_sec;
    }
  }

  [[nodiscard]] static bool TimespecLess(const timespec& left,
                                         const timespec& right) {
    return left.tv_sec < right.tv_sec ||
           (left.tv_sec == right.tv_sec && left.tv_nsec < right.tv_nsec);
  }

  [[nodiscard]] static std::uint32_t DifferenceMicros(
      const timespec& later, const timespec& earlier) {
    std::int64_t seconds =
        static_cast<std::int64_t>(later.tv_sec) -
        static_cast<std::int64_t>(earlier.tv_sec);
    std::int64_t nanoseconds =
        static_cast<std::int64_t>(later.tv_nsec) -
        static_cast<std::int64_t>(earlier.tv_nsec);
    if (nanoseconds < 0) {
      --seconds;
      nanoseconds += 1000000000LL;
    }
    if (seconds < 0) {
      return 0;
    }
    const std::uint64_t result =
        static_cast<std::uint64_t>(seconds) * 1000000ULL +
        static_cast<std::uint64_t>(nanoseconds / 1000LL);
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(
            result, std::numeric_limits<std::uint32_t>::max()));
  }

  std::uint16_t index_ = 0;
  InterfaceConfig configuration_;
  std::chrono::microseconds cyclePeriod_{5000};
  std::chrono::milliseconds outputCommandTimeout_{100};
  RealtimeConfig realtimeConfiguration_;
  EventLog& log_;
  std::atomic<std::uint64_t>& globalFaultedEpoch_;
  std::string interfaceName_;
  std::thread worker_;
  std::atomic_bool running_{false};
  std::atomic_bool linkUp_{false};
  std::atomic_bool outputsPermitted_{false};
  std::atomic<std::uint64_t> safetyEpoch_{1};
  std::atomic<std::uint64_t> safetyEpochApplied_{0};
  std::atomic<std::uint64_t> inputPublicationPeriodUs_{0};
  std::uint64_t observedSafetyEpoch_ = 0;
  bool outputsWerePermitted_ = false;
  std::array<std::uint64_t, EC_MAXSLAVE> lastOutputCommandUs_{};
  std::array<bool, EC_MAXSLAVE> staleOutput_{};
  std::atomic<std::uint16_t> staleOutputFaults_{0};
  std::atomic<std::uint64_t> faultedEpoch_{0};
  SpscQueue<OutputCommand, kMaximumOutputCommands> outputCommands_;
  SpscQueue<InputChunk, kMaximumInputChunks> inputChunks_;

  ecx_contextt context_{};
  std::vector<std::uint8_t> ioMap_;
  bool contextAttempted_ = false;
  bool contextOpen_ = false;
  bool dcConfigured_ = false;
  DistributedClockController distributedClockController_;
  int expectedWorkingCounter_ = 0;

  std::atomic<protocol::BusState> state_{
      protocol::BusState::Starting};
  std::atomic_bool reinitializing_{false};
  std::atomic<std::uint16_t> subDeviceCount_{0};
  std::array<std::atomic<std::uint32_t>, EC_MAXSLAVE> outputSizes_{};
  std::array<std::atomic<std::uint16_t>, EC_MAXSLAVE> outputBits_{};
  std::atomic<std::uint16_t> faults_{0};
  std::atomic<std::uint64_t> lostFrames_{0};
  std::atomic<std::uint64_t> cycleOverruns_{0};
  std::atomic<std::uint32_t> currentJitterUs_{0};
  std::atomic<std::uint32_t> maximumJitterUs_{0};
  std::atomic<std::uint64_t> cycleSequence_{0};
  std::atomic<std::uint8_t> schedulingMode_{
      static_cast<std::uint8_t>(protocol::SchedulingMode::Other)};
  std::atomic<std::uint8_t> realtimeErrorBits_{0};
  std::atomic<std::uint64_t> resetCounterSequence_{0};
  std::atomic<std::uint64_t> faultSequence_{0};
  std::atomic<std::uint16_t> faultReason_{
      static_cast<std::uint16_t>(
          protocol::DisableReason::ProcessDataFault)};
  std::atomic_bool outputTopologyVerified_{false};
  std::atomic_bool distributedClockSynchronized_{false};
  std::size_t inputChunkCursor_ = 0;
  std::string lastLoggedTopology_;
};

struct AdapterRuntime {
  AdapterResolution resolution;
  bool runtimeUnlocked = false;
  bool workerStarted = false;
  std::string boundInterface;
  std::string boundIdPath;
  std::uint64_t observedFaultSequence = 0;
};

struct ClientConnection {
  int descriptor = -1;
  std::uint64_t id = 0;
  std::uint32_t pid = 0;
  uid_t uid = 0;
  gid_t gid = 0;
  std::vector<gid_t> peerGroups;
  bool helloReceived = false;
  protocol::ClientRole role = protocol::ClientRole::Observer;
  bool subscribeInputs = false;
  std::uint16_t inputPeriodMs = 100;
  std::array<std::uint64_t, kMaximumBuses> lastInputSequence{};
  std::array<std::uint64_t, kMaximumBuses> selectedInputSequence{};
  std::array<std::chrono::steady_clock::time_point, kMaximumBuses>
      nextInputPublication{};
  protocol::FrameDecoder decoder;
  std::deque<SharedFrame> outgoing;
  std::size_t outgoingOffset = 0;
  std::size_t outgoingBytes = 0;
  bool closeAfterWrite = false;
  std::chrono::steady_clock::time_point acceptedAt =
      std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point rateUpdated = acceptedAt;
  double rateTokens = 500;
  double rateCapacity = 500;
  double rateRefillPerSecond = 1000;
  std::size_t optionalQueueDrops = 0;
  std::chrono::steady_clock::time_point firstOptionalDrop{};
};

class Daemon final {
 public:
  Daemon(DaemonConfig configuration,
         std::filesystem::path configurationPath,
         std::string configurationText, std::filesystem::path socketPath,
         std::filesystem::path sysfsRoot,
         std::filesystem::path udevDataRoot)
      : configuration_(std::move(configuration)),
        configurationPath_(std::move(configurationPath)),
        initialConfigurationText_(std::move(configurationText)),
        socketPath_(std::move(socketPath)),
        scanner_(std::move(sysfsRoot), std::move(udevDataRoot)),
        log_(configuration_.logDirectory,
             configuration_.logCountLimit,
             configuration_.freeSpaceThresholdBytes),
        safety_(InitialEpoch(), configuration_.heartbeatTimeout,
                configuration_.outputCommandTimeout),
        preemptRtAvailable_(DetectPreemptRt()) {
    if (const group* controller =
            getgrnam(configuration_.controllerGroup.c_str());
        controller != nullptr) {
      configuration_.controllerGids.push_back(controller->gr_gid);
      std::sort(configuration_.controllerGids.begin(),
                configuration_.controllerGids.end());
      configuration_.controllerGids.erase(
          std::unique(configuration_.controllerGids.begin(),
                      configuration_.controllerGids.end()),
          configuration_.controllerGids.end());
    } else {
      log_.Event("controller group '" +
                 configuration_.controllerGroup +
                 "' is unavailable; controller access is root/explicit IDs only");
    }
    buses_.reserve(configuration_.interfaces.size());
    adapters_.resize(configuration_.interfaces.size());
    for (std::size_t index = 0;
         index < configuration_.interfaces.size(); ++index) {
      buses_.emplace_back(std::make_unique<EthercatBus>(
          static_cast<std::uint16_t>(index),
          configuration_.interfaces[index], configuration_, log_,
          globalFaultedEpoch_));
    }
  }

  ~Daemon() { Shutdown(); }

  [[nodiscard]] int Run() {
    OpenSocket();
    log_.Event("ec-systemcore daemon started; epoch=" +
               std::to_string(safety_.epoch()) +
               ", buses=" + std::to_string(buses_.size()));
    ScanAdapters(true);

    auto nextStatus = std::chrono::steady_clock::now();
    auto nextAdapterScan = std::chrono::steady_clock::now();
    auto nextConfigurationCheck =
        std::chrono::steady_clock::now() + std::chrono::milliseconds{500};

    while (gRunning != 0) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= nextAdapterScan || forceAdapterScan_) {
        ScanAdapters(false);
        forceAdapterScan_ = false;
        nextAdapterScan = now + std::chrono::milliseconds{250};
      }
      HandleBusFaults();
      HandleSafetyTimeouts(now);
      DrainInputs(now);
      if (now >= nextStatus) {
        BroadcastStatus();
        BroadcastBusInformation();
        nextStatus = now + std::chrono::milliseconds{100};
      }
      if (now >= nextConfigurationCheck) {
        nextConfigurationCheck = now + std::chrono::milliseconds{500};
        if (ConfigurationChanged()) {
          log_.Event(
              "configuration changed; safely restarting into a new epoch");
          AdvanceEpoch(protocol::DisableReason::Reinitialize, 0xffffU);
          exitCode_ = 75;
          break;
        }
      }

      PollClients(10);
      RemoveClosedClients();
    }

    AdvanceEpoch(protocol::DisableReason::Shutdown, 0xffffU);
    FlushClientsFor(std::chrono::milliseconds{20});
    Shutdown();
    return exitCode_;
  }

 private:
  [[nodiscard]] static bool DetectPreemptRt() {
    try {
      const std::string realtime =
          ReadBoundedFile("/sys/kernel/realtime", 32);
      if (TrimAscii(realtime) == "1") {
        return true;
      }
    } catch (const std::exception&) {
    }
    try {
      const std::string version = ReadBoundedFile("/proc/version", 4096);
      return version.find("PREEMPT_RT") != std::string::npos ||
             version.find("PREEMPT RT") != std::string::npos;
    } catch (const std::exception&) {
      return false;
    }
  }

  void OpenSocket() {
    if (!socketPath_.is_absolute() ||
        socketPath_.string().size() >= sizeof(sockaddr_un::sun_path)) {
      throw std::runtime_error("IPC socket path is invalid");
    }
    const std::filesystem::path parent = socketPath_.parent_path();
    std::error_code filesystemError;
    std::filesystem::create_directories(parent, filesystemError);
    if (filesystemError) {
      throw std::runtime_error("unable to create IPC directory: " +
                               filesystemError.message());
    }
    const auto parentStatus =
        std::filesystem::symlink_status(parent, filesystemError);
    if (filesystemError ||
        !std::filesystem::is_directory(parentStatus) ||
        std::filesystem::is_symlink(parentStatus)) {
      throw std::runtime_error(
          "IPC parent must be a real directory, not a symlink");
    }

    const std::filesystem::path lockPath =
        parent / "ec-systemcore.lock";
    lockDescriptor_ =
        open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0640);
    if (lockDescriptor_ < 0 ||
        flock(lockDescriptor_, LOCK_EX | LOCK_NB) != 0) {
      throw std::runtime_error(
          "another ec-systemcore daemon owns the IPC endpoint");
    }

    struct stat existing {};
    if (lstat(socketPath_.c_str(), &existing) == 0) {
      if (!S_ISSOCK(existing.st_mode)) {
        throw std::runtime_error(
            "refusing to replace a non-socket IPC path");
      }
      if (unlink(socketPath_.c_str()) != 0) {
        throw std::runtime_error("unable to remove stale IPC socket");
      }
    } else if (errno != ENOENT) {
      throw std::runtime_error("unable to inspect IPC socket path");
    }

    listenDescriptor_ =
        socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listenDescriptor_ < 0) {
      throw std::runtime_error("unable to create IPC socket");
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socketPath_.c_str(),
                socketPath_.string().size() + 1U);
    if (bind(listenDescriptor_,
             reinterpret_cast<const sockaddr*>(&address),
             sizeof(address)) != 0 ||
        chmod(socketPath_.c_str(), 0660) != 0 ||
        listen(listenDescriptor_, static_cast<int>(kMaximumClients)) != 0) {
      throw std::runtime_error("unable to bind/listen on IPC socket");
    }
  }

  void Shutdown() {
    if (shutdown_) {
      return;
    }
    shutdown_ = true;
    for (const auto& bus : buses_) {
      bus->SetSafety(safety_.epoch(), false);
    }
    for (const auto& bus : buses_) {
      bus->Stop();
    }
    for (ClientConnection& client : clients_) {
      if (client.descriptor >= 0) {
        close(client.descriptor);
        client.descriptor = -1;
      }
    }
    clients_.clear();
    if (listenDescriptor_ >= 0) {
      close(listenDescriptor_);
      listenDescriptor_ = -1;
    }
    if (!socketPath_.empty()) {
      struct stat status {};
      if (lstat(socketPath_.c_str(), &status) == 0 &&
          S_ISSOCK(status.st_mode)) {
        unlink(socketPath_.c_str());
      }
    }
    if (lockDescriptor_ >= 0) {
      close(lockDescriptor_);
      lockDescriptor_ = -1;
    }
  }

  void ScanAdapters(bool initial) {
    std::string scanError;
    const std::vector<AdapterIdentity> identities =
        scanner_.Scan(&scanError);
    if (!scanError.empty() && scanError != lastScanError_) {
      log_.Event("adapter scan: " + scanError);
      lastScanError_ = scanError;
    }

    struct PlannedBinding {
      AdapterResolution resolution;
      bool ready = false;
      bool bindingChanged = false;
      bool linkLost = false;
      std::string interfaceName;
      std::string idPath;
      bool linkUp = false;
    };
    std::vector<PlannedBinding> planned;
    planned.reserve(buses_.size());
    bool safetyChange = false;
    protocol::DisableReason safetyReason =
        protocol::DisableReason::AdapterMismatch;
    std::uint16_t safetyBus = 0xffffU;
    for (std::size_t index = 0; index < buses_.size(); ++index) {
      AdapterRuntime& runtime = adapters_[index];
      const InterfaceConfig& mapping =
          configuration_.interfaces[index];
      AdapterResolution next = ResolveAdapter(
          identities, mapping.physicalInterface, mapping.lock,
          runtime.runtimeUnlocked);
      const bool nextReady =
          next.identity.has_value() &&
          (next.state == protocol::AdapterLockState::Unlocked ||
           next.state == protocol::AdapterLockState::Matched ||
           next.state ==
               protocol::AdapterLockState::RuntimeUnlocked);
      const std::string nextInterface =
          next.identity.has_value() ? next.identity->interfaceName
                                    : std::string{};
      const std::string nextIdPath =
          next.identity.has_value() ? next.identity->idPath
                                    : std::string{};
      const bool nextLink =
          next.identity.has_value() && next.identity->linkUp;
      const bool priorLink =
          runtime.resolution.identity.has_value() &&
          runtime.resolution.identity->linkUp;
      const bool bindingChanged =
          nextInterface != runtime.boundInterface ||
          nextIdPath != runtime.boundIdPath ||
          next.state != runtime.resolution.state ||
          next.reason != runtime.resolution.reason;
      const bool linkLost = priorLink && !nextLink;

      if (!initial && (bindingChanged || linkLost)) {
        safetyChange = true;
        safetyBus = static_cast<std::uint16_t>(index);
        safetyReason =
            linkLost ? protocol::DisableReason::LinkLoss
                     : protocol::DisableReason::AdapterMismatch;
      }
      planned.push_back({
          .resolution = std::move(next),
          .ready = nextReady,
          .bindingChanged = bindingChanged,
          .linkLost = linkLost,
          .interfaceName = nextInterface,
          .idPath = nextIdPath,
          .linkUp = nextLink,
      });
    }
    if (safetyChange) {
      AdvanceEpoch(safetyReason, safetyBus);
    }

    for (std::size_t index = 0; index < buses_.size(); ++index) {
      AdapterRuntime& runtime = adapters_[index];
      PlannedBinding& next = planned[index];
      if (!next.ready) {
        if (runtime.workerStarted) {
          buses_[index]->Stop();
          runtime.workerStarted = false;
        }
      } else if (!runtime.workerStarted || next.bindingChanged) {
        if (runtime.workerStarted) {
          buses_[index]->Stop();
        }
        buses_[index]->Start(next.interfaceName, next.linkUp,
                             safety_.epoch());
        runtime.workerStarted = true;
      } else {
        buses_[index]->SetLinkUp(next.linkUp);
      }
      runtime.boundInterface = next.interfaceName;
      runtime.boundIdPath = next.idPath;
      runtime.resolution = std::move(next.resolution);
    }
  }

  void HandleBusFaults() {
    for (std::size_t index = 0; index < buses_.size(); ++index) {
      AdapterRuntime& runtime = adapters_[index];
      const std::uint64_t sequence = buses_[index]->faultSequence();
      if (sequence == runtime.observedFaultSequence) {
        continue;
      }
      runtime.observedFaultSequence = sequence;
      AdvanceEpoch(buses_[index]->faultReason(),
                   static_cast<std::uint16_t>(index));
    }
  }

  void ApplySafetyToBuses() {
    for (const auto& bus : buses_) {
      bus->SetSafety(safety_.epoch(), safety_.outputsEnabled());
    }
  }

  void AdvanceEpoch(protocol::DisableReason reason,
                    std::uint16_t busIndex) {
    const std::optional<SafetyGate::ClientId> controller =
        safety_.controllerId();
    safety_.Invalidate();
    ApplySafetyToBuses();
    BroadcastOutputsDisabled(reason, busIndex);
    if (controller.has_value()) {
      if (ClientConnection* client = FindClient(*controller);
          client != nullptr) {
        client->closeAfterWrite = true;
      }
    }
  }

  void HandleSafetyTimeouts(
      std::chrono::steady_clock::time_point now) {
    std::optional<SafetyGate::ClientId> controller =
        safety_.controllerId();
    SafetyGate::Result result = safety_.ExpireOutputCommands(now);
    if (result.epochChanged) {
      ApplySafetyToBuses();
      BroadcastOutputsDisabled(
          protocol::DisableReason::OutputCommandTimeout, 0xffffU);
      if (controller.has_value()) {
        if (ClientConnection* client = FindClient(*controller);
            client != nullptr) {
          client->closeAfterWrite = true;
        }
      }
      return;
    }
    controller = safety_.controllerId();
    result = safety_.Expire(now);
    if (!result.epochChanged) {
      return;
    }
    ApplySafetyToBuses();
    BroadcastOutputsDisabled(
        protocol::DisableReason::HeartbeatTimeout, 0xffffU);
    if (controller.has_value()) {
      if (ClientConnection* client = FindClient(*controller);
          client != nullptr) {
        client->closeAfterWrite = true;
      }
    }
  }

  [[nodiscard]] bool AllBusesOperational() const {
    if (buses_.empty()) {
      return false;
    }
    if (globalFaultedEpoch_.load(std::memory_order_acquire) ==
        safety_.epoch()) {
      return false;
    }
    bool anyVerifiedOutputs = false;
    for (std::size_t index = 0; index < buses_.size(); ++index) {
      const AdapterRuntime& adapter = adapters_[index];
      const bool adapterReady =
          adapter.resolution.identity.has_value() &&
          (adapter.resolution.state ==
               protocol::AdapterLockState::Unlocked ||
           adapter.resolution.state ==
               protocol::AdapterLockState::Matched ||
           adapter.resolution.state ==
               protocol::AdapterLockState::RuntimeUnlocked) &&
          adapter.resolution.identity->linkUp;
      if (!adapterReady ||
          buses_[index]->Snapshot().state !=
              protocol::BusState::Operational ||
          !buses_[index]->SafetyBarrierComplete(safety_.epoch()) ||
          !buses_[index]->distributedClockHealthy()) {
        return false;
      }
      anyVerifiedOutputs =
          anyVerifiedOutputs ||
          buses_[index]->outputTopologyVerified();
    }
    return anyVerifiedOutputs;
  }

  [[nodiscard]] bool OutputCapabilityAvailable() const {
    return std::any_of(
        buses_.begin(), buses_.end(),
        [](const std::unique_ptr<EthercatBus>& bus) {
          return bus->outputTopologyVerified();
        });
  }

  void PollClients(int timeoutMilliseconds) {
    std::vector<pollfd> descriptors;
    descriptors.reserve(clients_.size() + 1U);
    descriptors.push_back(
        pollfd{.fd = listenDescriptor_, .events = POLLIN, .revents = 0});
    for (const ClientConnection& client : clients_) {
      short events = POLLIN;
      if (!client.outgoing.empty()) {
        events = static_cast<short>(events | POLLOUT);
      }
      descriptors.push_back(
          pollfd{.fd = client.descriptor, .events = events, .revents = 0});
    }

    const int result =
        poll(descriptors.data(), descriptors.size(), timeoutMilliseconds);
    if (result < 0) {
      if (errno != EINTR) {
        throw std::runtime_error("IPC poll failed");
      }
      return;
    }
    const std::size_t polledClientCount = clients_.size();
    if ((descriptors[0].revents & POLLIN) != 0) {
      AcceptClients();
    }

    for (std::size_t index = 0; index < polledClientCount; ++index) {
      ClientConnection& client = clients_[index];
      const short events = descriptors[index + 1U].revents;
      if ((events & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        CloseClient(client);
        continue;
      }
      if ((events & POLLIN) != 0) {
        ReadClient(client);
      }
      if (client.descriptor >= 0 && (events & POLLOUT) != 0) {
        FlushClient(client);
      }
      if (!client.helloReceived &&
          std::chrono::steady_clock::now() - client.acceptedAt >
              std::chrono::seconds{1}) {
        CloseClient(client);
      }
    }
  }

  void AcceptClients() {
    for (;;) {
      const int descriptor =
          accept4(listenDescriptor_, nullptr, nullptr,
                  SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (descriptor < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          return;
        }
        if (errno == EINTR) {
          continue;
        }
        throw std::runtime_error("IPC accept failed");
      }
      if (clients_.size() >= kMaximumClients) {
        close(descriptor);
        continue;
      }
      ucred credentials{};
      socklen_t credentialLength = sizeof(credentials);
      if (getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials,
                     &credentialLength) != 0 ||
          credentialLength != sizeof(credentials) ||
          credentials.pid <= 0) {
        close(descriptor);
        continue;
      }
      const std::size_t clientsForUid =
          static_cast<std::size_t>(std::count_if(
              clients_.begin(), clients_.end(),
              [&credentials](const ClientConnection& existing) {
                return existing.descriptor >= 0 &&
                       existing.uid == credentials.uid;
              }));
      if (clientsForUid >= 8) {
        close(descriptor);
        continue;
      }
      ClientConnection client;
      client.descriptor = descriptor;
      client.id = nextClientId_++;
      if (client.id == 0) {
        client.id = nextClientId_++;
      }
      client.pid = static_cast<std::uint32_t>(credentials.pid);
      client.uid = credentials.uid;
      client.gid = credentials.gid;
      client.peerGroups.push_back(credentials.gid);
#if defined(SO_PEERGROUPS)
      std::array<gid_t, 256> peerGroups{};
      socklen_t peerGroupsLength =
          static_cast<socklen_t>(sizeof(peerGroups));
      if (getsockopt(descriptor, SOL_SOCKET, SO_PEERGROUPS,
                     peerGroups.data(), &peerGroupsLength) == 0 &&
          peerGroupsLength % sizeof(gid_t) == 0) {
        const std::size_t groupCount =
            std::min<std::size_t>(
                peerGroups.size(),
                peerGroupsLength / sizeof(gid_t));
        client.peerGroups.insert(
            client.peerGroups.end(), peerGroups.begin(),
            peerGroups.begin() +
                static_cast<std::ptrdiff_t>(groupCount));
      }
#endif
      std::sort(client.peerGroups.begin(), client.peerGroups.end());
      client.peerGroups.erase(
          std::unique(client.peerGroups.begin(),
                      client.peerGroups.end()),
          client.peerGroups.end());
      clients_.emplace_back(std::move(client));
    }
  }

  void ReadClient(ClientConnection& client) {
    std::array<std::uint8_t,
               protocol::kLengthPrefixSize +
                   protocol::kMaximumFrameLength>
        bytes{};
    std::size_t totalRead = 0;
    for (;;) {
      const ssize_t count =
          recv(client.descriptor, bytes.data(), bytes.size(), 0);
      if (count == 0) {
        CloseClient(client);
        return;
      }
      if (count < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          return;
        }
        if (errno == EINTR) {
          continue;
        }
        CloseClient(client);
        return;
      }
      totalRead += static_cast<std::size_t>(count);
      // Bound one client's work per event-loop pass without treating a
      // legitimate burst as a protocol violation. Large topologies can emit
      // well over 64 KiB of whole-image writes at one watchdog refresh; leave
      // the remainder in the socket receive queue so other clients and safety
      // work still get serviced.
      const bool yieldAfterBatch = totalRead >= 64U * 1024U;
      std::vector<protocol::Frame> frames;
      if (!client.decoder.Feed(
              std::span<const std::uint8_t>{
                  bytes.data(), static_cast<std::size_t>(count)},
              &frames)) {
        CloseClient(client);
        return;
      }
      for (const protocol::Frame& frame : frames) {
        if (!ConsumeRateToken(client) || !HandleFrame(client, frame)) {
          return;
        }
      }
      if (yieldAfterBatch) {
        return;
      }
    }
  }

  [[nodiscard]] bool ConsumeRateToken(ClientConnection& client) {
    const auto now = std::chrono::steady_clock::now();
    const double elapsed =
        std::chrono::duration<double>(now - client.rateUpdated).count();
    client.rateUpdated = now;
    client.rateTokens =
        std::min(client.rateCapacity,
                 client.rateTokens +
                     (elapsed * client.rateRefillPerSecond));
    if (client.rateTokens < 1.0) {
      client.closeAfterWrite = true;
      QueueAck(client, protocol::MessageType::Error, 0,
               protocol::AckStatus::QueueFull, 0, true);
      return false;
    }
    client.rateTokens -= 1.0;
    return true;
  }

  [[nodiscard]] bool HandleFrame(ClientConnection& client,
                                 const protocol::Frame& frame) {
    if ((frame.requestId == 0 &&
         frame.type != protocol::MessageType::Hello) ||
        static_cast<std::uint8_t>(frame.type) >
            static_cast<std::uint8_t>(
                protocol::MessageType::AdapterRescan)) {
      CloseClient(client);
      return false;
    }
    if (!client.helloReceived &&
        frame.type != protocol::MessageType::Hello) {
      CloseClient(client);
      return false;
    }
    if (client.helloReceived &&
        frame.type == protocol::MessageType::Hello) {
      CloseClient(client);
      return false;
    }

    switch (frame.type) {
      case protocol::MessageType::Hello:
        return HandleHello(client, frame);
      case protocol::MessageType::Heartbeat:
        return HandleHeartbeat(client, frame);
      case protocol::MessageType::OutputEnable:
        return HandleOutputEnable(client, frame);
      case protocol::MessageType::OutputWrite:
        return HandleOutputWrite(client, frame);
      case protocol::MessageType::ClearCounters:
        return HandleClearCounters(client, frame);
      case protocol::MessageType::ReleaseControl:
        return HandleRelease(client, frame);
      case protocol::MessageType::SubscribeInputs:
        return HandleSubscribeInputs(client, frame);
      case protocol::MessageType::AdapterUnlock:
        return HandleAdapterUnlock(client, frame);
      case protocol::MessageType::AdapterRescan:
        return HandleAdapterRescan(client, frame);
      default:
        CloseClient(client);
        return false;
    }
  }

  [[nodiscard]] bool HandleHello(ClientConnection& client,
                                 const protocol::Frame& frame) {
    protocol::HelloPayload hello;
    if (!protocol::DecodeHelloPayload(frame.payload, &hello)) {
      CloseClient(client);
      return false;
    }
    protocol::ClientRole granted = protocol::ClientRole::Observer;
    if (hello.role == protocol::ClientRole::Controller &&
        ControllerAuthorized(client)) {
      const SafetyGate::Result acquisition =
          safety_.Acquire(client.id, client.pid,
                          std::chrono::steady_clock::now());
      if (acquisition.status == protocol::AckStatus::Ok) {
        granted = protocol::ClientRole::Controller;
        std::size_t outputTargets = 0;
        for (const InterfaceConfig& mapping :
             configuration_.interfaces) {
          outputTargets += static_cast<std::size_t>(
              std::count_if(
                  mapping.expectedSubDevices.begin(),
                  mapping.expectedSubDevices.end(),
                  [](const ExpectedSubDevice& subDevice) {
                    return subDevice.outputBytes.value_or(0) != 0;
                  }));
        }
        const ControllerRateLimit controllerRate =
            CalculateControllerRateLimit(
                outputTargets,
                configuration_.outputCommandTimeout);
        client.rateCapacity =
            static_cast<double>(controllerRate.burstFrames);
        client.rateRefillPerSecond =
            static_cast<double>(controllerRate.framesPerSecond);
        client.rateTokens = client.rateCapacity;
        ApplySafetyToBuses();
      }
    }
    client.helloReceived = true;
    client.role = granted;
    client.subscribeInputs = hello.subscribeInputs;
    client.inputPeriodMs = hello.inputPeriodMs;
    UpdateInputPublicationDemand();
    protocol::HelloAckPayload acknowledgement{
        .grantedRole = granted,
        .outputsEnabled = safety_.outputsEnabled(),
        .heartbeatTimeoutMs = static_cast<std::uint16_t>(
            configuration_.heartbeatTimeout.count()),
        .epoch = safety_.epoch(),
        .peerPid = client.pid,
        .capabilities =
            OutputCapabilityAvailable()
                ? kCapabilities
                : (kCapabilities & ~protocol::CapabilityOutputs),
    };
    return QueueFrame(
        client,
        protocol::EncodeFrame(
            protocol::MessageType::HelloAck, frame.requestId,
            protocol::EncodeHelloAckPayload(acknowledgement)),
        true);
  }

  [[nodiscard]] bool HandleHeartbeat(ClientConnection& client,
                                     const protocol::Frame& frame) {
    std::uint64_t epoch = 0;
    if (!protocol::DecodeEpochPayload(frame.payload, &epoch)) {
      return QueueAck(client, protocol::MessageType::Error,
                      frame.requestId,
                      protocol::AckStatus::Malformed, 0, true);
    }
    const SafetyGate::Result result = safety_.Heartbeat(
        client.id, epoch, std::chrono::steady_clock::now());
    return QueueAck(client, protocol::MessageType::Ack,
                    frame.requestId, result.status, 0, true);
  }

  [[nodiscard]] bool HandleOutputEnable(
      ClientConnection& client, const protocol::Frame& frame) {
    protocol::OutputEnablePayload request;
    if (!protocol::DecodeOutputEnablePayload(frame.payload, &request)) {
      return QueueAck(client, protocol::MessageType::Error,
                      frame.requestId,
                      protocol::AckStatus::Malformed, 0, true);
    }
    const SafetyGate::Result result = safety_.SetOutputEnabled(
        client.id, request.epoch, request.enabled,
        AllBusesOperational(), std::chrono::steady_clock::now());
    ApplySafetyToBuses();
    const bool queued =
        QueueAck(client, protocol::MessageType::Ack, frame.requestId,
                 result.status, 0, true);
    if (result.status == protocol::AckStatus::Ok &&
        !request.enabled) {
      BroadcastOutputsDisabled(
          protocol::DisableReason::Requested, 0xffffU);
      client.closeAfterWrite = true;
    }
    return queued;
  }

  [[nodiscard]] bool HandleOutputWrite(
      ClientConnection& client, const protocol::Frame& frame) {
    protocol::OutputWritePayload request;
    if (!protocol::DecodeOutputWritePayload(frame.payload, &request)) {
      return QueueAck(client, protocol::MessageType::Error,
                      frame.requestId,
                      protocol::AckStatus::Malformed, 0, true);
    }
    const SafetyGate::Result authorization =
        safety_.AuthorizeWrite(client.id, request.epoch);
    if (authorization.status != protocol::AckStatus::Ok) {
      return QueueAck(client, protocol::MessageType::Ack,
                      frame.requestId, authorization.status, 0, true);
    }
    if (request.busIndex >= buses_.size()) {
      return QueueAck(client, protocol::MessageType::Ack,
                      frame.requestId,
                      protocol::AckStatus::BadTarget, 0, true);
    }
    const protocol::AckStatus bounds =
        buses_[request.busIndex]->ValidateOutput(
            request.subDeviceIndex, request.offset, request.data);
    if (bounds != protocol::AckStatus::Ok) {
      return QueueAck(client, protocol::MessageType::Ack,
                      frame.requestId, bounds, 0, true);
    }
    OutputCommand command;
    command.epoch = request.epoch;
    command.subDeviceIndex = request.subDeviceIndex;
    command.offset = request.offset;
    command.length =
        static_cast<std::uint16_t>(request.data.size());
    command.acceptedMonotonicUs = MonotonicMicros();
    if (command.acceptedMonotonicUs == 0) {
      return QueueAck(client, protocol::MessageType::Ack,
                      frame.requestId,
                      protocol::AckStatus::InternalError, 0, true);
    }
    std::copy(request.data.begin(), request.data.end(),
              command.data.begin());
    const protocol::AckStatus queued =
        buses_[request.busIndex]->QueueOutput(command)
            ? protocol::AckStatus::Ok
            : protocol::AckStatus::QueueFull;
    if (queued == protocol::AckStatus::Ok) {
      const SafetyGate::Result refreshed =
          safety_.RecordOutputWrite(
              client.id, request.epoch,
              std::chrono::steady_clock::now());
      if (refreshed.status != protocol::AckStatus::Ok) {
        return QueueAck(client, protocol::MessageType::Ack,
                        frame.requestId, refreshed.status, 0, true);
      }
    }
    return QueueAck(client, protocol::MessageType::Ack,
                    frame.requestId, queued, 0, true);
  }

  [[nodiscard]] bool HandleClearCounters(
      ClientConnection& client, const protocol::Frame& frame) {
    if (!frame.payload.empty()) {
      return QueueAck(client, protocol::MessageType::Error,
                      frame.requestId,
                      protocol::AckStatus::Malformed, 0, true);
    }
    for (const auto& bus : buses_) {
      bus->ResetCounters();
    }
    return QueueAck(client, protocol::MessageType::Ack,
                    frame.requestId, protocol::AckStatus::Ok, 0, true);
  }

  [[nodiscard]] bool HandleRelease(ClientConnection& client,
                                   const protocol::Frame& frame) {
    std::uint64_t epoch = 0;
    if (!protocol::DecodeEpochPayload(frame.payload, &epoch)) {
      return QueueAck(client, protocol::MessageType::Error,
                      frame.requestId,
                      protocol::AckStatus::Malformed, 0, true);
    }
    const SafetyGate::Result result =
        safety_.Release(client.id, epoch);
    const bool queued =
        QueueAck(client, protocol::MessageType::Ack,
                 frame.requestId, result.status, 0, true);
    if (result.epochChanged) {
      ApplySafetyToBuses();
      BroadcastOutputsDisabled(
          protocol::DisableReason::Requested, 0xffffU);
      client.closeAfterWrite = true;
    }
    return queued;
  }

  [[nodiscard]] bool HandleSubscribeInputs(
      ClientConnection& client, const protocol::Frame& frame) {
    protocol::SubscribeInputsPayload subscription;
    if (!protocol::DecodeSubscribeInputsPayload(frame.payload,
                                                &subscription)) {
      return QueueAck(client, protocol::MessageType::Error,
                      frame.requestId,
                      protocol::AckStatus::Malformed, 0, true);
    }
    client.subscribeInputs = subscription.enabled;
    client.inputPeriodMs = subscription.periodMs;
    client.lastInputSequence.fill(0);
    client.selectedInputSequence.fill(0);
    UpdateInputPublicationDemand();
    return QueueAck(client, protocol::MessageType::Ack,
                    frame.requestId, protocol::AckStatus::Ok, 0, true);
  }

  [[nodiscard]] bool HandleAdapterUnlock(
      ClientConnection& client, const protocol::Frame& frame) {
    protocol::AdapterUnlockPayload request;
    if (!protocol::DecodeAdapterUnlockPayload(frame.payload, &request)) {
      return QueueAck(client, protocol::MessageType::Error,
                      frame.requestId,
                      protocol::AckStatus::Malformed, 0, true);
    }
    const SafetyGate::Result authorization =
        safety_.AuthorizeController(client.id, request.epoch);
    if (authorization.status != protocol::AckStatus::Ok) {
      return QueueAck(client, protocol::MessageType::Ack,
                      frame.requestId, authorization.status, 0, true);
    }
    if (request.busIndex >= adapters_.size()) {
      return QueueAck(client, protocol::MessageType::Ack,
                      frame.requestId,
                      protocol::AckStatus::BadTarget, 0, true);
    }
    adapters_[request.busIndex].runtimeUnlocked = true;
    const bool queued =
        QueueAck(client, protocol::MessageType::Ack, frame.requestId,
                 protocol::AckStatus::Ok, 0, true);
    AdvanceEpoch(protocol::DisableReason::AdapterMismatch,
                 request.busIndex);
    forceAdapterScan_ = true;
    return queued;
  }

  [[nodiscard]] bool HandleAdapterRescan(
      ClientConnection& client, const protocol::Frame& frame) {
    if (!frame.payload.empty()) {
      return QueueAck(client, protocol::MessageType::Error,
                      frame.requestId,
                      protocol::AckStatus::Malformed, 0, true);
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= nextObserverRescanAllowed_) {
      forceAdapterScan_ = true;
      nextObserverRescanAllowed_ = now + std::chrono::milliseconds{250};
    }
    return QueueAck(client, protocol::MessageType::Ack,
                    frame.requestId, protocol::AckStatus::Ok, 0, true);
  }

  bool QueueAck(ClientConnection& client, protocol::MessageType type,
                std::uint32_t requestId, protocol::AckStatus status,
                std::uint32_t detail, bool required) {
    return QueueFrame(
        client,
        protocol::EncodeFrame(
            type, requestId,
            protocol::EncodeAckPayload(status, detail)),
        required);
  }

  bool QueueFrame(ClientConnection& client,
                  std::vector<std::uint8_t> frame, bool required) {
    try {
      return QueueFrame(
          client,
          std::make_shared<const std::vector<std::uint8_t>>(
              std::move(frame)),
          required);
    } catch (const std::bad_alloc&) {
      if (required) {
        CloseClient(client);
      }
      return false;
    }
  }

  bool QueueFrame(ClientConnection& client,
                  const SharedFrame& frame, bool required) {
    if (client.descriptor < 0 || !frame || frame->empty() ||
        frame->size() >
            kMaximumClientOutputBytes -
                std::min(client.outgoingBytes,
                         kMaximumClientOutputBytes)) {
      if (required) {
        CloseClient(client);
      } else {
        const auto now = std::chrono::steady_clock::now();
        if (client.optionalQueueDrops == 0) {
          client.firstOptionalDrop = now;
        }
        ++client.optionalQueueDrops;
        if (client.optionalQueueDrops >= 100 ||
            now - client.firstOptionalDrop >= std::chrono::seconds{5}) {
          CloseClient(client);
        }
      }
      return false;
    }
    if (!required && client.optionalQueueDrops != 0 &&
        client.outgoingBytes < kMaximumClientOutputBytes / 4U) {
      client.optionalQueueDrops = 0;
    }
    client.outgoingBytes += frame->size();
    client.outgoing.emplace_back(frame);
    return true;
  }

  bool QueueFramesAtomically(
      ClientConnection& client,
      const std::vector<SharedFrame>& frames,
      std::size_t totalBytes) {
    if (client.descriptor < 0 ||
        totalBytes >
            kMaximumClientOutputBytes -
                std::min(client.outgoingBytes,
                         kMaximumClientOutputBytes)) {
      const auto now = std::chrono::steady_clock::now();
      if (client.optionalQueueDrops == 0) {
        client.firstOptionalDrop = now;
      }
      ++client.optionalQueueDrops;
      if (client.optionalQueueDrops >= 100 ||
          now - client.firstOptionalDrop >= std::chrono::seconds{5}) {
        CloseClient(client);
      }
      return false;
    }
    for (const SharedFrame& frame : frames) {
      client.outgoing.emplace_back(frame);
    }
    client.outgoingBytes += totalBytes;
    return true;
  }

  void FlushClient(ClientConnection& client) {
    while (client.descriptor >= 0 && !client.outgoing.empty()) {
      const std::vector<std::uint8_t>& frame =
          *client.outgoing.front();
      const ssize_t count =
          send(client.descriptor, frame.data() + client.outgoingOffset,
               frame.size() - client.outgoingOffset,
               MSG_NOSIGNAL);
      if (count < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          return;
        }
        if (errno == EINTR) {
          continue;
        }
        CloseClient(client);
        return;
      }
      if (count == 0) {
        CloseClient(client);
        return;
      }
      client.outgoingOffset += static_cast<std::size_t>(count);
      client.outgoingBytes -= static_cast<std::size_t>(count);
      if (client.outgoingOffset == frame.size()) {
        client.outgoing.pop_front();
        client.outgoingOffset = 0;
      }
    }
    if (client.closeAfterWrite && client.outgoing.empty()) {
      CloseClient(client);
    }
  }

  void CloseClient(ClientConnection& client) {
    if (client.descriptor < 0) {
      return;
    }
    const bool controller =
        safety_.controllerId().has_value() &&
        *safety_.controllerId() == client.id;
    close(client.descriptor);
    client.descriptor = -1;
    client.outgoing.clear();
    client.outgoingBytes = 0;
    UpdateInputPublicationDemand();
    if (controller) {
      const SafetyGate::Result result = safety_.Disconnect(client.id);
      if (result.epochChanged) {
        ApplySafetyToBuses();
        BroadcastOutputsDisabled(
            protocol::DisableReason::Disconnect, 0xffffU);
      }
    }
  }

  void RemoveClosedClients() {
    clients_.erase(
        std::remove_if(clients_.begin(), clients_.end(),
                       [](const ClientConnection& client) {
                         return client.descriptor < 0;
                       }),
        clients_.end());
  }

  [[nodiscard]] ClientConnection* FindClient(std::uint64_t id) {
    const auto client =
        std::find_if(clients_.begin(), clients_.end(),
                     [id](const ClientConnection& candidate) {
                       return candidate.id == id &&
                              candidate.descriptor >= 0;
                     });
    return client == clients_.end() ? nullptr : &*client;
  }

  [[nodiscard]] bool ControllerAuthorized(
      const ClientConnection& client) const {
    return std::find(configuration_.controllerUids.begin(),
                     configuration_.controllerUids.end(),
                     client.uid) !=
               configuration_.controllerUids.end() ||
           std::any_of(
               client.peerGroups.begin(), client.peerGroups.end(),
               [this](gid_t peerGroup) {
                 return std::binary_search(
                     configuration_.controllerGids.begin(),
                     configuration_.controllerGids.end(),
                     peerGroup);
               });
  }

  void UpdateInputPublicationDemand() {
    std::uint16_t fastestPeriodMs = 0;
    for (const ClientConnection& client : clients_) {
      if (!client.helloReceived || !client.subscribeInputs ||
          client.descriptor < 0) {
        continue;
      }
      if (fastestPeriodMs == 0 ||
          client.inputPeriodMs < fastestPeriodMs) {
        fastestPeriodMs = client.inputPeriodMs;
      }
    }
    const std::chrono::milliseconds period{fastestPeriodMs};
    for (const auto& bus : buses_) {
      bus->SetInputPublicationPeriod(period);
    }
  }

  void BroadcastOutputsDisabled(protocol::DisableReason reason,
                                std::uint16_t busIndex) {
    const SharedFrame frame =
        std::make_shared<const std::vector<std::uint8_t>>(
            protocol::EncodeFrame(
                protocol::MessageType::OutputsDisabled, 0,
                protocol::EncodeOutputsDisabledPayload(
                    {.reason = reason,
                     .busIndex = busIndex,
                     .epoch = safety_.epoch()})));
    for (ClientConnection& client : clients_) {
      if (client.helloReceived && client.descriptor >= 0) {
        QueueFrame(client, frame, false);
      }
    }
  }

  [[nodiscard]] protocol::StatusPayload Status() const {
    protocol::StatusPayload status;
    status.configuredBuses =
        static_cast<std::uint16_t>(buses_.size());
    status.epoch = safety_.epoch();
    status.controllerPid = safety_.controllerPid();
    status.monotonicTimestampUs = MonotonicMicros();
    status.aggregateState = static_cast<std::uint8_t>(
        buses_.empty() ? protocol::BusState::Starting
                       : protocol::BusState::Operational);

    bool allOperational = !buses_.empty();
    bool anyFault = false;
    bool anyInitializing = false;
    bool allRealtimeApplied = !buses_.empty();
    bool timingDegraded = false;
    bool fifo = !buses_.empty();
    for (std::size_t index = 0; index < buses_.size(); ++index) {
      const BusSnapshot bus = buses_[index]->Snapshot();
      if (bus.state != protocol::BusState::Operational) {
        allOperational = false;
      }
      const bool adapterFault =
          adapters_[index].resolution.state ==
              protocol::AdapterLockState::Mismatch ||
          adapters_[index].resolution.state ==
              protocol::AdapterLockState::Missing ||
          !adapters_[index].resolution.identity.has_value() ||
          !adapters_[index].resolution.identity->linkUp;
      anyFault = anyFault ||
                 bus.state == protocol::BusState::Fault ||
                 adapterFault;
      anyInitializing =
          anyInitializing || bus.reinitializing ||
          bus.state == protocol::BusState::Initializing;
      if (bus.schedulingMode != protocol::SchedulingMode::Fifo ||
          bus.realtimeErrorBits != 0) {
        allRealtimeApplied = false;
      }
      fifo = fifo &&
             bus.schedulingMode == protocol::SchedulingMode::Fifo;
      timingDegraded =
          timingDegraded || bus.cycleOverruns != 0 ||
          bus.currentJitterUs >
              static_cast<std::uint32_t>(
                  configuration_.cyclePeriod.count());
      status.activeAdapters = static_cast<std::uint16_t>(
          std::min<std::size_t>(
              std::numeric_limits<std::uint16_t>::max(),
              static_cast<std::size_t>(status.activeAdapters) +
                  (adapters_[index].resolution.identity.has_value() ? 1U
                                                                    : 0U)));
      status.subDevices = static_cast<std::uint16_t>(
          std::min<std::uint32_t>(
              std::numeric_limits<std::uint16_t>::max(),
              static_cast<std::uint32_t>(status.subDevices) +
                  bus.subDeviceCount));
      const std::uint32_t busFaultCount =
          bus.state == protocol::BusState::Fault
              ? std::max<std::uint32_t>(1U, bus.faults)
              : bus.faults;
      status.activeFaults = static_cast<std::uint16_t>(
          std::min<std::uint32_t>(
              std::numeric_limits<std::uint16_t>::max(),
              static_cast<std::uint32_t>(status.activeFaults) +
                  busFaultCount +
                  (adapterFault
                       ? 1U
                       : 0U)));
      status.lostFrames =
          status.lostFrames >
                  std::numeric_limits<std::uint64_t>::max() -
                      bus.lostFrames
              ? std::numeric_limits<std::uint64_t>::max()
              : status.lostFrames + bus.lostFrames;
      status.cycleOverruns =
          status.cycleOverruns >
                  std::numeric_limits<std::uint64_t>::max() -
                      bus.cycleOverruns
              ? std::numeric_limits<std::uint64_t>::max()
              : status.cycleOverruns + bus.cycleOverruns;
      status.maximumJitterUs =
          std::max(status.maximumJitterUs, bus.maximumJitterUs);
      status.currentJitterUs =
          std::max(status.currentJitterUs, bus.currentJitterUs);
      status.realtimeErrorBits |= bus.realtimeErrorBits;
    }

    if (allOperational) {
      status.flags |= protocol::StatusOperational;
    }
    if (safety_.outputsEnabled()) {
      status.flags |= protocol::StatusOutputsEnabled;
    }
    if (safety_.hasController()) {
      status.flags |= protocol::StatusControllerConnected;
    }
    if (preemptRtAvailable_) {
      status.flags |= protocol::StatusPreemptRtAvailable;
    }
    if (allRealtimeApplied) {
      status.flags |= protocol::StatusRealtimeApplied;
    }
    if (configuration_.realtime.requestFifo ||
        configuration_.realtime.requestMemoryLock ||
        configuration_.realtime.cpuAffinity.has_value()) {
      status.flags |= protocol::StatusRealtimeRequested;
    }
    if (timingDegraded) {
      status.flags |= protocol::StatusTimingDegraded;
    }
    if (anyInitializing) {
      status.flags |= protocol::StatusReinitializing;
    }
    if (anyFault) {
      status.aggregateState =
          static_cast<std::uint8_t>(protocol::BusState::Fault);
    } else if (anyInitializing) {
      status.aggregateState =
          static_cast<std::uint8_t>(
              protocol::BusState::Initializing);
    } else if (!allOperational && !buses_.empty()) {
      status.aggregateState =
          static_cast<std::uint8_t>(
              protocol::BusState::WaitingForLink);
    }
    status.schedulingMode =
        fifo ? protocol::SchedulingMode::Fifo
             : protocol::SchedulingMode::Other;
    return status;
  }

  void BroadcastStatus() {
    const SharedFrame frame =
        std::make_shared<const std::vector<std::uint8_t>>(
            protocol::EncodeFrame(
                protocol::MessageType::Status, 0,
                protocol::EncodeStatusPayload(Status())));
    for (ClientConnection& client : clients_) {
      if (client.helloReceived && client.descriptor >= 0) {
        QueueFrame(client, frame, false);
      }
    }
  }

  void BroadcastBusInformation() {
    for (std::size_t index = 0; index < buses_.size(); ++index) {
      const AdapterRuntime& adapter = adapters_[index];
      const BusSnapshot snapshot = buses_[index]->Snapshot();
      const AdapterIdentity* identity =
          adapter.resolution.identity.has_value()
              ? &*adapter.resolution.identity
              : nullptr;
      protocol::BusInfoPayload information{
          .busIndex = static_cast<std::uint16_t>(index),
          .state =
              adapter.resolution.state ==
                      protocol::AdapterLockState::Mismatch
                  ? protocol::BusState::Fault
                  : (identity == nullptr
                         ? protocol::BusState::WaitingForLink
                         : snapshot.state),
          .linkUp = identity != nullptr && identity->linkUp,
          .lockState = adapter.resolution.state,
          .lockReason = adapter.resolution.reason,
          .subDeviceCount = snapshot.subDeviceCount,
          .logicalName =
              configuration_.interfaces[index].logicalName,
          .physicalInterface =
              identity != nullptr ? identity->interfaceName
                                  : configuration_.interfaces[index]
                                        .physicalInterface,
          .idPath = identity != nullptr ? identity->idPath
                                        : std::string{},
          .permanentMac =
              identity != nullptr &&
                      identity->permanentMac.has_value()
                  ? *identity->permanentMac
                  : std::string{},
          .usbSerial =
              identity != nullptr && identity->usbSerial.has_value()
                  ? *identity->usbSerial
                  : std::string{},
          .usbVendorId =
              identity != nullptr &&
                      identity->usbVendorId.has_value()
                  ? *identity->usbVendorId
                  : std::string{},
          .usbProductId =
              identity != nullptr &&
                      identity->usbProductId.has_value()
                  ? *identity->usbProductId
                  : std::string{},
          .epoch = safety_.epoch(),
      };
      const std::vector<std::uint8_t> payload =
          protocol::EncodeBusInfoPayload(information);
      if (payload.empty()) {
        continue;
      }
      const SharedFrame frame =
          std::make_shared<const std::vector<std::uint8_t>>(
              protocol::EncodeFrame(
                  protocol::MessageType::BusInfo, 0, payload));
      for (ClientConnection& client : clients_) {
        if (client.helloReceived && client.descriptor >= 0) {
          QueueFrame(client, frame, false);
        }
      }
    }
  }

  void DrainInputs(std::chrono::steady_clock::time_point now) {
    for (std::size_t busIndex = 0; busIndex < buses_.size();
         ++busIndex) {
      const bool hasSubscriber =
          std::any_of(clients_.begin(), clients_.end(),
                      [](const ClientConnection& client) {
                        return client.helloReceived &&
                               client.subscribeInputs &&
                               client.descriptor >= 0;
                      });
      InputChunk chunk;
      while (buses_[busIndex]->PopInput(&chunk)) {
        InputPublicationAccumulator& publication =
            inputPublications_[busIndex];
        if (!hasSubscriber) {
          publication.Reset();
          continue;
        }
        if (chunk.snapshotChunkCount == 0 ||
            chunk.snapshotChunkCount > 128 ||
            chunk.snapshotChunkIndex >= chunk.snapshotChunkCount) {
          publication.Reset();
          continue;
        }
        if (chunk.snapshotChunkIndex == 0) {
          publication.Reset();
          publication.epoch = chunk.header.epoch;
          publication.cycleSequence =
              chunk.header.cycleSequence;
          publication.expectedChunks =
              chunk.snapshotChunkCount;
          publication.frames.reserve(
              chunk.snapshotChunkCount);
        }
        if (publication.epoch != chunk.header.epoch ||
            publication.cycleSequence !=
                chunk.header.cycleSequence ||
            publication.expectedChunks !=
                chunk.snapshotChunkCount ||
            publication.nextChunk !=
                chunk.snapshotChunkIndex) {
          publication.Reset();
          continue;
        }
        chunk.header.data.assign(chunk.data.begin(),
                                 chunk.data.begin() + chunk.length);
        const std::vector<std::uint8_t> payload =
            protocol::EncodePdoInputPayload(chunk.header);
        if (payload.empty()) {
          publication.Reset();
          continue;
        }
        SharedFrame frame =
            std::make_shared<const std::vector<std::uint8_t>>(
                protocol::EncodeFrame(
                    protocol::MessageType::PdoInput, 0, payload));
        if (!frame || frame->empty() ||
            frame->size() >
                kMaximumClientOutputBytes -
                    std::min(publication.totalBytes,
                             kMaximumClientOutputBytes)) {
          publication.Reset();
          continue;
        }
        publication.totalBytes += frame->size();
        publication.frames.emplace_back(std::move(frame));
        ++publication.nextChunk;
        if (publication.nextChunk !=
            publication.expectedChunks) {
          continue;
        }
        for (ClientConnection& client : clients_) {
          if (!client.helloReceived || !client.subscribeInputs ||
              client.descriptor < 0) {
            continue;
          }
          if (now < client.nextInputPublication[busIndex]) {
            continue;
          }
          client.nextInputPublication[busIndex] =
              now + std::chrono::milliseconds{
                        client.inputPeriodMs};
          if (publication.epoch == safety_.epoch()) {
            QueueFramesAtomically(
                client, publication.frames,
                publication.totalBytes);
          }
        }
        publication.Reset();
      }
    }
  }

  [[nodiscard]] bool ConfigurationChanged() const {
    try {
      return ReadBoundedFile(configurationPath_) !=
             initialConfigurationText_;
    } catch (const std::exception&) {
      return true;
    }
  }

  void FlushClientsFor(std::chrono::milliseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
      bool pending = false;
      for (ClientConnection& client : clients_) {
        if (client.descriptor >= 0 && !client.outgoing.empty()) {
          pending = true;
          FlushClient(client);
        }
      }
      if (!pending) {
        break;
      }
      std::this_thread::yield();
    }
  }

  DaemonConfig configuration_;
  std::filesystem::path configurationPath_;
  std::string initialConfigurationText_;
  std::filesystem::path socketPath_;
  SysfsAdapterScanner scanner_;
  EventLog log_;
  SafetyGate safety_;
  bool preemptRtAvailable_ = false;
  std::atomic<std::uint64_t> globalFaultedEpoch_{0};
  std::vector<std::unique_ptr<EthercatBus>> buses_;
  std::vector<AdapterRuntime> adapters_;
  std::vector<ClientConnection> clients_;
  std::array<InputPublicationAccumulator, kMaximumBuses>
      inputPublications_{};
  int listenDescriptor_ = -1;
  int lockDescriptor_ = -1;
  std::uint64_t nextClientId_ = 1;
  std::string lastScanError_;
  bool forceAdapterScan_ = false;
  std::chrono::steady_clock::time_point nextObserverRescanAllowed_{};
  bool shutdown_ = false;
  int exitCode_ = 0;
};

[[nodiscard]] std::filesystem::path EnvironmentPath(
    const char* name, std::string_view fallback) {
  const char* value = std::getenv(name);
  const std::string selected =
      value == nullptr || *value == '\0' ? std::string{fallback}
                                         : std::string{value};
  if (!IsSafeAbsolutePath(selected)) {
    throw std::runtime_error(std::string{name} +
                             " must be a safe absolute path");
  }
  return selected;
}

int RunMain(int argc, char** argv) {
  std::filesystem::path configurationPath =
      EnvironmentPath("EC_SYSTEMCORE_CONFIG", kDefaultConfigPath);
  bool validateOnly = false;
  bool resolveOnly = false;

  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if (argument == "--config") {
      if (++index >= argc || !IsSafeAbsolutePath(argv[index])) {
        throw std::runtime_error(
            "--config requires a safe absolute path");
      }
      configurationPath = argv[index];
    } else if (argument == "--validate-config") {
      validateOnly = true;
    } else if (argument == "--resolve-adapters") {
      resolveOnly = true;
    } else if (argument == "--version") {
#ifdef EC_SYSTEMCORE_VERSION
      std::cout << EC_SYSTEMCORE_VERSION << '\n';
#else
      std::cout << "unknown\n";
#endif
      return 0;
    } else if (!argument.empty() && argument.front() != '-' &&
               argc == 2) {
      if (!IsSafeAbsolutePath(argument)) {
        throw std::runtime_error(
            "configuration path must be a safe absolute path");
      }
      configurationPath = argument;
    } else {
      throw std::runtime_error("unknown command-line argument: " +
                               std::string{argument});
    }
  }
  if (validateOnly && resolveOnly) {
    throw std::runtime_error(
        "--validate-config and --resolve-adapters are mutually exclusive");
  }

  std::string configurationText;
  DaemonConfig configuration =
      LoadConfig(configurationPath, &configurationText);
  if (validateOnly) {
    std::cout << "configuration valid: "
              << configuration.interfaces.size()
              << " enabled buses\n";
    return 0;
  }

  const std::filesystem::path sysfsRoot =
      EnvironmentPath("EC_SYSTEMCORE_SYSFS_ROOT", "/sys");
  const std::filesystem::path udevDataRoot =
      EnvironmentPath("EC_SYSTEMCORE_UDEV_DATA_ROOT",
                      "/run/udev/data");
  if (resolveOnly) {
    SysfsAdapterScanner scanner{sysfsRoot, udevDataRoot};
    std::string scanError;
    const std::vector<AdapterIdentity> identities =
        scanner.Scan(&scanError);
    if (!scanError.empty()) {
      std::cerr << scanError << '\n';
    }
    bool allResolved = true;
    for (std::size_t index = 0;
         index < configuration.interfaces.size(); ++index) {
      const InterfaceConfig& mapping =
          configuration.interfaces[index];
      const AdapterResolution resolution = ResolveAdapter(
          identities, mapping.physicalInterface, mapping.lock);
      std::cout << index << '\t' << mapping.logicalName << '\t'
                << static_cast<unsigned int>(resolution.state) << '\t'
                << static_cast<unsigned int>(resolution.reason);
      if (resolution.identity.has_value()) {
        std::cout << '\t' << resolution.identity->interfaceName << '\t'
                  << resolution.identity->idPath;
      }
      std::cout << '\n';
      if (!resolution.identity.has_value() ||
          (resolution.state !=
               protocol::AdapterLockState::Unlocked &&
           resolution.state !=
               protocol::AdapterLockState::Matched)) {
        allResolved = false;
      }
    }
    return allResolved ? 0 : 2;
  }

  const std::filesystem::path socketPath =
      EnvironmentPath("EC_SYSTEMCORE_SOCKET", kDefaultSocketPath);
  Daemon daemon{std::move(configuration), configurationPath,
                std::move(configurationText), socketPath, sysfsRoot,
                udevDataRoot};
  return daemon.Run();
}

}  // namespace
}  // namespace ec_systemcore

int main(int argc, char** argv) {
  struct sigaction action {};
  action.sa_handler = ec_systemcore::SignalHandler;
  action.sa_flags = 0;
  if (sigemptyset(&action.sa_mask) != 0 ||
      sigaction(SIGINT, &action, nullptr) != 0 ||
      sigaction(SIGTERM, &action, nullptr) != 0) {
    std::cerr
        << "ec-systemcore-daemon: unable to install shutdown handlers\n";
    return 1;
  }
  struct sigaction ignorePipe {};
  ignorePipe.sa_handler = SIG_IGN;
  ignorePipe.sa_flags = 0;
  if (sigemptyset(&ignorePipe.sa_mask) != 0 ||
      sigaction(SIGPIPE, &ignorePipe, nullptr) != 0) {
    std::cerr
        << "ec-systemcore-daemon: unable to ignore SIGPIPE\n";
    return 1;
  }

  try {
    return ec_systemcore::RunMain(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "ec-systemcore-daemon: " << error.what() << '\n';
    return 1;
  }
}
