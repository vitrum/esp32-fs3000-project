"""Strict FIT activity decoding; never substitute ground speed for airspeed."""

from datetime import datetime
import hashlib
import io
import math

import fitdecode


def finite_number(value, minimum=0):
    if isinstance(value, (int, float)) and math.isfinite(value) and value >= minimum:
        return float(value)
    return None


def decode_activity(raw: bytes):
    records = []
    missing_time = 0
    unsupported_units = set()
    wind_units = set()
    try:
        with fitdecode.FitReader(io.BytesIO(raw), check_crc=fitdecode.CrcCheck.RAISE,
                                 error_handling=fitdecode.ErrorHandling.RAISE) as reader:
            for message in reader:
                if not isinstance(message, fitdecode.FitDataMessage) or message.name != "record":
                    continue
                standard = {field.name: field.value for field in message.fields
                            if not getattr(field.field_def, "is_dev", False)}
                developer = {field.name.casefold(): field for field in message.fields
                             if getattr(field.field_def, "is_dev", False) and isinstance(field.name, str)}
                timestamp = standard.get("timestamp")
                if not isinstance(timestamp, datetime) or timestamp.tzinfo is None:
                    missing_time += 1
                    continue
                utc_ms = round(timestamp.timestamp() * 1000)
                if records and utc_ms < records[-1]["utc_ms"]:
                    raise ValueError("FIT record timestamps moved backwards; upload a single ordered activity")
                wind = None
                field = developer.get("airspeed")
                if field:
                    unit = (field.units or "").strip().lower()
                    if unit in ("m/s", "km/h"):
                        wind_units.add(unit)
                        wind = finite_number(field.value)
                        if wind is not None and unit == "km/h":
                            wind /= 3.6
                    else:
                        unsupported_units.add(unit or "(missing)")
                source = developer.get("power_source")
                source = finite_number(source.value) if source else None
                power = finite_number(standard.get("power"))
                cadence = finite_number(standard.get("cadence"))
                fractional = finite_number(standard.get("fractional_cadence"))
                if cadence is not None and fractional is not None:
                    cadence += fractional
                if source == 0:
                    power = cadence = None
                density_field = developer.get("air_density")
                density = None
                if density_field and density_field.units in ("kg/m3", "kg/m^3"):
                    density = finite_number(density_field.value)
                records.append({
                    "utc_ms": utc_ms, "power_w": power, "cadence_rpm": cadence,
                    "wind_mps": wind, "speed_mps": finite_number(
                        standard.get("enhanced_speed") if standard.get("enhanced_speed") is not None
                        else standard.get("speed")),
                    "air_density": density, "power_source": source,
                })
                if len(records) > 200000:
                    raise ValueError("FIT exceeds the 200000-record activity limit")
    except fitdecode.FitError as error:
        raise ValueError(f"Invalid or damaged FIT (strict CRC/structure validation): {error}") from error
    if not records:
        raise ValueError("FIT contains no record messages with valid UTC timestamps")
    fields = {key: sum(record[key] is not None for record in records)
              for key in ("power_w", "cadence_rpm", "wind_mps", "speed_mps")}
    warnings = []
    if missing_time:
        warnings.append(f"{missing_time} records without UTC timestamps were excluded")
    if unsupported_units:
        warnings.append("Unsupported airspeed units: " + ", ".join(sorted(unsupported_units)))
    if not fields["wind_mps"]:
        warnings.append("No usable developer airspeed field; wind and CdA are unavailable, not GPS speed")
    return {"records": records, "summary": {
        "records": len(records), "start_utc_ms": records[0]["utc_ms"],
        "end_utc_ms": records[-1]["utc_ms"], "available_fields": fields,
        "wind_source": "FIT developer airspeed", "wind_input_units": sorted(wind_units),
        "warnings": warnings, "sha256": hashlib.sha256(raw).hexdigest(),
        "validation": "fitdecode 0.11.0; strict header/file CRC and structure",
    }}
