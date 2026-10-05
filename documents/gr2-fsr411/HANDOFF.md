# Handoff: GR2 FSR 4.1.1, Phase 3 (job A) done

Updated 2026-10-05 (third session: jitter added, tested and committed). Read `PLAN.md` first. This file holds only the state of the work in progress.

## State

- Branch `fsr411`. Phase 2 is `200501fa`. **Phase 3 job A (FSR 4.1.1 as the game's AA, with jitter, status display, Insert/Home keys, test sequence) is committed** in the commit after it (see `git log`). The user tested it in play and said it looks good (2026-10-05).
- `build/shadps4` is the Phase 3 build with the jitter. The test launcher `GR2fork-FSR4-test/start-gr2-test.sh` runs it straight from `build/`. The live install is untouched.
- Test copy config: `fsr411_enabled` is now `true` (it was `false`), so the user's test starts on the verified path (self-test at start-up). The emulator itself bumped `config_version`.
- The FP8 extensions are enabled whenever the GPU has them (`vk_instance.cpp:367-376`), not only when FSR is on at start, so a mid-game switch-on also works (the upscaler is created lazily; expect a short freeze).
- The test save data did not change. Only the 3 `param.sfo` files got a new date, and one in-game photo appeared in `user/screenshots/`.

## What was built (Phase 3 job A)

- Hook in `Rasterizer::DispatchDirect`: when `cs.pgm_hash == 0x6f705679`, FSR is on and `RunFsr411()` succeeds, the game's `BindResources`/bind/dispatch is skipped. `IncDispatch`, `ResetBindings(true)` and the interval-flush tail run in both cases.
  - Hash confirmed: the name `0x6f705679` is embedded in the captured AA SPIR-V (`spv_ev15087_cs.spv`, previous session scratchpad).
  - The AA's only buffer (`ssbo_1`) is `NonWritable` (constants), so skipping the dispatch loses no side effect. It writes only `cs_img4`.
  - The AA shader encodes sRGB itself before `imageStore`, and its alpha is the centre sample alpha of `cs_img0`. `fsr411_store.comp` does the same.
- `RunFsr411()` (vk_rasterizer.cpp, after `DispatchDirect`):
  - Needs 5 bindings and 5 `image_infos`. Reuses the views and layouts the bind resolved (`image_infos[0]` color, `[2]` velocity, `[4]` output storage UNORM view). Only depth gets a new view: `FindViewHandle` with `eR32Sfloat`, which gives a depth-aspect view of the D32S8 image.
  - Guard: 0 not depth, 1 depth, 2 `eR16G16Sfloat`, 4 storage, all sizes equal. Otherwise it warns once and the game's AA runs.
  - Logs the 5 bindings for the first 2 FSR frames.
  - `motion_scale = (0.5*W, -0.5*H)`, sharpness from the setting (jitter: see below).
  - Reset on the first frame, or when more than 4 flips passed since the last FSR frame (now counted on the GPU thread, see Jitter). The first 50 resets are logged.
- `Fsr411Pass::RecordAa()`: lazily creates an RGBA16F `upscaled` image (on a size change: `scheduler.Finish()` first, because the runtime also frees its size-bound resources without a wait), a compute-to-compute barrier, `Record()`, then the store dispatch (push descriptors, immutable nearest sampler).
- `Fsr411Pass::Record()` bumps `BumpForeignPipelineGen(1)` and `BumpForeignPushGen(1)` before the runtime records, so a failure halfway still invalidates the dedup caches.
- `~Fsr411Pass` waits for `ticks.back()`. Errors are logged once per distinct message.
- `Fsr411::Image::layout` (default General) is used for `r_input_color`, `r_velocity`, `r_depth`, `r_debug_visualization`, `r_result_color`.

## Jitter (built and tested 2026-10-05)

- Halton(2, 3), 8 phases (bbport `JitterPhases` at 1:1), `Fsr411Jitter(index)` in `vk_rasterizer.cpp`; index 0 = none.
- Frame boundary: `Presenter::PrepareFrame` calls `Rasterizer::Fsr411Flip()` (`++fsr411_flips`). It runs on the GPU thread in stream order (EOP flips from the GfxFlip IRQ, other flips through `SendCommand`). `DebugState.GetFrameNum()` is not used any more: the presenter thread advances it, out of order with the draws.
- `SelectDrawJitter(full_screen)` runs on every draw before the dyn state (Draw: after `BindResources`, before the glue dyn term; DrawIndirect: `full_screen = false`). A draw is jittered when `db_desc.first == fsr411_depth`, `1 <= fsr411_flips <= 4` (after a flip, before the AA), not clip-disabled, and on Draw not `num_indices <= 6 && instances <= 1`. So UI after the AA, shadow maps and full-screen passes stay still.
- The value changes only at a successful `RunFsr411` (`fsr411_jitter_index % 8 + 1`), so every pass of one frame has the same offset (the G-buffer depth test is EQUAL against the prepass).
- Viewport: `x += jitter[0]`, `y += jitter[1]` in the non-clip-disabled branch of `UpdateViewportScissorState`. Push viewport floats are unchanged: only clip-disabled shaders read them, so `push_vp_memo` needs no change.
- Skip-caches: `DynStateFlags(is_indexed)` puts the jitter index into bits 3+ of the DynState memo `flags`, used by both `UpdateDynamicState` and the draw_glue dyn term.
- `Frame.jitter` = the jitter the scene drew with, or 0 when no draw took it (first FSR frame). Sign +1, as bbport's FSR4 default. The environment variable `GR2_FSR411_INVERT_JITTER` (any value) flips only the sign given to FSR.
- `DispatchDirect`: when the AA dispatch takes the game path (FSR off, guard refusal, failure), `fsr411_depth` is cleared, so the next frame has no jitter. At most one jittered frame goes through the game AA after a toggle.
- Menu key: Insert now also opens the devtools menu bar (Ctrl+F10 still works; Insert is ignored while a text field is typed in), `core/devtools/layer.cpp`.
- Status display: `DebugState.fsr411_frames` / `fsr411_state` (relaxed atomics, set in `DispatchDirect` at each AA dispatch), text from `DebugState.Fsr411StateText()`: waiting for the game's AA pass / off, the game's AA runs / running, without jitter / running, with jitter. Shown in the menu (Insert or Ctrl+F10 > Display > FSR 4.1.1, under the checkbox, with the FSR frame count) and in the "Video debug info" window ("FSR 4.1.1: ..."). That window's old "FSR: off" line is the presenter's FSR 1 filter; it now reads "FSR 1 upscale filter: off". The user read that old line as FSR 4.1.1 being off (2026-10-05).
- Test sequence (Home key, `Rasterizer::Fsr411TestStep`, called at each AA dispatch): 4 modes in a row (`fsr411_test_mode_`: 0 normal, 1 FSR jitter sign reversed, 2 FSR without jitter, 3 the game's AA), each 90 AA passes to settle, then 8 game-only screenshots (`VideoCore::RequestScreenshot(GameOnly)`, consumed by the same frame's `PrepareFrame`, 2560x1440 before host scaling). Log: "FSR 4.1.1 test: mode M shot K", then "Saved screenshot: ..." lines, and "test: done". Then mode 0 again. RenderDoc cannot replace it: RenderDoc 1.45 does not expose VK_KHR_cooperative_matrix, VK_EXT_shader_float8 or VK_VALVE_shader_mixed_float_dot_product (checked with `strings librenderdoc.so`), so FSR 4.1.1 and the jitter are off under RenderDoc.
- History reset: now `fsr411_runs == 0 || fsr411_flips > 4` (GPU-thread flips). Log text: "history reset N after M flips".
- Frame-boundary order, checked in code: liverpool signals `GfxFlip` while it parses the `PatchedFlip` NOP in the graphics PM4 stream (`liverpool.cpp:719`), and the EOP-flip handler calls `SubmitFlip` -> `PrepareFrame` from there. `mv_jitter_cancellation` stays 0 (`mlsr{}`), which is right: GR2's velocity is not jittered.
- Check in the log: "FSR 4.1.1: frame N jitter (x, y), J of D scene-depth draws jittered, depth image I, F flips" for FSR frames 0-3 and every 3600th. Expected: from frame 1, J > 0, J a little lower than D, F = 1.
  - D = 0: the draw depth id differs from the AA's binding 1 id (then match by guest address instead).
  - J = 0 with D > 0: the ids match, but the flip window was shut during the scene (F = 0 or F > 4, so the flip order assumption is wrong) or the shape filter refused every draw.
  - J close to D with many full-screen passes on the depth: the selection is too wide.
- **The reset counter changed, so the smoke-test reset results below no longer prove it.** `PrepareFrame` counts every guest flip, also repeats of the same buffer, and does not count blank (index -1) flips; the old presenter counter skipped reused frames. Re-check on the next run: about 5-7 resets per 180 s run, all at loads, none in continuous play, and sane "after M flips" values. Too many flips in play would also switch the jitter off (window 1-4).
- What to look for: still edges smoother than before, no shimmer on a still camera (a wrong sign shows as wobbling edges: try `GR2_FSR411_INVERT_JITTER=1`), no flicker in shadows, no holes or lines on geometry (would mean the offset changed in the middle of a frame), the UI and the HUD still sharp and still.

## Still-camera test results (2026-10-05, test copy, city square, 2560x1440, sharpness 30)

Run with the Home key; measured with `still_test.py` (static edge pixels: 176,256 of 3,686,400; luma 0-1). Crops in `GR2fork-FSR4-test/still-test-2026-10-05/` (`crops.png`: columns jitter / reversed / no jitter / game AA; `rail_zoom.png`: rows in the same order).

| Mode | Flicker mean | Flicker p95 | Edge strength |
|---|---|---|---|
| FSR, jitter (sign +1) | 0.00167 | 0.00463 | 0.14381 |
| FSR, jitter sign reversed | 0.00232 | 0.00600 | 0.13268 |
| FSR, no jitter | 0.00063 | 0.00238 | 0.17756 |
| Game AA | 0.00047 | 0.00222 | 0.15111 |

- **Sign +1 is right.** The reversed sign flickers 39% more and its edges are 8% weaker; in the zoom the rails are doubled and soft.
- With the jitter, sloping rails and leaves are smooth. Without it (and with the game AA) they show stair steps.
- The jitter adds flicker, but the mean is 0.43 of one 8-bit step and the p95 is 1.2 steps: not visible.

- Moving camera (second run, camera orbiting Kat; each mode is a different view, so the flicker and edge numbers do not apply): no corruption, rails stay smooth in motion, no visible smear on the background. Kat's hair silhouette over 3 consecutive frames (brightened zoom) shows no trail; she stays at the same screen place while the camera orbits, so her (0, 0) motion is nearly right there. Trails are still expected when characters move across the screen (Phase 3b). Pictures: `moving_*.png` in the same folder.

- Kat running, camera following (third run, about 140 FPS): FSR shows no visible trail on her legs, feet or hair over 3 consecutive frames; the game AA shows stair steps on her ankle bands, FSR does not. Limit: at 140 FPS a limb moves only a few pixels per frame, so a trail from her (0, 0) motion stays short; it grows at lower frame rates. No walking NPCs were in view. Pictures: `running_*.png`. Note: in this run the 32 screenshots were saved about 1 s after the sequence; `still_test.py` must run after all "Saved screenshot" lines are in the log.

## Smoke test results (by Claude, 2026-10-05, test copy)

- 3 runs of 180 s, virtual Cross presses (`documents/gr2-fsr411/press_loop.py n 5 24` after 45 s) to reach gameplay.
- Log: the self-test passed. The 5 bindings on two frames: color image 44 (same both frames), depth image 4 `D32SfloatS8Uint` in `DepthStencilReadOnlyOptimal`, velocity image 11 `R16G16Sfloat`, history/output images 46/47 swap each frame, output storage in General. No errors, no crash.
- Resets: 5 to 7 per run, all at large flip gaps (loads/menus). None during continuous gameplay, so GR2 flips at most 4 times per AA dispatch.
- Screenshots (`spectacle -b -n -f`), FSR on vs off, same area (Kat on the wooden deck), not the same frame: no corruption. Sky mean RGB (202,203,175) on vs (201,203,174) off. Floor mean red 82 on vs 90 off in every shot, but the camera angle differed. Colour is not confirmed equal.
- MangoHud was missing from all FSR-on gameplay shots of run 3, but present in a later FSR-on run (title scene, FSR running, 209 FPS, 4.8 ms) and in all FSR-off shots. Its log lines are identical. Cause unknown; FSR does not hide it.
- User report (2026-10-05): in one FSR-on run the camera did not follow Raven during gravity shift and traversal. In a later FSR-on run (same build, same setting) it followed. So FSR is not the cause; the cause is unknown. Not yet checked in the live build. The game boots straight into Raven's mission ("Red and Black"), not a title demo.
- Not checked yet: ghosting, smearing of characters, edge quality in motion, speed. These need the user's eyes.

## Next steps, in order

1. Done 2026-10-05: jitter run, log check (J > 0, F = 1), still and moving tests above. Still open from it: the reset counter re-check (7 resets in about 48 min of play, one after only 6 flips; no reset during continuous play seen) and a colour/brightness comparison on the same frame.
2. Camera-cut reset (not done; only the flip-gap rule exists).
3. Phase 3b character motion vectors (required), Phase 4 upscaling, Phase 5 AppImage: see `PLAN.md`.

## Rules (unchanged)

- Test only in `GR2fork-FSR4-test`. Do not commit `fsr4_411*`, PS4 modules, saves or game files.
- Commit only when the user asks. Author `yogesh <77014257+yogesh1239@users.noreply.github.com>` via `git -c user.name=... -c user.email=...`.
- Do not delete captures (35.5 GB in `GR2fork-FSR4-test/user/captures/`) without the user's permission.
- Explain to the user in plain STE100 English, technical part last.
