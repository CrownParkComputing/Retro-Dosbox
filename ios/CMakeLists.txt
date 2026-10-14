# The iOS application: DOSDeck.
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
# WHY IT IS NOT CALLED RETRO-DOS, AND WHY IT IS SWIFT
# ---------------------------------------------------
# Apple rejected the whole Retro-* family under guideline 4.3 -- spam, several
# apps too alike from one developer. The answer is not a rename: it is that the
# iOS app is its own app, with its own name, icon, identifier and listing, and
# shares only the engine underneath. DOSDeck is that app. Android keeps
# Retro-DOS and its signing key; nothing here is a second copy of it.
#
# The rename alone was not enough, because the INTERFACE was still the family's
# -- frontend/retrodos_frontend.cpp, an SDL3 window with a Dear ImGui shell,
# the same file the Android app draws. So the interface is now ios/DOSDeck:
# SwiftUI, written for this platform, sharing no screen with anything else we
# ship. Swift owns main(), the window and the application lifecycle; SDL is
# linked only because DOSBox-X needs it for audio and timing, and never sees a
# window. ios/build-core.sh compiles no ImGui at all.
#
# The name credits nothing on its own, so the app does it explicitly:
# RETRODOS_APP_CORE puts "powered by DOSBox-X" under the wordmark on the first
# screen. That is the GPL's requirement and the right thing besides.
cmake_minimum_required(VERSION 3.20)
# Swift is a first-class language of this target, not a bolt-on: the whole
# interface is written in it.
project(DOSDeck LANGUAGES CXX Swift)

set(RETRODOS_VERSION "1.0" CACHE STRING "CFBundleShortVersionString")
# App Store Connect refuses a CFBundleVersion it has already seen, and says so
# only after the whole upload has transferred, so CI stamps a UTC timestamp.
set(RETRODOS_BUILD "1" CACHE STRING "CFBundleVersion")
set(RETRODOS_BUNDLE_ID "com.crownparkcomputing.dosdeck" CACHE STRING "bundle id")
set(RETRODOS_DISPLAY_NAME "DOSDeck" CACHE STRING "CFBundleDisplayName")
set(RETRODOS_APP_NAME "DOSDeck" CACHE STRING "the name the app calls itself")
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
# demo/FREEDOS.IMG, relative to the bundle. FREEDOS.IMG lives under android/
# because that is where it was first packaged; it is read here, never
# modified.
set(DEMO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../android/app/src/main/assets/demo")

# DEMO.COM is BUILT here rather than copied, because its banner names the app
# and the committed copy names the Android one. The review notes send the
# reviewer to the Demo page as the fastest way to see the app work, so that
# banner is very likely the first text they read -- and it used to greet them
# with "Retro-DOS", which is the leftover guideline 4.3 reads as one app
# submitted twice. Android keeps the committed 354-byte file untouched;
# make_demo.py defaults to Retro-DOS precisely so that stays byte-identical.
set(DEMO_COM "${CMAKE_CURRENT_BINARY_DIR}/bundle/demo/DEMO.COM")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/../demo/make_demo.py"
          "${DEMO_COM}" --name "${RETRODOS_APP_NAME}"
  RESULT_VARIABLE demo_rc OUTPUT_VARIABLE demo_out ERROR_VARIABLE demo_out)
if(NOT demo_rc EQUAL 0)
  message(FATAL_ERROR "could not build DEMO.COM: ${demo_out}")
endif()
message(STATUS "demo: ${demo_out}")

set(DEMO_FILES "${DEMO_COM}" "${DEMO_SRC}/FREEDOS.IMG")

# NO INTERFACE ASSETS.
#
# The ImGui frontend bundled a UI font and a PNG wordmark under assets/ui/,
# which retrodos_brand.cpp loaded through SDL_GetBasePath. The Swift interface
# needs neither: it draws the name as live text in SF Mono, which is sharper at
# every size, reads correctly at any accessibility setting, and cannot drift out
# of step with the app's name the way a baked image can.
#
# tool/make_dosdeck_art.py still produces the wordmark -- the store listing
# uses it -- it is just not shipped inside the app any more.
foreach(f ${DEMO_FILES})
  if(NOT EXISTS "${f}")
    message(FATAL_ERROR "bundled demo content missing: ${f}")
  endif()
endforeach()

configure_file(Info.plist.in "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" @ONLY)

# The Swift interface. Listed file by file rather than globbed: a GLOB is
# evaluated when CMake runs, so a new screen added later would silently not be
# compiled until somebody re-ran cmake by hand.
set(SWIFT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/DOSDeck")
set(SWIFT_SOURCES
  "${SWIFT_DIR}/DOSDeckApp.swift"        # @main, the scene, the audio session
  "${SWIFT_DIR}/Theme.swift"             # the amber-phosphor look and its controls
  "${SWIFT_DIR}/Storage.swift"           # the user's folders, and what is in them
  "${SWIFT_DIR}/Zip.swift"               # unpacking an archive into a mountable folder
  "${SWIFT_DIR}/Engine.swift"            # the one session, and what can be done inside it
  "${SWIFT_DIR}/EmuSurface.swift"        # Metal, fed by the engine's framebuffer tap
  "${SWIFT_DIR}/FrontDoor.swift"         # Windows or DOS, and About
  "${SWIFT_DIR}/DosSetupView.swift"      # the DOS wizard
  "${SWIFT_DIR}/WindowsMachine.swift"    # the Windows machine and its phases
  "${SWIFT_DIR}/WindowsSetupView.swift"  # the Windows wizard and its walkthrough
  "${SWIFT_DIR}/SessionView.swift"       # the machine running, and the panels over it
  "${SWIFT_DIR}/DosKeyboard.swift")      # a PC keyboard, in scancodes
foreach(f ${SWIFT_SOURCES})
  if(NOT EXISTS "${f}")
    message(FATAL_ERROR "Swift source missing: ${f}")
  endif()
endforeach()

# One empty C++ translation unit, and it is not vestigial: it keeps CXX in the
# target so that the toolchain links the C++ runtime. Everything in
# libretrodos.a is C++, and a Swift-only target would leave every libc++ symbol
# undefined at link time.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/app_main.cpp"
     "// Intentionally empty. main() is Swift's, from @main in DOSDeckApp.swift;\n"
     "// this file exists so the target still compiles C++ and therefore links\n"
     "// libc++, which the engine in libretrodos.a needs.\n")

set(ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/Assets.xcassets")
if(NOT EXISTS "${ASSETS}/AppIcon.appiconset/Icon-App-1024x1024@1x.png")
  # make_dosdeck_art.py, NOT make_icons.py: the latter builds the Retro-*
  # family look deliberately, and running it here would replace this app's
  # icons with the very artwork guideline 4.3 objected to.
  message(FATAL_ERROR "app icons missing -- run tool/make_dosdeck_art.py")
endif()

add_executable(DOSDeck MACOSX_BUNDLE
  "${CMAKE_CURRENT_BINARY_DIR}/app_main.cpp"
  ${SWIFT_SOURCES}
  "${ASSETS}"
  ${DEMO_FILES})

# The catalog is compiled by actool into Assets.car, and the icon NAME is what
# puts CFBundleIcons in the built Info.plist. A delivery without that key is
# accepted and then rejected in processing, by email, long after CI went green.
set_source_files_properties("${ASSETS}" PROPERTIES
  MACOSX_PACKAGE_LOCATION Resources)

set_source_files_properties(${DEMO_FILES} PROPERTIES
  MACOSX_PACKAGE_LOCATION "Resources/demo")

set_target_properties(DOSDeck PROPERTIES
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
  XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "${RETRODOS_CODE_SIGN_IDENTITY}"
  # Swift reaches the engine through ONE header, which is plain C on purpose:
  # the C++ underneath has std::string in its signatures and Swift should not
  # have to know that it exists.
  XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER
    "${CMAKE_CURRENT_SOURCE_DIR}/bridge/dosdeck_bridge.h"
  XCODE_ATTRIBUTE_SWIFT_VERSION "5.0"
  XCODE_ATTRIBUTE_ALWAYS_SEARCH_USER_PATHS "NO")

# The app's own name reaches the C++ through one define, so a window title or a
# settings folder cannot keep saying Retro-DOS while the icon says DOSDeck --
# which is precisely the kind of leftover App Review reads as one app submitted
# twice. It is set on the TARGET, so the library built by build-core.sh (which
# has no idea which app it is going into) is unaffected... except that
# retrodos_config.cpp and retrodos_win98.cpp ARE in that library and write the
# name into a conf and a settings file, so build-core.sh passes the same two.
target_compile_definitions(DOSDeck PRIVATE
  RETRODOS_APP_NAME="${RETRODOS_APP_NAME}"
  RETRODOS_APP_CORE="${RETRODOS_APP_CORE}")

# -all_load, and it is not optional: nothing in the empty main references the
# emulator, so without it the linker discards essentially all of it and the app
# starts into a black screen with no error.
target_link_options(DOSDeck PRIVATE
  "-Wl,-all_load" "${LIBDIR}/libretrodos.a")

target_link_libraries(DOSDeck PRIVATE
  "${LIBDIR}/libSDL3.a" z iconv
  # MetalKit for the view the picture is drawn into. The rest of the Swift
  # frameworks (SwiftUI, Metal, AVFoundation, Compression,
  # UniformTypeIdentifiers) are autolinked from their import statements.
  "-framework MetalKit"
  "-framework CoreFoundation" "-framework CoreMIDI" "-framework AudioToolbox"
  "-framework UIKit" "-framework Foundation" "-framework CoreGraphics"
  "-framework CoreVideo" "-framework CoreMedia" "-framework AVFoundation"
  "-framework CoreBluetooth" "-framework CoreMotion" "-framework Metal"
  "-framework QuartzCore" "-framework OpenGLES"
  "-framework UniformTypeIdentifiers" "-framework ImageIO"
  "-weak_framework GameController" "-weak_framework CoreHaptics")
