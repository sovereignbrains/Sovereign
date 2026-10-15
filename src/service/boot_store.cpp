#include "boot_store.h"

#include <windows.h>
#include <dpapi.h>
#include <sddl.h>

#include <wil/resource.h>

#include <cstddef>
#include <format>
#include <string>
#include <vector>

#include "protocol.h"

namespace sovereign::service {

namespace {

// Mixed into the encryption: another program's DPAPI blob isn't taken for ours.
constexpr char kEntropy[] = "sovereign boot config v1";

DATA_BLOB Blob(const void* data, std::size_t size) {
  return DATA_BLOB{.cbData = static_cast<DWORD>(size), .pbData = static_cast<BYTE*>(const_cast<void*>(data))};
}

DATA_BLOB EntropyBlob() { return Blob(kEntropy, sizeof(kEntropy) - 1); }

std::string Failed(const char* what) { return std::format("{}: error {}", what, GetLastError()); }

}  // namespace

std::wstring BootStorePath() {
  wchar_t programData[MAX_PATH]{};
  const DWORD n = ExpandEnvironmentStringsW(L"%ProgramData%", programData, MAX_PATH);
  if (n == 0 || n > MAX_PATH || programData[0] == L'%') {
    return {};
  }
  return std::wstring(programData) + L"\\Sovereign\\boot-config.bin";
}

FileBootStore::FileBootStore(std::wstring path, std::wstring sddl) : path_(std::move(path)), sddl_(std::move(sddl)) {}

std::string FileBootStore::Save(const std::string& config) {
  if (path_.empty()) {
    return "no place for the boot config (%ProgramData%)";
  }
  DATA_BLOB in = Blob(config.data(), config.size());
  DATA_BLOB entropy = EntropyBlob();
  DATA_BLOB out{};
  if (!CryptProtectData(&in, L"Sovereign boot config", &entropy, nullptr, nullptr,
                        CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN, &out)) {
    return Failed("encrypting the boot config");
  }
  const wil::unique_hlocal encrypted(out.pbData);

  // The folder is the installer's (--install makes it for the logs); a
  // missing one is made, the file's own security doesn't depend on it.
  const std::wstring folder = path_.substr(0, path_.find_last_of(L'\\'));
  CreateDirectoryW(folder.c_str(), nullptr);

  PSECURITY_DESCRIPTOR rawDescriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl_.c_str(), SDDL_REVISION_1, &rawDescriptor, nullptr)) {
    return Failed("the boot config's security");
  }
  const wil::unique_hlocal descriptor(rawDescriptor);
  SECURITY_ATTRIBUTES attributes{
      .nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = rawDescriptor, .bInheritHandle = FALSE};
  // Written aside and moved over the old one: a crash halfway leaves the old
  // file whole. A leftover from such a crash goes first - CREATE_ALWAYS over
  // it would keep its security, not take ours.
  const std::wstring temporary = path_ + L".new";
  DeleteFileW(temporary.c_str());
  {
    const wil::unique_hfile file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW,
                                             FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
      return Failed("creating the boot config");
    }
    DWORD written = 0;
    if (!WriteFile(file.get(), out.pbData, out.cbData, &written, nullptr) || written != out.cbData ||
        !FlushFileBuffers(file.get())) {
      const std::string error = Failed("writing the boot config");
      DeleteFileW(temporary.c_str());
      return error;
    }
  }
  if (!MoveFileExW(temporary.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    const std::string error = Failed("replacing the boot config");
    DeleteFileW(temporary.c_str());
    return error;
  }
  return {};
}

void FileBootStore::Forget() {
  if (!path_.empty()) {
    DeleteFileW(path_.c_str());
  }
}

std::optional<std::string> FileBootStore::Load() {
  if (path_.empty()) {
    return std::nullopt;
  }
  const wil::unique_hfile file(
      CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!file) {
    return std::nullopt;
  }
  LARGE_INTEGER size{};
  // A config is what box_start takes - kMaxMessageBytes at most - plus DPAPI's few hundred bytes.
  if (!GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0 ||
      size.QuadPart > static_cast<LONGLONG>(ipc::kMaxMessageBytes + std::size_t{64} * 1024)) {
    return std::nullopt;
  }
  std::vector<BYTE> bytes(static_cast<std::size_t>(size.QuadPart));
  DWORD read = 0;
  if (!ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size()) {
    return std::nullopt;
  }
  DATA_BLOB in = Blob(bytes.data(), bytes.size());
  DATA_BLOB entropy = EntropyBlob();
  DATA_BLOB out{};
  if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
    return std::nullopt;
  }
  const wil::unique_hlocal decrypted(out.pbData);
  return std::string(reinterpret_cast<const char*>(out.pbData), out.cbData);
}

}  // namespace sovereign::service
