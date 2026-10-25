#!/usr/bin/env python3
"""Create a fresh, physically provisioned lab installation. Never overwrite or flash."""
import argparse
import csv
import json
import os
from pathlib import Path
import re
import secrets
import uuid


def mac(value):
    if not re.fullmatch(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", value):
        raise argparse.ArgumentTypeError("expected STA MAC xx:xx:xx:xx:xx:xx")
    result = bytes.fromhex(value.replace(":", ""))
    if result[0] & 1 or not any(result):
        raise argparse.ArgumentTypeError("unicast nonzero STA MAC required")
    return result


def pins(value):
    try:
        soil, tank, pump = map(int, value.split(","))
    except ValueError as exc:
        raise argparse.ArgumentTypeError("expected soil,tank,pump") from exc
    allowed = {0, 1, 3, 4, 5, 6, 7, 10}
    if soil not in {0, 1, 3, 4} or tank not in allowed or pump not in allowed or len({soil, tank, pump}) != 3:
        raise argparse.ArgumentTypeError("invalid/conflicting C3 pins; ADC1 only; exclude straps, USB, flash")
    return soil, tank, pump


def generate(args):
    if not 1 <= len(args.node_mac) <= 10 or len(set([args.hub_mac, *args.node_mac])) != len(args.node_mac) + 1:
        raise ValueError("1..10 distinct Node STA MACs and a distinct Hub STA MAC required")
    if args.pins and len(args.pins) != len(args.node_mac):
        raise ValueError("supply --pins once per node, or omit all to disable hardware")
    if not re.fullmatch(r"[A-Z]{2}", args.country):
        raise ValueError("country must be an ISO alpha-2 regulatory country code")
    # Restrict this engineering profile to channels 1..11; local RF rules still apply.
    if not 1 <= args.channel <= 11:
        raise ValueError("engineering profile supports channels 1..11")
    previous_umask = os.umask(0o077)
    try:
        args.output.mkdir(parents=True, exist_ok=False)
        pmk = secrets.token_bytes(16)
        devices = [secrets.token_bytes(6) for _ in range(len(args.node_mac) + 1)]
        plants = [uuid.uuid4().bytes for _ in devices]
        lmks = [secrets.token_bytes(16) for _ in args.node_mac]
        app_keys = [secrets.token_bytes(16) for _ in args.node_mac]
        manifest = {"format": 1, "channel": args.channel, "country": args.country, "devices": []}
        for index, address in enumerate([args.hub_mac, *args.node_mac]):
            role = 1 if index == 0 else 0
            name = "hub" if role else f"node-{index - 1}"
            rows = [["key", "type", "encoding", "value"], ["provision", "namespace", "", ""]]

            def data(key, encoding, value):
                rows.append([key, "data", encoding, value.hex() if isinstance(value, bytes) else value])

            for key, value in (("role", role), ("channel", args.channel), ("count", len(args.node_mac) if role else 1)):
                data(key, "u8", value)
            data("country", "string", args.country)
            for key, value in (("device", devices[index]), ("plant", plants[index]), ("pmk", pmk)):
                data(key, "hex2bin", value)
            peers = range(len(args.node_mac)) if role else [index - 1]
            for slot, node_index in enumerate(peers):
                pair = {
                    "mac": args.node_mac[node_index] if role else args.hub_mac,
                    "id": devices[node_index + 1] if role else devices[0],
                    "lmk": lmks[node_index], "app": app_keys[node_index],
                }
                for suffix, value in pair.items():
                    data(f"p{slot}{suffix}", "hex2bin", value)
            if not role and args.pins:
                for key, value in zip(("soil_pin", "tank_pin", "pump_pin"), args.pins[index - 1]):
                    data(key, "i32", value)
            with (args.output / f"{name}.csv").open("x", newline="") as output:
                csv.writer(output).writerows(rows)
            manifest["devices"].append({"name": name, "mac": address.hex(":"), "device_id": "spo-" + devices[index].hex(),
                "plant_id": str(uuid.UUID(bytes=plants[index])), "hardware_enabled": bool(not role and args.pins)})
        with (args.output / "manifest.json").open("x") as output:
            json.dump(manifest, output, indent=2)
            output.write("\n")
    finally:
        os.umask(previous_umask)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hub-mac", type=mac, required=True)
    parser.add_argument("--node-mac", type=mac, action="append", required=True)
    parser.add_argument("--pins", type=pins, action="append", help="verified soil,tank,pump GPIOs, once per Node; omitted => all disabled")
    parser.add_argument("--country", required=True)
    parser.add_argument("--channel", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        generate(args)
    except (ValueError, FileExistsError) as exc:
        parser.error(str(exc))
    print(f"Created {len(args.node_mac) + 1} private CSV files in {args.output}. Nothing was flashed.")
    print("Fresh lab installation only. Never restore these images over existing safety state with the same keys.")


if __name__ == "__main__":
    main()
