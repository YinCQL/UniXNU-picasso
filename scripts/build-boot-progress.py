#!/usr/bin/env python3
"""Build the opt-in on-screen boot/UI diagnostic against the source XNU ABI."""
import hashlib
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "drivers/NativeMobileDisplay/boot-progress"
OUT = ROOT / "build/boot-progress"
OUT.mkdir(parents=True, exist_ok=True)


def record(path):
    return json.loads(path.read_text().strip().rstrip(","))


def compile_record(record_path, source, output, include):
    info = record(record_path)
    command = []
    arguments = info["arguments"]
    index = 0
    while index < len(arguments):
        value = arguments[index]
        if value == info["file"]:
            command.append(str(source))
        elif value in ("-o", "-MF", "-MT", "-MJ"):
            if value == "-o":
                command += ["-o", str(output)]
            index += 1
        elif value == "-Xclang" and arguments[index + 1].startswith("-W"):
            index += 1
        elif value == "-D" and arguments[index + 1].startswith("XNU_BOOT_PROGRESS="):
            index += 1
        elif not value.startswith(("-W", "-DXNU_BOOT_PROGRESS=")):
            command.append(value)
        index += 1
    command += [
        "-DXNU_BOOT_PROGRESS=1",
        "-mgeneral-regs-only",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I",
        str(ROOT / ".cache/xnu-work/osfmk"),
        "-I",
        str(include),
    ]
    subprocess.run(command, cwd=info["directory"], check=True, timeout=120)


kernel_record = (
    ROOT
    / "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json"
)
bsd_record = (
    ROOT
    / "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/kern_exec.o.json"
)

objects = []
for name in (
    "BootProgress.c",
    "BootFaultProbe.c",
    "BootProcessProbe.c",
    "BootUiTraceProbe.c",
    "PicassoBootProgressStubs.c",
):
    output = OUT / (Path(name).stem + ".o")
    compile_record(kernel_record, SOURCE / name, output, SOURCE)
    objects.append(output)

policy = OUT / "BootWatchdogPolicy.o"
compile_record(bsd_record, SOURCE / "BootWatchdogPolicy.c", policy, SOURCE)
objects.append(policy)

archive = OUT / "libBootProgress.a"
archive.unlink(missing_ok=True)
subprocess.run(["xcrun", "ar", "rcs", str(archive), *map(str, objects)], check=True)

sources = sorted(path for path in SOURCE.iterdir() if path.is_file())
report = {
    "archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
    "source_sha256": {
        str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sources
    },
    "objects": [path.name for path in objects],
    "hardware_execution": False,
}
(OUT / "build-report.json").write_text(json.dumps(report, indent=2) + "\n")
print(
    "PASS: boot progress, UI trace and launch-job policy built "
    f"({archive.stat().st_size} bytes)"
)
