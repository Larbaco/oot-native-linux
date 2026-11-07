# OoT Native launcher

The launcher source is `oot_native_launcher.py`. In a release package it finds
the builder, Python, .NET runtime, and native tools beside itself through the
package layout; it does not ask the user to select a builder checkout.

The GUI asks for a user-owned `.nup` dump and a dedicated build directory. It
validates the dump with `nxs2.py check-dump` before running the local builder.
It does not edit the dump or download files. Build artifacts contain game data
derived from that dump and must not be shared.

Headless entry points use the same builder validation and build command:

```sh
oot-native-launcher --check /path/to/your-dump.nup
oot-native-launcher --build /path/to/your-dump.nup --out /path/to/empty-build
```

The launcher is distributed under GPL-3.0-or-later as part of the release;
individual Python and Tcl/Tk license notices must accompany the packaged
PyInstaller build.
