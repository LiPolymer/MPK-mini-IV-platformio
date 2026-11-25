# SPDX-License-Identifier: MIT
import os
from pathlib import Path
import subprocess
import sys

Import("env")  # noqa: F821

def pack_application(target, source, env):
    raw = Path(source[0].get_abspath())
    elf = Path(source[1].get_abspath())
    package = Path(target[0].get_abspath())
    process_env = dict(os.environ, **env["ENV"])
    nm = subprocess.check_output(
        [env.subst("$NMTOOL"), str(elf)],
        env=process_env, text=True,
    )
    storage = {}
    for line in nm.splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[2] in ("__storage_start", "__storage_end"):
            storage[fields[2]] = fields[0]
    root = Path(env.subst("$PROJECT_DIR"))
    subprocess.run(
        [sys.executable, str(root / "tools" / "mkimage.py"), str(raw),
         str(package), "--app-package", "--version", version,
         "--storage", storage["__storage_start"], storage["__storage_end"]],
        cwd=root, check=True,
    )


version = env.GetProjectOption("custom_firmware_version")
env.Depends("$BUILD_DIR/mpk4_app_package.bin", env.Value(version))
package = env.Command(
    "$BUILD_DIR/mpk4_app_package.bin",
    ["$BUILD_DIR/${PROGNAME}.bin", "$BUILD_DIR/${PROGNAME}.elf",
     "$PROJECT_DIR/tools/mkimage.py", "$PROJECT_DIR/tools/mpk4_layout.py"],
    env.VerboseAction(pack_application, "Packaging $TARGET"),
)
env.Default(package)
env.Depends("upload", package)
env.AddCustomTarget(
    "flash", dependencies=package,
    actions=env.VerboseAction("$UPLOADCMD", "Flashing $SOURCE"),
    title="Flash MPK mini IV", description="Build and flash over Akai USB HID",
)
