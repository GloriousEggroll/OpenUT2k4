# Third-party code in Download/

## unshield (MIT)

`Download/ArenaDlCab.h/.cpp` is a C++ port of the InstallShield cabinet reading parts of
[unshield](https://github.com/twogood/unshield) (lib/libunshield.c, file.c, file_group.c, directory.c,
helper.c; ported from commit 0d1e7cbac7986fbd5aa050da8249a27428fee057). Copyright (c) 2003 David Eriksson,
MIT licence: see `unshield-LICENSE.txt`, which must ship with binaries that contain this code.

Not used: unshield's bundled RSA MD5 code (the engine's FMD5 is used), its iconv / OpenSSL dependencies, the
command line tool. zlib comes from the engine (`AddEngineThirdPartyPrivateStaticDependencies(Target, "zlib")`).
