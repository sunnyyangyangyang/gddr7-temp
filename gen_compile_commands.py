#!/usr/bin/env python3
"""Generate compile_commands.json for clangd/IDE support.
Usage: scripts/gen_compile_commands.py <project_dir> <kernel_src_dir>
Example: make ide   (generates compile_commands.json in project root)"""
import json
import sys

def main(project_dir, kdir):
    incs = [
        "-I.",
        f"-I{kdir}/arch/x86/include",
        f"-I{kdir}/arch/x86/include/generated",
        f"-I{kdir}/include",
        f"-I{kdir}/arch/x86/include/uapi",
        f"-I{kdir}/arch/x86/include/generated/uapi",
        f"-I{kdir}/include/uapi",
        f"-I{kdir}/include/generated/uapi",
    ]

    cmd = (
        "clang -Wall -Wextra -Wno-sign-compare -Wno-unused-parameter "
        + " ".join(incs)
        + f" -nostdinc "
        f"-include {kdir}/include/linux/compiler-version.h "
        f"-include {kdir}/include/linux/kconfig.h "
        f"-include {kdir}/include/linux/compiler_types.h "
        "-D__KERNEL__ -DMODULE -std=gnu11 -m64 -mcmodel=kernel "
        "-fno-strict-aliasing -O2 -g -c gddr7_temp.c"
    )

    result = [{
        "directory": project_dir,
        "command": cmd,
        "file": f"{project_dir}/gddr7_temp.c",
    }]

    with open(f"{project_dir}/compile_commands.json", "w") as f:
        json.dump(result, f, indent=2)
        f.write("\n")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
