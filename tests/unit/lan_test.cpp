// The closed local network's addresses (src/tray/lan.h): what may be let in,
// and the list the window shows.

#include <exception>
#include <iostream>
#include <string>

#include "check.h"
#include "lan.h"

namespace {

using namespace sovereign::tray;

void TestLanAddress() {
  CHECK(LanAddress("192.168.31.1") == "192.168.31.1");
  CHECK(LanAddress("  10.0.0.7\t") == "10.0.0.7");
  CHECK(LanAddress("172.16.5.4") == "172.16.5.4");
  CHECK(LanAddress("169.254.1.1") == "169.254.1.1");
  CHECK(LanAddress("192.168.31.77/24") == "192.168.31.0/24");  // a subnet, written from its start
  CHECK(LanAddress("10.1.2.3/8") == "10.0.0.0/8");
  // Not the local network's: the internet, or wider than its range.
  for (const char* bad : {"8.8.8.8", "172.32.0.1", "192.169.0.1", "192.168.0.0/15", "10.0.0.0/7", "0.0.0.0/0", "",
                          "192.168.1", "192.168.1.1.1", "192.168.01.1", "192.168.1.256", "192.168.1.1/",
                          "192.168.1.1/33", "192.168.1.1/x", "fe80::1", "router"}) {
    CHECK(!LanAddress(bad));
  }
}

void TestLanEntries() {
  const std::vector<LanDevice> seen = {
      {.address = "192.168.31.45", .mac = "aa:bb:cc:dd:ee:01", .router = false},
      {.address = "192.168.31.1", .mac = "aa:bb:cc:dd:ee:00", .router = true},
      {.address = "192.168.31.9", .mac = "aa:bb:cc:dd:ee:02", .router = false},
      {.address = "192.168.31.9", .mac = "aa:bb:cc:dd:ee:02", .router = false},  // the same on two tables: once
  };
  const std::vector<LanHost> allowed = {{.address = "192.168.31.200", .name = "Принтер"},
                                        {.address = "192.168.31.45", .name = ""}};
  const auto entries = LanEntries(seen, allowed);
  CHECK(entries.size() == 4);
  if (entries.size() == 4) {
    CHECK(entries[0].address == "192.168.31.1" && entries[0].router && entries[0].seen && !entries[0].allowed);
    // What's let in next, by number (45 before 200), whether around or not.
    CHECK(entries[1].address == "192.168.31.45" && entries[1].allowed && entries[1].seen);
    CHECK(entries[2].address == "192.168.31.200" && entries[2].allowed && !entries[2].seen &&
          entries[2].name == "Принтер");
    CHECK(entries[3].address == "192.168.31.9" && !entries[3].allowed && entries[3].mac == "aa:bb:cc:dd:ee:02");
  }
  CHECK(LanEntries({}, {}).empty());
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestLanAddress();
    TestLanEntries();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 1;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
