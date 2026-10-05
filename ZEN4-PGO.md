# GR2fork with the recording thread and a Zen 4 PGO build

This repository is [GR2fork](https://github.com/junminlee2004/GR2fork) (branch GR2fork-EXODUS)
with these additions:

| Addition | What it does |
|---|---|
| Recording thread (`vk_record_thread`, GPU section, **on** by default) | A second thread records the Vulkan commands, so the GPU command thread starts the next draw sooner. Gravity Rush Remastered, 2560x1440: 156.3 FPS off, 186.9 FPS on (+19.6%), identical picture. `vk_record_kick_kb` (default 8) sets how many KiB are collected before each hand-over. |
| `SHADPS4_MARCH` CMake option | Sets `-march` for x86_64 builds. Default `x86-64-v3`, as before. |
| PGO training hook | A training build (`-DSHADPS4_PGO_GEN`) writes its profile at a normal exit. |
| `upload_repeat_probe`, `upload_dedup` (off) | Measurement tools. `upload_dedup` was 10.82% slower and stays off. |

## Releases

- **Pre-release-GR2fork-EXODUS-…**: Build and Release makes these automatically after each
  push to `main`: Windows, Linux AppImage and macOS. They use the normal CPU target
  (x86-64-v3, which needs AVX2) and no PGO, so they run on most CPUs from the last ten years.
- **PGO Zen 4 …**: a Linux program built by hand with profile-guided optimisation.
  **Caveat: it runs only on AMD Zen 4 (Ryzen 7000 series) and newer CPUs.** On an older CPU
  it stops at start with an "illegal instruction" error. It does not update when GR2fork
  changes are merged.

The PGO program was trained and measured only on Gravity Rush Remastered (CUSA01130) on a
Ryzen 7 7800X3D. Street walk, same settings: 285.5-291.9 FPS, against 227.6-238.0 FPS for
the same build without PGO (+17.49% to +18.74% per draw). Other games can gain less.

## Build the PGO program

Clang and lld. The profile (`pgo.profdata`) is attached to the PGO release.

```sh
F="-march=znver4 -flto -fno-stack-protector -fprofile-use=$PWD/pgo.profdata -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date"
cmake -B build-pgo -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DSHADPS4_MARCH=znver4 -DCMAKE_C_FLAGS="$F" -DCMAKE_CXX_FLAGS="$F" \
      -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld -flto -fprofile-use=$PWD/pgo.profdata"
cmake --build build-pgo
```

To make a new profile: build with `-fprofile-generate -DSHADPS4_PGO_GEN` (no `-flto`), play
with `LLVM_PROFILE_FILE=grr-%p.profraw`, quit with Ctrl+Shift+End and confirm (a killed
process writes nothing), then `llvm-profdata merge -o pgo.profdata *.profraw`.

## Automatic GR2fork merges

`Merge GR2fork updates` runs every day at 03:17 UTC. It merges new GR2fork-EXODUS commits
into `main` and starts Build and Release, which makes a new pre-release. A conflict with the
changes above stops the merge: nothing is pushed, the run fails, GitHub sends an email, and
the run summary lists the files to merge by hand. A merge without a conflict can still break
the build; the failed Build and Release run shows it.
