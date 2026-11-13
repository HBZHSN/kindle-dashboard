"""Offline WSL bootstrap: official upstream headers; system shared libraries."""
import io
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
include = ROOT / ".build/include"
include.mkdir(parents=True, exist_ok=True)
sources = [
    ("freetype/freetype", "VER-2-13-2", "include/", ""),
    ("curl/curl", "curl-8_5_0", "include/", ""),
    ("yaml/libyaml", "0.2.5", "include/", ""),
    ("pnggroup/libpng", "v1.6.43", "", ""),
    ("harfbuzz/harfbuzz", "6.0.0", "src/", ""),
]
for repo, tag, prefix, dest in sources:
    archive = ROOT / ".build" / (repo.split("/")[-1] + "-headers.tar.gz")
    if not archive.exists():
        urllib.request.urlretrieve(f"https://codeload.github.com/{repo}/tar.gz/refs/tags/{tag}", archive)
    with tarfile.open(archive) as tar:
        for member in tar.getmembers():
            relative = member.name.split("/", 1)[-1]
            if repo.endswith("libpng"):
                if relative not in ("png.h", "pngconf.h", "scripts/pnglibconf.h.prebuilt"):
                    continue
                relative = "pnglibconf.h" if relative.endswith(".prebuilt") else relative
            else:
                if repo.endswith("harfbuzz") and relative == "src/hb-version.h.in":
                    content = tar.extractfile(member).read().decode()
                    for key, value in {"HB_VERSION_MAJOR":"6", "HB_VERSION_MINOR":"0", "HB_VERSION_MICRO":"0", "HB_VERSION":"6.0.0"}.items():
                        content = content.replace("@"+key+"@", value)
                    (include / "hb-version.h").write_text(content, encoding="utf-8")
                    continue
                if not relative.startswith(prefix) or not relative.endswith(".h"):
                    continue
                relative = relative[len(prefix):]
            if member.isfile():
                target = include / relative
                assert target.resolve().is_relative_to(include.resolve())
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(tar.extractfile(member).read())
    print(repo)
(include / "nlohmann").mkdir(exist_ok=True)
urllib.request.urlretrieve("https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp",
                           include / "nlohmann/json.hpp")
