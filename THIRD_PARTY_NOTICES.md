# Third-party notices

mxl-browser-source is MIT-licensed (`LICENSE`). The image also contains the software below. In the
image, the CEF and Chromium licences are in `/usr/share/doc/mxl-browser-source/cef/`, the Debian
copyright files of every Ubuntu package in `/usr/share/doc/<package>/copyright`.

| Component | Use | Licence |
| --- | --- | --- |
| Chromium Embedded Framework (CEF), minimal distribution (version in `/api/v1/info`) | renders the page | BSD-3-Clause (`cef/LICENSE.txt`) |
| Chromium and its third-party components, shipped inside CEF | the browser engine | listed per component in `cef/CREDITS.html` |
| MXL (`dmf-mxl/mxl`, commit in the image label `io.dmf.mxl.revision`) | MXL flows | Apache-2.0 |
| nmos-cpp (`sony/nmos-cpp`) | IS-04/IS-05 node | Apache-2.0 |
| C++ REST SDK (Ubuntu `libcpprest2.10`) | HTTP for nmos-cpp | MIT |
| Boost (Ubuntu packages) | nmos-cpp | BSL-1.0 |
| libsamplerate (Ubuntu `libsamplerate0`) | audio resampling | BSD-2-Clause |
| picojson (`third_party/picojson`) | JSON | BSD-2-Clause |
| stb_image_write (`third_party/stb`) | preview JPEGs | public domain or MIT |
| doctest (`third_party/doctest`, tests only, not in the image) | unit tests | MIT |
| Fonts: DejaVu, Liberation 2, Noto, Noto CJK, Noto Color Emoji (Ubuntu packages) | page text | Bitstream Vera / DejaVu licence (DejaVu), SIL OFL 1.1 (the others) |
