// The config kept for boot (src/service/boot_store.h) in a temporary folder:
// kept and read back whole, encrypted on disk, replaced, forgotten; a damaged
// file reads as none. The service's own security (SYSTEM and Administrators)
// would lock this test out - it runs as the user: the owner's rights instead.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <wil/resource.h>

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "boot_store.h"
#include "check.h"

namespace {

namespace fs = std::filesystem;
using sovereign::service::FileBootStore;

constexpr wchar_t kOwnerOnly[] = L"D:P(A;;FA;;;OW)";

std::string Read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The file's DACL as SDDL: protected, the one ACE given.
std::wstring Dacl(const fs::path& path) {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  PACL dacl = nullptr;
  if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr,
                            &descriptor) != ERROR_SUCCESS) {
    return {};
  }
  const wil::unique_hlocal owned(descriptor);
  LPWSTR text = nullptr;
  if (!ConvertSecurityDescriptorToStringSecurityDescriptorW(descriptor, SDDL_REVISION_1, DACL_SECURITY_INFORMATION,
                                                            &text, nullptr)) {
    return {};
  }
  const wil::unique_hlocal ownedText(text);
  return text;
}

void TestKeptAndForgotten(const fs::path& dir) {
  const fs::path file = dir / "boot-config.bin";
  FileBootStore store(file.wstring(), kOwnerOnly);
  CHECK(!store.Load());  // nothing kept yet
  store.Forget();        // nothing to forget: fine

  const std::string config = R"({"outbounds":[{"type":"vless","uuid":"secret-uuid-1"}]})";
  CHECK(store.Save(config).empty());
  CHECK(store.Load() == config);
  // Encrypted: the keys aren't in the file as text.
  CHECK(Read(file).find("secret-uuid-1") == std::string::npos);
  // Its own security, not the folder's: protected, the owner only.
  const std::wstring dacl = Dacl(file);
  CHECK(dacl.starts_with(L"D:P") && dacl.find(L";;;OW)") != std::wstring::npos && dacl.find(L";;;BU)") == std::wstring::npos);
  CHECK(!fs::exists(dir / "boot-config.bin.new"));

  // Replaced whole; a leftover of a crashed save doesn't get in the way.
  { std::ofstream(dir / "boot-config.bin.new") << "half"; }
  CHECK(store.Save(R"({"b":2})").empty());
  CHECK(store.Load() == std::string(R"({"b":2})"));
  CHECK(!fs::exists(dir / "boot-config.bin.new"));

  // Read by a store of its own, as the service does after a reboot.
  CHECK(FileBootStore(file.wstring(), kOwnerOnly).Load() == std::string(R"({"b":2})"));

  store.Forget();
  CHECK(!fs::exists(file));
  CHECK(!store.Load());
}

void TestDamagedAndMissing(const fs::path& dir) {
  const fs::path file = dir / "damaged.bin";
  { std::ofstream(file, std::ios::binary) << "not a DPAPI blob"; }
  CHECK(!FileBootStore(file.wstring(), kOwnerOnly).Load());
  { std::ofstream(file, std::ios::binary | std::ios::trunc); }  // empty
  CHECK(!FileBootStore(file.wstring(), kOwnerOnly).Load());
  // A folder that isn't there is made.
  FileBootStore deeper((dir / "made" / "boot-config.bin").wstring(), kOwnerOnly);
  CHECK(deeper.Save("{}").empty());
  CHECK(deeper.Load() == std::string("{}"));
  // No place at all: said, not thrown.
  FileBootStore nowhere(L"", kOwnerOnly);
  CHECK(!nowhere.Save("{}").empty());
  CHECK(!nowhere.Load());
  nowhere.Forget();
  CHECK(sovereign::service::BootStorePath().ends_with(L"\\Sovereign\\boot-config.bin"));
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    const fs::path dir = fs::temp_directory_path() / ("sovereign-boot-store-" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    TestKeptAndForgotten(dir);
    TestDamagedAndMissing(dir);
    fs::remove_all(dir, ec);
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 1;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}
