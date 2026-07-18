# gddr7_temp — RTX 5090 GDDR7 VRAM Temperature Kernel Module

Reads per-module GDDR7 DQR temperature sensors from an NVIDIA RTX 5090 (Blackwell / GB202) and exposes them through `/proc/gddr7_temp`.

This is a **kernel module** approach to the same problem solved by [olealgoritme/gddr6](https://github.com/olealgoritme/gddr6). The DQR register offsets, validity checks, and MR-code decoding logic are all derived from that project. Thank you!

## Why kernel space?

The gddr6 userspace tool requires `iomem=relaxed` or root access to `/dev/mem`. This module uses `ioremap` instead — no kernel boot parameter tweaks needed, just load the module and read.

Read-only, no writes to GPU MMIO anywhere. The registers accessed simply aren't privilege-locked; this does not defeat any hardware protection.

## Supported GPUs

| GPU | Status |
|---|---|
| RTX 5090 (GB202) | Supported |
| Other cards | Planned — register offsets need per-card reverse engineering |

## Building

This repo contains an RPM spec (`gddr7_temp-kmod.spec`) using the **akmod** build pattern. Build with `local-copr-repo` (`lc`) or standard mock:

```bash
# Using local-copr-repo (recommended for local iteration)
lc build --source . --torepo /path/to/your/repo

# Or install the produced RPMs and run akmods
sudo dnf install ./akmod-gddr7_temp*.rpm ./gddr7_temp-kmod-common*.rpm
pkexec akmods --force gddr7_temp
```

The `gddr7_temp-kmod.spec.in` file is a COPR/Koji-ready template (for future packaging upstream).

## Usage

After the module is loaded, read temperatures via `/proc/gddr7_temp` (root-only):

```bash
$ sudo cat /proc/gddr7_temp
hotspot: 60 C
module0: 58 C
module1: 56 C
module2: 58 C
module3: 54 C
module4: 60 C
module5: 58 C
module6: 56 C
module7: 56 C
```

- **hotspot** — highest temperature across all modules
- **moduleN** — individual GDDR7 module reading (up to 16)

For live monitoring:

```bash
sudo watch -n 1 cat /proc/gddr7_temp
```

## Safety notes

- DQR register block is `ioremap`'d once at load, `iounmap`'d at unload. No per-read remapping overhead.
- Runtime PM (`pm_runtime_resume_and_get`) wraps each read to handle GPU sleep states gracefully.
- A `0xFFFFFFFF` bus readback (completer abort / D3cold) is discarded as invalid.
- `/proc/gddr7_temp` is root-only (`0400`).

## License

GPL-2.0-only
