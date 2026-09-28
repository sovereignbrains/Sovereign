#include "kill_switch_wfp.h"

// windows.h before fwpmu.h, which leans on its types; initguid.h makes this
// file define the FWPM_LAYER_*/FWPM_CONDITION_* GUIDs fwpmu.h declares (as
// selectany: no clash with another file doing the same).
#include <windows.h>
#include <initguid.h>
#include <fwpmu.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <utility>

namespace sovereign::service {

namespace {

// Sovereign's WFP objects. The filters' keys are the base with the last byte
// the filter's index, so they can be deleted without enumerating.
constexpr GUID kProviderKey = {0xd8c802c7, 0x0bdf, 0x4b20, {0xa6, 0x30, 0x7e, 0xd6, 0x5f, 0x46, 0xc3, 0xc7}};
constexpr GUID kSublayerKey = {0xaf699ec7, 0xb73e, 0x4d82, {0xa2, 0x42, 0xa0, 0xf3, 0xe6, 0xcb, 0x34, 0x24}};
constexpr GUID kFilterKeyBase = {0x7057ae37, 0xbeab, 0x4dc5, {0xb5, 0x43, 0x87, 0x87, 0x89, 0x9b, 0x37, 0x00}};
constexpr std::size_t kMaxFilters = 64;

GUID FilterKey(std::size_t index) {
  GUID key = kFilterKeyBase;
  key.Data4[7] = static_cast<unsigned char>(index);
  return key;
}

std::string Failed(const char* what, DWORD code) { return std::format("{}: 0x{:08X}", what, code); }

// An engine session, closed with the object; transactions abort unless
// committed.
class Engine {
 public:
  Engine() {
    FWPM_SESSION0 session{};
    session.txnWaitTimeoutInMSec = 5000;
    status_ = FwpmEngineOpen0(nullptr, RPC_C_AUTHN_DEFAULT, nullptr, &session, &handle_);
  }
  ~Engine() {
    if (inTransaction_) {
      FwpmTransactionAbort0(handle_);
    }
    if (handle_ != nullptr) {
      FwpmEngineClose0(handle_);
    }
  }
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  Engine(Engine&&) = delete;
  Engine& operator=(Engine&&) = delete;

  DWORD Status() const { return status_; }
  HANDLE Handle() const { return handle_; }

  DWORD Begin() {
    const DWORD result = FwpmTransactionBegin0(handle_, 0);
    inTransaction_ = result == ERROR_SUCCESS;
    return result;
  }
  DWORD Commit() {
    const DWORD result = FwpmTransactionCommit0(handle_);
    inTransaction_ = false;
    return result;
  }

 private:
  HANDLE handle_ = nullptr;
  DWORD status_ = ERROR_SUCCESS;
  bool inTransaction_ = false;
};

// Every filter of ours, gone; what isn't there is fine.
DWORD DeleteFilters(HANDLE engine) {
  for (std::size_t i = 0; i < kMaxFilters; ++i) {
    const GUID key = FilterKey(i);
    const DWORD result = FwpmFilterDeleteByKey0(engine, &key);
    if (result != ERROR_SUCCESS && result != static_cast<DWORD>(FWP_E_FILTER_NOT_FOUND)) {
      return result;
    }
  }
  return ERROR_SUCCESS;
}

std::string RemoveAll(Engine& engine) {
  if (const DWORD result = DeleteFilters(engine.Handle()); result != ERROR_SUCCESS) {
    return Failed("deleting the kill switch's filters", result);
  }
  const DWORD sublayer = FwpmSubLayerDeleteByKey0(engine.Handle(), &kSublayerKey);
  if (sublayer != ERROR_SUCCESS && sublayer != static_cast<DWORD>(FWP_E_SUBLAYER_NOT_FOUND)) {
    return Failed("deleting the kill switch's sublayer", sublayer);
  }
  const DWORD provider = FwpmProviderDeleteByKey0(engine.Handle(), &kProviderKey);
  if (provider != ERROR_SUCCESS && provider != static_cast<DWORD>(FWP_E_PROVIDER_NOT_FOUND)) {
    return Failed("deleting the kill switch's provider", provider);
  }
  return {};
}

std::uint32_t V4Address(const Prefix& p) {
  return (static_cast<std::uint32_t>(p.bytes[0]) << 24) | (static_cast<std::uint32_t>(p.bytes[1]) << 16) |
         (static_cast<std::uint32_t>(p.bytes[2]) << 8) | static_cast<std::uint32_t>(p.bytes[3]);
}

std::uint32_t V4Mask(int bits) { return bits <= 0 ? 0 : 0xFFFFFFFFU << (32 - bits); }

// The conditions of one filter, and the values they point at.
class Conditions {
 public:
  explicit Conditions(FWP_BYTE_BLOB* appId) : appId_(appId) {}

  void Add(const KillSwitchRule& rule) {
    if (rule.coreApp) {
      FWPM_FILTER_CONDITION0& c = Next(FWPM_CONDITION_ALE_APP_ID, FWP_MATCH_EQUAL);
      c.conditionValue.type = FWP_BYTE_BLOB_TYPE;
      c.conditionValue.byteBlob = appId_;
    }
    if (rule.loopback) {
      FWPM_FILTER_CONDITION0& c = Next(FWPM_CONDITION_FLAGS, FWP_MATCH_FLAGS_ALL_SET);
      c.conditionValue.type = FWP_UINT32;
      c.conditionValue.uint32 = FWP_CONDITION_FLAG_IS_LOOPBACK;
    }
    for (const Prefix& p : rule.localPrefixes) {
      AddPrefix(FWPM_CONDITION_IP_LOCAL_ADDRESS, p);
    }
    for (const Prefix& p : rule.remotePrefixes) {
      AddPrefix(FWPM_CONDITION_IP_REMOTE_ADDRESS, p);
    }
    if (rule.remotePort) {
      FWPM_FILTER_CONDITION0& c = Next(FWPM_CONDITION_IP_REMOTE_PORT, FWP_MATCH_EQUAL);
      c.conditionValue.type = FWP_UINT16;
      c.conditionValue.uint16 = *rule.remotePort;
    }
    if (rule.protocol) {
      FWPM_FILTER_CONDITION0& c = Next(FWPM_CONDITION_IP_PROTOCOL, FWP_MATCH_EQUAL);
      c.conditionValue.type = FWP_UINT8;
      c.conditionValue.uint8 = *rule.protocol;
    }
  }

  bool Ok() const { return ok_; }
  UINT32 Count() const { return count_; }
  FWPM_FILTER_CONDITION0* Data() { return count_ == 0 ? nullptr : conditions_.data(); }

 private:
  static constexpr std::size_t kMax = 16;

  FWPM_FILTER_CONDITION0& Next(const GUID& field, FWP_MATCH_TYPE match) {
    if (count_ == kMax) {
      ok_ = false;
      return conditions_[kMax - 1];
    }
    FWPM_FILTER_CONDITION0& c = conditions_[count_++];
    c = {};
    c.fieldKey = field;
    c.matchType = match;
    return c;
  }

  void AddPrefix(const GUID& field, const Prefix& p) {
    FWPM_FILTER_CONDITION0& c = Next(field, FWP_MATCH_EQUAL);
    if (!ok_) {
      return;
    }
    const std::size_t slot = count_ - 1;
    if (p.v6) {
      FWP_V6_ADDR_AND_MASK& mask = v6_[slot];
      std::copy(p.bytes.begin(), p.bytes.end(), mask.addr);
      mask.prefixLength = static_cast<UINT8>(p.bits);
      c.conditionValue.type = FWP_V6_ADDR_MASK;
      c.conditionValue.v6AddrMask = &mask;
    } else {
      FWP_V4_ADDR_AND_MASK& mask = v4_[slot];
      mask.addr = V4Address(p);
      mask.mask = V4Mask(p.bits);
      c.conditionValue.type = FWP_V4_ADDR_MASK;
      c.conditionValue.v4AddrMask = &mask;
    }
  }

  FWP_BYTE_BLOB* appId_;
  std::array<FWPM_FILTER_CONDITION0, kMax> conditions_{};
  std::array<FWP_V4_ADDR_AND_MASK, kMax> v4_{};
  std::array<FWP_V6_ADDR_AND_MASK, kMax> v6_{};
  UINT32 count_ = 0;
  bool ok_ = true;
};

}  // namespace

WfpKillSwitch::WfpKillSwitch(std::wstring corePath) : corePath_(std::move(corePath)) {}

std::string WfpKillSwitch::Apply(const std::vector<KillSwitchRule>& rules) {
  if (rules.size() > kMaxFilters) {
    return "too many kill switch filters";
  }
  Engine engine;
  if (engine.Status() != ERROR_SUCCESS) {
    return Failed("opening the filtering engine", engine.Status());
  }
  if (const DWORD begun = engine.Begin(); begun != ERROR_SUCCESS) {
    return Failed("starting a filtering transaction", begun);
  }
  if (rules.empty()) {
    if (std::string error = RemoveAll(engine); !error.empty()) {
      return error;
    }
    const DWORD committed = engine.Commit();
    return committed == ERROR_SUCCESS ? std::string() : Failed("committing", committed);
  }

  if (const DWORD result = DeleteFilters(engine.Handle()); result != ERROR_SUCCESS) {
    return Failed("deleting the kill switch's filters", result);
  }
  FWPM_PROVIDER0 provider{};
  provider.providerKey = kProviderKey;
  wchar_t providerName[] = L"Sovereign";
  provider.displayData.name = providerName;
  provider.flags = FWPM_PROVIDER_FLAG_PERSISTENT;
  if (const DWORD added = FwpmProviderAdd0(engine.Handle(), &provider, nullptr);
      added != ERROR_SUCCESS && added != static_cast<DWORD>(FWP_E_ALREADY_EXISTS)) {
    return Failed("adding the kill switch's provider", added);
  }
  GUID providerKey = kProviderKey;
  FWPM_SUBLAYER0 sublayer{};
  sublayer.subLayerKey = kSublayerKey;
  wchar_t sublayerName[] = L"Sovereign kill switch";
  sublayer.displayData.name = sublayerName;
  sublayer.flags = FWPM_SUBLAYER_FLAG_PERSISTENT;
  sublayer.providerKey = &providerKey;
  sublayer.weight = 0xFFFF;
  if (const DWORD added = FwpmSubLayerAdd0(engine.Handle(), &sublayer, nullptr);
      added != ERROR_SUCCESS && added != static_cast<DWORD>(FWP_E_ALREADY_EXISTS)) {
    return Failed("adding the kill switch's sublayer", added);
  }

  FWP_BYTE_BLOB* appId = nullptr;
  if (const DWORD got = FwpmGetAppIdFromFileName0(corePath_.c_str(), &appId); got != ERROR_SUCCESS) {
    return Failed("the core's app id", got);
  }
  const auto freeAppId = [&] { FwpmFreeMemory0(reinterpret_cast<void**>(&appId)); };

  for (std::size_t i = 0; i < rules.size(); ++i) {
    const KillSwitchRule& rule = rules[i];
    Conditions conditions(appId);
    conditions.Add(rule);
    if (!conditions.Ok()) {
      freeAppId();
      return "a kill switch filter with too many conditions";
    }
    std::wstring name = L"Sovereign kill switch: ";
    name.append(rule.name.begin(), rule.name.end());
    FWPM_FILTER0 filter{};
    filter.filterKey = FilterKey(i);
    filter.displayData.name = name.data();
    filter.flags = FWPM_FILTER_FLAG_PERSISTENT;
    filter.providerKey = &providerKey;
    filter.layerKey = rule.v6 ? FWPM_LAYER_ALE_AUTH_CONNECT_V6 : FWPM_LAYER_ALE_AUTH_CONNECT_V4;
    filter.subLayerKey = kSublayerKey;
    filter.weight.type = FWP_UINT8;
    filter.weight.uint8 = rule.weight;
    filter.numFilterConditions = conditions.Count();
    filter.filterCondition = conditions.Data();
    filter.action.type = rule.permit ? FWP_ACTION_PERMIT : FWP_ACTION_BLOCK;
    if (const DWORD added = FwpmFilterAdd0(engine.Handle(), &filter, nullptr, nullptr); added != ERROR_SUCCESS) {
      freeAppId();
      return Failed(("adding the kill switch filter \"" + rule.name + "\"").c_str(), added);
    }
  }
  freeAppId();
  const DWORD committed = engine.Commit();
  return committed == ERROR_SUCCESS ? std::string() : Failed("committing the kill switch", committed);
}

bool WfpKillSwitch::Active() {
  Engine engine;
  if (engine.Status() != ERROR_SUCCESS) {
    return false;
  }
  FWPM_SUBLAYER0* sublayer = nullptr;
  if (FwpmSubLayerGetByKey0(engine.Handle(), &kSublayerKey, &sublayer) != ERROR_SUCCESS) {
    return false;
  }
  FwpmFreeMemory0(reinterpret_cast<void**>(&sublayer));
  return true;
}

std::string RemoveKillSwitch() {
  Engine engine;
  if (engine.Status() != ERROR_SUCCESS) {
    return Failed("opening the filtering engine", engine.Status());
  }
  if (const DWORD begun = engine.Begin(); begun != ERROR_SUCCESS) {
    return Failed("starting a filtering transaction", begun);
  }
  if (std::string error = RemoveAll(engine); !error.empty()) {
    return error;
  }
  const DWORD committed = engine.Commit();
  return committed == ERROR_SUCCESS ? std::string() : Failed("committing", committed);
}

}  // namespace sovereign::service
