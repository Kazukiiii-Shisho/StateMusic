# StateMusicAudio — REFramework native audio plugin

Native audio plugin used by the **State Music** mod for Monster Hunter Wilds. The compiled
`StateMusicAudio.dll` ships with the mod on Nexus Mods — this repository is the source only.

## Third-party components
This repository vendors two single-file libraries, each under its own permissive license
(the full license text is at the end of each file — do not strip it):

- **miniaudio** by David Reid — public domain (Unlicense) or MIT-0.
  https://github.com/mackron/miniaudio
- **stb_vorbis** by Sean Barrett — public domain (or MIT), part of the stb collection.
  https://github.com/nothings/stb

Built against, but not included here:
- **REFramework** by praydog — https://github.com/praydog/REFramework
- **sol2** by ThePhD — https://github.com/ThePhD/sol2

## License
The code written for StateMusicAudio (`Plugin.cpp`, `CMakeLists.txt`) is released under the
MIT License — see `LICENSE`. The vendored libraries keep their own licenses as stated above.
