#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Isolated Morph transport-slot regression against the pinned upstream source."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
BASE = "a43cd4fcda23ec3d577dc425261c3eb28bd5abff"
SOURCE = "Core/Src/Preset/presetManager.c"


def function(source, name):
    # Mask comments and string literals while locating unmodified C function bodies.
    masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',
                    lambda m: " " * len(m[0]), source, flags=re.S)
    signature = (r"^(?:IN_ITCM_RAM )?(?:static )?(?:uint8_t|void) "
                 + re.escape(name) + r"\([^;]*?\)\s*\{")
    match = re.search(signature, masked, re.M)
    if match is None:
        raise RuntimeError("Missing source function: " + name)
    depth, end = 1, match.end()
    while depth:
        if end >= len(masked):
            raise RuntimeError("Unclosed function: " + name)
        depth += (masked[end] == "{") - (masked[end] == "}")
        end += 1
    return source[match.start():end]


def main():
    candidate = (ROOT / SOURCE).read_text(encoding="utf-8")
    upstream = subprocess.check_output(
        ["git", "show", BASE + ":" + SOURCE], cwd=ROOT, text=True)

    predicate = function(candidate, "preset_isTransportParameter")
    names = ["interpolate", "preset_sendDrumsetParametersImmediate",
             "preset_morph_tick", "preset_morph"]

    includes = [".", "Core/Inc", "Drivers/CMSIS/Include",
                "Drivers/CMSIS/Device/ST/STM32F7xx/Include",
                "Drivers/STM32F7xx_HAL_Driver/Inc",
                "Middlewares/Third_Party/FatFs/src",
                "Middlewares/JS/STM32_USB_Device_Library/Core/Inc",
                "Middlewares/JS/STM32_USB_Device_Library/Class/CDC/Inc",
                "USB_DEVICE_MIDI/Target"]
    flags = [os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-g",
             "-DSTM32F765xx", "-DUSE_HAL_DRIVER", "-D__fp16=_Float16",
             "-Wno-pointer-to-int-cast", "-Wno-int-to-pointer-cast",
             "-Wno-implicit-function-declaration", "-Wno-unused-function",
             "-ffunction-sections", "-fdata-sections", "-fno-pie", "-no-pie",
             "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
             "-fno-omit-frame-pointer"]
    flags += ["-I" + str(ROOT / item) for item in includes]

    env = os.environ.copy()
    env.setdefault("ASAN_OPTIONS", "detect_leaks=0")

    with tempfile.TemporaryDirectory(prefix="lxr-morph-pr-") as directory:
        directory = Path(directory)
        for label, source in (("upstream", upstream), ("candidate", candidate)):
            # The upstream fixture uses only the candidate predicate for the
            # assertion; its dispatch/kit-load/Morph function bodies remain original.
            extracted = [predicate] + [function(source, name) for name in names]
            (directory / "sweep_functions.inc").write_text(
                "\n\n".join(extracted) + "\n", encoding="utf-8")
            binary = directory / ("sweep-" + label)
            subprocess.run(
                flags + ["-I" + str(directory), str(HERE / "test_sweeps.c"),
                         str(ROOT / "Core/Src/Parameters.c"),
                         "-Wl,--gc-sections", "-o", str(binary)],
                cwd=ROOT, env=env, check=True)
            outcome = subprocess.run([str(binary)], cwd=ROOT, env=env,
                                     text=True, capture_output=True)
            if label == "upstream":
                if outcome.returncode == 0 or "NRPN_DATA_ENTRY_COARSE" not in outcome.stderr:
                    raise AssertionError(
                        "Pinned upstream did not exhibit the expected reserved-NRPN failure:\n"
                        + outcome.stdout + outcome.stderr)
                print("PASS: unchanged upstream dispatch reproduces expected NRPN assertion")
            else:
                if outcome.returncode != 0:
                    raise AssertionError(
                        "Isolated Morph candidate failed:\n"
                        + outcome.stdout + outcome.stderr)
                print(outcome.stdout.strip())
                print("PASS: isolated candidate rejects transport dispatch while retaining valid sweeps")
    print("Scope: source-extracted host control-path tests only; no hardware/audio/ARM timing test")


if __name__ == "__main__":
    main()
