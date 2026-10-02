#include "x25519.h"

#include <cstddef>

namespace sovereign::tray {

namespace {

// A field element: 16 limbs of 16 bits (TweetNaCl's gf), with room to carry.
using Gf = std::array<std::int64_t, 16>;

void Carry(Gf& o) {
  for (std::size_t i = 0; i < 16; ++i) {
    o[i] += std::int64_t{1} << 16;
    const std::int64_t c = o[i] >> 16;
    // The last limb's carry wraps around times 38 (2^256 = 38 mod p).
    o[i < 15 ? i + 1 : 0] += c - 1 + (i == 15 ? 37 * (c - 1) : 0);
    o[i] -= c * (std::int64_t{1} << 16);
  }
}

// p and q swapped if b is 1, without a branch.
void Select(Gf& p, Gf& q, std::int64_t b) {
  const std::int64_t mask = ~(b - 1);
  for (std::size_t i = 0; i < 16; ++i) {
    const std::int64_t t = mask & (p[i] ^ q[i]);
    p[i] ^= t;
    q[i] ^= t;
  }
}

X25519Key Pack(const Gf& n) {
  Gf t = n;
  Carry(t);
  Carry(t);
  Carry(t);
  for (int j = 0; j < 2; ++j) {
    Gf m{};
    m[0] = t[0] - 0xffed;
    for (std::size_t i = 1; i < 15; ++i) {
      m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
      m[i - 1] &= 0xffff;
    }
    m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
    const std::int64_t b = (m[15] >> 16) & 1;
    m[14] &= 0xffff;
    Select(t, m, 1 - b);
  }
  X25519Key out{};
  for (std::size_t i = 0; i < 16; ++i) {
    out[2 * i] = static_cast<std::uint8_t>(t[i] & 0xff);
    out[2 * i + 1] = static_cast<std::uint8_t>((t[i] >> 8) & 0xff);
  }
  return out;
}

Gf Unpack(const X25519Key& n) {
  Gf o{};
  for (std::size_t i = 0; i < 16; ++i) {
    o[i] = n[2 * i] + (std::int64_t{n[2 * i + 1]} << 8);
  }
  o[15] &= 0x7fff;
  return o;
}

Gf Add(const Gf& a, const Gf& b) {
  Gf o{};
  for (std::size_t i = 0; i < 16; ++i) {
    o[i] = a[i] + b[i];
  }
  return o;
}

Gf Sub(const Gf& a, const Gf& b) {
  Gf o{};
  for (std::size_t i = 0; i < 16; ++i) {
    o[i] = a[i] - b[i];
  }
  return o;
}

Gf Mul(const Gf& a, const Gf& b) {
  std::array<std::int64_t, 31> t{};
  for (std::size_t i = 0; i < 16; ++i) {
    for (std::size_t j = 0; j < 16; ++j) {
      t[i + j] += a[i] * b[j];
    }
  }
  for (std::size_t i = 0; i < 15; ++i) {
    t[i] += 38 * t[i + 16];
  }
  Gf o{};
  for (std::size_t i = 0; i < 16; ++i) {
    o[i] = t[i];
  }
  Carry(o);
  Carry(o);
  return o;
}

Gf Invert(const Gf& in) {
  Gf c = in;
  for (int a = 253; a >= 0; --a) {
    c = Mul(c, c);
    if (a != 2 && a != 4) {
      c = Mul(c, in);
    }
  }
  return c;
}

}  // namespace

X25519Key X25519(const X25519Key& scalar, const X25519Key& point) {
  X25519Key z = scalar;
  z[31] = static_cast<std::uint8_t>((scalar[31] & 127) | 64);
  z[0] &= 248;
  const Gf x = Unpack(point);
  Gf a{};
  Gf b = x;
  Gf c{};
  Gf d{};
  a[0] = 1;
  d[0] = 1;
  const Gf k121665 = {0xDB41, 1};
  for (int i = 254; i >= 0; --i) {
    const std::int64_t r = (z[static_cast<std::size_t>(i) >> 3] >> (i & 7)) & 1;
    Select(a, b, r);
    Select(c, d, r);
    Gf e = Add(a, c);
    a = Sub(a, c);
    c = Add(b, d);
    b = Sub(b, d);
    d = Mul(e, e);
    const Gf f = Mul(a, a);
    a = Mul(c, a);
    c = Mul(b, e);
    e = Add(a, c);
    a = Sub(a, c);
    b = Mul(a, a);
    c = Sub(d, f);
    a = Mul(c, k121665);
    a = Add(a, d);
    c = Mul(c, a);
    a = Mul(d, f);
    d = Mul(b, x);
    b = Mul(e, e);
    Select(a, b, r);
    Select(c, d, r);
  }
  return Pack(Mul(a, Invert(c)));
}

X25519Key X25519Public(const X25519Key& privateKey) {
  X25519Key nine{};
  nine[0] = 9;
  return X25519(privateKey, nine);
}

}  // namespace sovereign::tray
