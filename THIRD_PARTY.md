# Sources and assets

- Layout/data behavior: user's sibling `kindle-dash-gen` project (reference revision `242a686666db49519770e75bb8a891d6dcad439b`) and https://kindle.lenscoder.cn/dash.html. The sibling repository is not modified. `reference.html` is the downloaded visual reference. Confirm permission for redistribution of its layout and weather artwork before publishing a distribution.
- Font files: DejaVu Sans extracted from the reference HTML; license in `assets/fonts/LICENSE`. Noto Sans SC is used for Chinese news text under the SIL Open Font License 1.1; license in `assets/fonts/NotoSansSC-LICENSE.txt`.
- Weather icons: static rasterizations of the sibling project's `img/weather` SVG files. Circle masks and fallback cloud use its Pillow drawing behavior.
- Exchange sessions: generated with https://github.com/gerrymanoim/exchange_calendars (Apache-2.0); shipped as UTC session spans for XSHG/XNYS, 2020 through 2026.
- Runtime libraries: FreeType (FTL/GPL), HarfBuzz (MIT-style), libpng (libpng license), libyaml (MIT), libcurl (curl license), OpenSSL (Apache-2.0 for 3.x), nlohmann/json (MIT). Preserve the corresponding upstream notices when packaging linked libraries.
- FBInk: https://github.com/NiLuJe/FBInk (GPL-3.0); called as a separate executable. No FBInk binary is currently bundled.

Development dependencies are listed in `requirements-dev.txt`; they are not Kindle runtime dependencies.
