# The conformance harness's wire-level pieces (P3, issue #5): reading what a
# client puts on the wire and bringing it to a form that compares byte for byte
# with the pinned reference sing-box. Platform-independent on purpose - the
# fuzz and TSan jobs build it too.
add_library(sovereign_conformance STATIC
  client_hello.cpp
  client_hello.h
)
target_include_directories(sovereign_conformance PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
