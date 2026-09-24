/*
 * retro-dosbox — bundled demo content.
 *
 * An emulator with no games looks broken. On a fresh install, before any folder
 * has been granted, the library is empty and there is nothing to press -- and
 * for the iOS App Store that is not merely untidy: a reviewer with no way to
 * make the app do anything is a rejection.
 *
 * So two pieces of content ship inside the app and appear automatically when
 * nothing else is found:
 *
 *   DEMO      -- a small VGA demonstration written for this project
 *   FREEDOS   -- FreeDOS 1.3, booted from its boot floppy image. The floppy
 *                runs the FreeDOS installer on boot; answering N to its first
 *                question leaves a real DOS prompt. Installing is not possible
 *                from this image alone: the package floppies are not bundled.
 *
 * Both are extracted out of the package to a real filesystem path on first use,
 * because DOSBox-X mounts a directory BY PATH and cannot read out of an APK or
 * an app bundle.
 *
 * See demo/NOTICE.md for the licensing of the bundled content.
 */
#ifndef RETRODOS_DEMO_H
#define RETRODOS_DEMO_H

#include <string>

namespace retrodos {

/** Which bundled item an entry refers to. */
enum class DemoKind { Demo, FreeDos };

/**
 * Extract the bundled content into [dest_dir], creating it if needed.
 *
 * Cheap to call repeatedly: files already present and the right size are left
 * alone, so this is a no-op after the first launch.
 *
 * Returns false if the content could not be unpacked, in which case the caller
 * should simply not offer the demo rather than showing an entry that cannot
 * start.
 */
bool demo_prepare(const std::string &dest_dir);

/** The autoexec command that starts [kind], and whether it must be emitted
 *  unquoted (see build_conf's run_raw). */
std::string demo_command(DemoKind kind, bool &run_raw);

/** Display name for the launcher list. */
std::string demo_title(DemoKind kind);

/** True when the item starts by waiting for the keyboard, so the frontend
 *  should open its on-screen keyboard as the machine comes up. */
bool demo_wants_keyboard(DemoKind kind);

} /* namespace retrodos */

#endif /* RETRODOS_DEMO_H */
