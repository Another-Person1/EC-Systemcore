#pragma once

#include "ec_systemcore/protocol.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ec_systemcore {

struct AdapterIdentityLock {
  std::string idPath;
  std::optional<std::string> permanentMac;
  std::optional<std::string> usbSerial;
  std::optional<std::string> usbVendorId;
  std::optional<std::string> usbProductId;

  [[nodiscard]] bool pathOnly() const {
    return !permanentMac.has_value() && !usbSerial.has_value() &&
           !usbVendorId.has_value() && !usbProductId.has_value();
  }
};

struct AdapterIdentity {
  std::string interfaceName;
  std::string idPath;
  std::optional<std::string> permanentMac;
  std::optional<std::string> usbSerial;
  std::optional<std::string> usbVendorId;
  std::optional<std::string> usbProductId;
  bool ethernet = false;
  bool linkUp = false;
};

struct AdapterResolution {
  protocol::AdapterLockState state =
      protocol::AdapterLockState::Missing;
  protocol::AdapterLockReason reason =
      protocol::AdapterLockReason::InterfaceMissing;
  std::optional<AdapterIdentity> identity;
};

inline std::string TrimAscii(std::string value) {
  const auto notSpace = [](unsigned char character) {
    return std::isspace(character) == 0;
  };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), notSpace));
  value.erase(
      std::find_if(value.rbegin(), value.rend(), notSpace).base(),
      value.end());
  return value;
}

inline std::optional<std::string> NormalizeMac(std::string_view input) {
  std::string hexadecimal;
  hexadecimal.reserve(12);
  for (const char character : input) {
    const auto value = static_cast<unsigned char>(character);
    if (std::isxdigit(value) != 0) {
      hexadecimal.push_back(
          static_cast<char>(std::tolower(static_cast<unsigned char>(value))));
    } else if (character != ':' && character != '-' &&
               std::isspace(value) == 0) {
      return std::nullopt;
    }
  }
  if (hexadecimal.size() != 12) {
    return std::nullopt;
  }
  const unsigned int firstByte =
      static_cast<unsigned int>(
          std::isdigit(static_cast<unsigned char>(hexadecimal[0])) != 0
              ? hexadecimal[0] - '0'
              : hexadecimal[0] - 'a' + 10) *
          16U +
      static_cast<unsigned int>(
          std::isdigit(static_cast<unsigned char>(hexadecimal[1])) != 0
              ? hexadecimal[1] - '0'
              : hexadecimal[1] - 'a' + 10);
  const bool allZero =
      std::all_of(hexadecimal.begin(), hexadecimal.end(),
                  [](char character) { return character == '0'; });
  const bool allBroadcast =
      std::all_of(hexadecimal.begin(), hexadecimal.end(),
                  [](char character) { return character == 'f'; });
  if ((firstByte & 1U) != 0U || allZero || allBroadcast) {
    return std::nullopt;
  }

  std::string normalized;
  normalized.reserve(17);
  for (std::size_t index = 0; index < hexadecimal.size(); index += 2) {
    if (!normalized.empty()) {
      normalized.push_back(':');
    }
    normalized.append(hexadecimal, index, 2);
  }
  return normalized;
}

inline std::optional<std::string> NormalizeUsbId(std::string_view input) {
  std::string value = TrimAscii(std::string{input});
  if (value.starts_with("0x") || value.starts_with("0X")) {
    value.erase(0, 2);
  }
  if (value.size() != 4 ||
      !std::all_of(value.begin(), value.end(), [](char character) {
        return std::isxdigit(static_cast<unsigned char>(character)) != 0;
      })) {
    return std::nullopt;
  }
  std::transform(value.begin(), value.end(), value.begin(), [](char character) {
    return static_cast<char>(
        std::tolower(static_cast<unsigned char>(character)));
  });
  return value;
}

inline bool ValidateAdapterLock(const AdapterIdentityLock& lock,
                                std::string* error) {
  const auto fail = [error](const std::string& message) {
    if (error != nullptr) {
      *error = message;
    }
    return false;
  };
  const auto printableAscii = [](std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](char character) {
      const auto byte = static_cast<unsigned char>(character);
      return byte >= 0x20U && byte <= 0x7eU;
    });
  };
  if (lock.idPath.empty() || lock.idPath.size() > 1024 ||
      !printableAscii(lock.idPath)) {
    return fail("adapter lock id_path must contain 1..1024 bytes");
  }
  if (lock.permanentMac.has_value() &&
      !NormalizeMac(*lock.permanentMac).has_value()) {
    return fail("adapter lock permanent_mac is invalid");
  }
  if (lock.usbSerial.has_value() &&
      (lock.usbSerial->empty() || lock.usbSerial->size() > 256 ||
       !printableAscii(*lock.usbSerial))) {
    return fail("adapter lock usb_serial is invalid");
  }
  if (lock.usbVendorId.has_value() &&
      !NormalizeUsbId(*lock.usbVendorId).has_value()) {
    return fail("adapter lock usb_vendor_id is invalid");
  }
  if (lock.usbProductId.has_value() &&
      !NormalizeUsbId(*lock.usbProductId).has_value()) {
    return fail("adapter lock usb_product_id is invalid");
  }
  return true;
}

class SysfsAdapterScanner final {
 public:
  explicit SysfsAdapterScanner(
      std::filesystem::path sysfsRoot,
      std::filesystem::path udevDataRoot = "/run/udev/data")
      : root_(std::move(sysfsRoot)),
        udevDataRoot_(std::move(udevDataRoot)) {}

  [[nodiscard]] const std::filesystem::path& root() const { return root_; }

  [[nodiscard]] std::vector<AdapterIdentity> Scan(
      std::string* error = nullptr) const {
    std::vector<AdapterIdentity> adapters;
    std::error_code filesystemError;
    const std::filesystem::path networkRoot = root_ / "class" / "net";
    std::filesystem::directory_iterator iterator{networkRoot, filesystemError};
    if (filesystemError) {
      if (error != nullptr) {
        *error = "unable to enumerate " + networkRoot.string() + ": " +
                 filesystemError.message();
      }
      return adapters;
    }

    while (iterator != std::filesystem::directory_iterator{}) {
      const std::filesystem::directory_entry entry = *iterator;
      iterator.increment(filesystemError);
      if (filesystemError) {
        if (error != nullptr) {
          *error = "network-interface enumeration changed during scan: " +
                   filesystemError.message();
        }
        filesystemError.clear();
        break;
      }
      if (adapters.size() >= 256) {
        if (error != nullptr) {
          *error = "sysfs exposes more than 256 network interfaces";
        }
        break;
      }
      const std::string name = entry.path().filename().string();
      if (name.empty() || name.size() > 255 || name == "lo") {
        continue;
      }

      AdapterIdentity identity;
      identity.interfaceName = name;
      const std::optional<std::string> type =
          ReadFirstLine(entry.path() / "type");
      identity.ethernet = type.has_value() && TrimAscii(*type) == "1";
      if (!identity.ethernet) {
        continue;
      }
      const std::optional<bool> carrier =
          ReadCarrier(entry.path() / "carrier");
      identity.linkUp =
          carrier.value_or(ReadOperstateUp(entry.path() / "operstate"));

      std::error_code canonicalError;
      std::filesystem::path device =
          std::filesystem::weakly_canonical(entry.path() / "device",
                                            canonicalError);
      if (canonicalError || !IsWithinRoot(device)) {
        device.clear();
      }
      identity.idPath = ReadIdPath(entry.path(), device);
      if (!IsBoundedPrintableAscii(identity.idPath, 1024)) {
        identity.idPath.clear();
      }
      identity.permanentMac = ReadPermanentMac(entry.path());
      identity.usbSerial = ReadParentAttribute(device, "serial", false);
      if (identity.usbSerial.has_value() &&
          !IsBoundedPrintableAscii(*identity.usbSerial, 256)) {
        identity.usbSerial.reset();
      }
      identity.usbVendorId = ReadParentAttribute(device, "idVendor", true);
      identity.usbProductId = ReadParentAttribute(device, "idProduct", true);
      adapters.emplace_back(std::move(identity));
    }

    std::sort(adapters.begin(), adapters.end(),
              [](const AdapterIdentity& left, const AdapterIdentity& right) {
                if (left.idPath != right.idPath) {
                  return left.idPath < right.idPath;
                }
                return left.interfaceName < right.interfaceName;
              });
    return adapters;
  }

 private:
  [[nodiscard]] static bool IsBoundedPrintableAscii(
      std::string_view value, std::size_t maximumBytes) {
    return !value.empty() && value.size() <= maximumBytes &&
           std::all_of(value.begin(), value.end(), [](char character) {
             const auto byte = static_cast<unsigned char>(character);
             return byte >= 0x20U && byte <= 0x7eU;
           });
  }

  static std::optional<std::string> ReadFirstLine(
      const std::filesystem::path& path) {
    std::ifstream stream(path);
    std::string value;
    if (!stream || !std::getline(stream, value) || value.size() > 4096) {
      return std::nullopt;
    }
    value = TrimAscii(std::move(value));
    if (value.empty()) {
      return std::nullopt;
    }
    return value;
  }

  static std::optional<bool> ReadCarrier(
      const std::filesystem::path& path) {
    const auto value = ReadFirstLine(path);
    if (!value.has_value()) {
      return std::nullopt;
    }
    if (*value == "1" || *value == "up") {
      return true;
    }
    if (*value == "0" || *value == "down") {
      return false;
    }
    return std::nullopt;
  }

  static bool ReadOperstateUp(const std::filesystem::path& path) {
    const auto value = ReadFirstLine(path);
    return value.has_value() && *value == "up";
  }

  [[nodiscard]] bool IsWithinRoot(
      const std::filesystem::path& candidate) const {
    std::error_code rootError;
    const std::filesystem::path canonicalRoot =
        std::filesystem::weakly_canonical(root_, rootError);
    if (rootError) {
      return false;
    }
    std::error_code relativeError;
    const std::filesystem::path relative =
        std::filesystem::relative(candidate, canonicalRoot, relativeError);
    if (relativeError || relative.empty()) {
      return candidate == canonicalRoot;
    }
    const std::string relativeText = relative.generic_string();
    return relativeText != ".." && !relativeText.starts_with("../");
  }

  std::string ReadIdPath(const std::filesystem::path& interfacePath,
                         const std::filesystem::path& device) const {
    if (const auto ifindex = ReadFirstLine(interfacePath / "ifindex");
        ifindex.has_value() && !ifindex->empty() &&
        std::all_of(ifindex->begin(), ifindex->end(), [](char character) {
          return character >= '0' && character <= '9';
        })) {
      if (const auto udevRecord =
              ReadTextFile(udevDataRoot_ / ("n" + *ifindex));
          udevRecord.has_value()) {
        constexpr std::string_view prefix = "E:ID_PATH=";
        std::istringstream lines(*udevRecord);
        std::string line;
        while (std::getline(lines, line)) {
          if (line.starts_with(prefix) && line.size() > prefix.size()) {
            return TrimAscii(line.substr(prefix.size()));
          }
        }
      }
    }
    if (const auto explicitPath = ReadFirstLine(interfacePath / "id_path");
        explicitPath.has_value()) {
      return *explicitPath;
    }
    if (!device.empty()) {
      if (const auto explicitPath = ReadFirstLine(device / "id_path");
          explicitPath.has_value()) {
        return *explicitPath;
      }
      if (const auto uevent = ReadTextFile(device / "uevent");
          uevent.has_value()) {
        constexpr std::string_view prefix = "ID_PATH=";
        std::istringstream lines(*uevent);
        std::string line;
        while (std::getline(lines, line)) {
          if (line.starts_with(prefix)) {
            return line.substr(prefix.size());
          }
        }
      }

      std::error_code relativeError;
      const std::filesystem::path relative =
          std::filesystem::relative(device, root_, relativeError);
      if (!relativeError && !relative.empty() &&
          !relative.generic_string().starts_with("..")) {
        return "sysfs:" + relative.generic_string();
      }
      return "sysfs:" + device.generic_string();
    }
    return {};
  }

  static std::optional<std::string> ReadPermanentMac(
      const std::filesystem::path& interfacePath) {
    if (const auto explicitAddress =
            ReadFirstLine(interfacePath / "perm_address");
        explicitAddress.has_value()) {
      return NormalizeMac(*explicitAddress);
    }

    const auto assignmentType =
        ReadFirstLine(interfacePath / "addr_assign_type");
    if (assignmentType.has_value() && *assignmentType != "0") {
      return std::nullopt;
    }
    const auto address = ReadFirstLine(interfacePath / "address");
    return address.has_value() ? NormalizeMac(*address) : std::nullopt;
  }

  static std::optional<std::string> ReadTextFile(
      const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
      return std::nullopt;
    }
    std::string value;
    value.resize(4097);
    stream.read(value.data(), static_cast<std::streamsize>(value.size()));
    const std::streamsize bytes = stream.gcount();
    if (bytes <= 0 || bytes > 4096 ||
        (bytes == 4097 && !stream.eof())) {
      return std::nullopt;
    }
    value.resize(static_cast<std::size_t>(bytes));
    return value;
  }

  std::optional<std::string> ReadParentAttribute(
      std::filesystem::path device, const std::string& attribute,
      bool hexadecimal) const {
    for (unsigned int depth = 0;
         !device.empty() && depth < 16 && IsWithinRoot(device); ++depth) {
      if (const auto value = ReadFirstLine(device / attribute);
          value.has_value()) {
        if (hexadecimal) {
          return NormalizeUsbId(*value);
        }
        return *value;
      }
      const auto parent = device.parent_path();
      if (parent == device) {
        break;
      }
      device = parent;
    }
    return std::nullopt;
  }

  std::filesystem::path root_;
  std::filesystem::path udevDataRoot_;
};

inline AdapterResolution ResolveAdapter(
    const std::vector<AdapterIdentity>& adapters,
    const std::string& preferredInterface,
    const std::optional<AdapterIdentityLock>& lock,
    bool runtimeUnlocked = false) {
  const auto findPreferred = [&]() -> const AdapterIdentity* {
    const auto preferred =
        std::find_if(adapters.begin(), adapters.end(),
                     [&preferredInterface](const AdapterIdentity& identity) {
                       return identity.interfaceName == preferredInterface;
                     });
    return preferred == adapters.end() ? nullptr : &*preferred;
  };

  if (!lock.has_value() || runtimeUnlocked) {
    const AdapterIdentity* selected = findPreferred();
    if (selected == nullptr) {
      return {};
    }
    return {
        .state = runtimeUnlocked
                     ? protocol::AdapterLockState::RuntimeUnlocked
                     : protocol::AdapterLockState::Unlocked,
        .reason = protocol::AdapterLockReason::None,
        .identity = *selected,
    };
  }

  const AdapterIdentityLock& expected = *lock;
  const auto pathMatch =
      std::find_if(adapters.begin(), adapters.end(),
                   [&expected](const AdapterIdentity& identity) {
                     return identity.idPath == expected.idPath;
                   });
  if (pathMatch == adapters.end()) {
    const AdapterIdentity* preferred = findPreferred();
    const auto normalizedExpectedMac =
        expected.permanentMac.has_value()
            ? NormalizeMac(*expected.permanentMac)
            : std::optional<std::string>{};
    const bool relatedIdentity =
        std::any_of(adapters.begin(), adapters.end(),
                    [&expected, &normalizedExpectedMac](
                        const AdapterIdentity& identity) {
                      return (normalizedExpectedMac.has_value() &&
                              identity.permanentMac ==
                                  normalizedExpectedMac) ||
                             (expected.usbSerial.has_value() &&
                              identity.usbSerial == expected.usbSerial);
                    });
    return {
        .state = preferred != nullptr || relatedIdentity
                     ? protocol::AdapterLockState::Mismatch
                     : protocol::AdapterLockState::Missing,
        .reason = preferred != nullptr || relatedIdentity
                      ? protocol::AdapterLockReason::IdPathMismatch
                      : protocol::AdapterLockReason::InterfaceMissing,
        .identity = preferred != nullptr
                        ? std::optional<AdapterIdentity>{*preferred}
                        : std::nullopt,
    };
  }
  if (std::count_if(adapters.begin(), adapters.end(),
                    [&expected](const AdapterIdentity& identity) {
                      return identity.idPath == expected.idPath;
                    }) != 1) {
    return {
        .state = protocol::AdapterLockState::Mismatch,
        .reason =
            protocol::AdapterLockReason::IdentityUnavailable,
        .identity = std::nullopt,
    };
  }

  const auto mismatch = [&pathMatch](const auto& expectedValue,
                                     const auto& observedValue,
                                     protocol::AdapterLockReason reason)
      -> std::optional<AdapterResolution> {
    if (!expectedValue.has_value()) {
      return std::nullopt;
    }
    if (!observedValue.has_value()) {
      return AdapterResolution{
          .state = protocol::AdapterLockState::Mismatch,
          .reason = protocol::AdapterLockReason::IdentityUnavailable,
          .identity = *pathMatch,
      };
    }
    if (*expectedValue != *observedValue) {
      return AdapterResolution{
          .state = protocol::AdapterLockState::Mismatch,
          .reason = reason,
          .identity = *pathMatch,
      };
    }
    return std::nullopt;
  };

  const auto normalizedMac =
      expected.permanentMac.has_value()
          ? NormalizeMac(*expected.permanentMac)
          : std::optional<std::string>{};
  const auto normalizedVendor =
      expected.usbVendorId.has_value()
          ? NormalizeUsbId(*expected.usbVendorId)
          : std::optional<std::string>{};
  const auto normalizedProduct =
      expected.usbProductId.has_value()
          ? NormalizeUsbId(*expected.usbProductId)
          : std::optional<std::string>{};
  if (const auto result =
          mismatch(normalizedMac, pathMatch->permanentMac,
                   protocol::AdapterLockReason::PermanentMacMismatch);
      result.has_value()) {
    return *result;
  }
  if (const auto result =
          mismatch(expected.usbSerial, pathMatch->usbSerial,
                   protocol::AdapterLockReason::UsbSerialMismatch);
      result.has_value()) {
    return *result;
  }
  if (const auto result =
          mismatch(normalizedVendor, pathMatch->usbVendorId,
                   protocol::AdapterLockReason::UsbVendorMismatch);
      result.has_value()) {
    return *result;
  }
  if (const auto result =
          mismatch(normalizedProduct, pathMatch->usbProductId,
                   protocol::AdapterLockReason::UsbProductMismatch);
      result.has_value()) {
    return *result;
  }

  return {
      .state = protocol::AdapterLockState::Matched,
      .reason = expected.pathOnly()
                    ? protocol::AdapterLockReason::PathOnlyIdentity
                    : protocol::AdapterLockReason::None,
      .identity = *pathMatch,
  };
}

}  // namespace ec_systemcore
