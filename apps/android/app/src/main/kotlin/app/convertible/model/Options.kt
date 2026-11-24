package app.convertible.model

import app.convertible.json.Json
import kotlin.math.roundToLong

enum class OptionType {
    INT, FLOAT, BOOL, ENUM, STRING;

    companion object {
        fun from(text: String): OptionType = when (text) {
            "int" -> INT
            "float" -> FLOAT
            "bool" -> BOOL
            "enum" -> ENUM
            else -> STRING
        }
    }
}

/** A typed option value; serialises to the JSON type the core expects. */
sealed interface OptionValue {
    val json: Any

    data class IntValue(val value: Long) : OptionValue {
        override val json: Any get() = value
    }

    data class FloatValue(val value: Double) : OptionValue {
        override val json: Any get() = value
    }

    data class BoolValue(val value: Boolean) : OptionValue {
        override val json: Any get() = value
    }

    data class Text(val value: String) : OptionValue {
        override val json: Any get() = value
    }

    companion object {
        /** Converts a parsed JSON default to the option's type. Null when absent or not convertible. */
        fun of(type: OptionType, raw: Any?): OptionValue? = when (type) {
            OptionType.INT -> (raw as? Number)?.let { IntValue(it.toDouble().roundToLong()) }
            OptionType.FLOAT -> (raw as? Number)?.let { FloatValue(it.toDouble()) }
            OptionType.BOOL -> (raw as? Boolean)?.let { BoolValue(it) }
            OptionType.ENUM, OptionType.STRING -> when (raw) {
                is String -> Text(raw)
                is Number -> Text(raw.toString())
                else -> null
            }
        }
    }
}

/** One entry of `cvt_options_schema_json`. */
data class OptionSpec(
    val key: String,
    val label: String,
    val type: OptionType,
    val default: OptionValue?,
    val min: Double?,
    val max: Double?,
    val choices: List<String>,
    val description: String,
    val advanced: Boolean,
) {
    /** Clamps / validates a candidate value for this option; null when it does not fit the type. */
    fun coerce(value: OptionValue): OptionValue? = when (type) {
        OptionType.INT -> when (value) {
            is OptionValue.IntValue -> OptionValue.IntValue(clamp(value.value.toDouble()).roundToLong())
            is OptionValue.FloatValue -> OptionValue.IntValue(clamp(value.value).roundToLong())
            else -> null
        }
        OptionType.FLOAT -> when (value) {
            is OptionValue.FloatValue -> OptionValue.FloatValue(clamp(value.value))
            is OptionValue.IntValue -> OptionValue.FloatValue(clamp(value.value.toDouble()))
            else -> null
        }
        OptionType.BOOL -> value as? OptionValue.BoolValue
        OptionType.ENUM -> (value as? OptionValue.Text)?.takeIf { it.value in choices }
        OptionType.STRING -> value as? OptionValue.Text
    }

    private fun clamp(v: Double): Double {
        var x = v
        min?.let { if (x < it) x = it }
        max?.let { if (x > it) x = it }
        return x
    }
}

/** Quality presets of the standard `preset` option. */
enum class Preset(val id: String) {
    HIGH("high"), BALANCED("balanced"), SMALL("small");

    companion object {
        fun from(id: String?): Preset? = entries.firstOrNull { it.id == id }
    }
}

object OptionKeys {
    const val PRESET = "preset"
    const val STRIP_METADATA = "strip_metadata"
}

/** How a numeric option is edited. */
enum class NumberControl { SLIDER, STEPPER }

/** How an enum option is edited. */
enum class ChoiceControl { SEGMENTED, CHIPS, MENU }

/** Option-form logic shared by the UI and the conversion call. */
object OptionsForm {
    /** Integer ranges with more steps than this are edited with a stepper/field, not a slider. */
    const val MAX_SLIDER_STEPS = 1000

    /** Enums with more choices than this are a menu (a chip row would wrap over many lines). */
    const val MAX_CHIP_CHOICES = 6

    /**
     * A slider only for bounded ranges a thumb can actually address: floats, and integers with at most
     * [MAX_SLIDER_STEPS] steps (e.g. "quality" 1..100). "bitrate_kbps" 0..2000 or "sample_rate" 0..384000 get a
     * stepper with a typed field.
     */
    fun numberControl(spec: OptionSpec): NumberControl {
        val min = spec.min
        val max = spec.max
        if (min == null || max == null || !min.isFinite() || !max.isFinite() || max <= min) return NumberControl.STEPPER
        if (spec.type == OptionType.INT && max - min > MAX_SLIDER_STEPS) return NumberControl.STEPPER
        return NumberControl.SLIDER
    }

    fun choiceControl(spec: OptionSpec): ChoiceControl = when {
        spec.choices.size in 2..3 -> ChoiceControl.SEGMENTED
        spec.choices.size > MAX_CHIP_CHOICES -> ChoiceControl.MENU
        else -> ChoiceControl.CHIPS
    }

    /** The stepper's increment: 1 for integers; for floats a power of ten near a hundredth of the range. */
    fun step(spec: OptionSpec): Double {
        if (spec.type == OptionType.INT) return 1.0
        val span = (spec.max ?: 0.0) - (spec.min ?: 0.0)
        if (span <= 0.0 || !span.isFinite()) return 0.1
        return Math.pow(10.0, Math.floor(Math.log10(span)) - 2)
    }

    /** True when the schema offers the High / Balanced / Small control. */
    fun hasPreset(schema: List<OptionSpec>): Boolean =
        schema.any { it.key == OptionKeys.PRESET && it.type == OptionType.ENUM }

    /** Options shown in the always-visible part of the form (the preset has its own control). */
    fun basic(schema: List<OptionSpec>): List<OptionSpec> = schema.filter { !it.advanced && it.key != OptionKeys.PRESET }

    /** Options behind the "Advanced" disclosure. */
    fun advanced(schema: List<OptionSpec>): List<OptionSpec> = schema.filter { it.advanced && it.key != OptionKeys.PRESET }

    /** The effective value of an option: the user's choice, else the schema default. */
    fun valueOf(spec: OptionSpec, values: Map<String, OptionValue>): OptionValue? = values[spec.key] ?: spec.default

    /**
     * Values to send for a conversion: only keys the schema knows, coerced to their types, and only when they differ
     * from the schema default (so the core's own defaults stay authoritative). App defaults (preset, strip metadata)
     * are applied when the user has not set the option on this file.
     */
    fun resolve(
        schema: List<OptionSpec>,
        values: Map<String, OptionValue>,
        defaultPreset: Preset?,
        stripMetadata: Boolean,
    ): Map<String, OptionValue> {
        val out = LinkedHashMap<String, OptionValue>()
        for (spec in schema) {
            val chosen = values[spec.key]
                ?: when (spec.key) {
                    OptionKeys.PRESET -> defaultPreset?.let { OptionValue.Text(it.id) }
                    OptionKeys.STRIP_METADATA -> OptionValue.BoolValue(stripMetadata)
                    else -> null
                }
                ?: continue
            val coerced = spec.coerce(chosen) ?: continue
            if (coerced != spec.default) out[spec.key] = coerced
        }
        return out
    }

    /** JSON object for `options_json`, or null when there is nothing to send. */
    fun toJson(values: Map<String, OptionValue>): String? =
        if (values.isEmpty()) null else Json.write(values.mapValues { it.value.json })
}
