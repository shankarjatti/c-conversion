# Third-party material

* **GNU Radio 3.10** (GPL-3.0+) — the decoders are GNU Radio C++ flowgraphs; installed on the machine, not included.
* **gr-dsd** (the DSD voice decoder as a GNU Radio block; GPL) — installed on the machine (`/usr/local`), not included.
* **tetra-kit** (TETRA decoder, GPL) — `terrestrial/third_party/tetra_runtime/` holds two **prebuilt** shared libraries built from it
  (`libtetrakit_capi.so`: a thin C wrapper, source `capi_shim.cc` + `Makefile` included; `libtetra_codec.so`: the ACELP speech codec) in the
  directory layout the decoder expects. They are included only so the module runs; rebuild them from tetra-kit if you prefer.
* **nlohmann/json** (MIT), **ZeroMQ** (MPL-2.0), **Qt 5** (LGPL-3.0), **PulseAudio** client library (LGPL), **SQLite** (public domain) — build/runtime
  dependencies from the system (development headers can be unpacked without root, see `cmake/LocalDeps.cmake`).
* **Map tiles** `air_maritime/data/world_zoom12.mbtiles` — offline tiles (zoom 0–12, only part of the world at zoom 7–12) of the CARTO "dark_all" basemap,
  map data © OpenStreetMap contributors, tiles © CARTO. Attribution is shown in the console.
* No licence has been chosen for the code of this repository yet.
