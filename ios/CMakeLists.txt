# The iOS application: Autoexec.
#
#   ios/build-core.sh                      # produces the static libraries
#   cmake -S ios -B ios/build/app -G Xcode \
#         -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64
#   cmake --build ios/build/app --config Release
#
# CMake generates the Xcode project rather than one being committed: a
# .pbxproj is 40 KB of generated state that nobody can review in a diff, and
# every setting worth having is expressed here instead.
#
# WHY IT IS NOT CALLED RETRO-DOS
# ------------------------------
# Apple rejected the whole Retro-* family under guideline 4.3 -- spam, several
# apps too alike from one developer. The answer is not a rename: it is that the
# iOS app is its own app, with its own name, icon, identifier and listing, and
# shares only the engine underneath. Autoexec is that app. Android keeps
# Retro-DOS and its signing key; nothing here is a second copy of it.
#
# The name credits nothing on its own, so the app does it explicitly:
# RETRODOS_APP_CORE puts "powered by DOSBox-X" under the wordmark on the first
# screen. That is the GPL's requirement and the right thing besides.
cmake_minimum_required(VERSION 3.20)
project(Autoexec LANGUAGES CXX)

set(RETRODOS_VERSION "1.0" CACHE STRING "CFBundleShortVersionString")
# App Store Connect refuses a CFBundleVersion it has already seen, and says so
# only after the whole upload has transferred, so CI stamps a UTC timestamp.
set(RETRODOS_BUILD "1" CACHE STRING "CFBundleVersion")
set(RETRODOS_BUNDLE_ID "com.crownparkcomputing.autoexec" CACHE STRING "bundle id")
set(RETRODOS_DISPLAY_NAME "Autoexec" CACHE STRING "CFBundleDisplayName")
set(RETRODOS_APP_NAME "Autoexec" CACHE STRING "the name the app calls itself")
set(RETRODOS_APP_CORE "DOSBox-X" CACHE STRING "the emulator it credits")
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

# Autoexec's own wordmark, copied in under the name the frontend asks for.
#
# retrodos_brand.cpp loads "assets/ui/wordmark.png" and nothing else, so the
# choice of which mark is made here, at packaging time, rather than by teaching
# the app about platforms. Retro-DOS's mark is deliberately NOT used: it is the
# family artwork, and the family look is what guideline 4.3 objected to.
set(RETRODOS_WORDMARK "${UI_SRC}/wordmark-autoexec.png"
    CACHE FILEPATH "the wordmark this app draws in its rail")
if(NOT EXISTS "${RETRODOS_WORDMARK}")
  message(FATAL_ERROR "wordmark missing: ${RETRODOS_WORDMARK}"
                      " -- run tool/make_autoexec_art.py")
endif()
configure_file("${RETRODOS_WORDMARK}"
               "${CMAKE_CURRENT_BINARY_DIR}/bundle/assets/ui/wordmark.png" COPYONLY)

set(UI_FILES "${UI_SRC}/ui-font.ttf"
             "${CMAKE_CURRENT_BINARY_DIR}/bundle/assets/ui/wordmark.png")
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

add_executable(Autoexec MACOSX_BUNDLE
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

set_target_properties(Autoexec PROPERTIES
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

# The app's own name reaches the C++ through one define, so a window title or a
# settings folder cannot keep saying Retro-DOS while the icon says Autoexec --
# which is precisely the kind of leftover App Review reads as one app submitted
# twice. It is set on the TARGET, so the library built by build-core.sh (which
# has no idea which app it is going into) is unaffected... except that the
# frontend IS in that library, so build-core.sh passes the same two defines.
target_compile_definitions(Autoexec PRIVATE
  RETRODOS_APP_NAME="${RETRODOS_APP_NAME}"
  RETRODOS_APP_CORE="${RETRODOS_APP_CORE}")

# -all_load, and it is not optional: nothing in the empty main references the
# emulator, so without it the linker discards essentially all of it and the app
# starts into a black screen with no error.
target_link_options(Autoexec PRIVATE
  "-Wl,-all_load" "${LIBDIR}/libretrodos.a")

target_link_libraries(Autoexec PRIVATE
  "${LIBDIR}/libSDL3.a" z iconv
  "-framework CoreFoundation" "-framework CoreMIDI" "-framework AudioToolbox"
  "-framework UIKit" "-framework Foundation" "-framework CoreGraphics"
  "-framework CoreVideo" "-framework CoreMedia" "-framework AVFoundation"
  "-framework CoreBluetooth" "-framework CoreMotion" "-framework Metal"
  "-framework QuartzCore" "-framework OpenGLES"
  "-framework UniformTypeIdentifiers" "-framework ImageIO"
  "-weak_framework GameController" "-weak_framework CoreHaptics")
