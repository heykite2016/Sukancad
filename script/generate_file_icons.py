#!/usr/bin/env python3
"""Build the DWG/DXF document icons used by file associations."""

from pathlib import Path
import subprocess
import tempfile

from PIL import Image


ROOT = Path(__file__).resolve().parent.parent
ICON_DIR = ROOT / "resources" / "icons"
MACOS_DIR = ROOT / "resources" / "macos"
WINDOWS_DIR = ROOT / "resources" / "windows"


def render_svg(source: Path, destination: Path) -> None:
    subprocess.run(
        [
            "/usr/bin/sips",
            "-s",
            "format",
            "png",
            str(source),
            "--out",
            str(destination),
        ],
        check=True,
        stdout=subprocess.DEVNULL,
    )


def build_icns(source_image: Path, destination: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="cadlens-file-icons-") as raw:
        iconset = Path(raw) / "CadLens.iconset"
        iconset.mkdir()
        image = Image.open(source_image).convert("RGBA")
        sizes = {
            "icon_16x16.png": 16,
            "icon_16x16@2x.png": 32,
            "icon_32x32.png": 32,
            "icon_32x32@2x.png": 64,
            "icon_128x128.png": 128,
            "icon_128x128@2x.png": 256,
            "icon_256x256.png": 256,
            "icon_256x256@2x.png": 512,
            "icon_512x512.png": 512,
            "icon_512x512@2x.png": 1024,
        }
        for name, size in sizes.items():
            image.resize((size, size), Image.Resampling.LANCZOS).save(
                iconset / name
            )
        subprocess.run(
            ["/usr/bin/iconutil", "-c", "icns", str(iconset),
             "-o", str(destination)],
            check=True,
        )


def build_ico(source_image: Path, destination: Path) -> None:
    image = Image.open(source_image).convert("RGBA")
    image.save(
        destination,
        format="ICO",
        sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64),
               (128, 128), (256, 256)],
    )


def main() -> None:
    MACOS_DIR.mkdir(parents=True, exist_ok=True)
    for format_name in ("dwg", "dxf"):
        svg = ICON_DIR / f"file-{format_name}.svg"
        with tempfile.TemporaryDirectory(
            prefix="cadlens-file-icon-render-"
        ) as raw:
            png = Path(raw) / f"{format_name}.png"
            render_svg(svg, png)
            build_icns(
                png, MACOS_DIR / f"CadLens{format_name.upper()}.icns"
            )
            build_ico(
                png, WINDOWS_DIR / f"CadLens{format_name.upper()}.ico"
            )


if __name__ == "__main__":
    main()
