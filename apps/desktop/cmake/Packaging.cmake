# Install rules and CPack configuration for the desktop app.
#
#   cmake --build <build> --target package     (or: cpack --config <build>/CPackConfig.cmake)
#
# macOS   Convertible-<version>-macos-<arch>.zip      Convertible.app (unsigned)
# Windows Convertible-<version>-windows-<arch>.zip    Convertible.exe + licences
# Linux   convertible-desktop-<version>-linux-<arch>.tar.gz and .deb
#         (binary, .desktop entry, hicolor icons, licences)

set(_cvd_licences
  "${CVD_REPO_ROOT}/LICENSE"
  "${CVD_ASSETS}/fonts/Inter-LICENSE.txt"
  "${CVD_ASSETS}/fonts/JetBrainsMono-LICENSE.txt")

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
  set(_cvd_arch "arm64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  set(_cvd_arch "x64")
else()
  set(_cvd_arch "${CMAKE_SYSTEM_PROCESSOR}")
endif()
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
  string(REPLACE ";" "-" _cvd_arch "${CMAKE_OSX_ARCHITECTURES}")
  string(REPLACE "x86_64" "x64" _cvd_arch "${_cvd_arch}")
endif()

set(CPACK_PACKAGE_NAME "Convertible")
set(CPACK_PACKAGE_VENDOR "Convertible contributors")
set(CPACK_PACKAGE_VERSION "${CVD_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Offline, privacy-first universal file converter")
set(CPACK_PACKAGE_HOMEPAGE_URL "")
set(CPACK_RESOURCE_FILE_LICENSE "${CVD_REPO_ROOT}/LICENSE")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/packages")
set(CPACK_STRIP_FILES ON)

if(APPLE)
  install(TARGETS convertible_desktop BUNDLE DESTINATION . COMPONENT app)
  install(FILES ${_cvd_licences} DESTINATION "Convertible.app/Contents/Resources/Licenses" COMPONENT app)
  set(CPACK_GENERATOR "ZIP")
  set(CPACK_PACKAGE_FILE_NAME "Convertible-${CVD_VERSION}-macos-${_cvd_arch}")
  set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
elseif(WIN32)
  install(TARGETS convertible_desktop RUNTIME DESTINATION . COMPONENT app)
  install(FILES ${_cvd_licences} DESTINATION "licences" COMPONENT app)
  set(CPACK_GENERATOR "ZIP")
  set(CPACK_PACKAGE_FILE_NAME "Convertible-${CVD_VERSION}-windows-${_cvd_arch}")
  set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
else()
  include(GNUInstallDirs)
  install(TARGETS convertible_desktop RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT app)
  install(FILES "${CVD_DIR}/packaging/linux/app.convertible.desktop.desktop"
          DESTINATION "${CMAKE_INSTALL_DATADIR}/applications" COMPONENT app)
  foreach(_size 16 24 32 48 64 128 256 512)
    install(FILES "${CVD_ASSETS}/brand/icons/desktop/icon-${_size}.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_size}x${_size}/apps"
            RENAME "app.convertible.desktop.png" COMPONENT app)
  endforeach()
  install(FILES ${_cvd_licences} DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/convertible-desktop" COMPONENT app)
  set(CPACK_GENERATOR "TGZ;DEB")
  set(CPACK_PACKAGE_FILE_NAME "convertible-desktop-${CVD_VERSION}-linux-${_cvd_arch}")
  set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
  set(CPACK_DEBIAN_PACKAGE_NAME "convertible-desktop")
  set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
  set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Convertible contributors")
  set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
  set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6 (>= 2.35), libstdc++6 (>= 14), libx11-6, libxext6")
  set(CPACK_DEBIAN_PACKAGE_DESCRIPTION
      "Offline, privacy-first universal file converter.\n Everything converts on this device; nothing is uploaded.")
endif()

include(CPack)
