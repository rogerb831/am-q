"""Writes shape_ids_v2.amqs, the frozen state chunk that pins shape ids (area 15 R7).

Run once; the output is committed and must never be regenerated or edited. It is written
from the documented chunk format and the frozen host ids, not by the plugin:
  u32 magic "AMQS", u32 version 2, u32 pair count, then per pair u32 host id + f64 plain value,
  little-endian (plugin/src/amq_plugin.cpp, docs/params/parameter-table.md).
Band k (1..10) holds shape id k - 1; bands 11..24 are absent and load as the default (Bell).
"""
import pathlib
import struct

here = pathlib.Path(__file__).resolve().parent
golden = here.parent.parent / "params" / "host_ids.golden.txt"
ids = {}
for line in golden.read_text(encoding="utf-8").splitlines():
    if line and not line.startswith("#"):
        name, hid = line.split()
        ids[name] = int(hid)

pairs = [(ids[f"band{k:02d}.shape"], float(k - 1)) for k in range(1, 11)]
data = struct.pack("<III", 0x53514D41, 2, len(pairs))
for hid, value in pairs:
    data += struct.pack("<Id", hid, value)
(here / "shape_ids_v2.amqs").write_bytes(data)
print(f"wrote {len(data)} bytes")
