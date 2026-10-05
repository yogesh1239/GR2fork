# FSR 4.1.1 for Gravity Rush 2

This build can replace Gravity Rush 2's own anti-aliasing with AMD FSR 4.1.1 (the FP8 machine-learning
model). Edges are smoother and steadier, and fine lines such as rails and leaves lose their stair steps.

FSR runs at the game's full resolution. It is anti-aliasing, not upscaling, so it does not make the
game faster. It costs a little GPU time on each frame.

## Words used here

| Word | Meaning |
|---|---|
| Anti-aliasing (AA) | Removing jagged edges and flicker. |
| Model files | AMD's FSR 4.1.1 data files. They belong to AMD and are **not** in this repository. |
| Jitter | A very small shift of the camera on each frame. FSR uses it to collect extra detail. |
| Motion vectors | A hidden picture that records how far each pixel moved since the last frame. |
| `user` folder | The folder where the emulator keeps its settings and saves. |

## What you need

1. **An AMD Radeon RX 9000 card (RDNA 4).** The FP8 model needs hardware that older cards do not have.
2. **A Vulkan driver with the FP8 features.** Tested: Linux, RX 9070 XT, Mesa 26.2.4 (RADV). Windows is
   not tested.
3. **The model files.** You must make them yourself from your own copy of AMD's FSR DLLs. See below.
4. **Output up to 3840x2160.** Larger sizes are refused.

If one of these is missing, nothing breaks: the game's own anti-aliasing runs, as before.

## Make the model files

The model files cannot be shared, so this repository does not contain them. The tool `fsr4cap` builds
them on your computer from two AMD DLLs that you already have (OptiScaler and many PC games ship them).

1. Get the tool: [`tools/fsr4cap` in yogesh1239/bloodborne_pc](https://github.com/yogesh1239/bloodborne_pc/tree/master/tools/fsr4cap).
2. Install Proton GE (the tool runs AMD's DLL through Proton) and `nix-shell`, or the tools named at the
   top of `build_assets.sh`.
3. In the `bloodborne_pc` folder, run:

   ```bash
   bash tools/fsr4cap/build_assets.sh <amd_fidelityfx_upscaler_dx12.dll 4.1.x> <amd_fidelityfx_loader_dx12.dll 2.3.x>
   ```

4. The tool writes a folder `fsr4_411` with four sets: `t1080_m0`, `t1080_m1`, `t2160_m0`, `t2160_m1`.
   Each set holds 29 `.spv` files, `dispatch.txt` and `initializer.bin`.
5. Copy the whole `fsr4_411` folder into your `user` folder:
   - a `user` folder in the folder you start the emulator from, if it exists;
   - otherwise `~/.local/share/shadPS4/` on Linux.

Output sizes up to 1920x1080 use the `t1080` sets. Larger outputs use the `t2160` sets.

## Turn it on

1. Start Gravity Rush 2.
2. Press **Insert** (or Ctrl+F10) to open the menu bar.
3. Open **Display > FSR 4.1.1**.
4. Tick **Anti-aliasing (replaces the game's)**.
5. Set **Sharpness** (0 to 100). 30 is a good start.
6. Click **Save**.

The same menu shows the status:

| Status | Meaning |
|---|---|
| waiting for the game's anti-aliasing pass | The game has not drawn a 3D frame yet. |
| off, the game's anti-aliasing runs | FSR is off, or it cannot run. The log says why. |
| running, without jitter | FSR runs. The first frame after a start or a cut has no jitter. |
| running, with jitter | FSR runs normally. |

You can also set it in `user/config.json`, in the GPU section: `"fsr411_enabled": true` and
`"fsr411_sharpness": 30`. Per-game settings work too.

## Known limits

- **A faint trail can stay at Kat's feet and scarf** when she runs. You see it only when you look closely.
- **Particles and effects** keep the game's own motion vectors.
- **The ink outline** around characters is not tracked. It is 1 pixel wide, and FSR covers it.
- **No speed gain.** On the test PC the emulator, not the GPU, limits the frame rate, so there is no
  upscaling mode.
- **RenderDoc** cannot record FSR 4.1.1: it does not support the FP8 features, so FSR stays off under it.
- **Gravity Rush Remastered** is not supported. It is a different game.
- **Never share the model files.** They belong to AMD.

## Credits

- FSR 4.1.1 is AMD's. The Vulkan replay of it and the character-motion design come from the Bloodborne
  PC port ([yogesh1239/bloodborne_pc](https://github.com/yogesh1239/bloodborne_pc)).
- The way the upscaler connects to the game follows
  [IFreemz/shadPS4-Bloodborne-DLSS](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS).
- GR2fork by junminlee2004, on top of [shadPS4](https://github.com/shadps4-emu/shadPS4).

The code is GPL-2.0-or-later, the same as shadPS4.

---

## Technical section

Developer notes, plan and test history: [`PLAN.md`](PLAN.md) and [`HANDOFF.md`](HANDOFF.md) (written
for the repository owner).

- Hook: `Rasterizer::DispatchDirect` replaces GR2's temporal AA compute dispatch (`pgm_hash 0x6f705679`)
  with `RunFsr411()`. If the guard refuses the bindings or FSR fails, the game's dispatch runs.
- Inputs: the game's colour, depth (D32S8, depth aspect) and velocity (`R16G16Sfloat`, NDC units,
  `motion_scale = (0.5 W, -0.5 H)`). Output: the AA's storage image, sRGB encoded by `fsr411_store.comp`.
- Jitter: Halton(2, 3), 8 phases, added to the viewport of scene-depth draws between the flip and the AA.
  `GR2_FSR411_INVERT_JITTER` (any value) flips the sign given to FSR.
- Character motion: GR2 draws characters forward-shaded without velocity. Motion pipelines store each
  vertex's clip position (`motion_history.h`) and write object vectors to attachment 7;
  `fsr411_motion.comp` merges them with the game's velocity and marks uncovered background (no history).
- Recording thread: the FSR passes record directly through `Scheduler::CommandBuffer()`, so the
  recording thread is synced once per FSR frame (at the AA dispatch). Per-draw additions (jitter, motion
  push constants, attachment 7) go through the deferred `Record()` closures by value.
- Timing: `BB_FSR4_PROFILE=1` prints per-pass GPU times and logs the merge pass time.
- Test sequence: the **Home** key runs 5 modes (FSR, FSR without marks, no AA, game AA, motion view),
  8 screenshots each (the log names the files). `still_test.py` measures flicker and edge strength.
- If the motion shaders change, clear `user/cache`: the shader cache keys do not change.
