# XDBARDDRV - packages

**To download the objects save file of a version, use the
[Releases](https://github.com/cesco030/xdbarddrv/releases) page of the
repository** (the newest: [latest release](https://github.com/cesco030/xdbarddrv/releases/latest)).

This branch holds everything a build produced, one folder for each version.
Every package was built on PUB400.COM, a public IBM i system, from the sources
of this repository: a clone of the commit named in the file `COMMIT` of the
folder (it carries the tag `v<version>`), compiled there by `build/dist.sh` in
library CESCO2. `dist.log` is the log of that build. When a folder arrives
here, the workflow of this branch makes the release of its tag and attaches the
objects save file.

| File | What |
|---|---|
| `xdb-<version>-objects.savf` | the objects: `RSTOBJ ... SAVLIB(CESCO2)` (the same file as in the release) |
| `xdb-<version>-sources.savf` | the sources as a save file of stream files |
| `README.md` | how to install, update and use it |
| `WRKXDBPRF-help.html` | the help of the command `WRKXDBPRF` as a page |
| `LICENSE`, `THIRD-PARTY-NOTICES.txt` | licence (MIT) and the notices of the PostgreSQL code in it |
| `dist.log`, `COMMIT`, `SHA256SUMS` | the log of the build, the commit it was built from, checksums |

Download a save file in binary, check it with `sha256sum -c SHA256SUMS`, and
follow the README of the folder.

The software is offered as it is, without warranty of any kind.
