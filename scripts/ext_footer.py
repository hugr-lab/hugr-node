#!/usr/bin/env python3
"""Read a .duckdb_extension file's footer (spec 008): the metadata duckdb itself checks at load.

The last 512 bytes of the file are 8 fields of 32 bytes, stored in reverse order, then a 256-byte signature
(duckdb's ExtensionHelper::ParseExtensionMetaData). Field 0 is the magic value, 1 the platform, 2 the duckdb version,
3 the extension's version, 4 the ABI type.

    scripts/ext_footer.py <file>...   # one JSON object per file, one per line
"""
import json
import os
import sys

FOOTER_SIZE = 512
FIELD_SIZE = 32
MAGIC = "4"


def read_footer(path):
    with open(path, "rb") as f:
        f.seek(0, os.SEEK_END)
        if f.tell() < FOOTER_SIZE:
            raise ValueError(f"{path}: shorter than a duckdb extension footer")
        f.seek(-FOOTER_SIZE, os.SEEK_END)
        metadata = f.read(FOOTER_SIZE - 256)
    fields = [metadata[i * FIELD_SIZE:(i + 1) * FIELD_SIZE].rstrip(b"\0").decode() for i in range(8)]
    fields.reverse()
    if fields[0] != MAGIC:
        raise ValueError(f"{path}: no duckdb extension footer (magic {fields[0]!r})")
    name = os.path.basename(path)
    return {
        "name": name[: -len(".duckdb_extension")] if name.endswith(".duckdb_extension") else name,
        "platform": fields[1],
        "duckdb_version": fields[2],
        "extension_version": fields[3],
        "abi": fields[4] or "CPP",
    }


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for path in sys.argv[1:]:
        print(json.dumps(read_footer(path)))
