# Third-party notices for `host/`

`nxnative-switch2.patch` is a set of changes to [nxnative](https://github.com/seedds/nxnative) and so contains
modified nxnative source. `dmsh_xlate_bin/` is a build of nxnative's shader translator wrapper and of three
[Ryujinx](https://github.com/Ryujinx/Ryujinx) libraries (`Ryujinx.Graphics.Shader`, `Ryujinx.Common`,
`Spv.Generator`, version 1.3.2 as carried in nxnative's `dm/third_party/ryujinx`, with the two source changes in the
patch). Both projects are MIT-licensed; their notices follow and apply to those files. nxnative itself started from
[d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) (MIT) and lists its other third-party components, with their
licences, in its `dm/third_party/README.md`.

The translator publish output also contains the following NuGet dependencies:
Gommon 2.8.1.2 (MIT), Humanizer 3.0.10 (MIT), Microsoft.IO.RecyclableMemoryStream
3.0.1 (MIT), MsgPack.Cli 1.0.1 (Apache-2.0), and System.CodeDom 4.4.0 (MIT).
Their notices are included in `LICENSES/` in the binary package.

## nxnative

```
MIT License

Copyright (c) 2026 seedds

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Ryujinx

```
MIT License

Copyright (c) Ryujinx Team and Contributors

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## nxnative's Windows port (not used by this tree)

pokopia-native's Windows/iPhone setup also fetched the `win/` folder of [NXNativeWindows](https://github.com/zenvyzenshiro/NXNativeWindows)
(MIT). This tree does not (the iPhone builder was removed); `host/windows/dmsh_xlate_bin/` is the translator above built as portable
.NET (AnyCPU) from the same patched sources, which the Linux build runs with .NET's Linux runtime.

## The changes themselves

The Switch 2 changes in the patch and the `dmsh_xlate` wrapper build are released under this repository's MIT licence
(`../LICENSE`). Nothing in `host/` comes from the game, from the console's firmware, or from Nintendo's SDK: the NVN2
entry points and structures were worked out by observing what the game calls, and `names.txt` lists API function names
only.

The Switch 2 texture layout (`dm/gfx/tex/s2tex.c`, `dm/gfx/tex/s2tex.h`, `dm/gfx/msl/s2tex.metal`) is our own MIT code,
written clean-room from a functional specification of the layout and verified bit for bit against the game's textures.
It replaces the earlier code, which followed the Eden-NS2 emulator's GPL-3.0-or-later code; none of that remains in the
patch.
