package com.crownparkcomputing.retrodos

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import org.libsdl.app.SDLActivity

/**
 * Retro-DOS.
 *
 * Deliberately thin. The launcher, settings, scaling, keyboard and overlay all
 * live in the native SDL3 + ImGui frontend, so they are shared with the desktop
 * build rather than reimplemented per platform.
 *
 * What cannot live there is the Storage Access Framework: only an Activity can
 * launch the folder picker and receive its result.
 *
 * The old startActivityForResult/onActivityResult pair is used rather than the
 * modern ActivityResultLauncher because SDLActivity extends plain
 * android.app.Activity, not androidx's ComponentActivity -- registerFor-
 * ActivityResult simply does not exist here.
 */
class MainActivity : SDLActivity() {

    companion object {
        private const val REQ_PICK_FOLDER = 0x5AF0
        private const val REQ_PICK_GAME   = 0x5AF1
        private const val REQ_PICK_ROOT   = 0x5AF2
    }

    override fun getLibraries(): Array<String> = arrayOf("SDL3", "retrodos")

    /** Not libmain.so -- the core keeps its own name. */
    override fun getMainSharedObject(): String =
        "${applicationInfo.nativeLibraryDir}/libretrodos.so"

    override fun getMainFunction(): String = "SDL_main"

    /** No arguments: the frontend discovers the library and generates the
     *  engine's config itself. Passing a game in from here would put half the
     *  launch logic on the Android side, where the desktop build cannot use it. */
    override fun getArguments(): Array<String> = arrayOf()

    override fun onCreate(savedInstanceState: Bundle?) {
        SafBridge.activity = this
        /* MediaBridge needs a Context for SharedPreferences and the Keystore;
         * it never touches the UI, but it cannot reach either without one. */
        MediaBridge.activity = this
        super.onCreate(savedInstanceState)
    }

    override fun onDestroy() {
        SafBridge.activity = null
        MediaBridge.activity = null
        super.onDestroy()
    }

    /** Called from SafBridge on the UI thread. */
    fun launchFolderPicker() {
        val i = Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).apply {
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or
                     Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION)
        }
        startActivityForResult(i, REQ_PICK_FOLDER)
    }

    /** The parent folder for everything: read AND write, persisted. */
    fun launchRootPicker() {
        val i = Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).apply {
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or
                     Intent.FLAG_GRANT_WRITE_URI_PERMISSION or
                     Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION)
        }
        startActivityForResult(i, REQ_PICK_ROOT)
    }

    /** A single game -- an archive or a bare DOS executable -- to copy in. */
    fun launchGamePicker() {
        val i = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        startActivityForResult(i, REQ_PICK_GAME)
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        if (requestCode == REQ_PICK_GAME) {
            if (resultCode == Activity.RESULT_OK) data?.data?.let { SafBridge.onGamePicked(it) }
            return
        }
        if (requestCode == REQ_PICK_ROOT) {
            if (resultCode == Activity.RESULT_OK) data?.data?.let { SafBridge.onRootPicked(it) }
            return
        }
        if (requestCode == REQ_PICK_FOLDER) {
            if (resultCode == Activity.RESULT_OK) data?.data?.let { SafBridge.onFolderPicked(it) }
            return
        }
        super.onActivityResult(requestCode, resultCode, data)
    }
}
