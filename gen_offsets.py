#!/usr/bin/env python3
"""Fill gpu_tables[] in gpu_offsets.h from offsets.yaml.

Reads the template (gpu_offsets.h) and replaces content between '{' and '};'
with array entries generated from offsets.yaml. Struct definition, header
guard, and includes are preserved — only data is overwritten."""
import re
import sys
import yaml


def to_c_hex(v):
    if isinstance(v, str) and v.startswith("0x"):
        return v
    return hex(int(v))


def escape_c_string(s: str) -> str:
    """Escape backslashes and double quotes for C string literals."""
    return s.replace("\\", "\\\\").replace('"', '\\"')


def _generate_entries(tables):
    """Generate C array initializer entries from YAML tables."""
    seen_ids = set()
    for t in tables:
        dev_id = int(t["device_id"], 16) if isinstance(t["device_id"], str) else t["device_id"]
        if dev_id in seen_ids:
            sys.exit(f"gen_offsets: duplicate device_id 0x{dev_id:04x}")
        seen_ids.add(dev_id)

        yield (
            f'    {{ .device_id = 0x{dev_id:04x}, .name = "{escape_c_string(t["name"])}",\n'
            f'      .dqr_module0 = {to_c_hex(t["dqr"]["module0"])}, '
            f'.dqr_vld_off = {to_c_hex(t["dqr"]["vld_off"])},\n'
            f'      .dqr_stride = {to_c_hex(t["dqr"]["stride"])}, '
            f'.dqr_num_modules = {t["dqr"]["num_modules"]},\n'
            f'      .therm_ch0 = {to_c_hex(t["therm"]["ch0"])}, '
            f'.therm_ch_stride = {to_c_hex(t["therm"]["stride"])},\n'
            f'      .therm_num_channels = {t["therm"]["num_channels"]} }},'
        )


def main(yaml_path, out_path):
    with open(yaml_path) as f:
        tables = yaml.safe_load(f)

    # Read template and replace only the array body between gpu_tables's '{' and '};'.
    # Uses regex to find the exact region so struct closing }; isn't confused.
    with open(out_path) as f:
        template = f.read()

    entries_text = "\n".join(_generate_entries(tables))
    pattern = r"(static const struct gpu_offset_table gpu_tables\[\] = \{)\s*(/\*.+\*/)?\s*(\};)"
    replacement = r"\1\n" + entries_text + "\n  \\3"

    with open(out_path, "w") as f:
        f.write(re.sub(pattern, replacement, template, flags=re.DOTALL))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
