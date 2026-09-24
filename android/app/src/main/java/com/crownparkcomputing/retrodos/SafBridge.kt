package com.crownparkcomputing.retrodos

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.DocumentsContract
import android.util.Log
import androidx.documentfile.provider.DocumentFile
import java.io.File

/**
 * Storage Access Framework bridge.
 *
 * Android will not hand an app broad file access -- MANAGE_EXTERNAL_STORAGE is
 * reserved for file managers -- so a library kept outside the app's own
 * directories is reachable only as a *document tree* the user explicitly
 * grants.
 *
 * That creates a problem the emulator cannot solve on its own: SAF yields
 * `content://` URIs, and DOSBox-X mounts a DIRECTORY BY PATH. There is no
 * `mount C content://...`. So this bridge does two separate jobs:
 *
 *   1. ENUMERATE over SAF, which is enough to build the launcher list. No
 *      real path is needed just to show names.
 *   2. STAGE the one game being launched into the app's own directory, which
 *      IS a real path. A DOS game is typically a few megabytes, so copying the
 *      selected title costs little -- and nothing else is ever copied.
 *
 * Everything here is called from native code via JNI, so the methods are
 * static and take/return only primitives and strings.
 */
object SafBridge {

    private const val TAG   = "retrodos"
    private const val PREFS = "retrodos_saf"
    private const val KEY   = "tree_uri"
    private const val ROOT  = "root_uri"

    /** Set by MainActivity so the bridge can reach a Context and the picker. */
    @Volatile @JvmStatic var activity: MainActivity? = null

    private fun ctx(): Context? = activity?.applicationContext

    /* ---------------------------------------------------------------- */
    /* Grant                                                             */
    /* ---------------------------------------------------------------- */

    /** Launch the system folder picker. Returns immediately; the grant lands
     *  asynchronously, so native code polls treeUri(). */
    @JvmStatic
    fun pick() {
        val a = activity ?: return
        a.runOnUiThread { a.launchFolderPicker() }
    }

    /** The persisted tree URI, or "" if the user has not granted one (or the
     *  grant has been revoked, which happens if the SD card is reformatted). */
    @JvmStatic
    fun treeUri(): String {
        val c = ctx() ?: return ""
        val saved = c.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getString(KEY, null) ?: return ""

        /* A saved string is not a live grant. Check it is still in the
         * persisted list, otherwise the launcher would show a library the app
         * can no longer read. */
        val uri = Uri.parse(saved)
        val held = c.contentResolver.persistedUriPermissions.any {
            it.uri == uri && it.isReadPermission
        }
        return if (held) saved else ""
    }

    /** Called by MainActivity when the user picks a folder. */
    @JvmStatic
    fun onFolderPicked(uri: Uri) {
        val c = ctx() ?: return
        try {
            /* Persist, or the grant dies with the process and the user is
             * asked again on every launch. */
            c.contentResolver.takePersistableUriPermission(
                uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        } catch (e: SecurityException) {
            Log.w(TAG, "could not persist grant: $e")
        }
        c.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit().putString(KEY, uri.toString()).apply()
        Log.i(TAG, "SAF folder granted: $uri")
    }

    /* ---------------------------------------------------------------- */
    /* The one parent folder                                             */
    /* ---------------------------------------------------------------- */

    /*
     * Different from the collection grant above. That one is read-only and
     * ENUMERATED, with each game copied in as it is played. This one is the
     * folder the app keeps everything in -- games/, discs/, machines/ -- so
     * it has to be a real, writable path: the emulator writes an 8 GB disk
     * image into machines/ and mounts a game folder out of games/ by path.
     *
     * A document-tree grant on external storage resolves to such a path
     * (/storage/emulated/0/<relative>, or /storage/<volume>/<relative>) and
     * the app can use it directly for anything it creates there itself.
     * Whether it can also read files the user dropped in from a PC depends
     * on the Android version and the folder, which is why rootPath() proves
     * the folder usable rather than assuming it, and the frontend falls back
     * to app storage when it is not.
     */

    /** Launch the folder picker for the parent folder, asking for write. */
    @JvmStatic
    fun pickRoot() {
        val a = activity ?: return
        a.runOnUiThread { a.launchRootPicker() }
    }

    /** Called by MainActivity when the user picks the parent folder. */
    @JvmStatic
    fun onRootPicked(uri: Uri) {
        val c = ctx() ?: return
        try {
            c.contentResolver.takePersistableUriPermission(
                uri, Intent.FLAG_GRANT_READ_URI_PERMISSION or
                     Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
        } catch (e: SecurityException) {
            Log.w(TAG, "could not persist root grant: $e")
        }
        c.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit().putString(ROOT, uri.toString()).apply()
        Log.i(TAG, "storage root granted: $uri")
    }

    /** Forget the parent folder, for "run setup again". The grant itself is
     *  left in place: releasing it and re-taking it gains nothing. */
    @JvmStatic
    fun clearRoot() {
        ctx()?.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            ?.edit()?.remove(ROOT)?.apply()
    }

    /** A readable label for the granted root, or "" when none. */
    @JvmStatic
    fun rootLabel(): String {
        val saved = savedRoot() ?: return ""
        return try {
            val id = DocumentsContract.getTreeDocumentId(Uri.parse(saved))
            val colon = id.indexOf(':')
            if (colon < 0) id
            else {
                val vol = id.substring(0, colon)
                val rel = id.substring(colon + 1)
                (if (vol == "primary") "Internal storage" else "SD card $vol") +
                    (if (rel.isEmpty()) "" else " > " + rel.replace('/', '>'))
            }
        } catch (e: Exception) { "" }
    }

    /**
     * The real path of the granted parent folder, or "" when there is no
     * grant, it is no longer held, it does not map onto a path, or the app
     * cannot actually create a file there. That last check is the one that
     * matters: a path that exists but refuses writes would surface much later
     * as a disk image that could not be made.
     */
    @JvmStatic
    fun rootPath(): String {
        val c = ctx() ?: return ""
        val saved = savedRoot() ?: return ""
        val uri = Uri.parse(saved)
        val held = c.contentResolver.persistedUriPermissions.any {
            it.uri == uri && it.isReadPermission && it.isWritePermission
        }
        if (!held) return ""
        return try {
            val id = DocumentsContract.getTreeDocumentId(uri)
            val colon = id.indexOf(':')
            if (colon < 0) return ""
            val volume = id.substring(0, colon)
            val relative = id.substring(colon + 1)
            if (relative.contains("..") || relative.indexOf('\u0000') >= 0) return ""
            val volumeRoot = if (volume == "primary") File("/storage/emulated/0")
                             else File("/storage", volume)
            val folder = if (relative.isEmpty()) volumeRoot else File(volumeRoot, relative)
            val rootPath = volumeRoot.canonicalPath
            val folderPath = folder.canonicalPath
            if (folderPath != rootPath && !folderPath.startsWith(rootPath + File.separator))
                return ""
            if (!folder.isDirectory || !folder.canRead()) return ""
            /* Prove it, do not infer it. */
            val probe = File(folder, ".retrodos-write-test")
            val ok = try { probe.writeText("ok"); probe.exists() } catch (e: Exception) { false }
            probe.delete()
            if (ok) folder.absolutePath else ""
        } catch (e: Exception) {
            Log.w(TAG, "rootPath failed: $e"); ""
        }
    }

    private fun savedRoot(): String? =
        ctx()?.getSharedPreferences(PREFS, Context.MODE_PRIVATE)?.getString(ROOT, null)

    /* ---------------------------------------------------------------- */
    /* Enumerate                                                         */
    /* ---------------------------------------------------------------- */

    /** Names of the sub-folders of the granted tree: one per game.
     *  Returns an empty array when nothing is granted. */
    @JvmStatic
    fun listGames(): Array<String> {
        val c = ctx() ?: return emptyArray()
        val uriStr = treeUri()
        if (uriStr.isEmpty()) return emptyArray()

        val root = DocumentFile.fromTreeUri(c, Uri.parse(uriStr)) ?: return emptyArray()

        /* DocumentFile.listFiles() is notoriously slow -- one IPC per child --
         * so query the children in a single cursor instead. On a 3000-entry
         * library that is the difference between a moment and a minute. */
        return try {
            val childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(
                root.uri, DocumentsContract.getTreeDocumentId(root.uri))
            val out = ArrayList<String>(4096)
            c.contentResolver.query(
                childrenUri,
                arrayOf(DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                        DocumentsContract.Document.COLUMN_MIME_TYPE),
                null, null, null
            )?.use { cur ->
                while (cur.moveToNext()) {
                    val name = cur.getString(0) ?: continue
                    val mime = cur.getString(1) ?: ""
                    /* A collection mixes loose folders and archives, so both
                     * count as a game. The archive is extracted at launch;
                     * the name shown drops the extension. */
                    if (mime == DocumentsContract.Document.MIME_TYPE_DIR) out.add(name)
                    else if (name.endsWith(".zip", true)) out.add(name)
                }
            }
            Log.i(TAG, "listGames: cursor gave ${out.size} entries")

            /* Fall back to DocumentFile.listFiles() when the bulk cursor comes
             * back empty. It is far slower -- an IPC per child -- but it uses a
             * different provider path, and some providers refuse the
             * children-of-tree query for a granted SUBFOLDER while still
             * answering the per-document walk. Better slow than empty. */
            if (out.isEmpty()) {
                for (f in root.listFiles()) {
                    val n = f.name ?: continue
                    if (f.isDirectory || n.endsWith(".zip", true)) out.add(n)
                }
                Log.i(TAG, "listGames: listFiles fallback gave ${out.size} entries")
            }

            out.sortWith(String.CASE_INSENSITIVE_ORDER)
            Log.i(TAG, "listGames: ${out.size} entries under $uriStr")
            out.toTypedArray()
        } catch (e: Exception) {
            Log.w(TAG, "listGames failed: $e")
            emptyArray()
        }
    }

    /* ---------------------------------------------------------------- */
    /* Add one game                                                      */
    /* ---------------------------------------------------------------- */

    /* Where the picked game should be installed, remembered across the trip
     * through the system file picker. */
    @Volatile private var installDest: String = ""

    /* Progress, polled by native code once a frame. Empty means idle. */
    @Volatile private var installStatus: String = ""

    @JvmStatic fun installStatus(): String = installStatus

    /**
     * Ask for a single game and install it.
     *
     * Separate from the folder grant: that says "my collection lives there",
     * this says "add this one thing". A collection is browsed where it sits,
     * but one game the user picked is copied in, so it behaves exactly like a
     * downloaded title afterwards -- a real directory the emulator can mount.
     */
    @JvmStatic
    fun pickGame(destRoot: String) {
        val a = activity ?: return
        installDest = destRoot
        installStatus = ""
        a.runOnUiThread { a.launchGamePicker() }
    }

    /** Called by MainActivity once the user has chosen a file. */
    @JvmStatic
    fun onGamePicked(uri: Uri) {
        val c = ctx() ?: return
        val dest = installDest
        if (dest.isEmpty()) return

        installStatus = "Installing..."
        Thread {
            try {
                val doc = DocumentFile.fromSingleUri(c, uri)
                val display = doc?.name ?: "game"
                /* Folder name is the archive's, minus its extension: that is
                 * what the user will see in the library. */
                val base = display.substringBeforeLast('.')
                    .replace(Regex("[\\p{Cntrl}/\\\\:*?\"<>|]"), "_").trim()
                val out = File(dest, base.ifEmpty { "game" })
                out.mkdirs()

                if (display.endsWith(".zip", true)) {
                    c.contentResolver.openInputStream(uri)?.use { raw ->
                        java.util.zip.ZipInputStream(raw.buffered(64 * 1024)).use { zin ->
                            while (true) {
                                val e = zin.nextEntry ?: break
                                val target = File(out, e.name)
                                /* Reject entries that escape the folder. */
                                if (!target.canonicalPath.startsWith(
                                        out.canonicalPath + File.separator)) {
                                    zin.closeEntry(); continue
                                }
                                if (e.isDirectory) target.mkdirs()
                                else {
                                    target.parentFile?.mkdirs()
                                    target.outputStream().use { zin.copyTo(it, 64 * 1024) }
                                }
                                zin.closeEntry()
                            }
                        }
                    }
                    flattenSingle(out)
                } else {
                    /* A bare .EXE or .COM is a game too; drop it in a folder of
                     * its own so it has somewhere to be mounted from. */
                    c.contentResolver.openInputStream(uri)?.use { input ->
                        File(out, display).outputStream().use { input.copyTo(it, 64 * 1024) }
                    }
                }
                Log.i(TAG, "installed '$display' -> ${out.absolutePath}")
                installStatus = "Added $base"
            } catch (e: Exception) {
                Log.w(TAG, "install failed: $e")
                installStatus = "Could not add that file: ${e.message}"
            }
        }.apply { isDaemon = true }.start()
    }

    /** Lift a lone wrapper directory up, so the runnable sits at the mount
     *  point rather than one level below it. */
    private fun flattenSingle(dir: File) {
        repeat(3) {
            val kids = dir.listFiles() ?: return
            if (kids.size != 1 || !kids[0].isDirectory) return
            val inner = kids[0]
            for (f in inner.listFiles() ?: return) {
                if (!f.renameTo(File(dir, f.name))) return
            }
            inner.delete()
        }
    }

    /* ---------------------------------------------------------------- */
    /* Stage                                                             */
    /* ---------------------------------------------------------------- */

    /**
     * Copy one game's folder out of the granted tree into [destDir], a real
     * filesystem path the emulator can mount. Returns true on success.
     *
     * Skipped entirely when the destination already holds files, so launching
     * the same game twice costs nothing after the first time.
     */
    @JvmStatic
    fun stage(gameName: String, destDir: String): Boolean {
        val c = ctx() ?: return false
        val uriStr = treeUri()
        if (uriStr.isEmpty()) return false

        val dest = File(destDir)
        if (dest.isDirectory && (dest.list()?.isNotEmpty() == true)) return true
        if (!dest.isDirectory && !dest.mkdirs()) return false

        val root = DocumentFile.fromTreeUri(c, Uri.parse(uriStr)) ?: return false
        val src = root.findFile(gameName) ?: run {
            Log.w(TAG, "stage: '$gameName' not found in the granted tree")
            return false
        }
        return try {
            if (src.isDirectory) copyTree(c, src, dest) else unzip(c, src, dest)
            Log.i(TAG, "staged '$gameName' -> $destDir")
            true
        } catch (e: Exception) {
            Log.w(TAG, "stage failed for '$gameName': $e")
            false
        }
    }

    /** Extract an archive straight out of SAF, without a temporary copy. */
    private fun unzip(c: Context, src: DocumentFile, dest: File) {
        c.contentResolver.openInputStream(src.uri)?.use { raw ->
            java.util.zip.ZipInputStream(raw.buffered()).use { zin ->
                while (true) {
                    val e = zin.nextEntry ?: break
                    val out = File(dest, e.name)
                    /* Reject paths that escape the destination: a crafted
                     * archive with ../ entries would otherwise write anywhere
                     * the app can reach. */
                    if (!out.canonicalPath.startsWith(dest.canonicalPath + File.separator) &&
                        out.canonicalPath != dest.canonicalPath) {
                        Log.w(TAG, "unzip: skipping suspicious entry ${e.name}")
                        zin.closeEntry(); continue
                    }
                    if (e.isDirectory) out.mkdirs()
                    else {
                        out.parentFile?.mkdirs()
                        out.outputStream().use { zin.copyTo(it, 64 * 1024) }
                    }
                    zin.closeEntry()
                }
            }
        }
    }

    /* ---------------------------------------------------------------- */
    /* Process restart                                                   */
    /* ---------------------------------------------------------------- */

    /**
     * Kill this process and start the app again, via a trampoline activity
     * that lives in its own `:restart` process.
     *
     * This exists because DOSBox-X cannot run twice in one process: the
     * engine's thousands of globals are written once at startup and torn down
     * asymmetrically, so a second dosbox_x_main() boots a machine that
     * triple-faults. Rather than chase every global, the frontend notes which
     * game to auto-launch and asks for a fresh process; the relaunch happens
     * behind a game's natural loading moment.
     *
     * Kept here rather than in a new bridge object so the native side only
     * ever has to find one class. Called from native code via JNI.
     */
    @JvmStatic
    fun restart() {
        val a = activity ?: return
        a.runOnUiThread {
            val i = Intent(a, RestartActivity::class.java)
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK)
            a.startActivity(i)
            a.finishAffinity()
            Runtime.getRuntime().exit(0)
        }
    }

    private fun copyTree(c: Context, src: DocumentFile, dest: File) {
        for (child in src.listFiles()) {
            val name = child.name ?: continue
            val out = File(dest, name)
            if (child.isDirectory) {
                out.mkdirs()
                copyTree(c, child, out)
            } else {
                c.contentResolver.openInputStream(child.uri)?.use { input ->
                    out.outputStream().use { output -> input.copyTo(output, 64 * 1024) }
                }
            }
        }
    }
}
