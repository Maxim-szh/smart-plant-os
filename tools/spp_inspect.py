#!/usr/bin/env python3
"""Inspect the bounded SPP CBOR subset; does not decrypt radio frames or send commands."""
import argparse
import json
from pathlib import Path


def decode(data):
    if len(data) > 217:
        raise ValueError("payload too large")
    pos = 0

    def number(major):
        nonlocal pos
        if pos >= len(data):
            raise ValueError("truncated CBOR")
        first = data[pos]
        pos += 1
        if first >> 5 != major:
            raise ValueError("unexpected CBOR type")
        additional = first & 31
        if additional < 24:
            return additional
        if additional > 27:
            raise ValueError("indefinite or reserved encoding")
        size = 1 << (additional - 24)
        if pos + size > len(data):
            raise ValueError("truncated integer")
        value = int.from_bytes(data[pos:pos + size], "big")
        pos += size
        if value < {1: 24, 2: 256, 4: 65536, 8: 4294967296}[size]:
            raise ValueError("non-minimal encoding")
        return value

    count = number(5)
    if not 1 <= count <= 28:
        raise ValueError("invalid map size")
    result = {}
    previous = -1
    for _ in range(count):
        key = number(0)
        if key <= previous or key > 27:
            raise ValueError("duplicate, unordered or unknown key")
        previous = key
        if key < 24:
            result[str(key)] = number(0)
        else:
            size = number(2)
            if size != (6 if key == 26 else 16) or pos + size > len(data):
                raise ValueError("invalid byte string")
            result[str(key)] = {"hex": data[pos:pos + size].hex()}
            pos += size
    if pos != len(data) or not 1 <= result.get("0", 0) <= 13:
        raise ValueError("trailing data or invalid message kind")
    return result


def check_vectors(path):
    vectors = json.loads(path.read_text())
    for vector in vectors:
        actual = decode(bytes.fromhex(vector["cbor_hex"]))
        if actual != vector["map"]:
            raise ValueError(f"vector mismatch: {vector['name']}")
    print(f"PASS: {len(vectors)} SPP vectors")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("hex", nargs="?")
    parser.add_argument("--check-vectors", type=Path)
    args = parser.parse_args()
    try:
        if args.check_vectors:
            check_vectors(args.check_vectors)
        elif args.hex:
            print(json.dumps(decode(bytes.fromhex(args.hex)), indent=2))
        else:
            parser.error("provide CBOR hex or --check-vectors")
    except ValueError as exc:
        parser.error(str(exc))


if __name__ == "__main__":
    main()
