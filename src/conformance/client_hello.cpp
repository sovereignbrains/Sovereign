#include "client_hello.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <map>
#include <optional>
#include <utility>

namespace sovereign::conformance {

namespace {

constexpr std::uint8_t kHandshakeRecord = 22;
constexpr std::uint8_t kClientHelloMessage = 1;
constexpr std::uint16_t kGreasePlaceholder = 0x0A0A;

constexpr std::uint16_t kSupportedGroups = 10;
constexpr std::uint16_t kPadding = 21;
constexpr std::uint16_t kPreSharedKey = 41;
constexpr std::uint16_t kSupportedVersions = 43;
constexpr std::uint16_t kKeyShare = 51;
constexpr std::uint16_t kEncryptedClientHello = 0xFE0D;

// ECH payload: the inner ClientHello padded to a multiple of 32 bytes, plus
// the AEAD tag (draft-ietf-tls-esni 6.1.3; HPKE AES-128-GCM / ChaCha20: 16).
constexpr std::size_t kEchPadding = 32;
constexpr std::size_t kEchTag = 16;

// RFC 8446 3.3/3.4 wire primitives over a byte span; every read reports
// running out instead of reading past the end.
class Reader {
 public:
  explicit Reader(std::span<const std::uint8_t> data) : data_(data) {}

  bool Empty() const { return pos_ == data_.size(); }

  std::optional<std::uint8_t> U8() {
    if (data_.size() - pos_ < 1) {
      return std::nullopt;
    }
    return data_[pos_++];
  }

  std::optional<std::uint16_t> U16() {
    if (data_.size() - pos_ < 2) {
      return std::nullopt;
    }
    const auto value = static_cast<std::uint16_t>((data_[pos_] << 8U) | data_[pos_ + 1]);
    pos_ += 2;
    return value;
  }

  std::optional<std::uint32_t> U24() {
    if (data_.size() - pos_ < 3) {
      return std::nullopt;
    }
    const auto value = (static_cast<std::uint32_t>(data_[pos_]) << 16U) |
                       (static_cast<std::uint32_t>(data_[pos_ + 1]) << 8U) | data_[pos_ + 2];
    pos_ += 3;
    return value;
  }

  std::optional<std::span<const std::uint8_t>> Bytes(std::size_t n) {
    if (data_.size() - pos_ < n) {
      return std::nullopt;
    }
    const auto bytes = data_.subspan(pos_, n);
    pos_ += n;
    return bytes;
  }

  // Vectors with a one- or two-byte length prefix (RFC 8446 3.4).
  std::optional<std::span<const std::uint8_t>> Vec8() {
    const auto n = U8();
    return n ? Bytes(*n) : std::nullopt;
  }

  std::optional<std::span<const std::uint8_t>> Vec16() {
    const auto n = U16();
    return n ? Bytes(*n) : std::nullopt;
  }

 private:
  std::span<const std::uint8_t> data_;
  std::size_t pos_ = 0;
};

class Writer {
 public:
  void U8(std::uint8_t value) { out_.push_back(value); }
  void U16(std::uint16_t value) {
    out_.push_back(static_cast<std::uint8_t>(value >> 8U));
    out_.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  }
  void U24(std::uint32_t value) {
    out_.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out_.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out_.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  }
  void Bytes(std::span<const std::uint8_t> bytes) { out_.insert(out_.end(), bytes.begin(), bytes.end()); }
  void Vec8(std::span<const std::uint8_t> bytes) {
    U8(static_cast<std::uint8_t>(bytes.size()));
    Bytes(bytes);
  }
  void Vec16(std::span<const std::uint8_t> bytes) {
    U16(static_cast<std::uint16_t>(bytes.size()));
    Bytes(bytes);
  }
  std::vector<std::uint8_t> Take() { return std::move(out_); }

 private:
  std::vector<std::uint8_t> out_;
};

std::uint16_t Degrease(std::uint16_t value) { return IsGrease(value) ? kGreasePlaceholder : value; }

// A list of 16-bit values behind a one- or two-byte length (supported_groups,
// supported_versions) with its GREASE entries replaced. nullopt if malformed.
std::optional<std::vector<std::uint8_t>> DegreaseList(std::span<const std::uint8_t> data, bool shortPrefix) {
  Reader r(data);
  const auto list = shortPrefix ? r.Vec8() : r.Vec16();
  if (!list || !r.Empty() || list->size() % 2 != 0) {
    return std::nullopt;
  }
  Reader items(*list);
  Writer values;
  while (!items.Empty()) {
    values.U16(Degrease(*items.U16()));
  }
  Writer out;
  if (shortPrefix) {
    out.Vec8(values.Take());
  } else {
    out.Vec16(values.Take());
  }
  return out.Take();
}

// key_share (RFC 8446 4.2.8): each share's group de-GREASEd and its key bytes
// zeroed - fresh ephemeral keys per connection. Groups and key lengths stay.
std::optional<std::vector<std::uint8_t>> MaskKeyShares(std::span<const std::uint8_t> data) {
  Reader r(data);
  const auto shares = r.Vec16();
  if (!shares || !r.Empty()) {
    return std::nullopt;
  }
  Reader s(*shares);
  Writer list;
  while (!s.Empty()) {
    const auto group = s.U16();
    const auto key = s.Vec16();
    if (!group || !key) {
      return std::nullopt;
    }
    list.U16(Degrease(*group));
    list.Vec16(std::vector<std::uint8_t>(key->size(), 0));
  }
  Writer out;
  out.Vec16(list.Take());
  return out.Take();
}

// encrypted_client_hello in the outer ClientHello (draft-ietf-tls-esni 5):
// type, cipher suite, config_id, enc, payload. Per connection: enc (a fresh
// HPKE key), payload (ciphertext), config_id (random under GREASE ECH, and
// GREASE is indistinguishable from real ECH on the wire), and the payload's
// length (Chrome/uTLS GREASE picks a padded size at random). enc's length
// stays; the payload length is checked against the padding rule instead of
// compared. A fingerprint with its own candidates (CanonicalOptions) has the
// length and the AEAD checked against those - Firefox's GREASE picks
// AES-128-GCM or ChaCha20-Poly1305 per connection - and both masked.
std::optional<std::vector<std::uint8_t>> MaskEch(std::span<const std::uint8_t> data, const CanonicalOptions& options,
                                                 std::vector<std::string>& violations) {
  Reader r(data);
  const auto type = r.U8();
  const auto kdf = r.U16();
  const auto aead = r.U16();
  const auto configId = r.U8();
  const auto enc = r.Vec16();
  const auto payload = r.Vec16();
  if (!type || !kdf || !aead || !configId || !enc || !payload || !r.Empty()) {
    return std::nullopt;
  }
  if (*type != 0) {
    violations.push_back(std::format("encrypted_client_hello: type {} in the outer ClientHello (must be 0, outer)", *type));
  }
  if (options.echPayloadLengths.empty()) {
    if (payload->size() < kEchTag || (payload->size() - kEchTag) % kEchPadding != 0) {
      violations.push_back(std::format("encrypted_client_hello: payload of {} bytes is not a {}-byte-padded plaintext plus a {}-byte tag",
                                       payload->size(), kEchPadding, kEchTag));
    }
  } else if (!std::ranges::contains(options.echPayloadLengths, payload->size())) {
    violations.push_back(std::format("encrypted_client_hello: payload of {} bytes, not one of the fingerprint's {}",
                                     payload->size(), options.echPayloadLengths));
  }
  const bool aeadMasked = !options.echAeads.empty();
  if (aeadMasked && !std::ranges::contains(options.echAeads, *aead)) {
    violations.push_back(std::format("encrypted_client_hello: AEAD {:#06x}, not one of the fingerprint's", *aead));
  }
  Writer out;
  out.U8(*type);
  out.U16(*kdf);
  out.U16(aeadMasked ? std::uint16_t{0} : *aead);
  out.U8(0);
  out.Vec16(std::vector<std::uint8_t>(enc->size(), 0));
  out.Vec16({});
  return out.Take();
}

// Chrome's permutation moves every extension except GREASE (first and last)
// and pre_shared_key (must be last, RFC 8446 4.2.11): those keep their slots,
// the rest are put in type order.
void SortExtensions(std::vector<Extension>& extensions) {
  std::vector<std::size_t> slots;
  std::vector<Extension> movable;
  for (std::size_t i = 0; i < extensions.size(); ++i) {
    if (extensions[i].type == kGreasePlaceholder || extensions[i].type == kPreSharedKey) {
      continue;
    }
    slots.push_back(i);
    movable.push_back(std::move(extensions[i]));
  }
  std::ranges::stable_sort(movable, {}, &Extension::type);
  for (std::size_t k = 0; k < slots.size(); ++k) {
    extensions[slots[k]] = std::move(movable[k]);
  }
}

std::string Hex(std::span<const std::uint8_t> bytes) {
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const std::uint8_t b : bytes) {
    out += std::format("{:02x}", b);
  }
  return out;
}

std::string TypeList(const std::vector<Extension>& extensions) {
  std::string out;
  for (const Extension& e : extensions) {
    out += (out.empty() ? "" : ", ") + ExtensionName(e.type);
  }
  return out;
}

std::string SuiteList(const std::vector<std::uint16_t>& suites) {
  std::string out;
  for (const std::uint16_t s : suites) {
    out += std::format("{}{:04x}", out.empty() ? "" : " ", s);
  }
  return out;
}

// The k-th occurrence of each type: GREASE placeholders repeat, and pairing
// them by occurrence keeps them comparable.
std::map<std::pair<std::uint16_t, int>, const Extension*> ByOccurrence(const std::vector<Extension>& extensions) {
  std::map<std::pair<std::uint16_t, int>, const Extension*> out;
  std::map<std::uint16_t, int> seen;
  for (const Extension& e : extensions) {
    out[{e.type, seen[e.type]++}] = &e;
  }
  return out;
}

}  // namespace

bool IsGrease(std::uint16_t value) {
  const unsigned v = value;
  return (v & 0x0F0FU) == 0x0A0AU && (v >> 8U) == (v & 0xFFU);
}

std::string ExtensionName(std::uint16_t type) {
  switch (type) {
    case 0: return "server_name";
    case 5: return "status_request";
    case 10: return "supported_groups";
    case 11: return "ec_point_formats";
    case 13: return "signature_algorithms";
    case 16: return "application_layer_protocol_negotiation";
    case 18: return "signed_certificate_timestamp";
    case 21: return "padding";
    case 23: return "extended_master_secret";
    case 27: return "compress_certificate";
    case 35: return "session_ticket";
    case 41: return "pre_shared_key";
    case 43: return "supported_versions";
    case 45: return "psk_key_exchange_modes";
    case 51: return "key_share";
    case 17513: return "application_settings (old)";
    case 17613: return "application_settings";
    case 65037: return "encrypted_client_hello";
    case 65281: return "renegotiation_info";
    default: break;
  }
  return IsGrease(type) ? std::string("GREASE") : std::format("extension {}", type);
}

std::expected<ClientHello, std::string> ParseClientHello(std::span<const std::uint8_t> record) {
  Reader r(record);
  const auto recordType = r.U8();
  const auto recordVersion = r.U16();
  const auto fragment = r.Vec16();
  if (!recordType || !recordVersion || !fragment) {
    return std::unexpected("truncated record");
  }
  if (*recordType != kHandshakeRecord) {
    return std::unexpected(std::format("record type {} is not a handshake", *recordType));
  }
  if (!r.Empty()) {
    return std::unexpected("bytes after the first record");
  }

  Reader h(*fragment);
  const auto messageType = h.U8();
  const auto length = h.U24();
  if (!messageType || !length) {
    return std::unexpected("truncated handshake header");
  }
  if (*messageType != kClientHelloMessage) {
    return std::unexpected(std::format("handshake message {} is not a ClientHello", *messageType));
  }
  const auto body = h.Bytes(*length);
  if (!body) {
    return std::unexpected("the ClientHello continues past its record (fragmented or truncated)");
  }
  if (!h.Empty()) {
    return std::unexpected("bytes after the ClientHello in its record");
  }

  Reader b(*body);
  const auto version = b.U16();
  const auto random = b.Bytes(32);
  const auto sessionId = b.Vec8();
  const auto suites = b.Vec16();
  const auto compression = b.Vec8();
  if (!version || !random || !sessionId || !suites || !compression) {
    return std::unexpected("truncated ClientHello");
  }
  if (sessionId->size() > 32) {
    return std::unexpected("legacy_session_id longer than 32 bytes");
  }
  if (suites->empty() || suites->size() % 2 != 0) {
    return std::unexpected("malformed cipher_suites");
  }

  ClientHello hello;
  hello.recordVersion = *recordVersion;
  hello.legacyVersion = *version;
  std::ranges::copy(*random, hello.random.begin());
  hello.sessionId.assign(sessionId->begin(), sessionId->end());
  Reader s(*suites);
  while (!s.Empty()) {
    hello.cipherSuites.push_back(*s.U16());
  }
  hello.compressionMethods.assign(compression->begin(), compression->end());
  if (b.Empty()) {
    return hello;  // no extensions block at all is legal before TLS 1.3
  }

  const auto extensions = b.Vec16();
  if (!extensions || !b.Empty()) {
    return std::unexpected("malformed extensions block");
  }
  Reader e(*extensions);
  while (!e.Empty()) {
    const auto type = e.U16();
    const auto data = e.Vec16();
    if (!type || !data) {
      return std::unexpected("truncated extension");
    }
    if (std::ranges::any_of(hello.extensions, [&](const Extension& x) { return x.type == *type; })) {
      return std::unexpected(std::format("{} appears twice", ExtensionName(*type)));
    }
    hello.extensions.push_back({*type, {data->begin(), data->end()}});
  }
  return hello;
}

std::vector<std::uint8_t> Serialize(const ClientHello& hello) {
  Writer body;
  body.U16(hello.legacyVersion);
  body.Bytes(hello.random);
  body.Vec8(hello.sessionId);
  Writer suites;
  for (const std::uint16_t suite : hello.cipherSuites) {
    suites.U16(suite);
  }
  body.Vec16(suites.Take());
  body.Vec8(hello.compressionMethods);
  if (!hello.extensions.empty()) {
    Writer extensions;
    for (const Extension& e : hello.extensions) {
      extensions.U16(e.type);
      extensions.Vec16(e.data);
    }
    body.Vec16(extensions.Take());
  }
  const std::vector<std::uint8_t> bodyBytes = body.Take();

  Writer handshake;
  handshake.U8(kClientHelloMessage);
  handshake.U24(static_cast<std::uint32_t>(bodyBytes.size()));
  handshake.Bytes(bodyBytes);

  Writer record;
  record.U8(kHandshakeRecord);
  record.U16(hello.recordVersion);
  record.Vec16(handshake.Take());
  return record.Take();
}

Canonical Canonicalize(const ClientHello& hello, const CanonicalOptions& options) {
  Canonical canonical{hello, {}};
  ClientHello& h = canonical.hello;

  // random: 32 fresh bytes per ClientHello (RFC 8446 4.1.2).
  h.random.fill(0);
  // legacy_session_id: random in TLS 1.3 middlebox compatibility mode
  // (RFC 8446 D.4); REALITY also seals its authentication into it.
  std::ranges::fill(h.sessionId, std::uint8_t{0});
  // GREASE (RFC 8701): a reserved value picked per connection.
  for (std::uint16_t& suite : h.cipherSuites) {
    suite = Degrease(suite);
  }

  for (Extension& e : h.extensions) {
    std::optional<std::vector<std::uint8_t>> masked;
    switch (e.type) {
      case kSupportedGroups: masked = DegreaseList(e.data, /*shortPrefix=*/false); break;
      case kSupportedVersions: masked = DegreaseList(e.data, /*shortPrefix=*/true); break;
      case kKeyShare: masked = MaskKeyShares(e.data); break;
      case kEncryptedClientHello: masked = MaskEch(e.data, options, canonical.violations); break;
      // padding (RFC 7685) sizes the record to a length class: its length
      // follows every other field's, its bytes are zeroes by definition.
      case kPadding: masked = std::vector<std::uint8_t>{}; break;
      default: break;
    }
    if (masked) {
      e.data = std::move(*masked);
    } else if (e.type == kSupportedGroups || e.type == kSupportedVersions || e.type == kKeyShare ||
               e.type == kEncryptedClientHello) {
      canonical.violations.push_back(std::format("{}: malformed, compared as is", ExtensionName(e.type)));
    }
    e.type = Degrease(e.type);
  }

  if (options.extensionOrderRandomized) {
    SortExtensions(h.extensions);
  }
  return canonical;
}

std::vector<std::string> Diff(const ClientHello& reference, const ClientHello& ours) {
  std::vector<std::string> out;
  if (reference.recordVersion != ours.recordVersion) {
    out.push_back(std::format("record version: reference {:04x}, ours {:04x}", reference.recordVersion, ours.recordVersion));
  }
  if (reference.legacyVersion != ours.legacyVersion) {
    out.push_back(std::format("legacy_version: reference {:04x}, ours {:04x}", reference.legacyVersion, ours.legacyVersion));
  }
  if (reference.random != ours.random) {
    out.push_back("random: differs");
  }
  if (reference.sessionId != ours.sessionId) {
    out.push_back(std::format("legacy_session_id: reference {} bytes {}, ours {} bytes {}", reference.sessionId.size(),
                              Hex(reference.sessionId), ours.sessionId.size(), Hex(ours.sessionId)));
  }
  if (reference.cipherSuites != ours.cipherSuites) {
    out.push_back(std::format("cipher_suites: reference [{}], ours [{}]", SuiteList(reference.cipherSuites),
                              SuiteList(ours.cipherSuites)));
  }
  if (reference.compressionMethods != ours.compressionMethods) {
    out.push_back(std::format("compression_methods: reference {}, ours {}", Hex(reference.compressionMethods),
                              Hex(ours.compressionMethods)));
  }

  const auto referenceByType = ByOccurrence(reference.extensions);
  const auto oursByType = ByOccurrence(ours.extensions);
  bool sameSet = referenceByType.size() == oursByType.size();
  for (const auto& [key, extension] : referenceByType) {
    const auto it = oursByType.find(key);
    if (it == oursByType.end()) {
      out.push_back(std::format("{}: missing in ours", ExtensionName(extension->type)));
      sameSet = false;
      continue;
    }
    const auto& a = extension->data;
    const auto& b = it->second->data;
    if (a != b) {
      const auto mismatch = std::ranges::mismatch(a, b);
      const auto at = static_cast<std::size_t>(mismatch.in1 - a.begin());
      out.push_back(std::format("{}: {} vs {} bytes, first difference at byte {}: reference {}, ours {}",
                                ExtensionName(extension->type), a.size(), b.size(), at,
                                Hex(std::span(a).subspan(at, std::min<std::size_t>(8, a.size() - at))),
                                Hex(std::span(b).subspan(at, std::min<std::size_t>(8, b.size() - at)))));
    }
  }
  for (const auto& [key, extension] : oursByType) {
    if (!referenceByType.contains(key)) {
      out.push_back(std::format("{}: only in ours", ExtensionName(extension->type)));
      sameSet = false;
    }
  }
  if (sameSet) {
    const bool sameOrder = std::ranges::equal(reference.extensions, ours.extensions, {}, &Extension::type, &Extension::type);
    if (!sameOrder) {
      out.push_back(std::format("extension order: reference [{}], ours [{}]", TypeList(reference.extensions),
                                TypeList(ours.extensions)));
    }
  }
  return out;
}

}  // namespace sovereign::conformance
