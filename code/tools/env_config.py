"""Read local ENV settings and generate a private firmware configuration header."""

import ast
import configparser
from pathlib import Path
import string
from urllib.parse import urlsplit

LIMITS = {
    "WIFI_SSID": 32,
    "WIFI_PASSWORD": 64,
    "IOT_ENDPOINT": 512,
    "IOT_DEVICE_ID": 128,
    "IOT_USERNAME": 128,
    "IOT_PASSWORD": 512,
    "IOT_TOKEN": 2048,
    "OSS_ENDPOINT": 512,
    "OSS_BUCKET": 63,
    "OSS_REGION": 64,
    "OSS_FIT_PREFIX": 256,
    "OSS_SIGN_URL_ENDPOINT": 2048,
    "OSS_FIT_UPLOAD_URL": 8192,
    "OSS_ACCESS_KEY_ID": 128,
    "OSS_ACCESS_KEY_SECRET": 512,
    "OSS_SECURITY_TOKEN": 8192,
    "FIRMWARE_MANIFEST_URL": 2048,
    "FIRMWARE_DOWNLOAD_URL": 8192,
    "FIRMWARE_EXPECTED_SHA256": 64,
}
HOST_ONLY = {"OSS_ACCESS_KEY_ID", "OSS_ACCESS_KEY_SECRET", "OSS_SECURITY_TOKEN"}
FIRMWARE_KEYS = (
    "WIFI_SSID", "WIFI_PASSWORD", "IOT_ENDPOINT", "IOT_DEVICE_ID",
    "IOT_USERNAME", "IOT_PASSWORD", "IOT_TOKEN", "OSS_ENDPOINT", "OSS_BUCKET",
    "OSS_REGION", "OSS_FIT_PREFIX", "OSS_SIGN_URL_ENDPOINT", "OSS_FIT_UPLOAD_URL",
    "FIRMWARE_MANIFEST_URL", "FIRMWARE_DOWNLOAD_URL", "FIRMWARE_EXPECTED_SHA256",
)


def read_settings(path: Path, environment: dict) -> dict:
    values = {key: "" for key in LIMITS}
    if path.exists():
        parser = configparser.ConfigParser(
            interpolation=None, delimiters=("=",), comment_prefixes=("#",),
            strict=True, empty_lines_in_values=False)
        parser.optionxform = str
        try:
            parser.read_string("[env]\n" + path.read_text(encoding="utf-8"))
            if parser.sections() != ["env"] or parser.defaults():
                raise ValueError("Unexpected ENV section")
            for key, value in parser.items("env"):
                if key not in LIMITS:
                    raise ValueError("Unknown ENV field")
                if value.startswith(("'", '"')):
                    value = ast.literal_eval(value)
                    if not isinstance(value, str):
                        raise ValueError("Expected a quoted string")
                values[key] = value
        except (configparser.Error, OSError, UnicodeError, ValueError, SyntaxError):
            raise ValueError("Invalid ENV file; check keys, quoting and duplicate entries") from None
    for key in LIMITS:
        if key in environment:
            values[key] = environment[key]
        value = values[key]
        if not isinstance(value, str) or any(ord(character) < 32 or ord(character) == 127 for character in value):
            raise ValueError(f"{key}: control characters are not allowed")
        if len(value.encode("utf-8")) > LIMITS[key]:
            raise ValueError(f"{key}: value exceeds the supported byte length")
    password = values["WIFI_PASSWORD"]
    if password and not values["WIFI_SSID"]:
        raise ValueError("WIFI_SSID is required when WIFI_PASSWORD is set")
    if password and (len(password) < 8 or (len(password) == 64 and
                    any(character not in string.hexdigits for character in password))):
        raise ValueError("WIFI_PASSWORD must be 8-63 characters or a 64-digit hexadecimal PSK")
    for key in ("OSS_ENDPOINT", "OSS_SIGN_URL_ENDPOINT", "OSS_FIT_UPLOAD_URL",
                "FIRMWARE_MANIFEST_URL", "FIRMWARE_DOWNLOAD_URL"):
        if not values[key]:
            continue
        try:
            url = urlsplit(values[key])
            valid = url.scheme == "https" and bool(url.hostname) and not url.username and not url.password
        except ValueError:
            valid = False
        if not valid:
            raise ValueError(f"{key}: expected HTTPS URL without inline credentials")
    digest = values["FIRMWARE_EXPECTED_SHA256"]
    if digest and (len(digest) != 64 or any(character not in string.hexdigits for character in digest)):
        raise ValueError("FIRMWARE_EXPECTED_SHA256 must contain 64 hexadecimal digits")
    return values


def write_header(path: Path, values: dict) -> None:
    lines = ["#pragma once"]
    for key in FIRMWARE_KEYS:
        encoded = "".join(f"\\{byte:03o}" for byte in values[key].encode("utf-8"))
        lines.append(f'#define AERO_ENV_{key} "{encoded}"')
    content = "\n".join(lines) + "\n"
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != content:
        path.write_text(content, encoding="ascii")
    path.chmod(0o600)