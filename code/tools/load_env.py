"""PlatformIO pre-build hook; never prints configuration values."""

import os
from pathlib import Path
import sys

from SCons.Script import Import

Import("env")
build_env = globals()["env"]
project = Path(build_env.subst("$PROJECT_DIR"))
sys.path.insert(0, str(project / "tools"))
from env_config import read_settings, write_header

generated = Path(build_env.subst("$BUILD_DIR")) / "generated"
try:
    values = read_settings(project / ".env", dict(os.environ))
    write_header(generated / "aero_env_config.h", values)
except ValueError as error:
    raise RuntimeError(f"ENV configuration rejected: {error}") from None
build_env.Append(CPPPATH=[str(generated)], CPPDEFINES=[("AERO_HAS_ENV_CONFIG", 1)])
print("ENV configuration prepared; account values are not logged")