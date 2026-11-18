package com.ascendance.dac.data

import android.content.Context
import com.ascendance.dac.model.DspDefaults
import com.ascendance.dac.model.LocalPreset
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import java.io.File

class LocalPresetRepository(context: Context) {

    private val file = File(context.filesDir, "local_presets.json")
    private val json = Json {
        ignoreUnknownKeys = true
        prettyPrint = true
        encodeDefaults = true
    }

    suspend fun loadAll(): List<LocalPreset> = withContext(Dispatchers.IO) {
        if (!file.exists()) return@withContext emptyList()
        return@withContext try {
            val raw = file.readText()
            if (raw.isBlank()) emptyList() else json.decodeFromString<List<LocalPreset>>(raw)
        } catch (_: Throwable) {
            emptyList()
        }
    }

    suspend fun saveAll(presets: List<LocalPreset>) = withContext(Dispatchers.IO) {
        val sanitized = presets.map { p ->
            p.copy(
                name = p.name.trim().ifEmpty { "Untitled Preset" },
                preampDb = p.preampDb.coerceIn(DspDefaults.MIN_PREAMP_DB, DspDefaults.MAX_PREAMP_DB),
                leftBands = p.leftBands.take(DspDefaults.MAX_BANDS).map { it.clamped() },
                rightBands = p.rightBands.take(DspDefaults.MAX_BANDS).map { it.clamped() },
                mixer = p.mixer.sanitized()
            )
        }
        val tmp = File(file.parentFile, "local_presets.json.tmp")
        tmp.writeText(json.encodeToString(sanitized))
        if (!tmp.renameTo(file)) {
            file.writeText(json.encodeToString(sanitized))
            tmp.delete()
        }
    }
}
