#!/usr/bin/env python3
import os
import sys
import struct

# Embed all files under <data_root> and <themes_root> into a single binary
# blob. Each entry is stored under its path *relative* to the root so the
# runtime can extract them to ONEWM_DATA_DIR and the existing find_data()
# lookups (which use "<datadir>/<rel>") resolve unchanged.
#
# Blob format:
#   "OWMA"                       magic (4 bytes)
#   uint32  entry_count
#   repeated:
#     uint16  name_len
#     name_len bytes  name (utf-8, relative path)
#     uint32  data_len
#     data_len bytes  file contents


def collect(root, out):
    for dirpath, _dirs, files in os.walk(root):
        for fn in sorted(files):
            full = os.path.join(dirpath, fn)
            name = os.path.relpath(full, root)
            with open(full, "rb") as f:
                data = f.read()
            out.append((name, data))


def main():
    if len(sys.argv) != 4:
        sys.stderr.write(
            "usage: embed_assets.py <data_root> <themes_root> <output.bin>\n"
        )
        sys.exit(2)

    data_root, themes_root, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
    entries = []
    collect(data_root, entries)
    collect(themes_root, entries)

    with open(out_path, "wb") as f:
        f.write(b"OWMA")
        f.write(struct.pack("<I", len(entries)))
        for name, data in entries:
            nb = name.encode("utf-8")
            f.write(struct.pack("<H", len(nb)))
            f.write(nb)
            f.write(struct.pack("<I", len(data)))
            f.write(data)

    total = sum(len(d) for _n, d in entries)
    print("embedded %d assets (%d bytes) -> %s" % (len(entries), total, out_path))


if __name__ == "__main__":
    main()
