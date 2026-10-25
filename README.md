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

## Live video in, NDI, Spout and Syphon

- **Capture devices** (cameras, capture cards, anything the OS lists as a video input): right-click a clip slot → *Capture device*, or set a clip's source to *Capture device* and pick the input. The device's best format is chosen (highest resolution, then frame rate, avoiding MJPEG), and frames take the same GPU YUV path as video files. macOS asks for camera permission the first time.
- **NDI sources**: right-click a clip slot → *NDI source*. Each NDI clip has its own receiver thread.
- **NDI out**: set a screen's output device to *NDI*. While outputs are live, the screen is rendered off-screen at its own resolution and published as **"Display Hawk - &lt;screen name&gt;"**, for vision mixers, streaming software or another media server.
- **Spout (Windows) / Syphon (macOS)** share GPU textures with other apps on the same machine (OBS, TouchDesigner, Resolume, MadMapper...), with no copy through system memory. Set a screen's output device to *Spout* / *Syphon* to publish it as "Display Hawk - &lt;screen name&gt;", or right-click a clip slot → *Spout source* / *Syphon source* to take another app's output in. Spout needs the Direct3D 11 backend (the Windows default) and Syphon the Metal backend (the macOS default). Built from the vendored, BSD-licensed Spout2 and Syphon SDKs in `third_party/`.
- NDI uses the **NDI runtime installed on the machine** (NDI Tools or the NDI Runtime from [ndi.video](https://ndi.video)); Display Hawk loads it when needed and runs normally without it. NDI® is a registered trademark of Vizrt NDI AB.

## Audio

- Video clips play their sound. Levels: **clip volume** (clip properties, up to 200% for quiet files) × **layer volume** (layer properties) × **audio master** (deck top bar, with **MUTE**). Layer volume and the audio master are playback controls like opacity: saved with the show, never undo steps.
- Bypassed layers, and layers left out by a solo, are silent. When a layer crossfades, the outgoing clip's sound fades out and the incoming clip's fades in over the same time; every other level change ramps over 40 ms so it never clicks.
- HAP movies with a soundtrack play it through an audio-only decoder started alongside the HAP reader.
- **Audio → Output device** picks the sound card (remembered on this machine). OSC: `/dh/layer/<l>/volume`, `/dh/audio/master`, `/dh/audio/mute`; the same actions are available for MIDI mappings.

## Audio reactive

**Audio → Audio reactive…** (Ctrl/Cmd+Shift+A) listens to a microphone or line-in and analyses it ~47 times a second: overall **level**, **bass** (20–250 Hz), **mid** (250 Hz–2 kHz), **treble** (2–12 kHz) and **beats** (bass onsets). Levels adapt to the loudness automatically; *Sensitivity* adds gain for very quiet inputs. Live meters and a beat lamp show what it hears.

Links (saved with the setup) connect a signal to the show:

- **Layer opacity / Master**: pulse between (1 − amount) and the fader value. The modulation is applied at render time, so faders do not move, nothing lands in undo, and switching audio reactive off returns exactly to the faders. *Beat* used here gives a sharp flash that decays.
- **Next column / Layer next clip (on beat)**: each beat triggers the next column, or the layer's next clip, that has something in it.
- **Effect parameter**: pushes any effect parameter from its set value towards its maximum (e.g. bass widens a panel chase or thickens panel outlines). Also applied at render time only.

## Effects

Every layer has an effect chain (select the layer: **Effects** in its properties), and so does the whole composition (select *Composition*). Effects run on the GPU, top to bottom; tick an effect's name to bypass it. Composition effects run before the panel mask, overlay and master fader.

| Group | Effects |
| --- | --- |
| Colour | Colour (brightness, contrast, saturation, hue), Tint, Invert, Posterize |
| Shape | Transform (position, scale, rotation, tile), Mirror, Kaleidoscope, Pixelate |
| Texture | Blur (two-pass Gaussian), RGB Split, Edges |
| Time | Strobe |
| Key | **Colour Key**: makes a colour transparent, e.g. the white background of a logo (key colour, tolerance, softness, spill removal for clean anti-aliased edges); **Luma Key**: removes dark or bright areas |
| LED panels | **Panel Outline** (edges of every fitted cabinet), **Panel Chase** (a band of lit cabinets travelling left to right, top to bottom, by panel number or in random order; reveals the content or adds white), **Panel Twinkle** (random cabinets flash and fade), **Panel Colours** (a hue per cabinet along an order, optionally drifting) |

The LED panel effects follow the real cabinet layout of every wall, including cabinets marked as not fitted (they are skipped), so chases and outlines work on stepped and irregular walls. Left-to-right and top-to-bottom orders use the cabinets' positions, so gaps keep their physical spacing.

Each parameter can be driven live: OSC `/dh/layer/<l>/effect/<e>/<p>` or `/dh/comp/effect/<e>/<p>` (effects and parameters count from 1; 0..1 spans the parameter's range), MIDI (right-click a parameter → *Learn MIDI control*), or audio reactive. Live control changes the value without adding undo steps.

## LED tools

- **Colour correction per slice:** red/green/blue gain, a gamma exponent and black level, applied on the GPU to that slice only, for matching cabinet batches or neighbouring walls.
- **Irregular walls:** in a wall's properties, click or drag on the panel grid to mark cabinets that are not fitted (stepped or odd-shaped walls). Panel maps cross them out and number only fitted cabinets; specs and port estimates count only fitted cabinets. *Black out areas without LED panels* (composition properties) multiplies the output by the real panel layout, so content never lands where there are no LEDs.
- **Cabinet layouts** (screen properties → *Cabinet layouts*, or a wall's *Map each cabinet on screen*): one slice per fitted cabinet, placed on the processor's output canvas and chained onto ports, for walls that are not simple rectangles.
  - *Arrange*: the same shape as the wall, packed in rows, or packed in columns (wrapping at the screen size).
  - *Chain*: rows or columns, straight or serpentine, starting from any corner: the order the processor's data runs through the cabinets.
  - Position, gap between cabinets, rotation (the whole layout and each cabinet's content) and flips.
  - *Port size* (1G / 5G / 10G presets or custom pixels): cabinets are split onto ports in chain order, optionally starting a new row per port. The output map draws each port's route with arrows, colour-coded; the setup report draws it on the processor view, and the CSV lists wall, cabinet and port per slice.
  - Layouts are stored with the screen and regenerate when the wall changes (cabinets marked as not fitted drop out of the chain); each cabinet keeps its own brightness, colour correction and angle.
- **Rotated walls:** a wall's *Angle* turns the whole wall about its centre (diamond or tilted walls). The panel map, panel overlay, panel mask and LED panel effects follow it, layers fitted into the wall turn with it, and slices and cabinet layouts made from the wall sample it at its angle while their output stays upright, the way the processor expects it.
- **Corner pin:** tick *Corner pin* on a slice to place it on the output as a free four-cornered shape, mapped with perspective; drag the numbered corners on the output map (they snap to the screen and other slices; Shift turns snapping off). Shows in the setup report as well.
- **Slice angle:** a slice can sample a rotated area of the composition (any angle, about its centre) while its output stays upright, for walls hung at an angle such as diamond layouts.
- **Moving Bars** test pattern: a vertical and a horizontal bar sweeping over black, for spotting tearing, scan and sync faults on LED processors.
- **File → Export setup report…** writes, for the LED technician: a PDF (wall specs with pitch, physical size and estimated 1G ports, screens, the panel map, and one page per screen showing what its processor receives with a slice coordinate table), the panel map and each screen's processor view as PNGs, and all slice mappings as CSV.

## Remote control: OSC, MIDI and DMX

**Control → OSC, MIDI & DMX…** (Ctrl/Cmd+M).

- **OSC** over UDP (off by default; default port 7400). Layers and columns count from 1, layer 1 is the bottom layer:

  | Address | Argument |
  | --- | --- |
  | `/dh/master` | 0..1 |
  | `/dh/blackout`, `/dh/live` | 0/1, or none to toggle |
  | `/dh/column/<c>/trigger` | (0 is ignored, so button releases do nothing) |
  | `/dh/layer/<l>/clip/<c>/trigger` | |
  | `/dh/layer/<l>/opacity` | 0..1 |
  | `/dh/layer/<l>/bypass`, `/dh/layer/<l>/solo` | 0/1, or none to toggle |
  | `/dh/layer/<l>/clear` | |
  | `/dh/layer/<l>/effect/<e>/<p>`, `/dh/comp/effect/<e>/<p>` | 0..1 across the parameter's range |

  Bundles and int/float/bool/double arguments are accepted, so TouchOSC, QLab, Companion, lighting desks or a few lines of Python all work.
- **MIDI** from every connected controller (WinMM on Windows, CoreMIDI on macOS; choose devices in the dialog). Mappings are saved with the setup: add one and press **Learn**, then press a key or move a knob, or right-click a clip / column number in the Clip Deck → *MIDI learn*. Notes trigger and flip toggles; CCs drive faders, set toggles (≥ 64 = on) and trigger once as they pass halfway.
- **DMX** from a lighting desk over **Art-Net** (UDP 6454; Display Hawk answers ArtPoll, so it shows up in the desk's node list) or **sACN / E1.31** (UDP 5568, joins the universe's multicast group; the highest-priority source wins, a terminated stream hands over to a backup at once, preview data is ignored). The patch is saved with the setup. From the start address:

  | Channel | Function |
  | --- | --- |
  | +0 | Master (0-255) |
  | +1 | Blackout (128 and up = on) |
  | +2 | Column trigger (0 = nothing, n = trigger column n) |
  | +3, +4, +5 | Layer 1 (bottom): clip (0 = stop, n = play column n), opacity, bypass (128 and up) |
  | +6, +7, +8 | Layer 2, and so on for the number of layers patched |

  Values act when they change, so the desk and the Display Hawk UI can both be used. When a desk first appears (or returns after 10 s of silence) its clip and column channels are taken as the starting point and only faders are applied, so a desk sitting at 0 does not stop every layer.
- The dialog's monitor shows every incoming OSC and MIDI message, and DMX changes on the patched channels.

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
