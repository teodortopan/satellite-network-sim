# SGP4 (David Vallado's reference implementation)

`SGP4.cpp` and `SGP4.h` are David Vallado's C++ implementation of the SGP4 orbit propagator,
"SGP4 Version 2020-07-13" (the version string in `SGP4.h`), as distributed in the C extension
of python-sgp4. `LICENSE` is python-sgp4's licence file.

## Origin

- Repository: https://github.com/brandon-rhodes/python-sgp4
- Release tag: `2.27` (annotated tag object `092d206d6e6556a3cbbbe86f6ba4293c1f4ec646`)
- Commit: `6e7428df7acc48e8176828289a8c14486a114759`
- Fetched from `https://raw.githubusercontent.com/brandon-rhodes/python-sgp4/<commit>/<path>`:

| File here | Path in the repository | sha256 |
|---|---|---|
| `SGP4.cpp` | `extension/SGP4.cpp` | `2ee7ad0e8f201e8251894083fe21e33a7aace2f43c871bf04357eb44a891b06e` |
| `SGP4.h` | `extension/SGP4.h` | `2a5ec44e059a52b3173d78d9a28bda8142b4f6497c1eb16febc2cea4b5006b0c` |
| `LICENSE` | `LICENSE` | `4474ea9eccbb829c6bc3652381bfe78b1c815de021d4a284a2a5b383a374d030` |

The files are unmodified, byte for byte, including their CRLF line endings
(`.gitattributes` here stops Git from converting them). Check with
`shasum -a 256 third_party/sgp4/SGP4.cpp third_party/sgp4/SGP4.h third_party/sgp4/LICENSE`.

## Licence

python-sgp4 is MIT licensed, copyright Brandon Rhodes (`LICENSE`). Vallado's two source files
themselves carry no licence text. The same code published elsewhere under AGPL-3.0 (CelesTrak's
GitHub copy) must not be used here; see `docs/phase-1-orbits.md`, section 5.

## How it is built and used

- Built as the static library `satsim::sgp4` by `satsim_add_third_party_library` in
  `cmake/SatsimTargets.cmake`: with the sanitizers, without satsim's warning flags. The
  directory is outside `include src tests`, so `clang-format` and `clang-tidy` do not check it,
  and its include directory is a SYSTEM one, so warnings from `SGP4.h` are not reported in our
  code. GCC's default warnings do show `-Wformat` notes for its TLE reader's `sscanf` calls.
- Only `src/orbit/propagator.cpp` includes `SGP4.h`; no Vallado type appears in a public
  header. It calls `sgp4init` and `sgp4` only.
- Known issue, not patched: `twoline2rv` (Vallado's TLE reader) passes `satrec.satnum` to
  `sgp4init`, which `strcpy`s it onto itself. That overlapping copy is undefined behaviour and
  AddressSanitizer stops on it. Our code never calls `twoline2rv`.
- `satnum` is `char[6]`: never pass a catalogue number longer than five characters to
  `sgp4init`. The propagator passes a fixed placeholder.
- The verification cases and expected output are in `tests/orbit/data/` (see the README
  there).
