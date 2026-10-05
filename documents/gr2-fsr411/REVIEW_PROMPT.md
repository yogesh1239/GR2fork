You are reviewing an FSR 4.1.1 (FP8, ML) integration in a shadPS4 fork for Gravity Rush 2
(CUSA03694). Repository: /home/yogesh/Games/GR2fork-source, branch `fsr411`.

## Rules (strict)

- Review only. Do not edit, create or delete files. Do not build, run the emulator or the game,
  and do not commit or push. A game may be running on this machine.
- Do not open or touch /home/yogesh/Games/GR2fork-linux64-2026-07-27-d7bef71 (the user's live
  install) or /home/yogesh/Games/GR2fork-FSR4-test (the test copy with saves).
- The owner is not a programmer. Start your report with a short summary in plain English: no
  jargon, short sentences, and say what each problem would mean in the game.

## What to review

All work since the branch base `4ee4e92d`:

- Commits `ee7328e6` (start), `200501fa` (Phase 1-2: FSR 4.1.1 FP8 runtime and settings),
  `b9f7617d` (Phase 3: FSR replaces the game's anti-aliasing, with jitter).
- Uncommitted work (Phase 3b): `git diff HEAD`, and these untracked files, which `git diff`
  does not show:
  `src/video_core/renderer_vulkan/motion_history.h`,
  `src/video_core/renderer_vulkan/vk_object_motion.{h,cpp}`,
  `src/video_core/host_shaders/fsr411_motion.comp`,
  `src/video_core/host_shaders/fsr411_motion_view.comp`,
  `documents/gr2-fsr411/test_motion_history.cpp`.
- Ignore `documents/gr2-fsr411/rdc` (capture notes) and `externals/vulkan-headers` (a submodule
  bump).

Read `documents/gr2-fsr411/HANDOFF.md` and `documents/gr2-fsr411/PLAN.md` first. They describe
the design, the measurements and the decisions. Lines marked "Wrong (corrected ...)" are
earlier mistakes that the current code no longer follows.

## The design in short

- `fsr411/fsr411.{h,cpp}` replays AMD's FSR 4.1.1 FP8 dispatches (extracted SPIR-V and
  constants). `vk_fsr411_pass.*` runs it at the game's AA dispatch (`RunFsr411` in
  `vk_rasterizer.cpp`), at native resolution, with a sub-pixel jitter applied through the
  viewport. Motion is the game's velocity image (previous minus current NDC, y up,
  `motion_scale` = (0.5 W, -0.5 H)). Depth is the game's depth buffer (standard depth).
- Object motion (Phase 3b): GR2's characters write no velocity. `MotionDraw` in
  `vk_pipeline_cache.cpp` selects their draws by register state. Those pipelines get extra
  SPIR-V (`emit_spirv_special.cpp`): the VS stores its clip position per vertex in a
  buffer-device-address history (`PushData::motion`, offsets 56 and 80) and loads last frame's;
  the FS writes (prevNDC - curNDC, depth, valid) to colour attachment 7 (RGBA32F, blended by
  validity). `motion_history.h` matches draws across frames.
- `fsr411_motion.comp` merges these vectors over the game's. It also marks background that a
  character uncovers this frame and gives it an off-screen vector, because FSR 4.1.1's prepass
  has no depth-based disocclusion test (HANDOFF: "Cause (2026-10-05)").
- Test-only code: the Home key test sequence (`Fsr411TestStep`, test modes 1-4), the motion view
  shader, and GPU timing behind the `BB_FSR4_PROFILE=1` environment variable.

## Focus on these risks (in this order)

1. **Regressions when FSR is off, and in other games.** `PushData` grew to 88 bytes, and every
   shader's push block now declares the motion members. Check the push constant ranges, the
   cost of pushing them, and the on-disk shader cache: the cache keys did not change, so can
   SPIR-V from an older build load with a different push block layout? Check that object
   motion stays fully off when FSR is off (`ObjectMotion::Enabled`, `SetObjectMotion`).
2. **Pipeline cache and reuse paths.** The `motion_vectors` key bit, `mrt_mask |= 0x80`,
   `write_masks[7]`, `motion_sel_`, `SnapshotRuntimeInputs`, the runtime-info fields added "in
   the padding" (`HwVertexRuntimeInfo`, `HwFragmentRuntimeInfo`), and the comment that a change
   of the flag sends the reuse path to a full refresh. Can a pipeline be reused with stale
   motion state?
3. **Vulkan correctness.** Attachment 7 with a format in the pipeline but no view in some
   render passes (`VK_EXT_dynamic_rendering_unused_attachments`), the blend state for slot 7,
   `BeginRendering` and `br_cache_` invalidation when the motion image is recreated, buffer
   device address bounds (history capacity, `over capacity`), and the atomic stores.
4. **Synchronization.** Fragment writes to attachment 7, then `PrepareRead`, then the merge
   compute pass, then FSR; the clear in `ObjectMotion::EndFrame`; the vertex-stage barrier for
   the position history across frames; the `cover` image ping-pong and the `merged` image
   (read-after-write and write-after-read across frames); the query pool timing.
5. **The merge shader.** Shared-memory tile indexing, the barriers versus the early return,
   reading the uninitialized `cover` image on the first frame after a resize (the code relies on
   FSR resetting its history on that frame: verify that it always does), and whether the
   off-screen vector (4, -4) really gives "no history" in the prepass.
6. **FSR glue.** Constants, the jitter sign, `mv_scale`, reset logic, resource lifetimes on a
   resize, and the descriptor bindings.
7. **Test-only code.** Confirm that it is inert by default and costs nothing when unused.
8. **Simplifications marked `ponytail:`.** Say whether each limit is acceptable for this
   game.

## Report format

1. A plain-English summary for the owner (5-10 sentences): overall verdict, the worst problems
   and what they would look like in the game.
2. Findings, most severe first. For each one, give:
   - severity (crash / wrong image / performance / maintainability);
   - `file:line`;
   - the defect, in one sentence;
   - a concrete failure scenario (which input or state leads to which wrong result);
   - the suggested fix;
   - your confidence: "verified in code" or "suspected", and what would confirm it.
3. The areas you checked and found correct, in one line each, so that the owner knows what
   was covered.

Do not report style preferences or formatting. Report a problem only if you can name a
concrete way it goes wrong.
