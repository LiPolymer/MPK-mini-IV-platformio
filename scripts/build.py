# SPDX-License-Identifier: MIT
import re

Import("env")  # noqa: F821

version = env.GetProjectOption("custom_firmware_version")
if re.fullmatch(r"[0-9]{2}\.[0-9]{2}", version) is None:
    raise ValueError("custom_firmware_version must be in NN.NN format")

env.Append(
    CFLAGS=["-std=gnu11"],
    CXXFLAGS=["-std=gnu++17", "-fno-use-cxa-atexit"],
    CPPDEFINES=[("FW_VERSION_BCD", "0x" + version.replace(".", ""))],
    LINKFLAGS=["-nostartfiles", "-mfloat-abi=soft", "-Wl,--no-warn-rwx-segments"],
)
