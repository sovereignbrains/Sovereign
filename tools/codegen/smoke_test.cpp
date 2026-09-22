#include <cassert>
#include <iostream>

#include "../../src/generated/anytls_outbound.gen.h"

int main() {
  sovereign::codegen::option::AnyTLSOutboundOptions opts;
  opts.server = "example.com";
  opts.serverPort = 443;
  opts.password = "hunter2";
  opts.tcpFastOpen = true;

  nlohmann::json j = opts;
  assert(j["server"] == "example.com");
  assert(j["server_port"] == 443);

  sovereign::codegen::option::AnyTLSOutboundOptions roundTripped = j;
  assert(roundTripped.server == opts.server);
  assert(roundTripped.serverPort == opts.serverPort);
  assert(roundTripped.password == opts.password);
  assert(roundTripped.tcpFastOpen == opts.tcpFastOpen);

  std::cout << "OK: " << j.dump() << "\n";
  return 0;
}
