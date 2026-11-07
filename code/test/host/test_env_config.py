import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("env_config", ROOT / "tools/env_config.py")
env_config = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(env_config)


class EnvConfigTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.path = self.root / ".env"

    def tearDown(self):
        self.temporary.cleanup()

    def test_absent_and_environment_precedence(self):
        values = env_config.read_settings(self.path, {})
        self.assertTrue(all(value == "" for value in values.values()))
        self.path.write_text("IOT_DEVICE_ID=local\n")
        values = env_config.read_settings(self.path, {"IOT_DEVICE_ID": "ci"})
        self.assertEqual(values["IOT_DEVICE_ID"], "ci")

    def test_quoted_special_characters_and_utf8(self):
        self.path.write_text("WIFI_SSID='测试网络'\nWIFI_PASSWORD=abcdefgh\nIOT_TOKEN='space # % $ \\\\ token'\n")
        values = env_config.read_settings(self.path, {})
        self.assertEqual(values["WIFI_SSID"], "测试网络")
        self.assertIn("# % $", values["IOT_TOKEN"])
        header = self.root / "private.h"
        env_config.write_header(header, values)
        self.assertNotIn("abcdefgh", header.read_text())
        self.assertEqual(header.stat().st_mode & 0o777, 0o600)
        timestamp = header.stat().st_mtime_ns
        env_config.write_header(header, values)
        self.assertEqual(header.stat().st_mtime_ns, timestamp)

    def test_invalid_config_does_not_echo_secret(self):
        for text in ("UNKNOWN=private-value\n", "IOT_TOKEN=private-value\nIOT_TOKEN=again\n",
                     "WIFI_PASSWORD=private-value\n", "IOT_TOKEN='private-value\\n'\n",
                     "WIFI_SSID=" + "x" * 33 + "\n", "[other]\nIOT_TOKEN=private-value\n"):
            self.path.write_text(text)
            with self.assertRaises(ValueError) as error:
                env_config.read_settings(self.path, {})
            self.assertNotIn("private-value", str(error.exception))

    def test_wifi_password_constraints(self):
        self.path.write_text("WIFI_SSID=test\nWIFI_PASSWORD=short\n")
        with self.assertRaises(ValueError):
            env_config.read_settings(self.path, {})
        self.path.write_text("WIFI_SSID=test\nWIFI_PASSWORD=" + "A" * 64 + "\n")
        self.assertEqual(len(env_config.read_settings(self.path, {})["WIFI_PASSWORD"]), 64)

    def test_generated_header_compiles_and_clears_deleted_values(self):
        self.path.write_text("IOT_TOKEN='quote \\\" slash \\\\ hash # percent %'\n")
        values = env_config.read_settings(self.path, {})
        header = self.root / "aero_env_config.h"
        env_config.write_header(header, values)
        source = '#include "env_settings.h"\n#include <cstring>\nint main(){return std::strlen(EnvSettings::iotToken())==0;}\n'
        binary = self.root / "check"
        built = subprocess.run(["c++", "-std=c++11", "-DAERO_HAS_ENV_CONFIG=1", "-I" + str(self.root),
                    "-I" + str(ROOT / "src"), "-x", "c++", "-", "-o", str(binary)],
                               input=source, text=True, capture_output=True)
        self.assertEqual(built.returncode, 0, built.stderr)
        self.assertEqual(subprocess.run([str(binary)]).returncode, 0)
        self.path.unlink()
        env_config.write_header(header, env_config.read_settings(self.path, {}))
        self.assertIn('#define AERO_ENV_IOT_TOKEN ""', header.read_text())

    def test_cloud_keys_are_host_only(self):
        self.path.write_text("OSS_ACCESS_KEY_ID=host-id\nOSS_ACCESS_KEY_SECRET=host-secret\nOSS_SECURITY_TOKEN=temporary-token\n")
        values = env_config.read_settings(self.path, {})
        self.assertEqual(values["OSS_ACCESS_KEY_SECRET"], "host-secret")
        header = self.root / "private.h"
        env_config.write_header(header, values)
        for key in env_config.HOST_ONLY:
            self.assertNotIn("AERO_ENV_" + key, header.read_text())

    def test_oss_and_download_validation(self):
        self.path.write_text("OSS_ENDPOINT=https://oss.example.test\nOSS_BUCKET=rides\nOSS_FIT_PREFIX=fit/\n"
                             "FIRMWARE_DOWNLOAD_URL=https://cdn.example.test/app.bin\nFIRMWARE_EXPECTED_SHA256=" + "a" * 64 + "\n")
        values = env_config.read_settings(self.path, {})
        self.assertEqual(values["OSS_FIT_PREFIX"], "fit/")
        for text in ("OSS_SIGN_URL_ENDPOINT=http://example.test\n", "FIRMWARE_EXPECTED_SHA256=invalid\n",
                     "OSS_FIT_UPLOAD_URL=https://user:private-value@example.test\n"):
            self.path.write_text(text)
            with self.assertRaises(ValueError) as error:
                env_config.read_settings(self.path, {})
            self.assertNotIn("private-value", str(error.exception))


if __name__ == "__main__":
    unittest.main()