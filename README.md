# gddr7_temp — RTX 5090 Temperature Monitoring Kernel Module

Reads GDDR7 DQR temperature sensors and internal THERM hotspot channels from an NVIDIA RTX 5090 (Blackwell / GB202), exposing all **16 sensors** through the standard Linux **hwmon** subsystem.

This is a kernel module approach to the same problem solved by [olealgoritme/gddr6](https://github.com/olealgoritme/gddr6). The DQR register offsets, validity checks, and MR-code decoding logic are all derived from that project. Thank you!

## What it reads

The module exposes two families of sensors, each as an independent hwmon device so monitoring tools show them separately instead of folding everything into one chip:

### GDDR7 DQR (8 modules + 1 hotspot)
Per-module memory temperature via the DQR register block at BAR0 offset `0x9024C0`. Each module has its own validity word and data word; MR-code decoding converts raw values to °C. The "hotspot" sensor reports the maximum across all 8 modules.

| hwmon device | sysfs path (example) | Meaning |
|---|---|---|
| `gddr7mod0` – `gddr7mod7` | `/sys/class/hwmon/hwmonN/` | Individual GDDR7 module temperature |
| `gddr7hotspot` | `/sys/class/hwmon/hwmonN/` | Max of modules 0–7 |

### THERM Internal Hotspot (6 channels + 1 hotspot)
Six raw internal temperature channels from Blackwell's NV_THERM module window at BAR0 offset `0xAD0A90`. Each is a 32-bit register: valid bit at bit 30, fixed-point temperature in the lower 16 bits (1/256 °C per LSB). The "hotspot" sensor reports the maximum of the 6 channels.

| hwmon device | sysfs path (example) | Meaning |
|---|---|---|
| `thermch0` – `thermch5` | `/sys/class/hwmon/hwmonN/` | Individual THERM channel temperature |
| `thermhotspot` | `/sys/class/hwmon/hwmonN/` | Max of therm channels 0–5 |

> **Note:** The THERM register layout is reverse-engineered. It is NOT an NVIDIA-documented interface. Treat values as experimental/best-effort.

The THERM register offsets and decoding approach were discovered through the combined work of:
- [igorsLAB — Blackwell Hotspot / IBHE Estimation Register Findings](https://www.igorslab.de/en/blackwell-hotspot-ibhe-estimation-register-findings-download-nvidia-question/)
- [ChipHell — 相关讨论帖](https://www.chiphell.com/thread-2848790-1-1.html)

Thank you to both communities for the research and insights!

## Why kernel space?

The gddr6 userspace tool requires `iomem=relaxed` or root access to `/dev/mem`. This module uses `ioremap` instead — no kernel boot parameter tweaks needed, just load the module and read.

Read-only, no writes to GPU MMIO anywhere. The registers accessed simply aren't privilege-locked; this does not defeat any hardware protection.

## Supported GPUs

| GPU | Status |
|---|---|
| RTX 5090 (GB202) | Supported |
| Other cards | Planned — register offsets need per-card reverse engineering |

## Building

Pre-built RPMs are available via COPR. Subscribe to the repo and install:

```bash
sudo dnf copr enable sunnyyang/gddr7-temp
sudo dnf install gddr7_temp
```

The spec file uses the **akmod** build pattern, so the kernel module is compiled automatically for your running kernel after installation.

## Usage

After the module is loaded, all 16 sensors are available through hwmon. Use `sensors` to view them:

```bash
$ sensors | grep -E "gddr7|therm"
gddr7hotspot-isa-0000
         hotspot:  +58.0°C

gddr7mod0-isa-0001
         module0:  +56.0°C

gddr7mod1-isa-0002
         module1:  +54.0°C
...

thermch0-isa-0008
         therm_ch0:        +42.0°C

thermhotspot-isa-000d
         hotspot_max:      +45.3°C
```

Or read directly from sysfs (per-sensor, millidegree Celsius):

```bash
$ cat /sys/class/hwmon/hwmon*/temp1_input
```

For live monitoring:

```bash
watch -n 1 sensors
```

## Auto-loading at boot

A systemd oneshot service (`gddr7_temp-load.service`) is installed alongside the module. It loads `gddr7_temp` after akmods finishes compiling, so the sensor data is available early in the boot process:

```bash
# Enable (done automatically by RPM post script)
pkexec systemctl enable gddr7_temp-load.service

# Manual load / unload
pkexec modprobe gddr7_temp
pkexec modprobe -r gddr7_temp
```

## Safety notes

- DQR and THERM register blocks are `ioremap`'d once at module load, `iounmap`'d at unload. No per-read remapping overhead.
- Runtime PM (`pm_runtime_resume_and_get`) wraps each read to handle GPU sleep states gracefully.
- A `0xFFFFFFFF` bus readback (completer abort / D3cold) is discarded as invalid.
- For THERM channels, bit 30 must be set for a reading to be considered valid.

## License

GPL-2.0-only
