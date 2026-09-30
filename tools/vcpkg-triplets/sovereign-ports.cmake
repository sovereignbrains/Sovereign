# What every Sovereign triplet adds for particular ports (included by them).

# ZXing's debug build writes its binarizer's thresholds to thresholds_new.pnm
# in the working directory on every scan (core/src/HybridBinarizer.cpp,
# #ifndef NDEBUG) - the tray's Debug build and the tests would leave it
# wherever they run. Its debug library is built with NDEBUG: the debug CRT
# and iterator checks stay (they follow _DEBUG), that file and ZXing's own
# asserts don't.
if(PORT STREQUAL "nu-book-zxing-cpp")
  set(VCPKG_C_FLAGS_DEBUG "${VCPKG_C_FLAGS_DEBUG} /DNDEBUG")
  set(VCPKG_CXX_FLAGS_DEBUG "${VCPKG_CXX_FLAGS_DEBUG} /DNDEBUG")
endif()
