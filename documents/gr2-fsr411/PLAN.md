# Plan: FSR 4.1.1 FP8 in the GR2 fork

Written 2026-10-05.
Source code: `/home/yogesh/Games/GR2fork-source`. Work branch `fsr411`, started from `GR2fork-EXODUS` at version `4ee4e92`.
This file lives in the source folder at `documents/gr2-fsr411/PLAN.md`.
Status: Phase 0 is done (see the result under Phase 0). Phase 1 is next.

---

## Words used in this plan

| Word | Meaning |
|---|---|
| Source code | The human-written text that a program is made from. |
| Build | The step that turns source code into a program you can run (an AppImage). |
| Branch | A separate line of work inside one project. |
| Render resolution | The size at which the game draws its 3D picture. |
| Output resolution | The size of the picture on your screen. You use 2560x1440. |
| Upscaling | Making a small picture larger and filling in detail. |
| Anti-aliasing | Removing jagged edges and flicker. |
| Depth | A hidden picture that records how far away each pixel is. |
| Motion vectors | A hidden picture that records how far each pixel moved since the last frame. |
| Jitter | A very small shift of the camera on each frame. FSR 4 uses it to collect extra detail. |
| Scene picture | The 3D picture before the game adds glow, final colours, and menus. |
| UI | The menus, text, and on-screen icons. |
| Smoothing pass | The game's own step that blends the current frame with older frames. |
| Patch | A GoldHEN patch: a small change to the game's code when it loads. You already use some. |
| RenderDoc | A free tool that records one frame of a game, with all of its hidden pictures. |
| Capture | One frame recorded by RenderDoc. |
| Model files | AMD's FSR 4.1.1 data files. They belong to AMD. |
| FP8 | The fast type of math that RX 9000 cards do in hardware. |
| GPU-bound / CPU-bound | The graphics card is the slow part / the processor is the slow part. |

---

## Short summary

**The goal:** add FSR 4.1.1 FP8 to the newest GR2 fork. Gravity Rush 2 then gets cleaner and more stable edges. Maybe it also runs faster.

**What makes this easier than it looks:**

1. **The FSR 4.1.1 FP8 code already exists.** Your Bloodborne port has it. It is about 950 lines, and I can copy it with no changes.
2. **Your model files are ready.** `Bloodborne PC/fsr4_411_fp8` has all 4 sets, and they are FP8 sets. The copy in `bbport-pr/fsr4_411` is the same, byte for byte.
3. **Your graphics card is ready.** I checked your RX 9070 XT with Mesa 26.2.4. It has all 9 features that FSR 4.1.1 FP8 needs.
4. **The two projects share a base.** The newest GR2 fork and your Bloodborne port both build on the same shadPS4 version (`b4e7ae7`, 2026-10-02).
5. **Gravity Rush 2 very likely makes its own motion vectors.** The game uses them for its motion blur. They cover the camera and moving objects. In the Bloodborne port, making motion vectors was the hardest part. Here the game may give them to us.
6. **The game already has resolution patches.** They tell the game which size to draw at, from 544p to 8K. You use the 1440p patch now.

**What makes it hard:**

1. **Both projects changed the main drawing code a lot.** Each changed more than 2,000 lines of one important file, in different ways. So I cannot copy the "connection" code. I must write it again by hand.
2. **We do not know the exact form of the game's motion vectors.** A RenderDoc capture must show us.
3. **The game probably does not use jitter.** We must add it.
4. **The game's own smoothing pass must be switched off** when FSR runs. If both run, the picture gets smeared twice.

---

## The most important question: speed or picture quality?

FSR 4.1.1 can do two different jobs:

- **Job A, anti-aliasing:** the game draws at full size (2560x1440), and FSR cleans up the edges. The picture gets better. The speed stays about the same. It is a little slower, because FSR needs some time on each frame.
- **Job B, upscaling:** the game draws a smaller picture (for example 1707x960), and FSR makes it 2560x1440. The game gets faster, **but only if the graphics card is the slow part.**

Emulators like shadPS4 are usually limited by the processor, not by the graphics card. The guide for your GR2 fork points the same way: "disable E cores", and "use an SSD or you will crash". If the processor is the slow part, job B gives almost no extra speed.

**You want both jobs (decided 2026-10-05).** The plan does job A first, then job B. Job B lets you choose a smaller drawing size, for example when you want a higher output size or a steadier frame rate. How much speed it gives depends on whether the graphics card is the slow part. The MangoHud measurement shows that, but it no longer decides whether job B happens.

You can make this measurement today with your current AppImage. No build is needed. MangoHud already runs on your computer. Look at the GPU load (the percentage) in a busy place in the city. If the GPU load stays below about 90% while the frame rate falls below your target, the processor is the slow part.

---

## Who tests the picture

I do most of the test rounds myself. I checked on 2026-10-05 that this computer allows it:

1. I can start the test build myself.
2. I can control the game with a virtual gamepad. Your account can create one (`/dev/uinput` is writable, and `python-evdev` is installed).
3. I can make the emulator save frames as image files: FSR's input, its output, and the motion vectors.
4. I can look at these images and compare FSR on and FSR off. I can also measure flicker between frames in a row.

Limits:

- I see the frames as still images, one at a time. You see smooth motion at 144 frames per second. You notice trails and shimmer in motion better than I do.
- While I test, the game uses your screen. Do not use the mouse or keyboard during that time. The game must stay the active window to get the gamepad input.

So you test only at the end of each phase, not in every round.

---

## A second model: the Bloodborne DLSS project (checked 2026-10-05)

`github.com/IFreemz/shadPS4-Bloodborne-DLSS` adds NVIDIA DLSS to Bloodborne in shadPS4. DLSS does not work on your AMD card or on Linux. But the way it connects an upscaler to the game is a better model for this plan than your Bloodborne port:

1. **It is small.** It is one change of about 2,500 lines, including its menu. Your Bloodborne port's upscaler code is several times larger.
2. **It is close to the GR2 fork.** It builds on shadPS4 0.19.0, and the newest GR2 fork already contains 0.19.0.
3. **It uses the game's own resolution patch for the render size.** The upscaler then makes the picture as large as the window. This removes most of the hard work in job B.
4. **It keeps the menus with a simple trick.** It compares the frame with menus and the frame without menus. The difference is the menus. It adds that difference on top of the upscaled picture. Menus are a little softer than true full-size menus, but this is much simpler.
5. **It redraws the game's small motion vectors at full size.** Bloodborne makes its motion vectors at only 160x90. The project draws them again at full size. Gravity Rush 2 may need the same trick.
6. **It warns about one patch.** In Bloodborne, the patch that disables motion blur also removes the motion data. **Your "Disable motion blur" patch for Gravity Rush 2 may do the same.** Phase 1 tests this.

The plan now uses this project as the model for the connection code. It uses your Bloodborne port only for the FSR 4.1.1 code and model files.

The DLSS project is 1 day old and has 1 change. Nobody has reported on it yet, so its quality is not proven.

---

## About "geometry rendering" (your point)

You are right: on PS4 Pro, the game draws the shapes of objects at full 4K but works out their colours at a lower resolution. Digital Foundry reported this.

What I found about it:

1. **The Pro version of this method probably needs a special PS4 Pro chip feature (the "ID buffer").** The emulator does not copy that feature. I found no report of anybody who ran Gravity Rush 2 in PS4 Pro mode in shadPS4. You use normal PS4 mode now.
2. **The game has a step that may turn a small picture into a larger one.** Its settings include an input size, an output size, the motion vectors, and an older frame. It may also run in normal PS4 mode. I do not know yet.
3. **The plan checks this step in Phase 1 (check 5).** If it runs with a smaller input in normal mode, FSR can replace only that step. That is a large shortcut for job B. The plan does not depend on it.

---

## The phases

Each phase lists what I do, what you do, how we know it is done, and a time estimate. The times are rough. They depend most on what the captures in Phase 1 show.

### Phase 0: A safe test copy and a first build (no FSR yet)

**What I do:**

1. I update one small part of the source code, the "Vulkan headers". FSR 4.1.1 needs a graphics feature that the current copy does not know about. I make this change first, so any problem shows up before I add FSR code.
2. I build the newest GR2 fork with no other changes, and make an AppImage.
3. I make a test folder: `/home/yogesh/Games/GR2fork-FSR4-test`. It gets a **copy** of your `user` folder: system modules, patches, game settings, and saves.

**Safety of your files:** the new build keeps its settings and saves in a folder named `user`, in the folder it starts from. If that folder does not exist, it uses `~/.local/share/shadPS4` instead. Your other shadPS4 programs share that folder. So I always start the test build from the test folder, which has its own copy. **I do not use your live GR2fork folder.** The new build also converts settings to a newer format. That conversion happens only in the copy.

**What you do:**

1. Measure the GPU load with MangoHud, as described above. You can do this today.
2. Play Gravity Rush 2 with the test build for 10 to 15 minutes. Tell me if it runs as well as your current build.

**Done when:** the test build starts the game and runs about as well as your current build.

**Decision point:** if the newest version runs worse than your current build, we choose: fix the problem, or use the older branch (`gr2fork-legacy`). The older branch needs more changes by hand.

**Time (my work):** 30 to 60 minutes. The new compiler on your computer (clang 23) is newer than the one the project uses (clang 19), so some small build errors are possible.

**Result (2026-10-05):** the build worked with no errors on the `fsr411` branch, with the Vulkan headers at 1.4.357. The game ran. The user measured about 200 fps at 1440p, GPU load 70%, GPU power 100 W, and CPU load 33% across all 16 threads. The graphics card has spare capacity, so upscaling at 1440p gives only a small speed gain.

### Phase 1: Find the game's hidden pictures (RenderDoc)

**What I do:**

1. I install RenderDoc. This needs an admin command (`pkexec pacman -S --noconfirm renderdoc`). **I will ask you before I run it.**
2. I turn on the emulator's RenderDoc support in the test copy's settings only.
3. I study your captures myself with RenderDoc's scripting tools. You do not need to open RenderDoc.

**What you do:** press the capture key at 3 places. Press it 2 times in a row at each place, because two frames next to each other show whether the game already jitters. Use your normal patches, including the 1440p patch.

1. Standing still in the city.
2. Kat running or falling while the camera turns.
3. A place where the on-screen icons or a menu are visible.

**What I check, in order of importance:**

1. **Motion vectors.** Do they exist when your "Disable motion blur" patch is on? What form are they in? Are they full size, or smaller than the screen?
2. **Depth.** Which direction does it go (near = 0 or near = 1)? The model files expect one direction. The other direction needs one extra step.
3. **The game's own smoothing pass.** Which step is it, so I can switch it off when FSR runs?
4. **The outline pass.** Gravity Rush 2 draws black outlines. Where does that step sit compared with the place where FSR goes in?
5. **The resize step (the geometry rendering shortcut).** In normal PS4 mode, does it ever take a smaller input than its output?
6. **Jitter.** Does the camera move by a tiny amount between the two frames?
7. **The scene picture.** Which format does it use, and where do the menus start?

**Done when:** I know which hidden picture is which, and the form of each one.

**Decision point:** if the game's motion vectors are missing or unusable, I must make my own, as in the Bloodborne port. That adds about 1 to 2 weeks.

**Time (my work):** 30 to 90 minutes, plus your 15 minutes of play.

**Result so far (2026-10-05, one gameplay recording: Kat running across a bridge):**

1. **Motion vectors: found, full size, usable.** The game writes them at 2560x1440. It does this even with the "Disable motion blur" patch on, so you can keep that patch.
2. **Depth: found.** It goes in the direction that the model files expect. No extra step is needed.
3. **The game's own smoothing pass: found.** It is an edge smoother (FXAA) plus a blend with the previous frame. FSR replaces this one step.
4. **The step after it** copies the finished picture to the screen buffer. Menus would be drawn after that.
5. **One weakness:** characters (Kat and the people in the city) have no motion vectors of their own. The game gives them "no motion". Kat moves with the camera, so that is mostly right for her. People who walk across the screen would leave short trails with FSR. **You decided this must be fixed (2026-10-05).** Phase 3b does it.

6. **Jitter: the game does not jitter.** I read the camera numbers in 6 recordings. In every one, the camera is centred to within 0.0001 of a pixel. The game's smoothing step also has no jitter correction. So I add jitter myself in Phase 2.

**Phase 1 is done (2026-10-05).** The third recording session (02:08 to 02:13) gave 32 frames. I pressed F12 every 5 seconds with a virtual keyboard while you played. They show:

- the pause menu;
- the health bar, the gravity gauge, mission text, subtitles and button hints (the UI, for job B);
- people who move near Kat (for Phase 3b);
- Kat falling and flying fast (large motion);
- a comic panel and loading screens.

### Phase 2: General connection work (no game knowledge needed)

I can do this phase at the same time as Phase 1.

**What I do:**

1. I copy the FSR 4.1.1 code from the Bloodborne port, with no changes.
2. I turn on the graphics card features that FSR 4.1.1 FP8 needs.
3. I add settings: upscaler (Off / FSR 1 / FSR 4.1.1), sharpness, and the folder for the model files.
4. I add these settings to the in-game menu (Ctrl+F10, Display menu) and to the Big Picture settings screen.
5. I copy the model files into the test folder. **This is a copy. The originals stay where they are.**
6. I add the jitter machinery. It stays off for now.
7. I make the program go back to FSR 1 if the model files or a graphics feature are missing.

**What you do:** nothing in this phase, except a short test at the end.

**Done when:** the build works, the game runs exactly as before when the setting is Off, and the log file says that FSR 4.1.1 FP8 loaded.

**Time (my work):** 1 to 3 hours.

**Status (2026-10-05): built and tested by me. Only your short test is left.**

- The program builds.
- With FSR 4.1.1 on, the start-up check passed: "FSR 4.1.1: t2160_m0 output 2560x1440 fp8", then "self-test at 2560x1440: passed". The game then ran for 26 seconds with no errors.
- FSR does not change the picture yet. Phase 3 connects it to the game.

1. Done: I copied the FSR 4.1.1 code from the Bloodborne port with no changes.
2. Done: the program now turns on the graphics card features that FSR 4.1.1 FP8 needs, when the card has them.
3. Done: two new settings, "FSR 4.1.1 anti-aliasing" (on/off) and "Sharpness" (0 to 100). Both are in the in-game menu (Ctrl+F10, Display, FSR 4.1.1). You can switch FSR on and off while you play, to compare.
4. Done: I copied the model files into the test folder (2.8 MB). The originals stay where they are.
5. Done: a start-up check. When the setting is on, the program runs FSR 4.1.1 one time on empty pictures at the window size. The log then says "FSR 4.1.1 self-test at 2560x1440: passed" or "failed". If it fails, FSR stays off and the game keeps its own smoothing.
6. Moved to Phase 3: the jitter. It must shift exactly the same drawing steps that FSR later reads, so I write it together with the connection in Phase 3.
7. Not done (not needed now): the Big Picture settings screen. The Ctrl+F10 menu is enough for testing.
8. Before the first new build, I kept a copy of the Phase 0 program (`build/shadps4.phase0`). With it, we can compare the new program with the old one.

### Phase 3: Job A, FSR 4.1.1 as anti-aliasing at full size

**What I do:**

1. I connect FSR 4.1.1 at the place found in Phase 1. It gets the scene picture, the depth, the motion vectors, and the jitter.
2. I write a small converter that changes the game's motion vectors into the form FSR needs.
3. I put FSR's result back into the game's picture. The game then adds its glow, final colours, and menus as usual.
4. I switch off the game's own smoothing pass while FSR runs.
5. I make FSR forget old frames after a loading screen or a camera cut, so no "ghost" of the old scene shows.

**What you do:** play with FSR 4.1.1 on and off. Compare screenshots. Tell me about smeared trails behind moving things, flicker, shimmer, or crashes.

**Done when:** edges look cleaner and more stable than with FSR off, without obvious trails, and the speed is about the same.

**Time (my work):** 2 to 6 hours. Add 3 to 8 hours if I must make my own motion vectors.

**Status (2026-10-05): the connection works, without jitter. Your test is next.**

- FSR 4.1.1 now replaces the game's smoothing step when the setting is on. The game adds its glow, menus and text after it, as before.
- Step 2 was not needed: the game's motion vectors already have a form that FSR takes with one scale number.
- My own short test: no crash, no errors, and no broken picture. The sky brightness matches FSR off. I did not compare the same frame, so small colour changes are still possible.
- Not done yet: the jitter (so edges stay jagged for now), and the reset on camera cuts. FSR forgets old frames only after loading screens and menus.
- Kat and the people in the city will smear until Phase 3b.

### Phase 3b: Motion vectors for characters (required)

The game draws Kat and the people in the city in a separate step, and that step makes no motion vectors. So I make them myself.

**How it works:**

1. On each frame, the emulator saves where every corner point of each character was drawn on screen.
2. On the next frame, it draws each character once more into a hidden picture. For each pixel it writes "where this point was last frame, minus where it is now". That is exactly a motion vector.
3. This covers walking, running, arm and leg movement, and hair and cloth, because it uses the final positions of the points after all animation.
4. I put these character motion vectors on top of the game's own motion vectors. Then I give the combined picture to FSR.

Your Bloodborne port already does the same thing for Bloodborne's characters. I reuse its method, but I must adapt it to Gravity Rush 2 and to the newer emulator code.

**What you do:** test scenes with people walking across the screen and Kat running past the camera. Tell me if trails remain.

**Done when:** people walking across the screen leave no visible trails with FSR on.

**Time (my work):** 4 to 10 hours.

**Risks:**

- Characters that appear or disappear (for example after a camera cut) have no "last frame". For one frame they get no motion. FSR handles single frames like this well.
- Objects drawn in an unusual way (for example particles or some effects) may still have no motion. I check this with recordings after the fix.

### Phase 4: Job B, real upscaling

**Dropped (user, 2026-10-05):** the emulator, not the PC, limits the frame rate, so a smaller game picture gives no speed. Kept here only for reference.

**What I do:**

1. I make the game draw at a smaller size with the existing resolution patch. For example, the 1080p patch for "Quality" at 1440p output. Other sizes need new numbers in the patch.
2. FSR makes the picture as large as the window, before the menus are drawn.
3. I add the menus back with the "difference" trick from the DLSS project. The menus become a little softer. Fully sharp menus (the Bloodborne port's method) are possible later, but they take much more work.
4. If check 5 in Phase 1 found the shortcut, I compare it with steps 1 to 3 and use the simpler one.

**What you do:** test as in Phase 3. Also compare the frame rate.

**Done when:** the game runs faster at the same output size, and the menus stay sharp.

**Time (my work):** 2 to 5 hours with the DLSS project's method.

### Phase 5: Finish

1. Settings are saved correctly per game.
2. I measure how much time FSR needs per frame.
3. I make an AppImage **without** the model files.
4. I write a short guide on how to put the model files in place.

**Time (my work):** about 1 hour.

---

## What this plan will not fix

- **Crashes and other emulation problems.** FSR changes only the picture.
- **Frame rate, if the processor is the slow part.** Job B helps only when the graphics card is the slow part.
- **Other graphics cards.** These FP8 model files work only on RX 9000 cards. Other cards need different model files.
- **Gravity Rush Remastered.** It is a different game with different hidden pictures. It would need its own Phase 1 and Phase 3.
- **Sharing.** You cannot share a build that contains AMD's model files. The main shadPS4 project would also not accept this work, because its rules forbid code for one game. The GR2 fork already has code just for Gravity Rush 2, so its maintainer may accept it. That is the maintainer's choice.

---

## What you must do (all phases)

1. Measure the GPU load with MangoHud. You can do this today.
2. Allow the RenderDoc installation (an admin command) when I ask.
3. Play the test builds and press the capture key at the places I name.
4. Tell me about problems you see: trails, flicker, shimmer, crashes.
5. Keep the model files private.

---

## Things I am not sure about

- **The motion vectors may be smaller than the screen.** Motion blur often uses a half-size buffer. FSR would then be less sharp on moving edges.
- **Kat's arms and legs may not have motion vectors.** I found only one list of bone positions in the game's shaders, not a "previous frame" list. If so, her limbs may leave short trails when she moves fast.
- **The added jitter may make some effects shimmer,** for example shadows, ambient shading, or clouds.
- **The newest GR2 fork may run the game differently from your current build.** It has 817 changes your build does not have.
- **There may be no speed gain at all.** See "speed or picture quality" above.

---

## Technical section (you can skip this)

### Findings this plan builds on

- **bbport runtime:** `bbport-pr/gpu/shadps4/video_core/renderer_vulkan/fsr411/fsr411.{h,cpp}` (61 + 890 lines) is plain Vulkan + C++ standard library, with no shadPS4 types.
  - Interface: `Fsr411::Upscaler(physical, device, dir)`, `Record(Frame)`.
  - `Frame` inputs: color RGBA16F, depth D32 (depth-aspect view), motion RG16F (render pixels), output; all in `VK_IMAGE_LAYOUT_GENERAL`. Plus jitter, `motion_scale`, `pre_exposure`, sharpness, reset, auto exposure.
  - Caller waits for the frame `kFramesInFlight = 8` frames back (constant-buffer ring).
- **Assets:** `t{1080,2160}_m{0,1}`. Each set has 29 `.spv` files, `initializer.bin` (131072 bytes, size checked) and `dispatch.txt` (`fp8 1`, `scratch 83232256`, per-pass group divisors).
  - Set selection (fsr411.cpp:670-683): the t2160 tier is used when the output is wider than 1920 or taller than 1080; `_m1` only for ultra performance (scale 3.0). Constraints: render ≤ output ≤ 3840x2160.
  - Assets were captured with `HIGH_DYNAMIC_RANGE | AUTO_EXPOSURE`, without `DEPTH_INVERTED` (`tools/fsr4cap/fsr4cap.c:237`).
  - `Bloodborne PC/fsr4_411_fp8` and `bbport-pr/fsr4_411` are byte-identical FP8 sets.
- **Device (RX 9070 XT, RADV, Mesa 26.2.4):** `VK_EXT_shader_float8` + `shaderFloat8CooperativeMatrix`, `VK_KHR_cooperative_matrix`, `VK_VALVE_shader_mixed_float_dot_product`, `VK_KHR_compute_shader_derivatives`, `vulkanMemoryModel`, `computeFullSubgroups` and `shaderIntegerDotProduct` are all reported by vulkaninfo.
- **GR2 (static analysis of `PS4 Roms/CUSA03694/eboot.bin`, game version 1.11):**
  - G-buffer vertex shaders, rigid and skinned, take `u_m4WorldViewProj` + `u_m4PrevWorldViewProj` and output `f3Current`/`f3Previous`, so per-object velocity exists.
  - A depth-reprojection compute pass (`u_txZ`, `u_txStencil`, `u_m4InversePrevProj`) handles background/camera motion.
  - The motion blur is McGuire-style (`u_txVelocity`, `u_txTileMax`, `u_txNeighborMax`, `u_txCompressed`, `u_fVelocityScale`).
  - A temporal resample compute pass takes `u_txColor`, `u_txAccumulation`, `u_txVelocity`, `u_txStencil`, `u_f2SourceSize`, `u_f4TargetSize`, `u_fLo/HiFrequency`.
  - There is a previous-frame blend PS (`u_txPrev`, `u_weight/u_blend/u_limit`).
  - Pass names: Resolve, CbResolve, Antialias, Composite, MotionBlur, Bloom, Tonemap, Contour.
  - No camera-jitter or Halton names; only shadow and cloud jitter.
  - `sceKernelIsNeoMode` is imported. No ID-buffer, MSAA or EQAA names appear in the 392 embedded shaders.
- **shadPS4 Pro mode:** `sceKernelIsNeoMode` (`src/core/libraries/kernel/process.cpp:19`) is gated by `isPS4Pro` + PSF `support_neo_mode`. The ID buffer is not emulated. EQAA regs exist (`regs_depth.h:166`) but are unused. User config: `neo_mode: false`.
- **Patches:** `user/patches/GoldHEN/GravityDaze2-Orbis.xml` (v1.11).
  - The 1440p patch writes 0x0a00/0x05a0 and 2560.0/1440.0 at 0x00846825, 0x0085d8e0, 0x0119fa01, 0x0142aa0b, plus 640x360 quarter buffers.
  - The "Disable motion blur" patch skips two full-screen draws (0x0143e71f, 0x0143ea18).
  - The legacy branch drops motion-blur PS hash `0xf696fe23` (`origin/gr2fork-legacy:src-gr2/video_core/renderer_vulkan/vk_rasterizer.cpp:489`).
- **User's GR2 config:** `custom_configs/CUSA03694.json`, borderless 2560x1440, vblank 144, FSR1 off. Patches on: 1440p, 144fps v4.0, Disable motion blur.

### Phase 0 details

- **Vulkan headers:** `externals/vulkan-headers` is `VK_HEADER_VERSION 330` and lacks `VK_VALVE_shader_mixed_float_dot_product`. The system headers are 1.4.357 and have it.
  - Bump the submodule to the v1.4.357 tag. Check that the bundled `vulkan.hpp` then provides `vk::PhysicalDeviceShaderMixedFloatDotProductFeaturesVALVE`.
  - Fallback: define the C struct and sType locally and chain it through raw `pNext`.
  - This is required: `Record411` checks `IsFsr411Supported()` even for FP8 sets (bbport `vk_fsr4.cpp:229`).
- **Loader linkage:** EXODUS defines `VK_NO_PROTOTYPES` (`vk_common.h:8-9`) and uses the vulkan-hpp dynamic dispatcher (`vk_platform.cpp:273-274`). It does not link libvulkan. `fsr411.cpp` calls about 40 C entry points.
  - Decision: link `Vulkan::Vulkan` (libvulkan.so.1, the same loader the dispatcher dlopens), and compile `fsr411.cpp` without `VK_NO_PROTOTYPES`.
  - Alternative: route the calls through `VULKAN_HPP_DEFAULT_DISPATCHER`.
- **Build:**
  - `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld`, then `cmake --build build`.
  - CI uses clang-19 + mold. Local: clang/lld 23.1.1, no mold, no ccache.
  - AppImage: `.github/workflows/scripts/linux-appimage-sdl.sh`, which downloads linuxdeploy and checkrt (needs network).
- **User dir:** `src/common/path_util.cpp:89-102` uses `cwd/user` if it exists, else `$XDG_DATA_HOME/shadPS4` or `~/.local/share/shadPS4` (shared with the other shadPS4 builds). Always launch with cwd = the test folder.
  - EXODUS imports legacy settings and writes sparse per-game JSON (commit `4903e5dd`). Copy only.

### Phase 1 details

- **RenderDoc:** `renderdoc_enabled` (`src/core/emulator_settings.h:852`), `src/video_core/renderdoc.cpp`, capture hotkey in `src/input/input_handler.cpp`.
- **Identifying passes:** RenderDoc shows recompiled SPIR-V, so the eboot names (`u_txVelocity`, …) will not appear. Identify passes by formats, sizes, bindings, dispatch order and the emulator's shader hashes in the log.
- **Analysis:** headless, with the RenderDoc Python module.
- **Checklist:**
  1. Velocity: format (RG16F? packed `u_txCompressed`?), scale (`u_fVelocityScale`), resolution. Is it still written with the motion-blur patch on?
  2. Depth: format, and reversed-Z (clear value and compare op).
  3. Hash of the temporal resample dispatch and of the Antialias pass.
  4. Contour pass position.
  5. SourceSize vs TargetSize of the resample in base mode.
  6. Projection constants across 2 consecutive frames (jitter).
  7. Scene color format (R11G11B10F vs RGBA16F) and the first UI draw hash.

### Phase 1 results (capture `GR2fork-FSR4-test/user/captures/CUSA03694_capture_3.rdc`, 1440p patch, base mode)

Analysis is headless: `qrenderdoc --python <script>` on the normal display (`QT_QPA_PLATFORM=offscreen` hangs before the script runs). The Arch package ships no standalone Python module. Scripts are in `documents/gr2-fsr411/rdc/`. Run them with `RDC_OUT=<out.txt> RDC_CAP=<file.rdc> qrenderdoc --python <script>` (`jitter.py` takes `RDC_CAPS`, a comma list; `cbuf.py` takes `RDC_EVENTS`). Shader modules have no names, so passes were identified by targets, formats and order.

| Event | Pass | Notes |
|---|---|---|
| 8797-8836 | depth prepass | D32S8 `T2192` 2560x1440 |
| 8849-14345 | G-buffer | RT0 RGBA8 sRGB (albedo), RT1 RG16F `T2280` (normals), RT2 RGBA8, RT3 RG16F `T2223` (velocity; most draws write it) |
| 14443-14742 | forward-shaded draws (characters) into HDR `T2337` RGBA16F | write no velocity |
| 14748-14768 | linear depth R32F `T2215` + 5-level min chain | |
| 14773 | camera-motion compute | reads D32S8 + R32F, writes `T2223` |
| 14778/14783 | temporal lighting accumulation | R11G11B10 ping-pong `T2281/2282`, `T2293/2294`, reads velocity |
| 14788-14798 | deferred lighting, composite into HDR `T2337` | |
| 14888-15014 | fog (160x90 volume) into HDR `T2320`, transparents, refraction | |
| 15028-15052 | bloom at 640x360 (R11G11B10) | |
| 15058-15072 | motion-blur prep: RG8 compressed velocity, 64x60 tile/neighbour max | the blur itself is skipped by the patch |
| 15081 | tonemap: HDR `T2320` + bloom -> sRGB `T2366` | |
| **15087** | **game AA (compute): FXAA + history blend** | reads `T2366`, stencil, velocity `T2223`, history `T2375`; writes `T2374` (ping-pong) |
| 15094 | copy `T2374` -> VideoOut buffer `T2155` (R8G8B8A8_SRGB 2560x1440) | |
| 15105-15138 | 640x360 RGBA8 chain, blended into `T2155` | overlay; no HUD visible in this frame |
| 15148 | presenter `post_process.frag` | |

- **Velocity encoding** (from the AA shader's SPIR-V): `history_px = (px + 0.5) + 0.5*W*v.x`, `history_py = (py + 0.5) - 0.5*H*v.y`. So `v` is previous minus current in NDC with y up. In FSR terms (previous minus current, render pixels, y down): `motion_scale = (0.5*W, -0.5*H)`. No conversion pass is needed.
  - The AA uses history only where stencil bit `0x8` is set (static geometry) and motion is 16 px or less.
  - Character pixels hold exactly `(0, 0)`.
- **Velocity statistics:** |x| p99 0.0077, |y| p99 0.061 NDC (y = 0.058 at the bottom centre, about 42 px). This is plausible for forward running, but there is no second frame to confirm the scale.
- **Depth:** standard Z. The sky clears to 1.0; values are 0.973-0.997 from near to far. This matches assets captured without `DEPTH_INVERTED`.
- **Insertion point for job A:** replace dispatch 15087.
  - Input color: LDR `T2366`, or HDR `T2320` before bloom and tonemap.
  - Depth `T2192`, motion `T2223` with the scale above.
  - Output into `T2374`, so the copy at 15094 picks it up.
- **Job B:** the VideoOut buffer (`T2155`) gets the AA output (15094) plus overlays. The DLSS project's HUD-difference composite applies: pre-HUD = `T2374`, final = `T2155` at flip.
- **Jitter: none, confirmed on 6 frames.** G-buffer draws have WorldView (bytes 0-63), WorldViewProj (64-127) and PrevWorldViewProj (128-191) in their VS buffer. `P = inverse(WV) * WVP` gives `|P[2][0]|*1280` and `|P[2][1]|*720` below 0.0001 px in capture 3 (event 8849) and in session-3 captures 4, 7, 17, 25 and 31 (script `rdc/jitter.py`). The vertical FOV changes with gameplay (`P11` 2.0965 to 2.8050). The camera-motion compute (14773) gets the same centred projection. The AA shader has no unjitter term.
- **Projection form:** row vectors, `P22 = -1.00002`, `P32 = -0.2`, `P23 = -1`.
- **Captures kept** (`GR2fork-FSR4-test/user/captures/`, not in git): `gr2_bridge_kat.rdc` (the Phase 1 results above), `gr2_city_people_subtitle.rdc`, and session 3 `CUSA03694_capture_2..32.rdc` (34 GB in total). Useful ones: 2-3 pause menu; 4, 5, 11, 12, 25, 26, 30-32 HUD (health, gravity gauge, mission text); 7-10 moving NPCs with subtitles; 26, 28 button prompts; 22 comic panel; 13, 20, 21 black loading (21 has a subtitle on black); 25-32 fast falling.
- **HUD draw order:** to analyse for Phase 4 from captures 2, 11 and 26.

### Phase 2 details

- **Copy** `fsr411/fsr411.{h,cpp}` to `src/video_core/renderer_vulkan/fsr411/`. Add it to the `CMakeLists.txt` source list (around line 1174).
- **`vk_instance.{h,cpp}`:** port these parts of bbport `vk_instance.cpp`:
  - 216-222: feature chain
  - 357-374: the 4 extensions
  - 445 `shaderStorageImageWriteWithoutFormat`, 473 `vulkanMemoryModel`, 480 `computeFullSubgroups`, 483 `shaderIntegerDotProduct`
  - 560-575 and 627-638: device chain and unlink
  - plus the `vk_instance.h:280-296` getters
  - Raise `static_vector<const char*, 32> enabled_extensions` (EXODUS `vk_instance.cpp:232`) to 40. EXODUS already has 33 `add_extension` calls.
- **Wrapper:** port `Record411` (bbport `vk_fsr4.cpp:228-270`) without `bbport_settings.h` and the v07/FireBurn dependencies. Do the frames-in-flight wait with a scheduler tick.
- **Settings:**
  - `src/core/emulator_settings.h:700-719`. Append to `GPU_SETTINGS_JSON_FIELDS_B` (817-830), because of the 63-name nlohmann macro limit. Accessors at around 1207.
  - `vk_presenter.cpp:487-489`
  - `src/core/devtools/layer.cpp:101-125` (Ctrl+F10 Display menu)
  - `src/imgui/big_picture/settings_dialog_imgui.cpp:65-67, 126-127, 739-746`
  - Optional: `src/core/ipc/ipc.cpp` around 165-175, for GR2Launcher.
- **Frame boundary:** port bbport `core/libraries/videoout/driver.cpp:220` `AddDisplayBuffer` + `IsDisplayBuffer`.
- **Jitter:** viewport offset in `Rasterizer::UpdateViewportScissorState` (EXODUS `vk_rasterizer.cpp:3181`), as in bbport `vk_rasterizer.cpp:3244-3249`.
  - EXODUS's DynState skip-cache (3120-3148) and `push_vp_memo` (1468-1480) must see the jitter. Put it into the key, or bump `dynamic_state.invalidate_gen` each frame.
  - Sequence: Halton(2,3) with phases `clamp(ceil(8·(out/render)²), 8, 256)` (bbport `motion_history.h:206`).
  - Selection: scene-depth draws that are not full-screen (bbport rasterizer 1087-1090, 1215-1216). Check this rule again for GR2.
- **Assets:** copy `Bloodborne PC/fsr4_411_fp8/*` to `GR2fork-FSR4-test/user/fsr4_411/`.
- **Size check done:** `fsr411.cpp` takes any output up to 3840x2160 (`rw <= ow`). Above 1920x1080 it uses the t2160 tier; dispatch counts and the tensor table come from the output size aligned to 8. bbport commit f555009 verified FP8 bit-exact against the DLL at 1440p and ran 1707x960 -> 2560x1440 in game at 0.79-0.89 ms. Native ratio (1440p -> 1440p) is untested but is not restricted.
- **As built (2026-10-05):**
  - `src/video_core/renderer_vulkan/fsr411/fsr411.{h,cpp}`: copied unchanged from bbport.
  - It calls global Vulkan functions, while the emulator uses the dynamic dispatcher (`VK_NO_PROTOTYPES` in `vk_common.h`). Fix: `shadps4` links `vulkan` (libvulkan.so.1, the same library the dispatcher opens). Linux only.
  - `vk_instance`: the 4 extensions and their features are optional and unlinked when missing (RenderDoc may hide them). Getter `IsFsr411Fp8Supported()`. `enabled_extensions` grew from 32 to 40.
  - `vk_fsr411_pass.{h,cpp}`: `Fsr411Pass`, owned by `Rasterizer`. `Record()` waits for the frame 8 frames back with scheduler ticks, ends rendering, records into the scheduler's command buffer, and logs `Describe()`. An "unsupported size" error is transient; any other error disables FSR for the session. `SelfTest(w, h)` creates scratch RGBA16F/D32/RG16F/RGBA16F images (VMA), records one reset frame and calls `scheduler.Finish()`. It runs in the `Rasterizer` constructor when `fsr411_enabled` is set, at the configured window size.
  - Settings `fsr411_enabled` (bool) and `fsr411_sharpness` (0-100) at the end of `GPU_SETTINGS_JSON_FIELDS_B` (47 of 63 names). Menu: `core/devtools/layer.cpp`, Display > FSR 4.1.1 (live toggle, Save).
  - Assets dir: `<user>/fsr4_411` (copied, 2.8 MB).
  - Jitter: deferred to Phase 3. Rule: jitter draws whose depth attachment is the scene depth (`T2192` in capture 3) at scene size. That covers the prepass, G-buffer, forward characters and transparents, but not shadows, the fog volume, bloom, the overlay chain, the 64x60 tile pass or the HUD. Check the G-buffer depth compare op first: EQUAL against the prepass means all scene-depth passes need the same offset.

### Phase 3 details

- **Trigger:** before the game's temporal resample dispatch (hash from Phase 1). This is the equivalent of bbport `OnDispatch(0x9a9cf8a9)` and the native-AA `Run()` (`vk_temporal_upscaler.cpp:988-1215`).
- **New host shader `motion_convert.comp`:** converts game velocity to RG16F render pixels (decode, scale, sign, y-down). Register it with `add_host_shader` in `src/video_core/host_shaders/CMakeLists.txt`.
- **Depth:** if GR2 uses reversed-Z, either:
  - write `1 - z` into a D32 copy with a fragment pass that writes `gl_FragDepth` (like bbport `depth_resample.frag`), or
  - recapture the assets with `DEPTH_INVERTED` (`tools/fsr4cap/build_assets.sh` needs the AMD DLLs + Proton).
- **Color:** `Frame.color` must be RGBA16F. Copy first if the game uses R11G11B10F.
- **Write-back:** port `upscale_merge.comp` (41 lines; keeps alpha).
- **Game temporal pass:** skip it by hash while FSR is active, the same way the legacy branch drops `0xf696fe23`.
- **History reset:** on loads, camera cuts and setting changes.
- **Assumption to check:** the game velocity comes from unjittered WVP/PrevWVP, so a viewport jitter does not leak into it. The depth-reprojection pass samples jittered depth, which may give a small error.

### Template: IFreemz/shadPS4-Bloodborne-DLSS (commit e8d5c7b on shadPS4 0.19.0 c7e065d; EXODUS contains c7e065d)

- **Files:** `vk_bb_temporal_dlss.{h,cpp}` (90 + 943), `vk_bb_velocity_mirror.{h,cpp}` (41 + 293), `host_shaders/bb_dlss_motion.comp` (91), `bb_dlss_composite.comp` (54), `vk_dlss_ngx.*` (NGX bridge, not needed), rasterizer +93/-28, presenter +40/-9, `vk_graphics_pipeline` +34, `texture_cache.h` +11.
- **Rasterizer hooks:**
  - `TemporalDlssDraw` runs before every draw and returns the per-draw viewport jitter. Its `DrawInfo` carries vs/ps hash, color/depth ImageId, num_indices, instances and indirect.
  - `ReplayVelocityMirror` re-records the game's velocity draws into a render-size target, then restores the dynamic state, the rendering and the pipeline.
- **Presenter:** `TakePresentation(guest_address)` replaces `fsr_pass.Render` when the last display copy into that VideoOut buffer was upscaled. `pp_pass` then gets `source_size`.
- **Evaluate interface:** `DlssNgx::Evaluate(cmd, color, depth, motion, output, {jitter_x, jitter_y, reset, frame_ms})`. This maps 1:1 onto `Fsr411::Upscaler::Record(Frame)`. Swap the backend and drop NGX.
- **HUD:** `bb_dlss_composite.comp` computes `upscaled + (final_with_HUD - pre_HUD_snapshot)` (bilinear, render size), optional RCAS, and the game's display LUT at output size.
- **Bloodborne-specific constants** (`vk_bb_temporal_dlss.cpp:73-84`): DepthProducer `0xd3c8bb21`, DisplayCopy `0x38d65b32`, velocity shaders `0x34bc187c`/`0x749e4f9e`/`0xb25e4fae`, and the Scaleform HUD VS list. GR2 equivalents come from Phase 1.
- **Camera motion:** built from scene constants in double precision. The game's own float32 reprojection matrix had about 0.6 px noise, which made DLSS shimmer. Watch for the same problem with GR2's `u_m4InversePrevProj` pass.

### Phase 3b details (character object motion)

- **Targets:** forward-shaded draws into the HDR target with scene depth (events 14443-14742 in capture 3, 64 draws) write no velocity. The camera-motion compute (14773) leaves `(0, 0)` where stencil bit `0x8` is clear.
- **Method:** port bbport's ObjectMotion.
  - Recompiler variant of the draw's VS: store clip positions per vertex into a positions buffer, read the previous frame's positions.
  - The fragment shader writes previous minus current.
  - Draws are matched across frames by key (streams, shader, index hash and range, instance, order).
  - bbport files: `vk_object_motion.*`, `motion_history.h`, `runtime_info.h` 81-115/230, `spirv_emit_context.*`, `emit_spirv_special.cpp` +126, `vk_pipeline_cache.cpp` 507-635.
  - EXODUS's recompiler moved 73 commits past b4e7ae7, so expect manual merging.
- **Selection for GR2:** replace bbport's bone-palette `ClassifyBuffer` with a target-based rule: color = HDR scene target, depth = scene depth, not full-screen, not blended. Check whether the G-buffer skinned draws (which have `u_m4PrevWorldViewProj` but one skin palette) also need it for limb motion.
- **Output:** a separate RG16F object-motion target plus coverage, merged over `T2223` before FSR. This is the DLSS project's "velocity mirror" merge idea. Units: render pixels, previous minus current, y down; or NDC to match the game buffer, then use one `motion_scale`.
- **Draw matching fallback:** an unmatched draw writes nothing, so camera motion remains.

### Phase 4 details

- **Render size:** generate custom-size variants of the GoldHEN resolution patch (addresses above).
- **UI at output size:** port the bbport scaled path:
  - `RunScaled` (`vk_temporal_upscaler.cpp:1513-1746`)
  - `RedirectColor`/`RedirectDepth` (rasterizer 2800/2875) and `target_scale` viewport scaling (2889-2952)
  - `RedirectSampled` (2421)
  - `DisplayOverride` (1870), plus the presenter hook (bbport `vk_presenter.cpp:318-326` goes into EXODUS `PrepareFrame` 666-767, where `fsr_pass.Render` is at 743)
  - The UI trigger is GR2's first UI draw hash.
  - Remove the 1920x1080 hardcodes (`vk_scene_resolution.cpp:74,96`, `UiComposition::NativeViewport`).
- **Mip bias:** `log2(render/output)` on scene samplers (bbport `sampler.cpp`, rasterizer 1523).
- **Shortcut:** if the resample pass runs with SourceSize < TargetSize in base mode, replace it with FSR and let the game do post-processing and UI at the target size.

### Phase 5 details

- Fall back to FSR 1 on a fatal error (like bbport `RecordFsr4`, 1950-1956).
- Per-pass GPU timing (like `BB_FSR4_PROFILE`).
- Package the AppImage without the assets.

### Licensing and repository rules

- bbport and shadPS4 are both GPL-2.0-or-later, so the code is compatible.
- The AMD model data (`initializer.bin` and the SPIR-V derived from AMD's DXIL) is not redistributable.
- The repo's `AGENTS.md` (from upstream) forbids game-specific code, so this cannot go upstream. EXODUS already carries GR2-specific code (`core/libraries/gr2_online`, `gr2_photo`, `texture_cache/photo_readback`).

### Estimates (my working time, not counting waits for user tests)

| Phase | Estimate |
|---|---|
| 0 Baseline build + test copy | 30-60 min |
| 1 RenderDoc investigation | 30-90 min + 15 min user play |
| 2 Generic plumbing | 1-3 h |
| 3 Native AA (job A) | 2-6 h |
| 3b Character motion vectors (required) | 4-10 h |
| 4 Upscaling (job B) | 2-5 h (HUD-difference method) |
| 5 Finish | ~1 h |
