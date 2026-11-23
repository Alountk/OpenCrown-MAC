# Notice

This repository is a fork of [OpenCrown PC](https://github.com/Nezara/OpenCrown-PC) by Nezara, used with the author's permission to make and release a macOS (Apple Silicon) version. See [MAC.md](MAC.md).

## OpenCrown (Nezara)

Everything that came from OpenCrown PC is Nezara's work: the loader, the PS4 system libraries (HLE), the renderer, audio, input, saves, online play, the launcher, the lobby server and the artwork. It is **not** covered by the MIT licence below. Upstream has no open licence: ask the author before reusing it. The changes this fork makes inside those files belong to the same work and follow the same terms.

## MIT (this fork's own additions)

These files were written for the fork and are under the [MIT licence](LICENSE-MIT.txt):

- `linux-port/recomp/` (the static recompiler and its runtime and tests)
- `linux-port/play-mac.sh` and `linux-port/play-mac-online-test.sh`
- `linux-port/packaging/make-app-mac.sh` and `linux-port/packaging/make-icon.py`
- `linux-port/tests/t7_compare.py`, `play_mac_test.sh`, `play_mac_online_test.sh`, `make_app_mac_test.sh`
- `docs/MAC.md`, `docs/CHANGELOG.md`, `docs/ROADMAP.md`

Images and icons (including the app icon, made from the OpenCrown logo) are not MIT.

## The game

Dragon's Crown Pro belongs to Vanillaware and Atlus. No game files are in this repository: you need your own dump.
