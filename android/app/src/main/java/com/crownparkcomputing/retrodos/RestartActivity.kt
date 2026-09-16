package com.crownparkcomputing.retrodos

import android.app.Activity
import android.content.Intent
import android.os.Bundle

/**
 * Trampoline for restarting the app in a fresh process.
 *
 * Runs in its own `:restart` process (see the manifest), which is the whole
 * point: it survives the main process calling exit(), starts MainActivity
 * again -- Android gives that a brand-new process because the old one is gone
 * -- and then exits itself. The classic "phoenix" pattern.
 *
 * Needed because the DOSBox-X engine cannot run twice in one process; the
 * frontend requests this restart between games instead.
 */
class RestartActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val i = Intent(this, MainActivity::class.java)
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK)
        startActivity(i)
        finish()
        Runtime.getRuntime().exit(0)
    }
}
