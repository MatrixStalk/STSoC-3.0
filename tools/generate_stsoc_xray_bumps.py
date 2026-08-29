"""Generate X-Ray Engine bump pairs and CoP-compatible THM metadata.

The generated primary bump uses the layout consumed by this project:
    R = gloss, G = normal Z, B = normal Y, A = normal X
The companion ``_bump#`` texture stores BC3 error correction in RGB and
height in alpha.  Both textures contain a complete mip chain.
"""

from __future__ import annotations

import argparse
import io
import struct
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter


DDS_MAGIC = b"DDS "
DDS_HEADER_SIZE = 128
DDS_FLAGS_MIPMAPPED_LINEAR = 0x000A1007
DDS_CAPS_MIPMAPPED = 0x00401008
DDS_PF_FOURCC = 0x00000004
FOURCC_DXT5 = struct.unpack("<I", b"DXT5")[0]

THM_VERSION = 0x0012
THM_TEXTURE = 1
THM_CHUNK_VERSION = 0x0810
THM_CHUNK_TEXTUREPARAM = 0x0812
THM_CHUNK_TYPE = 0x0813
THM_CHUNK_TEXTURE_TYPE = 0x0814
THM_CHUNK_DETAIL_EXT = 0x0815
THM_CHUNK_MATERIAL = 0x0816
THM_CHUNK_BUMP = 0x0817
THM_CHUNK_EXT_NORMALMAP = 0x0818
THM_CHUNK_FADE_DELAY = 0x0819

TF_DXT1 = 0
TF_DXT3 = 2
TF_DXT5 = 3
TF_RGBA = 8
TT_IMAGE = 0
TT_BUMPMAP = 2
TBM_NONE = 1
TBM_USE = 2
TM_BLIN_PHONG = 1
TM_PHONG_METAL = 2
TM_METAL_OREN_NAYAR = 3
THM_FLAGS_RGBA_MIPPED = 0x02000100


@dataclass(frozen=True)
class Profile:
    name: str
    material: int
    height_contrast: float
    normal_strength: float
    virtual_height: float
    gloss_base: float
    gloss_luma: float
    gloss_neutral: float
    gloss_roughness_loss: float
    gloss_min: float
    gloss_max: float


PROFILES = {
    "metal": Profile("metal", TM_METAL_OREN_NAYAR, 0.90, 5.0, 0.045, 0.28, 0.22, 0.08, 0.08, 0.26, 0.68),
    "ammo": Profile("ammo", TM_METAL_OREN_NAYAR, 0.72, 4.0, 0.035, 0.42, 0.28, 0.04, 0.05, 0.40, 0.82),
    "watch": Profile("watch", TM_PHONG_METAL, 0.78, 4.0, 0.035, 0.25, 0.28, 0.06, 0.08, 0.24, 0.72),
    "polymer": Profile("polymer", TM_BLIN_PHONG, 0.82, 4.0, 0.040, 0.13, 0.12, 0.02, 0.08, 0.10, 0.36),
    "bakelite": Profile("bakelite", TM_BLIN_PHONG, 0.84, 4.0, 0.040, 0.14, 0.16, 0.00, 0.07, 0.11, 0.38),
    "wood": Profile("wood", TM_BLIN_PHONG, 1.00, 4.5, 0.050, 0.09, 0.12, 0.00, 0.08, 0.06, 0.28),
    "fabric": Profile("fabric", TM_BLIN_PHONG, 0.92, 3.0, 0.035, 0.035, 0.06, 0.00, 0.05, 0.025, 0.16),
    "glass": Profile("glass", TM_BLIN_PHONG, 0.10, 1.0, 0.010, 0.78, 0.12, 0.00, 0.00, 0.75, 0.95),
    "flat": Profile("flat", TM_BLIN_PHONG, 0.00, 0.0, 0.000, 0.02, 0.00, 0.00, 0.00, 0.02, 0.02),
}


def choose_profile(relative_path: Path) -> Profile:
    name = str(relative_path).replace("/", "\\").lower()
    stem = relative_path.stem.lower()

    if "\\arms\\" in f"\\{name}":
        if stem == "blank":
            return PROFILES["flat"]
        if "watch" in stem:
            return PROFILES["watch"]
        return PROFILES["fabric"]
    if "glass" in stem:
        return PROFILES["glass"]
    if "patron" in stem or "ammo" in stem:
        return PROFILES["ammo"]
    if "wood" in stem:
        return PROFILES["wood"]
    if "bakelit" in stem or "pistolgrip" in stem:
        return PROFILES["bakelite"]
    if "mag_ak" in stem or "rpk16_drum" in stem:
        return PROFILES["polymer"]
    return PROFILES["metal"]


def detect_source_texture_format(path: Path) -> int:
    data = path.read_bytes()
    if data[:4] != DDS_MAGIC or len(data) < DDS_HEADER_SIZE:
        raise ValueError(f"Not a legacy DDS: {path}")

    pf_size, _, fourcc, bit_count, *_ = struct.unpack_from("<8I", data, 76)
    if pf_size != 32:
        raise ValueError(f"Invalid DDS pixel format in {path}")
    fourcc_bytes = struct.pack("<I", fourcc)
    if fourcc_bytes == b"DXT1":
        return TF_DXT1
    if fourcc_bytes == b"DXT3":
        return TF_DXT3
    if fourcc_bytes == b"DXT5":
        return TF_DXT5
    if fourcc == 0 and bit_count == 32:
        return TF_RGBA
    raise ValueError(f"Unsupported DDS format {fourcc_bytes!r}/{bit_count} bpp: {path}")


def read_rgba_dds(path: Path) -> np.ndarray:
    data = path.read_bytes()
    source_format = detect_source_texture_format(path)
    height, width = struct.unpack_from("<II", data, 12)
    _, _, fourcc, bit_count, r_mask, g_mask, b_mask, a_mask = struct.unpack_from("<8I", data, 76)

    if source_format != TF_RGBA:
        with Image.open(path) as image:
            return np.asarray(image.convert("RGBA"), dtype=np.uint8).copy()
    if (fourcc, bit_count, r_mask, g_mask, b_mask, a_mask) != (
        0,
        32,
        0xFF,
        0xFF00,
        0xFF0000,
        0xFF000000,
    ):
        with Image.open(path) as image:
            return np.asarray(image.convert("RGBA"), dtype=np.uint8).copy()

    top_size = width * height * 4
    if len(data) < DDS_HEADER_SIZE + top_size:
        raise ValueError(f"Truncated DDS: {path}")
    return np.frombuffer(data, dtype=np.uint8, count=top_size, offset=DDS_HEADER_SIZE).reshape(height, width, 4).copy()


def make_height_and_gloss(rgba: np.ndarray, profile: Profile) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    rgb = rgba[:, :, :3].astype(np.float32) / 255.0
    luminance = rgb @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    saturation = rgb.max(axis=2) - rgb.min(axis=2)

    luma_image = Image.fromarray(np.rint(luminance * 255.0).astype(np.uint8), "L")
    small_blur = np.asarray(luma_image.filter(ImageFilter.GaussianBlur(1.2)), dtype=np.float32) / 255.0
    large_blur = np.asarray(luma_image.filter(ImageFilter.GaussianBlur(5.0)), dtype=np.float32) / 255.0

    local_relief = 0.75 * (luminance - large_blur) + 0.35 * (luminance - small_blur)
    height = np.clip(0.5 + profile.height_contrast * local_relief, 0.05, 0.95)
    if profile.name == "flat":
        height.fill(0.5)

    roughness_hint = np.clip(np.abs(luminance - small_blur) * 6.0, 0.0, 1.0)
    gloss = (
        profile.gloss_base
        + profile.gloss_luma * luminance
        + profile.gloss_neutral * (1.0 - saturation)
        - profile.gloss_roughness_loss * roughness_hint
    )
    gloss = np.clip(gloss, profile.gloss_min, profile.gloss_max)

    dx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 0.5
    dy = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * 0.5
    nx = -dx * profile.normal_strength
    ny = -dy * profile.normal_strength
    nz = np.ones_like(nx)
    length = np.sqrt(nx * nx + ny * ny + nz * nz)
    normal = np.stack((nx / length, ny / length, nz / length), axis=2)

    return (
        np.rint(height * 255.0).astype(np.uint8),
        np.rint(gloss * 255.0).astype(np.uint8),
        normal,
    )


def resize_luma(array: np.ndarray, size: tuple[int, int]) -> np.ndarray:
    return np.asarray(Image.fromarray(array, "L").resize(size, Image.Resampling.BOX), dtype=np.uint8)


def resize_normal(normal: np.ndarray, size: tuple[int, int]) -> np.ndarray:
    packed = np.rint(np.clip(normal * 0.5 + 0.5, 0.0, 1.0) * 255.0).astype(np.uint8)
    resized = np.asarray(Image.fromarray(packed, "RGB").resize(size, Image.Resampling.BOX), dtype=np.float32)
    unpacked = resized / 255.0 * 2.0 - 1.0
    length = np.linalg.norm(unpacked, axis=2, keepdims=True)
    return unpacked / np.maximum(length, 1e-8)


def encode_dxt5_level(rgba: np.ndarray, decode: bool = False) -> tuple[bytes, np.ndarray | None]:
    stream = io.BytesIO()
    Image.fromarray(rgba, "RGBA").save(stream, format="DDS", pixel_format="DXT5")
    dds = stream.getvalue()
    decoded = None
    if decode:
        with Image.open(io.BytesIO(dds)) as image:
            decoded = np.asarray(image.convert("RGBA"), dtype=np.uint8)
    return dds[DDS_HEADER_SIZE:], decoded


def build_dxt5_dds(width: int, height: int, levels: list[bytes]) -> bytes:
    top_linear_size = max(1, (width + 3) // 4) * max(1, (height + 3) // 4) * 16
    header = (
        DDS_MAGIC
        + struct.pack(
            "<7I",
            124,
            DDS_FLAGS_MIPMAPPED_LINEAR,
            height,
            width,
            top_linear_size,
            0,
            len(levels),
        )
        + struct.pack("<11I", *((0,) * 11))
        + struct.pack("<8I", 32, DDS_PF_FOURCC, FOURCC_DXT5, 0, 0, 0, 0, 0)
        + struct.pack("<5I", DDS_CAPS_MIPMAPPED, 0, 0, 0, 0)
    )
    if len(header) != DDS_HEADER_SIZE:
        raise AssertionError(f"Invalid DDS header size: {len(header)}")
    return header + b"".join(levels)


def generate_bump_pair(rgba: np.ndarray, profile: Profile) -> tuple[bytes, bytes, dict[str, float | int | str]]:
    height, gloss, normal = make_height_and_gloss(rgba, profile)
    width, image_height = rgba.shape[1], rgba.shape[0]
    primary_levels: list[bytes] = []
    correction_levels: list[bytes] = []
    level_width, level_height = width, image_height
    level_heightmap, level_gloss, level_normal = height, gloss, normal

    while True:
        desired_packed = np.clip(level_normal * 0.5 + 0.5, 0.0, 1.0)
        primary_rgba = np.empty((level_height, level_width, 4), dtype=np.uint8)
        primary_rgba[:, :, 0] = level_gloss
        primary_rgba[:, :, 1] = np.rint(desired_packed[:, :, 2] * 255.0).astype(np.uint8)
        primary_rgba[:, :, 2] = np.rint(desired_packed[:, :, 1] * 255.0).astype(np.uint8)
        primary_rgba[:, :, 3] = np.rint(desired_packed[:, :, 0] * 255.0).astype(np.uint8)

        primary_data, decoded = encode_dxt5_level(primary_rgba, decode=True)
        assert decoded is not None
        primary_levels.append(primary_data)

        decoded_packed = np.stack(
            (decoded[:, :, 3], decoded[:, :, 2], decoded[:, :, 1]), axis=2
        ).astype(np.float32) / 255.0
        error_rgb = np.clip(desired_packed + 0.5 - decoded_packed, 0.0, 1.0)
        correction_rgba = np.empty_like(primary_rgba)
        correction_rgba[:, :, :3] = np.rint(error_rgb * 255.0).astype(np.uint8)
        correction_rgba[:, :, 3] = level_heightmap
        correction_data, _ = encode_dxt5_level(correction_rgba)
        correction_levels.append(correction_data)

        if level_width == 1 and level_height == 1:
            break
        next_width, next_height = max(1, level_width // 2), max(1, level_height // 2)
        next_size = (next_width, next_height)
        level_heightmap = resize_luma(level_heightmap, next_size)
        level_gloss = resize_luma(level_gloss, next_size)
        level_normal = resize_normal(level_normal, next_size)
        level_width, level_height = next_width, next_height

    stats: dict[str, float | int | str] = {
        "profile": profile.name,
        "material": profile.material,
        "width": width,
        "height": image_height,
        "mips": len(primary_levels),
        "gloss_mean": float(gloss.mean()),
        "gloss_min": int(gloss.min()),
        "gloss_max": int(gloss.max()),
        "normal_xy_rms": float(np.sqrt(np.mean(normal[:, :, :2] ** 2))),
    }
    return build_dxt5_dds(width, image_height, primary_levels), build_dxt5_dds(width, image_height, correction_levels), stats


def chunk(chunk_id: int, data: bytes) -> bytes:
    return struct.pack("<II", chunk_id, len(data)) + data


def make_thm(
    *,
    width: int,
    height: int,
    texture_format: int,
    texture_type: int,
    material: int,
    virtual_height: float,
    bump_mode: int,
    bump_name: str,
) -> bytes:
    texture_params = struct.pack(
        "<8I",
        texture_format,
        THM_FLAGS_RGBA_MIPPED,
        0,
        0,
        0,
        0,
        width,
        height,
    )
    return b"".join(
        (
            chunk(THM_CHUNK_VERSION, struct.pack("<H", THM_VERSION)),
            chunk(THM_CHUNK_TYPE, struct.pack("<I", THM_TEXTURE)),
            chunk(THM_CHUNK_TEXTUREPARAM, texture_params),
            chunk(THM_CHUNK_TEXTURE_TYPE, struct.pack("<I", texture_type)),
            chunk(THM_CHUNK_DETAIL_EXT, b"\0" + struct.pack("<f", 1.0)),
            chunk(THM_CHUNK_MATERIAL, struct.pack("<If", material, 0.0)),
            chunk(
                THM_CHUNK_BUMP,
                struct.pack("<fI", virtual_height, bump_mode) + bump_name.encode("ascii") + b"\0",
            ),
            chunk(THM_CHUNK_EXT_NORMALMAP, b"\0"),
            chunk(THM_CHUNK_FADE_DELAY, b"\0"),
        )
    )


def parse_thm_chunks(data: bytes) -> list[tuple[int, bytes]]:
    chunks: list[tuple[int, bytes]] = []
    offset = 0
    while offset < len(data):
        if offset + 8 > len(data):
            raise ValueError("Truncated THM chunk header")
        chunk_id, size = struct.unpack_from("<II", data, offset)
        offset += 8
        end = offset + size
        if end > len(data):
            raise ValueError(f"Truncated THM chunk 0x{chunk_id:04X}")
        chunks.append((chunk_id, data[offset:end]))
        offset = end
    return chunks


def patch_source_thm(
    data: bytes,
    *,
    virtual_height: float,
    bump_name: str,
    material: int | None = None,
) -> bytes:
    replacements = {
        THM_CHUNK_BUMP: struct.pack("<fI", virtual_height, TBM_USE) + bump_name.encode("ascii") + b"\0"
    }
    if material is not None:
        replacements[THM_CHUNK_MATERIAL] = struct.pack("<If", material, 0.0)

    chunks = parse_thm_chunks(data)
    seen: set[int] = set()
    output: list[bytes] = []
    for chunk_id, chunk_data in chunks:
        if chunk_id in replacements:
            chunk_data = replacements[chunk_id]
            seen.add(chunk_id)
        output.append(chunk(chunk_id, chunk_data))
    for chunk_id, chunk_data in replacements.items():
        if chunk_id not in seen:
            output.append(chunk(chunk_id, chunk_data))
    return b"".join(output)


def write_atomic(path: Path, data: bytes) -> None:
    temp_path = path.with_name(path.name + ".tmp")
    temp_path.write_bytes(data)
    temp_path.replace(path)


def find_textures_root(selected_root: Path) -> Path:
    for candidate in (selected_root, *selected_root.parents):
        if candidate.name.lower() == "textures":
            return candidate
    return selected_root


def output_paths(source: Path) -> tuple[Path, Path, Path, Path]:
    bump_path = source.with_name(source.stem + "_bump.dds")
    correction_path = source.with_name(source.stem + "_bump#.dds")
    source_thm_path = source.with_suffix(".thm")
    bump_thm_path = bump_path.with_suffix(".thm")
    return bump_path, correction_path, source_thm_path, bump_thm_path


def process_texture(
    source: Path,
    root: Path,
    *,
    profile: Profile | None = None,
    preserve_source_thm: bool = True,
    update_existing_material: bool = False,
) -> dict[str, float | int | str]:
    relative = source.relative_to(root)
    profile = profile or choose_profile(relative)
    rgba = read_rgba_dds(source)
    height, width = rgba.shape[:2]
    primary, correction, stats = generate_bump_pair(rgba, profile)

    bump_path, correction_path, source_thm_path, bump_thm_path = output_paths(source)
    textures_root = find_textures_root(root)
    bump_reference = str(bump_path.relative_to(textures_root).with_suffix(""))

    if preserve_source_thm and source_thm_path.exists():
        source_thm = patch_source_thm(
            source_thm_path.read_bytes(),
            virtual_height=profile.virtual_height,
            bump_name=bump_reference,
            material=profile.material if update_existing_material else None,
        )
    else:
        source_thm = make_thm(
            width=width,
            height=height,
            texture_format=detect_source_texture_format(source),
            texture_type=TT_IMAGE,
            material=profile.material,
            virtual_height=profile.virtual_height,
            bump_mode=TBM_USE,
            bump_name=bump_reference,
        )
    bump_thm = make_thm(
        width=width,
        height=height,
        texture_format=TF_DXT5,
        texture_type=TT_BUMPMAP,
        material=TM_BLIN_PHONG,
        virtual_height=profile.virtual_height,
        bump_mode=TBM_NONE,
        bump_name="",
    )

    write_atomic(bump_path, primary)
    write_atomic(correction_path, correction)
    write_atomic(source_thm_path, source_thm)
    write_atomic(bump_thm_path, bump_thm)
    stats["source"] = str(relative)
    stats["bump"] = str(bump_path.relative_to(root))
    return stats


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(r"D:\STSoC 3.0\gamedata\textures\stsoc"),
        help="Texture subtree to process",
    )
    parser.add_argument("--pattern", default="*.dds", help="Source filename glob")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    root = args.root.resolve()
    sources = sorted(
        path
        for path in root.rglob(args.pattern)
        if not path.stem.endswith("_bump") and not path.stem.endswith("_bump#")
    )
    if not sources:
        raise SystemExit(f"No source DDS files found under {root}")

    print(f"Generating X-Ray bump pairs for {len(sources)} textures in {root}", flush=True)
    for index, source in enumerate(sources, 1):
        stats = process_texture(source, root)
        print(
            f"[{index:02d}/{len(sources):02d}] {stats['source']} | {stats['profile']} | "
            f"gloss {stats['gloss_min']}-{stats['gloss_max']} (mean {stats['gloss_mean']:.1f}) | "
            f"normal RMS {stats['normal_xy_rms']:.3f}",
            flush=True,
        )


if __name__ == "__main__":
    main()
