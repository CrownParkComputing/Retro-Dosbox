# The iOS application.
#
#   ios/build-core.sh                      # produces the static libraries
#   cmake -S ios -B ios/build/app -G Xcode \
#         -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64
#   cmake --build ios/build/app --config Release
#
# CMake generates the Xcode project rather than one being committed: a
# .pbxproj is 40 KB of generated state that nobody can review in a diff, and
# every setting worth having is expressed here instead.
cmake_minimum_required(VERSION 3.20)
project(RetroDos LANGUAGES CXX)

set(RETRODOS_VERSION "2.0" CACHE STRING "CFBundleShortVersionString")
# App Store Connect refuses a CFBundleVersion it has already seen, and says so
# only after the whole upload has transferred, so CI stamps a UTC timestamp.
set(RETRODOS_BUILD "1" CACHE STRING "CFBundleVersion")
set(RETRODOS_BUNDLE_ID "com.crownparkcomputing.dosboxretro" CACHE STRING "bundle id")
set(RETRODOS_DISPLAY_NAME "Retro-DOS" CACHE STRING "CFBundleDisplayName")
set(RETRODOS_TEAM_ID "88A3T9K9C5" CACHE STRING "DEVELOPMENT_TEAM")
set(RETRODOS_CODE_SIGN_STYLE "Automatic" CACHE STRING "Automatic or Manual")
set(RETRODOS_PROVISIONING_PROFILE "" CACHE STRING "profile name, Manual signing only")
set(RETRODOS_CODE_SIGN_IDENTITY "" CACHE STRING "e.g. Apple Distribution")

if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0")
endif()

# Which slice ios/build-core.sh produced. The simulator and the device need
# separate libraries; there is no fat static lib here on purpose, because the
# two are configured trees with different SDKs, not two arches of one build.
set(RETRODOS_LIB_PLATFORM "iphoneos" CACHE STRING "iphoneos or iphonesimulator")
set(LIBDIR "${CMAKE_CURRENT_SOURCE_DIR}/build/${RETRODOS_LIB_PLATFORM}/out")
foreach(lib libretrodos.a libSDL3.a)
  if(NOT EXISTS "${LIBDIR}/${lib}")
    message(FATAL_ERROR "${LIBDIR}/${lib} is missing -- run ios/build-core.sh first.")
  endif()
endforeach()

# The demo content the frontend looks for as demo/DEMO.COM and
# demo/FREEDOS.IMG, relative to the bundle. It lives under android/ because
# that is where it was first packaged; it is read here, never modified.
set(DEMO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../android/app/src/main/assets/demo")
set(DEMO_FILES "${DEMO_SRC}/DEMO.COM" "${DEMO_SRC}/FREEDOS.IMG")

# The interface assets: the UI font and the wordmark. They go into the bundle
# under assets/ui/, which is exactly the path retrodos_brand.cpp asks
# SDL_GetBasePath for.
set(UI_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../assets/ui")
set(UI_FILES "${UI_SRC}/ui-font.ttf" "${UI_SRC}/wordmark.png")
foreach(f ${UI_FILES})
  if(NOT EXISTS "${f}")
    message(FATAL_ERROR "bundled interface asset missing: ${f}")
  endif()
endforeach()
foreach(f ${DEMO_FILES})
  if(NOT EXISTS "${f}")
    message(FATAL_ERROR "bundled demo content missing: ${f}")
  endif()
endforeach()

configure_file(Info.plist.in "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" @ONLY)

# One translation unit, deliberately empty. Everything executable is already
# inside libretrodos.a, including the frontend's main(); Xcode still wants a
# source file to hang the target on.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/app_main.cpp"
     "// Intentionally empty: main() comes from libretrodos.a (the frontend),\n"
     "// which SDL renames to SDL_main and calls from its own UIKit entry.\n")

set(ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/Assets.xcassets")
if(NOT EXISTS "${ASSETS}/AppIcon.appiconset/Icon-App-1024x1024@1x.png")
  message(FATAL_ERROR "app icons missing -- run tool/make_icons.py")
endif()

add_executable(RetroDos MACOSX_BUNDLE
  "${CMAKE_CURRENT_BINARY_DIR}/app_main.cpp"
  "${ASSETS}"
  ${DEMO_FILES}
  ${UI_FILES})

# The catalog is compiled by actool into Assets.car, and the icon NAME is what
# puts CFBundleIcons in the built Info.plist. A delivery without that key is
# accepted and then rejected in processing, by email, long after CI went green.
set_source_files_properties("${ASSETS}" PROPERTIES
  MACOSX_PACKAGE_LOCATION Resources)

set_source_files_properties(${DEMO_FILES} PROPERTIES
  MACOSX_PACKAGE_LOCATION "Resources/demo")

set_source_files_properties(${UI_FILES} PROPERTIES
  MACOSX_PACKAGE_LOCATION "Resources/assets/ui")

set_target_properties(RetroDos PROPERTIES
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
  XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${RETRODOS_BUNDLE_ID}"
  XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${RETRODOS_TEAM_ID}"
  XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"          # iPhone and iPad
  XCODE_ATTRIBUTE_ENABLE_BITCODE "NO"
  XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC "YES"
  XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
  # Without these two, `xcodebuild archive` succeeds and produces an archive
  # with an EMPTY Products/ and no ApplicationProperties -- not an app archive
  # at all, so -exportArchive has nothing to export. CMake leaves SKIP_INSTALL
  # at its default for Xcode targets, and an archive is an install.
  XCODE_ATTRIBUTE_INSTALL_PATH "$(LOCAL_APPS_DIR)"
  XCODE_ATTRIBUTE_SKIP_INSTALL "NO"
  # Signing is set on the TARGET, never on the xcodebuild command line: a
  # command-line PROVISIONING_PROFILE_SPECIFIER applies to every target in the
  # workspace, which is how a build starts failing with "does not support
  # provisioning profiles" on something that is not the app.
  XCODE_ATTRIBUTE_CODE_SIGN_STYLE "${RETRODOS_CODE_SIGN_STYLE}"
  XCODE_ATTRIBUTE_PROVISIONING_PROFILE_SPECIFIER "${RETRODOS_PROVISIONING_PROFILE}"
  XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "${RETRODOS_CODE_SIGN_IDENTITY}")

# -all_load, and it is not optional: nothing in the empty main references the
# emulator, so without it the linker discards essentially all of it and the app
# starts into a black screen with no error.
target_link_options(RetroDos PRIVATE
  "-Wl,-all_load" "${LIBDIR}/libretrodos.a")

target_link_libraries(RetroDos PRIVATE
  "${LIBDIR}/libSDL3.a" z iconv
  "-framework CoreFoundation" "-framework CoreMIDI" "-framework AudioToolbox"
  "-framework UIKit" "-framework Foundation" "-framework CoreGraphics"
  "-framework CoreVideo" "-framework CoreMedia" "-framework AVFoundation"
  "-framework CoreBluetooth" "-framework CoreMotion" "-framework Metal"
  "-framework QuartzCore" "-framework OpenGLES"
  "-framework UniformTypeIdentifiers" "-framework ImageIO"
  "-weak_framework GameController" "-weak_framework CoreHaptics")
