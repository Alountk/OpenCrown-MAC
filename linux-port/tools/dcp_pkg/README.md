# dcp_pkg

Unpacks the player's own PS4 fake package (fPKG) into a game folder, so the dump does not have to be extracted by hand.

```
dcp_pkg info    <game.pkg>
dcp_pkg extract <game.pkg> <out folder>     # prints "progress <done> <total>" lines
```

Only fake packages (image key index 3) can be unpacked; packages encrypted for a console are refused. Nothing from the
game is stored in this repository.

## Licence

`pkg_extract.*` is taken from the odin-sphere-mac launcher, where it derives from shadPS4's removed `pkg.cpp`,
`pfs.h` and `pkg_type.cpp` (copyright shadPS4 Emulator Project, GPL-2.0-or-later; the licence text is in
`LICENSE-GPL-2.0.txt`). `crypto.h`, `crypto_openssl.cpp` (OpenSSL in place of shadPS4's Crypto++) and `utf8.h` come from
the same launcher and are linked into it, so they are treated as part of the GPL program. `main.cpp` and this README
are this fork's (MIT).

This is the only GPL code in the repository, so it is built as its own program and **run as a process**, never linked
into another target and never copied into a change meant for upstream OpenCrown PC (whose code has no open licence).
`libcrypto` (OpenSSL, Apache-2.0) is linked statically so the app needs no OpenSSL installed.
