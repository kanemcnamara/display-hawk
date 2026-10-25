# Third-party code

Vendored unmodified (except where noted) so Display Hawk builds with no extra downloads.

| Folder | Project | Version | Licence |
| --- | --- | --- | --- |
| `spout/` | [Spout2](https://github.com/leadedge/Spout2) SpoutDX (D3D11) and its support files from `SPOUTSDK/SpoutGL` | release 2.007.018 | BSD-2-Clause (`spout/LICENSE`) |
| `syphon/Syphon/` | [Syphon-Framework](https://github.com/Syphon/Syphon-Framework): core + Metal server/client and `Syphon_Prefix.pch` (force-included, as in its Xcode target); the legacy OpenGL implementation is left out | commit f4761677a45b8034a3c2069ec0f3d2553da81fba | BSD-3-Clause (`syphon/License.txt`) |

Spout is compiled on Windows only, Syphon on macOS only.

**Local patches:** `syphon/Syphon/SyphonServerRendererMetal.m` returns nil when the compiled Metal shaders are missing (they are not bundled; Display Hawk publishes BGRA frames, which Syphon copies with a blit), instead of creating a pipeline with nil functions, which aborts under Metal API validation.
