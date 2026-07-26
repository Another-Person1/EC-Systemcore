#include "ec_systemcore/adapter_identity.hpp"

#include <unistd.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void Require(bool condition, const char* expression, int line) {
  if (!condition) {
    throw std::runtime_error("check failed at line " +
                             std::to_string(line) + ": " + expression);
  }
}

#define CHECK(expression) Require((expression), #expression, __LINE__)

void Write(const std::filesystem::path& path,
           const std::string& value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("unable to create fixture " +
                             path.string());
  }
  output << value;
}

struct Fixture {
  Fixture() {
    std::array<char, 64> rootTemplate{};
    std::string rootPattern = "/tmp/ec-systemcore-sysfs-XXXXXX";
    std::copy(rootPattern.begin(), rootPattern.end(),
              rootTemplate.begin());
    if (mkdtemp(rootTemplate.data()) == nullptr) {
      throw std::runtime_error("mkdtemp failed");
    }
    root = rootTemplate.data();

    std::array<char, 64> outsideTemplate{};
    std::string outsidePattern =
        "/tmp/ec-systemcore-outside-XXXXXX";
    std::copy(outsidePattern.begin(), outsidePattern.end(),
              outsideTemplate.begin());
    if (mkdtemp(outsideTemplate.data()) == nullptr) {
      throw std::runtime_error("mkdtemp failed");
    }
    outside = outsideTemplate.data();
  }

  ~Fixture() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::remove_all(outside, error);
  }

  std::filesystem::path root;
  std::filesystem::path outside;
};

void CreateAdapter(const Fixture& fixture,
                   const std::string& idPath,
                   bool carrierUp) {
  const auto interfacePath = fixture.root / "class/net/eth9";
  const auto devicePath =
      fixture.root / "devices/platform/usb/1-1/1-1:1.0";
  std::filesystem::create_directories(interfacePath);
  std::filesystem::create_directories(devicePath);
  std::error_code error;
  std::filesystem::remove(interfacePath / "device", error);
  std::filesystem::create_directory_symlink(
      devicePath, interfacePath / "device");
  Write(interfacePath / "type", "1\n");
  Write(interfacePath / "ifindex", "42\n");
  Write(interfacePath / "carrier", carrierUp ? "1\n" : "0\n");
  Write(interfacePath / "operstate", carrierUp ? "up\n" : "unknown\n");
  Write(interfacePath / "addr_assign_type", "0\n");
  Write(interfacePath / "address", "02:11:22:33:44:55\n");
  Write(devicePath / "serial", "SERIAL-1\n");
  Write(devicePath / "idVendor", "1d6b\n");
  Write(devicePath / "idProduct", "0002\n");
  Write(fixture.root / "udev/n42",
        "E:OTHER=value\nE:ID_PATH=" + idPath + "\n");
}

}  // namespace

int main() {
  try {
    using namespace ec_systemcore;
    using namespace ec_systemcore::protocol;
    Fixture fixture;
    CreateAdapter(fixture, "usb-port-A", true);
    SysfsAdapterScanner scanner{fixture.root,
                                fixture.root / "udev"};
    std::string scanError;
    auto adapters = scanner.Scan(&scanError);
    CHECK(scanError.empty());
    CHECK(adapters.size() == 1);
    CHECK(adapters[0].idPath == "usb-port-A");
    CHECK(adapters[0].permanentMac ==
          std::optional<std::string>{"02:11:22:33:44:55"});
    CHECK(adapters[0].usbSerial ==
          std::optional<std::string>{"SERIAL-1"});
    CHECK(adapters[0].linkUp);

    const AdapterIdentityLock fullLock{
        .idPath = "usb-port-A",
        .permanentMac = "02:11:22:33:44:55",
        .usbSerial = "SERIAL-1",
        .usbVendorId = "1d6b",
        .usbProductId = "0002",
    };
    AdapterResolution resolution =
        ResolveAdapter(adapters, "eth9", fullLock);
    CHECK(resolution.state == AdapterLockState::Matched);
    CHECK(resolution.reason == AdapterLockReason::None);

    AdapterIdentityLock pathOnlyLock;
    pathOnlyLock.idPath = "usb-port-A";
    const AdapterResolution pathOnly =
        ResolveAdapter(adapters, "eth9", pathOnlyLock);
    CHECK(pathOnly.state == AdapterLockState::Matched);
    CHECK(pathOnly.reason ==
          AdapterLockReason::PathOnlyIdentity);

    std::filesystem::remove_all(fixture.root / "class/net/eth9");
    adapters = scanner.Scan();
    resolution = ResolveAdapter(adapters, "eth9", fullLock);
    CHECK(resolution.state == AdapterLockState::Missing);

    CreateAdapter(fixture, "usb-port-B", true);
    adapters = scanner.Scan();
    resolution = ResolveAdapter(adapters, "eth9", fullLock);
    CHECK(resolution.state == AdapterLockState::Mismatch);
    CHECK(resolution.reason == AdapterLockReason::IdPathMismatch);
    CHECK(ResolveAdapter(adapters, "eth9", fullLock, true).state ==
          AdapterLockState::RuntimeUnlocked);

    CreateAdapter(fixture, "usb-port-A", false);
    adapters = scanner.Scan();
    CHECK(adapters.size() == 1);
    CHECK(!adapters[0].linkUp);
    resolution = ResolveAdapter(adapters, "eth9", fullLock);
    CHECK(resolution.state == AdapterLockState::Matched);
    CHECK(!resolution.identity->linkUp);

    const auto escaped = fixture.root / "class/net/eth10";
    std::filesystem::create_directories(escaped);
    Write(escaped / "type", "1\n");
    Write(escaped / "ifindex", "43\n");
    Write(escaped / "carrier", "1\n");
    Write(escaped / "addr_assign_type", "0\n");
    Write(escaped / "address", "02:aa:bb:cc:dd:ee\n");
    Write(fixture.outside / "serial", "ESCAPED\n");
    std::filesystem::create_directory_symlink(
        fixture.outside, escaped / "device");
    adapters = scanner.Scan();
    const auto escapedIdentity =
        std::find_if(adapters.begin(), adapters.end(),
                     [](const AdapterIdentity& identity) {
                       return identity.interfaceName == "eth10";
                     });
    CHECK(escapedIdentity != adapters.end());
    CHECK(!escapedIdentity->usbSerial.has_value());
    CHECK(escapedIdentity->idPath.empty());

    std::cout << "adapter lifecycle test passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
