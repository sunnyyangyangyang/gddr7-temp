# gddr7_temp — NVIDIA GPU VRAM + THERM Temperature Monitoring Kernel Module

Reads VRAM temperature sensors (GDDR7 DQR / GDDR6 ADC) and internal THERM hotspot channels from supported NVIDIA GPUs, exposing each sensor through the standard Linux **hwmon** subsystem.

This is a kernel module approach to the same problem solved by [olealgoritme/gddr6](https://github.com/olealgoritme/gddr6). The DQR register offsets, validity checks, and MR-code decoding logic are all derived from that project. Thank you!

## What it reads

The module exposes two families of sensors, each as an independent hwmon device so monitoring tools show them separately instead of folding everything into one chip:

### VRAM Temperature Sensors
Per-module memory temperature via the VRAM register block at BAR0. Two decode algorithms depending on GPU generation:

- **GDDR7 DQR MR-code** (Blackwell, e.g. RTX 5090): reads validity + data words per module; MR-code decoding converts raw values to °C.
- **GDDR6 ADC fixed-point** (Ada / Ampere, e.g. RTX 40/30 series): reads lower 12-bit ADC value divided by 32 to get °C.

The "hotspot" sensor reports the maximum across all valid modules.

| hwmon device | sysfs path (example) | Meaning |
|---|---|---|
| `vrammod0` – `vrammodN` | `/sys/class/hwmon/hwmonN/` | Individual VRAM module temperature |
| `vramhotspot` | `/sys/class/hwmon/hwmonN/` | Max of all VRAM modules |

### THERM Internal Hotspot Channels
Raw internal temperature channels from the GPU's NV_THERM module window at BAR0. Two decode algorithms depending on generation:

- **Blackwell BJT** (RTX 50 series): 32-bit register, bit 30 valid flag, lower 16 bits fixed-point temperature (1/256 °C per LSB).
- **Legacy byte** (Ada / Ampere, RTX 40/30 series): bits 15:8 hold temperature in °C; values ≥ 127°C are discarded as invalid sentinels.

The "hotspot" sensor reports the maximum of all valid channels.

| hwmon device | sysfs path (example) | Meaning |
|---|---|---|
| `thermch0` – `thermchN` | `/sys/class/hwmon/hwmonN/` | Individual THERM channel temperature |
| `thermhotspot` | `/sys/class/hwmon/hwmonN/` | Max of all therm channels |

> **Note:** The THERM register layout is reverse-engineered. It is NOT an NVIDIA-documented interface. Treat values as experimental/best-effort.

The THERM register offsets and decoding approach were discovered through the combined work of:
- [igorsLAB — Blackwell Hotspot / IBHE Estimation Register Findings](https://www.igorslab.de/en/blackwell-hotspot-ibhe-estimation-register-findings-download-nvidia-question/)
- [ChipHell — 相关讨论帖](https://www.chiphell.com/thread-2848790-1-1.html)

Thank you to both communities for the research and insights!

## Why kernel space?

The gddr6 userspace tool requires `iomem=relaxed` or root access to `/dev/mem`. This module uses `ioremap` instead — no kernel boot parameter tweaks needed, just load the module and read.

Read-only, no writes to GPU MMIO anywhere. The registers accessed simply aren't privilege-locked; this does not defeat any hardware protection.

## Supported GPUs

### RTX 50 Series (Blackwell)

| GPU | Device ID | VRAM Sensor | THERM Sensor |
|---|---|---|---|
| RTX 5090 | 0x2b85 | GDDR7 DQR (16 modules) | Blackwell BJT (6 channels) |
| RTX 5070 Ti | 0x2c05 | — | Blackwell BJT (6 channels) |

### RTX 40 Series (Ada Lovelace)

| GPU | Device ID | VRAM Sensor | THERM Sensor |
|---|---|---|---|
| RTX 4090 | 0x2684 | GDDR6 ADC | Legacy byte |
| RTX 4090 D | 0x2685 | GDDR6 ADC | Legacy byte |
| RTX 4080 Super | 0x2702 | GDDR6 ADC | Legacy byte |
| RTX 4080 | 0x2704 | GDDR6 ADC | Legacy byte |
| RTX 4070 Ti Super | 0x2705 | GDDR6 ADC | Legacy byte |
| RTX 4070 Ti | 0x2782 | GDDR6 ADC | Legacy byte |
| RTX 4070 Super | 0x2783 | GDDR6 ADC | Legacy byte |
| RTX 4070 | 0x2786 | GDDR6 ADC | Legacy byte |
| RTX 4070 Mobile | 0x2860 | GDDR6 ADC | Legacy byte |
| RTX 4060 Mobile | 0x28e0 | GDDR6 ADC | Legacy byte |

### RTX 30 Series (Ampere)

| GPU | Device ID | VRAM Sensor | THERM Sensor |
|---|---|---|---|
| RTX 3090 Ti | 0x2203 | GDDR6 ADC | Legacy byte |
| RTX 3090 | 0x2204 | GDDR6 ADC | Legacy byte |
| RTX 3080 Ti | 0x2208 | GDDR6 ADC | Legacy byte |
| RTX 3080 | 0x2206 | GDDR6 ADC | Legacy byte |
| RTX 3080 LHR | 0x2216 | GDDR6 ADC | Legacy byte |
| RTX 3070 | 0x2484 | GDDR6 ADC | Legacy byte |
| RTX 3070 LHR | 0x2488 | GDDR6 ADC | Legacy byte |

### Workstation & Data Center GPUs

| GPU | Device ID | VRAM Sensor | THERM Sensor |
|---|---|---|---|
| RTX A2000 (GA106) | 0x2531 | GDDR6 ADC | Legacy byte |
| RTX A2000 | 0x2571 | GDDR6 ADC | Legacy byte |
| RTX A4500 | 0x2232 | GDDR6 ADC | Legacy byte |
| RTX A5000 | 0x2231 | GDDR6 ADC | Legacy byte |
| RTX A6000 | 0x26b1 | GDDR6 ADC | Legacy byte |
| NVIDIA L4 | 0x27b8 | GDDR6 ADC | Legacy byte |
| NVIDIA L40S | 0x26b9 | GDDR6 ADC | Legacy byte |
| NVIDIA A10 | 0x2236 | GDDR6 ADC | Legacy byte |

## Building

Pre-built RPMs are available via COPR. Subscribe to the repo and install:

```bash
sudo dnf copr enable sunnyyang/gddr7-temp
sudo dnf install gddr7_temp
```

The spec file uses the **akmod** build pattern, so the kernel module is compiled automatically for your running kernel after installation.

## Usage

After the module is loaded, all sensors are available through hwmon. Use `sensors` to view them:

```bash
$ sensors | grep -E "vram|therm"
vramhotspot-isa-0000
         hotspot:  +58.0°C

vrammod0-isa-0001
         vrammod0: +56.0°C

vrammod1-isa-0002
         vrammod1: +54.0°C
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
# Enable at boot (RPM post script does this). Add --now to load immediately:
sudo systemctl enable --now gddr7_temp-load.service
```

## Safety notes

- DQR and THERM register blocks are `ioremap`'d once at module load, `iounmap`'d at unload. No per-read remapping overhead.
- Runtime PM (`pm_runtime_resume_and_get`) wraps each read to handle GPU sleep states gracefully.
- A `0xFFFFFFFF` bus readback (completer abort / D3cold) is discarded as invalid.
- For THERM channels, Blackwell BJT requires bit 30 set; Legacy byte discards values ≥ 127°C.

## License

GPL-2.0-only
