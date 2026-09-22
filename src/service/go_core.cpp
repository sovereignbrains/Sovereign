#include "go_core.h"

#include <wil/result.h>

#include <cstdint>

namespace sovereign::service {

std::wstring ResolveGoCoreDllPath() {
  wchar_t path[MAX_PATH]{};
  THROW_LAST_ERROR_IF(GetModuleFileNameW(nullptr, path, MAX_PATH) == 0);

  std::wstring exePath(path);
  const size_t lastSlash = exePath.find_last_of(L'\\');
  const std::wstring dir = (lastSlash == std::wstring::npos) ? L"." : exePath.substr(0, lastSlash);
  return dir + L"\\sovereign-gocore.dll";
}

GoCore::GoCore(const std::wstring& dllPath) {
  // LoadLibraryW with an absolute path we computed ourselves (never a
  // bare filename) — deliberately sidesteps Windows' default DLL search
  // order, which would otherwise probe the current working directory and
  // other locations an attacker could plant a DLL in.
  module_.reset(LoadLibraryW(dllPath.c_str()));
  THROW_LAST_ERROR_IF(!module_);

  // GetProcAddress always returns FARPROC; casting it to the export's real
  // signature is the only way to call it and is inherent to the Win32 API
  // shape. The intermediate uintptr_t cast (rather than a direct
  // FARPROC->target reinterpret_cast) is deliberate: clang's
  // cast-function-type-mismatch diagnostic is a hard error here (not a
  // suppressible lint), and going through an integer breaks the pattern it
  // matches on.
  boxPing_ = reinterpret_cast<BoxPingFn>(  // NOLINT(performance-no-int-to-ptr)
      reinterpret_cast<std::uintptr_t>(GetProcAddress(module_.get(), "box_ping")));
  THROW_LAST_ERROR_IF(!boxPing_);

  boxFree_ = reinterpret_cast<BoxFreeFn>(  // NOLINT(performance-no-int-to-ptr)
      reinterpret_cast<std::uintptr_t>(GetProcAddress(module_.get(), "box_free")));
  THROW_LAST_ERROR_IF(!boxFree_);
}

std::string GoCore::Ping() {
  char* reply = boxPing_();
  if (!reply) {
    return {};
  }
  std::string result(reply);
  boxFree_(reply);
  return result;
}

}  // namespace sovereign::service
