package org.vita3k.emulator.data

import android.content.Context
import java.io.File

internal object AppStorage {
    private const val PREFS_NAME = "vita3k_app"
    private const val KEY_INITIAL_SETUP_COMPLETED = "initial_setup_completed"
    private const val KEY_STORAGE_VOLUME = "storage_volume"
    private const val DEFAULT_STORAGE_DIR_NAME = "vita"

    const val VOLUME_AUTO = "auto"
    const val VOLUME_INTERNAL = "internal"
    const val VOLUME_EXTERNAL = "external"

    /**
     * Where installed games, firmware and caches live.
     *
     * `getExternalFilesDir(null)` only ever resolves the *primary* external volume, which on
     * most phones is internal storage even with an SD card inserted - so a user wanting the
     * games on the card had no way to get there. `getExternalFilesDirs` returns one entry
     * per shared volume, with the primary first, which makes the removable card reachable.
     *
     * The app-specific directory on a secondary volume needs no storage permission, so this
     * works without granting MANAGE_EXTERNAL_STORAGE.
     */
    fun storageRootPath(context: Context): String {
        val volumes = runCatching { context.getExternalFilesDirs(null) }.getOrNull().orEmpty()
        val primary = runCatching { context.getExternalFilesDir(null) }.getOrNull()

        // getExternalFilesDirs returns one entry per shared volume with the primary first,
        // so everything after it is a removable one.
        val secondary = volumes.filterNotNull()
            .filter { it.absolutePath != primary?.absolutePath }

        val internalDir = context.filesDir
        val fallback = (primary ?: internalDir ?: File(context.cacheDir, "files")).absolutePath

        val preferred = when (storageVolumePreference(context)) {
            VOLUME_EXTERNAL -> secondary.firstOrNull()
            VOLUME_INTERNAL -> null
            else -> secondary.firstOrNull() // VOLUME_AUTO
        }

        return (preferred ?: primary ?: internalDir ?: File(context.cacheDir, "files")).absolutePath
    }

    fun storageVolumePreference(context: Context): String =
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .getString(KEY_STORAGE_VOLUME, VOLUME_AUTO) ?: VOLUME_AUTO

    fun setStorageVolumePreference(context: Context, volume: String) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit()
            .putString(KEY_STORAGE_VOLUME, volume)
            .apply()
    }

    /**
     * Volumes offered in settings: the removable ones only exist when a card is inserted, so
     * the entry is hidden rather than disabled when there is nothing to choose.
     */
    fun secondaryStoragePath(context: Context): String? {
        val primary = runCatching { context.getExternalFilesDir(null) }.getOrNull()
        return runCatching { context.getExternalFilesDirs(null) }.getOrNull().orEmpty()
            .filterNotNull()
            .firstOrNull { it.absolutePath != primary?.absolutePath }
            ?.absolutePath
    }

    fun defaultStoragePath(context: Context): String {
        val baseDir = File(storageRootPath(context))
        return File(baseDir, DEFAULT_STORAGE_DIR_NAME).absolutePath
    }

    fun isInitialSetupCompleted(context: Context): Boolean =
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .getBoolean(KEY_INITIAL_SETUP_COMPLETED, false)

    fun setInitialSetupCompleted(context: Context, completed: Boolean) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit()
            .putBoolean(KEY_INITIAL_SETUP_COMPLETED, completed)
            .apply()
    }
}