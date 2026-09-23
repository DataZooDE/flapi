vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO CrowCpp/crow
    REF "v${VERSION}"
    SHA512 0fdba3c3697f53ff231cc1637b613f382b5c0230b700745548c1a0ef03c3b25f92ec15f8d1f9bca1a74cffe07053d7a829732475a8a392ee8c682ccfba91539e
    HEAD_REF master
    # Only our patch. The upstream port's remove-cpm.patch targets crow
    # 1.3.0, whose CMakeLists includes CPM; 1.2.0 - the version this project
    # builds - has no CPM reference at all, and applying it there fails.
    PATCHES
        async-completion-keepalive.patch
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DCROW_BUILD_EXAMPLES=OFF
        -DCROW_BUILD_TESTS=OFF
        -DCMAKE_DISABLE_FIND_PACKAGE_Python3=ON
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/Crow)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/lib")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
