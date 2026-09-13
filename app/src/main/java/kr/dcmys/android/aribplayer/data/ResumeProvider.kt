package kr.dcmys.android.aribplayer.data

import android.content.ContentProvider
import android.content.ContentValues
import android.database.Cursor
import android.database.MatrixCursor
import android.net.Uri
import kotlinx.coroutines.runBlocking

class ResumeProvider : ContentProvider() {
    override fun onCreate(): Boolean = true

    override fun query(
        uri: Uri,
        projection: Array<out String>?,
        selection: String?,
        selectionArgs: Array<out String>?,
        sortOrder: String?,
    ): Cursor {
        val columns = projection
            ?.filter { it in ALL_COLUMNS }
            ?.toTypedArray()
            ?: ALL_COLUMNS
        val cursor = MatrixCursor(columns)
        val appContext = context ?: return cursor
        val callingPackage = getCallingPackage() ?: return cursor
        if (uri.authority != AUTHORITY || uri.pathSegments != listOf(ENTRIES_PATH)) {
            return cursor
        }

        val requestedUri = uri.getQueryParameter(QUERY_URI)
        val packageManager = appContext.packageManager
        val providerPackages = HashMap<String, String?>()
        val entries = runBlocking {
            AppDatabase.getInstance(appContext).mediaEntryDao().getAll()
        }
        entries.forEach { entry ->
            val entryUri = Uri.parse(entry.uriString)
            if (entryUri.scheme != CONTENT_SCHEME) return@forEach
            if (requestedUri != null && entry.uriString != requestedUri) return@forEach
            val authority = entryUri.authority ?: return@forEach
            val providerPackage = if (providerPackages.containsKey(authority)) {
                providerPackages[authority]
            } else {
                packageManager.resolveContentProvider(authority, 0)?.packageName.also {
                    providerPackages[authority] = it
                }
            }
            if (providerPackage != callingPackage) return@forEach
            cursor.addRow(columns.map { column ->
                when (column) {
                    "uri" -> entry.uriString
                    "resume_position_ms" -> entry.resumePositionMs
                    "duration_ms" -> entry.durationMs
                    "last_opened_ms" -> entry.lastOpenedEpochMs
                    else -> null
                }
            }.toTypedArray())
        }
        return cursor
    }

    override fun getType(uri: Uri): String =
        "vnd.android.cursor.dir/vnd.kr.dcmys.android.aribplayer.resume.entry"

    override fun insert(uri: Uri, values: ContentValues?): Uri =
        throw UnsupportedOperationException("ResumeProvider is read-only")

    override fun update(
        uri: Uri,
        values: ContentValues?,
        selection: String?,
        selectionArgs: Array<out String>?,
    ): Int = throw UnsupportedOperationException("ResumeProvider is read-only")

    override fun delete(
        uri: Uri,
        selection: String?,
        selectionArgs: Array<out String>?,
    ): Int = throw UnsupportedOperationException("ResumeProvider is read-only")

    private companion object {
        const val AUTHORITY = "kr.dcmys.android.aribplayer.resume"
        const val CONTENT_SCHEME = "content"
        const val ENTRIES_PATH = "entries"
        const val QUERY_URI = "uri"
        val ALL_COLUMNS = arrayOf(
            "uri",
            "resume_position_ms",
            "duration_ms",
            "last_opened_ms",
        )
    }
}
