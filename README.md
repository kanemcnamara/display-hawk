# Display Hawk

Media server for LED walls on **macOS and Windows** (C++17, Qt 6, Qt RHI). The output-mapping core: a composition canvas, LED wall definitions, and screens/slices that map the composition onto physical outputs pixel-exactly.

## Concepts

| Thing | What it is |
| --- | --- |
| **Composition** | The master canvas (any resolution). Content is rendered here. |
| **Content** | A test pattern (grid, colour bars, ramps, checker, solid, LED panel map) or a video/image file, scaled Fit / Fill / Stretch / Native into the whole composition or into one LED wall. |
| **LED wall** | A grid of identical panels: panel pixels, panel size in mm, columns Ã— rows and position in the composition. Shows pitch, physical size, total pixels and an estimate of 1G processor ports. |
| **Screen** | A video output (GPU output â†’ LED processor). Fullscreen on a display, a test window, or preview only. |
| **Slice** | Copies a rectangle of the composition onto a rectangle of a screen, with 90Â° rotation, flips and brightness. |

Slices that sample exactly a wall's area follow the wall when it is moved or resized.

## LED tools

- **Colour correction per slice:** red/green/blue gain, a gamma exponent and black level, applied on the GPU to that slice only, for matching cabinet batches or neighbouring walls.
- **Irregular walls:** in a wall's properties, click or drag on the panel grid to mark cabinets that are not fitted (stepped or odd-shaped walls). Panel maps cross them out and number only fitted cabinets; specs and port estimates count only fitted cabinets. *Black out areas without LED panels* (composition properties) multiplies the output by the real panel layout, so content never lands where there are no LEDs.
- **Moving Bars** test pattern: a vertical and a horizontal bar sweeping over black, for spotting tearing, scan and sync faults on LED processors.
- **File → Export setup report…** writes, for the LED technician: a PDF (wall specs with pitch, physical size and estimated 1G ports, screens, the panel map, and one page per screen showing what its processor receives with a slice coordinate table), the panel map and each screen's processor view as PNGs, and all slice mappings as CSV.

## Clip deck

| Thing | What it is |
| --- | --- |
| **Clip** | A test pattern or media file (video or image) in one slot, with its own fit mode (Fit / Fill / Stretch / Native) and loop setting. |
| **Layer** | A row of clips. One clip plays per layer. Layers stack bottom to top with opacity, a blend mode (Normal, Add, Screen, Multiply, Lighten, Darken), bypass/solo, a crossfade time and a target (whole composition or one LED wall). |
| **Column** | Clicking a column header triggers it as a scene: every layer plays its clip in that column; layers with an empty slot stop. |
| **Master / Blackout** | Dims or blacks out everything sent to the outputs (Ctrl/Cmd+B for blackout). |

- Click a clip to trigger it (again to restart a video). Drag files from Explorer/Finder onto slots (several files fill consecutive slots), drag clips between slots, double-click an empty slot to load media, right-click for test patterns, duplicate and clear.
- What is playing, layer opacity/bypass/solo and the master are *playback*: they are saved with the setup (so recovery and go-live-on-launch resume the show) but are never undo steps, and undo never changes what is playing.
- Each playing media clip has its own decoder; it is released shortly after the clip stops (after any crossfade).

## Using it

- **Input map** (top): composition with walls (dashed) and slice inputs (orange). **Output map** (bottom): the selected screen with slice outputs. Red outlines = outside the canvas.
- Drag to move/resize; edges snap to the canvas, other items and panel seams. Shift disables snapping, Alt+click selects items underneath, arrow keys nudge 1 px (Shift: 10 px).
- Zoom: mouse wheel, Ctrl/Cmd+scroll or trackpad pinch. Pan: trackpad scroll, middle-drag or drag empty space. Double-click or F resets the view.
- **Undo/redo** (Ctrl/Cmd+Z, Ctrl+Y / Cmd+Shift+Z): a whole drag or a burst of typing is one step.
- **F9** toggles outputs live; **Esc** on an output turns them off. **Ctrl/Cmd+I** identifies every physical display.
- Setups save as `.dhawk` (JSON, written atomically). Unsaved changes are autosaved every 15 s; after a crash you are offered the recovered setup on the next launch.
- Unattended installs: *Output â†’ Go live on launch* (or `--live`) recovers without asking and opens outputs at startup. *File â†’ Reopen last setup on launch* is on by default.
- Log: `%LOCALAPPDATA%\Display Hawk\Display Hawk\displayhawk.log` (Windows), `~/Library/Application Support/Display Hawk/Display Hawk/displayhawk.log` (macOS).

## Rendering

A dedicated render thread owns the GPU through Qt RHI: **Metal** on macOS, **Direct3D 11** on Windows, OpenGL 3.3 as the automatic fallback. It renders the composition, presents every output with vsync (paced by the first live output), and renders downscaled previews of exactly what each editor view shows, which it reads back as images for the UI. The UI thread never touches the GPU, so dialogs or a busy UI cannot drop output frames.

- `DISPLAYHAWK_RHI=opengl|d3d11|d3d12|metal` picks a backend (D3D12 needs an MSVC build of Qt).
- Video frames are uploaded as their native YUV planes (NV12/NV21, 4:2:0 and 4:2:2 planar, 10-bit P010/P016) and converted to RGB on the GPU with the frame's colour matrix (BT.601/709/2020) and range. On 4K H.264 this cuts the render thread's per-frame work about 3x versus CPU conversion, and matches it to within one 8-bit level. `DISPLAYHAWK_NO_GPU_YUV=1` forces the CPU path; `DISPLAYHAWK_PERF=1` logs per-frame preparation time.
- **HAP** (`.mov` with Hap, Hap Alpha, Hap R, Hap Q or Hap Q Alpha) is played by Display Hawk's own reader instead of FFmpeg: a worker thread reads each frame, undoes the Snappy compression (including chunked frames) and the render thread uploads the DXT/BC blocks straight into a compressed GPU texture, so there is no decoding at all (~0.1 ms per frame on the render thread). Hap Q's YCoCg is converted to RGB in a shader. `tools/make_hap_test.py` writes Hap, Hap Alpha (chunked), Hap Q and Hap Q Alpha test movies, validated against FFmpeg's reference decoder; Hap R (BC7) is implemented but not yet tested with a real file. To make your own HAP files use an encoder such as the free Hap codec for Adobe/AVF, or an FFmpeg build with `--enable-libsnappy` (`-c:v hap -format hap_q`).
- Shaders live in `shaders/` as Vulkan-style GLSL and are compiled to HLSL/MSL/GLSL/SPIR-V at build time by `qt_add_shaders`.
- Over Remote Desktop, Windows composites Direct3D windows at ~30 fps; outputs on local displays run at the display's refresh rate.

## Building

Qt 6.8 with **Qt Multimedia** and **Qt Shader Tools**, CMake â‰¥ 3.21 and Ninja. `aqtinstall` installs Qt without admin rights:

```sh
python -m pip install --user aqtinstall cmake ninja
```

**Windows** (MinGW toolchain, as used on the dev machine):

```powershell
python -m aqt install-qt windows desktop 6.8.3 win64_mingw -m qtmultimedia qtshadertools -O C:\Qt
python -m aqt install-tool windows desktop tools_mingw1310 qt.tools.win64_mingw1310 -O C:\Qt
.\build.ps1                     # -> build\Release\DisplayHawk.exe
```

**macOS** (Xcode command-line tools):

```sh
python3 -m aqt install-qt mac desktop 6.8.3 clang_64 -m qtmultimedia qtshadertools -O ~/Qt
./build.sh                      # -> build/Release/DisplayHawk.app
```

CI (`.github/workflows/build.yml`) builds Windows (MSVC) and macOS on every push and uploads the deployed app.

## Self-test

`DisplayHawk --selftest <dir>` checks undo/redo, goes live with a windowed output and verifies the rendered pixels: both editor previews, column triggering, Multiply/Add blending, blackout, crossfades, and that undo leaves playback alone. With `DISPLAYHAWK_TEST_VIDEO=<file>` it also plays a video clip. Images of each step and of the main window are saved into `<dir>`. Exit code 0 = pass; details in the log. Run it per backend with `DISPLAYHAWK_RHI`.

## Layout

```
shaders/     quad.vert / quad.frag (one shader, all backends)
src/model    Project data and JSON (walls, screens, slices, layers, clips; v1 files migrate);
             Document (selection, undo/redo, playback, file state, autosave)
src/render   RenderEngine (GUI-side API, backend choice), RenderThread (QRhi, outputs, previews),
             RhiQuad (quad drawing, blend modes), Patterns (test patterns)
src/media    MediaSource (video via Qt Multimedia, still images, HAP); HapPlayer, HapDecoder,
             MovReader (QuickTime sample tables), Snappy
src/ui       MainWindow, CanvasView (editors), ClipDeck, OutputWindow, IdentifyOverlay, PropertyPanel
```
