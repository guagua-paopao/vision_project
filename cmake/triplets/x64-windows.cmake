set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

# This workspace has multiple MSVC toolsets installed. v145 currently fails
# while launching c1.dll on this host; v142 is the stable ABI used by the
# existing CUDA/TensorRT/OpenCV build.
set(VCPKG_PLATFORM_TOOLSET v142)
