"""Разведка: где в .rdata лежит "/index.html" и как на неё ссылаются."""
import struct
import sys

# Русский текст в выводе + чужая кодировка консоли = UnicodeEncodeError
# на ровном месте (подробности в _console.py).
from _console import enable_utf8_output

enable_utf8_output()

path = sys.argv[1] if len(sys.argv) > 1 else "code-terraform.exe"
anchor = (sys.argv[2] if len(sys.argv) > 2 else "/index.html").encode()

data = open(path, "rb").read()
pe_off = struct.unpack_from("<I", data, 0x3C)[0]
assert data[pe_off:pe_off + 4] == b"PE\0\0"
machine, nsec = struct.unpack_from("<HH", data, pe_off + 4)
opt_size = struct.unpack_from("<H", data, pe_off + 20)[0]
opt_off = pe_off + 24
magic = struct.unpack_from("<H", data, opt_off)[0]
image_base = struct.unpack_from("<Q", data, opt_off + 24)[0] if magic == 0x20B else struct.unpack_from("<I", data, opt_off + 28)[0]
print(f"machine=0x{machine:x} sections={nsec} magic=0x{magic:x} image_base=0x{image_base:x}")

sec_off = opt_off + opt_size
sections = []
for i in range(nsec):
    off = sec_off + i * 40
    name = data[off:off + 8].rstrip(b"\0").decode(errors="replace")
    vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, off + 8)
    sections.append((name, vaddr, vsize, rawptr, rawsize))
    print(f"  {name:10} rva=0x{vaddr:08x} vsize=0x{vsize:08x} raw=0x{rawptr:08x} rawsize=0x{rawsize:08x}")

rdata = next(s for s in sections if s[0] == ".rdata")
_, rva_start, _, rawptr, rawsize = rdata
blob = data[rawptr:rawptr + rawsize]

pos = blob.find(anchor)
print(f"anchor at file 0x{rawptr + pos:x}, rva 0x{rva_start + pos:x}" if pos >= 0 else "anchor NOT found")
if pos < 0:
    sys.exit(1)

target_rva = rva_start + pos
target_va = image_base + target_rva

for label, needle in (("rva32", struct.pack("<I", target_rva)),
                      ("va64", struct.pack("<Q", target_va))):
    hits = []
    start = 0
    while True:
        h = blob.find(needle, start)
        if h < 0:
            break
        hits.append(h)
        start = h + 1
        if len(hits) > 20:
            break
    print(f"{label}: {len(hits)} ссылок -> {[hex(h) for h in hits[:10]]}")
    for h in hits[:5]:
        print("   ", " ".join(f"{b:02x}" for b in blob[h:h + 40]))
