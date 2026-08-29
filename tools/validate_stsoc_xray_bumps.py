"""Validate generated STSoC X-Ray bump pairs and THM associations."""

from __future__ import annotations

import argparse
import collections
import struct
from pathlib import Path

from PIL import Image

from generate_stsoc_xray_bumps import (
    DDS_MAGIC,
    TBM_NONE,
    TBM_USE,
    TF_DXT5,
    TF_RGBA,
    THM_CHUNK_BUMP,
    THM_CHUNK_TEXTUREPARAM,
    THM_CHUNK_TEXTURE_TYPE,
    THM_CHUNK_TYPE,
    THM_CHUNK_VERSION,
    THM_TEXTURE,
    THM_VERSION,
    TT_BUMPMAP,
    TT_IMAGE,
    choose_profile,
)


def parse_dds(path: Path) -> tuple[int, int, int, bytes, int]:
    data = path.read_bytes()
    assert data[:4] == DDS_MAGIC, path
    height, width = struct.unpack_from("<II", data, 12)
    mip_count = struct.unpack_from("<I", data, 28)[0]
    fourcc = data[84:88]
    return width, height, mip_count, fourcc, len(data)


def expected_dxt5_size(width: int, height: int) -> tuple[int, int]:
    total = 128
    mips = 0
    while True:
        total += max(1, (width + 3) // 4) * max(1, (height + 3) // 4) * 16
        mips += 1
        if width == 1 and height == 1:
            return total, mips
        width, height = max(1, width // 2), max(1, height // 2)


def parse_thm(path: Path) -> dict[int, bytes]:
    data = path.read_bytes()
    chunks: dict[int, bytes] = {}
    offset = 0
    while offset < len(data):
        chunk_id, size = struct.unpack_from("<II", data, offset)
        offset += 8
        chunks[chunk_id] = data[offset : offset + size]
        offset += size
    assert offset == len(data), path
    return chunks


def validate_thm_common(chunks: dict[int, bytes]) -> None:
    assert struct.unpack("<H", chunks[THM_CHUNK_VERSION])[0] == THM_VERSION
    assert struct.unpack("<I", chunks[THM_CHUNK_TYPE])[0] == THM_TEXTURE


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(r"D:\STSoC 3.0\gamedata\textures\stsoc"),
    )
    parser.add_argument(
        "--require-complete",
        action="store_true",
        help="Fail when a source texture has no complete generated set",
    )
    args = parser.parse_args()
    root = args.root.resolve()
    all_sources = sorted(
        path
        for path in root.rglob("*.dds")
        if not path.stem.endswith("_bump") and not path.stem.endswith("_bump#")
    )
    missing = [
        source
        for source in all_sources
        if not all(path.exists() for path in (
            source.with_name(source.stem + "_bump.dds"),
            source.with_name(source.stem + "_bump#.dds"),
            source.with_suffix(".thm"),
            source.with_name(source.stem + "_bump.thm"),
        ))
    ]
    if args.require_complete and missing:
        raise SystemExit(f"Incomplete generated sets: {len(missing)}")
    sources = [source for source in all_sources if source not in missing]
    profiles: collections.Counter[str] = collections.Counter()
    total_generated_bytes = 0

    for source in sources:
        relative = source.relative_to(root)
        profile = choose_profile(relative)
        profiles[profile.name] += 1
        width, height, _, source_fourcc, _ = parse_dds(source)
        assert source_fourcc == b"\0\0\0\0", source

        bump = source.with_name(source.stem + "_bump.dds")
        correction = source.with_name(source.stem + "_bump#.dds")
        source_thm = source.with_suffix(".thm")
        bump_thm = bump.with_suffix(".thm")
        expected_size, expected_mips = expected_dxt5_size(width, height)

        for generated_dds in (bump, correction):
            out_width, out_height, out_mips, out_fourcc, out_size = parse_dds(generated_dds)
            assert (out_width, out_height) == (width, height), generated_dds
            assert out_mips == expected_mips, generated_dds
            assert out_fourcc == b"DXT5", generated_dds
            assert out_size == expected_size, generated_dds
            with Image.open(generated_dds) as image:
                assert image.size == (width, height), generated_dds
                image.getpixel((0, 0))
            total_generated_bytes += out_size

        source_chunks = parse_thm(source_thm)
        bump_chunks = parse_thm(bump_thm)
        validate_thm_common(source_chunks)
        validate_thm_common(bump_chunks)

        source_params = struct.unpack("<8I", source_chunks[THM_CHUNK_TEXTUREPARAM])
        bump_params = struct.unpack("<8I", bump_chunks[THM_CHUNK_TEXTUREPARAM])
        assert source_params[0] == TF_RGBA and source_params[6:8] == (width, height), source_thm
        assert bump_params[0] == TF_DXT5 and bump_params[6:8] == (width, height), bump_thm
        assert struct.unpack("<I", source_chunks[THM_CHUNK_TEXTURE_TYPE])[0] == TT_IMAGE, source_thm
        assert struct.unpack("<I", bump_chunks[THM_CHUNK_TEXTURE_TYPE])[0] == TT_BUMPMAP, bump_thm

        source_bump_data = source_chunks[THM_CHUNK_BUMP]
        bump_bump_data = bump_chunks[THM_CHUNK_BUMP]
        _, source_mode = struct.unpack_from("<fI", source_bump_data)
        _, bump_mode = struct.unpack_from("<fI", bump_bump_data)
        expected_reference = str(bump.relative_to(root.parent).with_suffix("")).encode("ascii") + b"\0"
        assert source_mode == TBM_USE and source_bump_data[8:] == expected_reference, source_thm
        assert bump_mode == TBM_NONE and bump_bump_data[8:] == b"\0", bump_thm

    temporary_files = list(root.rglob("*.tmp"))
    assert not temporary_files, temporary_files
    if not missing:
        assert len(list(root.rglob("*_bump.dds"))) == len(sources)
        assert len(list(root.rglob("*_bump#.dds"))) == len(sources)
        assert len(list(root.rglob("*.thm"))) == len(sources) * 2

    profile_text = ", ".join(f"{name}={count}" for name, count in sorted(profiles.items()))
    print(f"Validated {len(sources)} diffuse textures, {len(sources)} bump pairs, and {len(sources) * 2} THM files")
    if missing:
        print(f"Incomplete source sets skipped: {len(missing)}")
    print(f"Profiles: {profile_text}")
    print(f"Generated DDS size: {total_generated_bytes / (1024 * 1024):.1f} MiB")


if __name__ == "__main__":
    main()
