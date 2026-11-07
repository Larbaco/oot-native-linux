# ampere-lower

This is a modified, reduced build of `ampere-spirv` from Eden-NS2
(https://github.com/0x2593/Eden-NS2), licensed under GPL-3.0-or-later; see `LICENSE`.
The Ampere lowering implementation incorporates behavior also present in Lichii
(https://github.com/zortos293/Lichii). Credit is due to the Lichii contributors.

Modification notice (2026-11-06): the code was adapted for the OoT Native shader
pipeline, the command-line frontend was reduced to lowering, and lowering behavior
was changed to support this project's shader formats and instruction cases. This
is more than an unchanged copy. Existing upstream copyright and SPDX notices are
retained in the source files. No game shader or game instruction data is included.

Build: `sh build.sh` (any C++20 compiler). The executable is a separate process;
it is not linked into the MIT-licensed runtime.
