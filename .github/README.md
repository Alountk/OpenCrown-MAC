# OpenCrown for Mac

**Dragon's Crown Pro on Apple Silicon.** This is a macOS contribution to [OpenCrown PC](https://github.com/Nezara/OpenCrown-PC) by **Nezara**, made with the author's permission. OpenCrown is Nezara's project: the loader, the PS4 system libraries, the renderer, saves, online play and the launcher are theirs. This fork only adds what a Mac needs to run it.

> [!IMPORTANT]
> **No game files are included, and none ever will be.** You need your own decrypted dump of Dragon's Crown Pro (CUSA10454).

## What it adds

A Mac cannot run the game's x86-64 code directly, so this fork **statically recompiles your own dump to C++** and builds it natively for arm64: no emulator, no JIT. Everything else (graphics over Metal, audio, controllers, saves, online) works as in OpenCrown, with Mac-specific fixes. Online play uses OpenCrown's public lobby, so Mac players join the same rooms as everyone else.

## Try it

1. Download the latest `.dmg` from [Releases](https://github.com/Alountk/opencrown-mac/releases), open it and drag **OpenCrown** to Applications.
2. The app is signed ad hoc and not notarized (that needs a paid Apple Developer account): the first time, right click it and choose **Open**.
3. Point it at your dump and press Play. The first run recompiles and builds the game on your Mac (a few minutes, needs `cmake` and the Xcode command line tools); later runs take seconds.

Full guide, controllers, building from source and keeping up with upstream: [docs/MAC.md](../docs/MAC.md). Progress: [docs/ROADMAP.md](../docs/ROADMAP.md). Releases are named `<OpenCrown version>-mac.<n>`: [docs/CHANGELOG.md](../docs/CHANGELOG.md).

## Licence and credit

OpenCrown's code is Nezara's and is not relicensed here; this fork uses it with their permission. The fork's own additions (the recompiler and the Mac scripts) are MIT. Details in [docs/NOTICE.md](../docs/NOTICE.md).

This is a hobby project, not affiliated with or endorsed by Vanillaware, Atlus, Sony or Nezara. Anything meant for OpenCrown itself goes to its author, with their agreement.

Upstream's own README, with everything about OpenCrown itself, is [here](../README.md).
