# The dependencies of the Debug presets (ci, ci-analyze, debug, release):
# static libraries on the DLL CRT, like x64-windows-static-md, plus
# sovereign-ports.cmake. The dist preset (Release, CRT linked in) uses
# x64-windows-static.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
include("${CMAKE_CURRENT_LIST_DIR}/sovereign-ports.cmake")
