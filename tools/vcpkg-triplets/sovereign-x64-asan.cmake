# The ASan presets' dependencies (ci-asan, ci-fuzz): static libraries built
# with /fsanitize=address like the code that links them. MSVC's STL marks
# every object with whether its containers are ASan-annotated
# (#pragma detect_mismatch "annotate_string"/"annotate_vector"), so an
# uninstrumented library next to instrumented code fails the link (LNK2038) -
# and an instrumented one is checked too.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_C_FLAGS "/fsanitize=address")
set(VCPKG_CXX_FLAGS "/fsanitize=address")
include("${CMAKE_CURRENT_LIST_DIR}/sovereign-ports.cmake")
