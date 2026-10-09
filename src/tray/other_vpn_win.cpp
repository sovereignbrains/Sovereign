// NetAdapters, RunningProcesses (other_vpn.h): the adapters from the IP
// helper, the processes from a Toolhelp snapshot.

#include "other_vpn.h"

#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <tlhelp32.h>

#include <wil/resource.h>

#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace sovereign::tray {

namespace {

std::string Utf8(const wchar_t* text) {
  if (text == nullptr || *text == L'\0') {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) {
    return {};
  }
  std::string out(static_cast<std::size_t>(size - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
  return out;
}

}  // namespace

std::vector<NetAdapter> NetAdapters() {
  constexpr ULONG kFlags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
  ULONG size = 32 * 1024;
  std::vector<std::uint8_t> buffer;
  ULONG result = ERROR_BUFFER_OVERFLOW;
  for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
    buffer.resize(size);
    result = GetAdaptersAddresses(AF_UNSPEC, kFlags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
  }
  std::vector<NetAdapter> adapters;
  if (result != ERROR_SUCCESS) {
    return adapters;
  }
  for (const auto* a = reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(buffer.data()); a != nullptr; a = a->Next) {
    NetAdapter adapter{.name = Utf8(a->FriendlyName),
                       .description = Utf8(a->Description),
                       .ifType = a->IfType,
                       .up = a->OperStatus == IfOperStatusUp,
                       .ipv4 = {}};
    for (const auto* u = a->FirstUnicastAddress; u != nullptr; u = u->Next) {
      if (u->Address.lpSockaddr != nullptr && u->Address.lpSockaddr->sa_family == AF_INET) {
        const auto* b = reinterpret_cast<const unsigned char*>(
            &reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.S_un.S_addr);
        adapter.ipv4.push_back(std::format("{}.{}.{}.{}", b[0], b[1], b[2], b[3]));
      }
    }
    adapters.push_back(std::move(adapter));
  }
  return adapters;
}

std::vector<std::string> RunningProcesses() {
  std::vector<std::string> names;
  const wil::unique_handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
  if (!snapshot || snapshot.get() == INVALID_HANDLE_VALUE) {
    return names;
  }
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof entry;
  for (BOOL more = Process32FirstW(snapshot.get(), &entry); more != FALSE; more = Process32NextW(snapshot.get(), &entry)) {
    names.push_back(Utf8(entry.szExeFile));
  }
  return names;
}

}  // namespace sovereign::tray
