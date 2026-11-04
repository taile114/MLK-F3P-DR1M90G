"""Build an ext4-rootfs payload from the paired NPU initramfs and a local overlay.

Run in the vendor Linux build environment. This tool writes archives, never disks.
Device nodes and hard links are preserved without extracting them as root.
"""
import argparse
import ctypes
import ctypes.util
import hashlib
import io
import json
import os
from pathlib import Path
import stat
import tarfile

BASE_SHA256 = "4dc1f9cfaadfa19cae2dd4b4cd9e0795e3289bbefd5f2d01af9adeada3ae074e"
ARMNN_SHA256 = "5def7ba75e4b59644be6f58deaf8b2bb3791c62ed38b52b6aea08b11a8f728ce"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def load_lz4():
    """Resolve liblz4 without relying on ldconfig.

    ctypes.util.find_library() needs an ld.so cache, which NixOS and some
    vendor build containers do not have. Fall back to the loader's soname and
    then to LD_LIBRARY_PATH, so the documented command works in both
    environments; LZ4_LIB overrides everything.
    """
    candidates = [os.environ.get("LZ4_LIB"), ctypes.util.find_library("lz4"), "liblz4.so.1", "liblz4.so"]
    for name in candidates:
        if not name:
            continue
        try:
            return ctypes.CDLL(name)
        except OSError:
            pass
    for directory in os.environ.get("LD_LIBRARY_PATH", "").split(":"):
        for soname in ("liblz4.so.1", "liblz4.so"):
            candidate = Path(directory) / soname
            if directory and candidate.exists():
                try:
                    return ctypes.CDLL(str(candidate))
                except OSError:
                    pass
    raise ValueError("liblz4 not found; install it or set LZ4_LIB=/path/to/liblz4.so.1")


def unpack_initrd(path):
    raw = path.read_bytes()
    if digest(raw) != BASE_SHA256:
        raise ValueError("Expected the paired frozen vendor NPU initramfs")
    raw = raw[64:]
    if raw[:4] != bytes.fromhex("02214c18"):
        raise ValueError("Expected legacy LZ4 initramfs")
    lib = load_lz4()
    lib.LZ4_decompress_safe.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
    pos, parts = 4, []
    while pos < len(raw):
        if pos + 4 > len(raw):
            raise ValueError("Truncated LZ4 block header")
        size = int.from_bytes(raw[pos:pos + 4], "little")
        pos += 4
        if not size:
            break
        if pos + size > len(raw):
            raise ValueError("Truncated LZ4 block")
        output = ctypes.create_string_buffer(8 * 1024 * 1024)
        count = lib.LZ4_decompress_safe(raw[pos:pos + size], output, size, len(output))
        if count < 0:
            raise ValueError("Invalid LZ4 block")
        parts.append(output.raw[:count])
        pos += size
    return b"".join(parts)


def cpio_entries(raw):
    pos, entries = 0, {}
    while raw[pos:pos + 6] == b"070701":
        fields = [int(raw[pos + 6 + i * 8:pos + 14 + i * 8], 16) for i in range(13)]
        pos += 110
        name = raw[pos:pos + fields[11] - 1].decode()
        pos = (pos + fields[11] + 3) & ~3
        data = raw[pos:pos + fields[6]]
        if len(data) != fields[6]:
            raise ValueError("Truncated CPIO entry")
        pos = (pos + fields[6] + 3) & ~3
        if name == "TRAILER!!!":
            return entries
        name = name.removeprefix("./") if hasattr(name, "removeprefix") else (name[2:] if name.startswith("./") else name)
        if name.startswith("/") or ".." in Path(name).parts:
            raise ValueError("Unsafe CPIO path")
        entries[name] = (fields, data)
    raise ValueError("CPIO trailer missing")


def prepare(base, overlay, output):
    if output.exists():
        raise FileExistsError(output)
    entries = cpio_entries(unpack_initrd(base))
    if digest(entries["opt/armnn_lib/lib/libarmnn.so.32.1"][1]) != ARMNN_SHA256:
        raise ValueError("Paired ArmNN library mismatch")
    model = json.loads((overlay / "opt/mlk_rul/model.json").read_text())
    required = {"weights.bin", "weights.tsv", "qparams_ff6.tsv", "input_f32.bin", "libmlk_rul.so"}
    if (model.get("execution_plan"), model.get("quantization_plan"), model.get("npu_branches")) != ("ff6", "qparams_ff6.tsv", 6):
        raise ValueError("Expected sealed FF6 model")
    if set(model.get("sha256", {})) != required:
        raise ValueError("Incorrect model hash manifest")
    for name, expected in model["sha256"].items():
        if digest((overlay / "opt/mlk_rul" / name).read_bytes()) != expected:
            raise ValueError("Model file checksum mismatch: " + name)
    # stage_rul.sh installs the model payload into a reused directory. A file
    # left from an older plan (for example qparams_head.tsv) would ship inside
    # the image and contradict the sealed manifest, so refuse it here.
    staged = {p.name for p in (overlay / "opt/mlk_rul").iterdir() if p.is_file()}
    stale = sorted(staged - required - {"model.json", "checksums.sha256"})
    if stale:
        raise ValueError("Unexpected file in opt/mlk_rul, restage the bundle: " + ", ".join(stale))
    for name in ("opt/qt_play/mlk_ps_local", "opt/mlk_rul/libmlk_rul.so"):
        elf = (overlay / name).read_bytes()
        if elf[:6] != b"\x7fELF\x02\x01" or int.from_bytes(elf[18:20], "little") != 183:
            raise ValueError("Expected an AArch64 executable: " + name)

    additions = {p.relative_to(overlay).as_posix(): p for p in sorted(overlay.rglob("*"))
                 if p.name not in (".gitkeep", ".gitignore")}
    # CPIO hard-link data can be stored in the last member of a link group.
    canonical = {}
    for name, (fields, data) in entries.items():
        if name not in additions and stat.S_ISREG(fields[1]) and fields[4] > 1:
            key = (fields[0], fields[7], fields[8])
            if key not in canonical or data:
                canonical[key] = name
    output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(output, "w:gz") as tar:
        deferred = []
        for name, (fields, data) in entries.items():
            if name in additions:
                continue
            info = tarfile.TarInfo(name)
            info.mode, info.uid, info.gid, info.mtime = stat.S_IMODE(fields[1]), fields[2], fields[3], fields[5]
            info.uname = info.gname = "root"
            if stat.S_ISREG(fields[1]):
                key = (fields[0], fields[7], fields[8])
                if fields[4] > 1 and canonical[key] != name:
                    info.type, info.linkname = tarfile.LNKTYPE, canonical[key]
                    deferred.append(info)
                    continue
                info.size = len(data)
            elif stat.S_ISDIR(fields[1]):
                info.type = tarfile.DIRTYPE
            elif stat.S_ISLNK(fields[1]):
                info.type, info.linkname = tarfile.SYMTYPE, data.decode()
            elif stat.S_ISCHR(fields[1]) or stat.S_ISBLK(fields[1]):
                info.type = tarfile.CHRTYPE if stat.S_ISCHR(fields[1]) else tarfile.BLKTYPE
                info.devmajor, info.devminor = fields[9:11]
            elif stat.S_ISFIFO(fields[1]):
                info.type = tarfile.FIFOTYPE
            else:
                raise ValueError("Unsupported CPIO file type: " + name)
            tar.addfile(info, io.BytesIO(data) if info.isreg() else None)
        for info in deferred:
            tar.addfile(info)
        for name, path in additions.items():
            info = tar.gettarinfo(str(path), arcname=name)
            info.uid = info.gid = 0
            info.uname = info.gname = "root"
            with path.open("rb") if info.isreg() else io.BytesIO() as stream:
                tar.addfile(info, stream if info.isreg() else None)
    with tarfile.open(output) as tar:
        for name, expected in model["sha256"].items():
            if digest(tar.extractfile("opt/mlk_rul/" + name).read()) != expected:
                raise ValueError("Packaged model checksum mismatch")
        if digest(tar.extractfile("opt/armnn_lib/lib/libarmnn.so.32.1").read()) != ARMNN_SHA256:
            raise ValueError("Packaged ArmNN checksum mismatch")
    report = {"base_initrd_sha256": BASE_SHA256, "rootfs_sha256": digest(output.read_bytes()),
              "armnn_sha256": ARMNN_SHA256, "execution_plan": "ff6", "model_sha256": model["sha256"],
              "rootfs_bytes": output.stat().st_size, "disk_written": False, "hardware_boot_verified": False}
    output.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vendor-initrd", type=Path, required=True)
    parser.add_argument("--overlay", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    prepare(args.vendor_initrd, args.overlay, args.output)
