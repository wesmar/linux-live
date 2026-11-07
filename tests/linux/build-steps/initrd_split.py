"""Walk the concatenated initrd, report segments; with 'save' also write them to the output directory."""
import sys

path, out = sys.argv[1], sys.argv[2]
save = len(sys.argv) > 3 and sys.argv[3] == "save"
data = open(path, "rb").read()
print(f"initrd size: {len(data)} B")
offset, segment, ranges = 0, 0, []
ZSTD = b"\x28\xb5\x2f\xfd"

while offset < len(data):
    while offset < len(data) and data[offset] == 0:
        offset += 1
    if offset >= len(data):
        break
    if data[offset:offset + 4] == ZSTD:
        print(f"segment {segment}: zstd, offset {offset}, size {len(data) - offset}")
        with open(out + "/partc.zst", "wb") as fh:
            fh.write(data[offset:])
        break
    if data[offset:offset + 6] != b"070701":
        sys.exit(f"segment {segment}: unknown magic at {offset}")
    start, entries = offset, 0
    while True:
        header = data[offset:offset + 110]
        filesize = int(header[54:62], 16)
        namesize = int(header[94:102], 16)
        name = data[offset + 110:offset + 110 + namesize - 1]
        offset += (110 + namesize + 3) & ~3
        offset += (filesize + 3) & ~3
        if name == b"TRAILER!!!":
            break
        entries += 1
    print(f"segment {segment}: cpio, offset {start}, size {offset - start}, entries {entries}")
    ranges.append((start, offset))
    segment += 1

if save:
    names = ("parta.cpio", "partb.cpio")
    if len(ranges) != 2:
        sys.exit(f"expected 2 cpio segments, found {len(ranges)}")
    for name, (start, end) in zip(names, ranges):
        with open(f"{out}/{name}", "wb") as fh:
            fh.write(data[start:end])
