#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

import argparse
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("qemu", type=Path)
    args = parser.parse_args()
    qemu = args.qemu.resolve()
    source = Path(__file__).with_name("dead-memory.asm")
    variants = {
        "enabled": ([], "dead-state BIOS: PASS"),
        "disabled": (["-DDEAD_FEATURE_DISABLED=1"],
                     "dead-state BIOS: disabled-feature PASS"),
        "smp": (["-DDEAD_SMP=1"], "dead-state BIOS: SMP PASS"),
    }
    roms = {}
    for name, (defines, _) in variants.items():
        suffix = "" if name == "enabled" else f"-{name}"
        rom = qemu.parent / f"dead-memory{suffix}-bios.bin"
        command = ["nasm", "-f", "bin", "-o", str(rom)]
        subprocess.run(command + defines + [str(source)], check=True)
        roms[name] = rom

    cases = (
        ("enabled", "dead64", "single", 1),
        ("enabled", "dead64", "multi", 1),
        ("smp", "dead64", "single", 2),
        ("smp", "dead64", "multi", 2),
        ("disabled", "max", "single", 1),
    )
    for name, cpu, thread, cpus in cases:
        command = [
            str(qemu), "-machine", "pc", "-accel", f"tcg,thread={thread}",
            "-cpu", cpu, "-smp", str(cpus), "-m", "16M",
            "-nodefaults", "-display", "none", "-serial", "none", "-monitor", "none",
            "-bios", str(roms[name]), "-chardev", "stdio,id=debug",
            "-device", "isa-debugcon,iobase=0xe9,chardev=debug",
            "-device", "isa-debug-exit,iobase=0xf4,iosize=4",
            "-no-reboot",
        ]
        label = f"cpu={cpu}, thread={thread}, cpus={cpus}"
        try:
            result = subprocess.run(command, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True,
                                    timeout=60 if cpus > 1 else 30)
        except subprocess.TimeoutExpired as error:
            print(f"{label}: timeout")
            if error.stdout:
                print(error.stdout.decode(errors="replace"), end="")
            return 1
        print(f"{label}: exit {result.returncode}")
        print(result.stdout, end="")
        if result.returncode != 1 or variants[name][1] not in result.stdout:
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
