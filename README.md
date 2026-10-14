# Phasmophobia port

This branch (`phasmophobia-port`) is [AnyPS5](https://github.com/boykopovar/AnyPS5) `main` as of 14 October 2026 plus the changes that run Phasmophobia (PPSA09030) on Windows. It is a working branch, not a release: The upstream thread for the title is [Game bring-up discussion #4680](https://github.com/boykopovar/AnyPS5/discussions/4680).

## What works

- The title starts, reaches the lobby, loads saves and plays a contract.
- Voice recognition works with the default Windows microphone.
- Measured on Windows 11 with an RTX 5070 Ti and pipelined draws on: about 32 to 33 presents a second in the lobby (about 1,550 draws a frame) and about 12 to 12.5 in the truck (training level) (6,250 to 6,450 draws a frame).
- The lobby whiteboards render correctly, also after returning from a contract.

## Known problems

- The frame rate falls with the number of draws in view; the truck being one of the worst cases.
- Multisampled targets are drawn with one sample.
- The very first start, with no shaders compiled yet, once stayed on "Initializing" after "Continue offline"; the next start went through.

## What is in here

- Fixes the title needs to run: kernel waits and module loading, network, SSL and entitlement functions, save data sizes, depth and DCC handling.
- A port of the pipelined draws draft ([#1201](https://github.com/boykopovar/AnyPS5/pull/1201)): one thread prepares draws while another records them. Off unless `APS5_PIPELINED_DRAWS=1` is set.
- Draw path speed work on top of it: an allocator cache for the driver, a compiled resource walk, per-shape reuse of what a draw prepares and records, pooled per-draw storage, a cached draw key and only the changed Vulkan state recorded per draw. Each has an environment switch that turns it off, and most have one that checks it against the original path.
- The window does not capture the host cursor, which stays visible and free; `APS5_CAPTURE_MOUSE=1` restores upstream's capture.

## Building and running

1. Build the `libs` and `relinker` targets as upstream describes in the [build instructions](docs/dev/BUILD.md). This branch was built with WinLibs GCC 16.1.0; upstream supports 15.2.0.
2. Decrypt the title: `eboot.bin`, the modules in `sce_module/` and the plugins in `Media/Plugins/` and `Media/Modules/`, kept in the same layout (ELF files, not SELF).
3. Convert it (see [Usage](docs/user/USAGE.md)):

   ```
   relinker --windows --module-dir Media/Plugins --module-dir Media/Modules --exclude-sce-module psvr2.prx <decrypted>/eboot.bin <game>/Phasmophobia.exe
   ```

   `psvr2.prx` (PS VR2) imports functions of `libSceHmd2` and `libSceVrTracker2` that AnyPS5 does not have, and Windows refuses to start the executable while it is converted (exit code `0xC0000139`). The title runs without it. Upstream marks `--exclude-sce-module` as deprecated.
4. The converted modules are written to `<game>/app0/`. Copy the rest of the title (`Media/`, `sce_sys/` and the other files) into `<game>/app0/` too, and the built libraries (`build/core/libs/libs/*.prx` with `libgcc_s_seh-1.dll`, `libstdc++-6.dll` and `libwinpthread-1.dll` from the toolchain) into `<game>/libs/`.
5. Start `Phasmophobia.exe` with `APS5_PIPELINED_DRAWS=1` set for the figures above. The first start compiles shaders and stutters for a few minutes.

Written with AI assistance.

---

# About

Tool for automatic executables porting to Linux and Windows.

[**Web converter**](https://boykopovar.github.io/AnyPS5/) - files stay on your device.

![anyps5_collage1](https://github.com/user-attachments/assets/0f8e13e8-1cb7-4c69-8075-39978e138994)

[Discord](https://discord.gg/BHFztBPUe)

Includes a [relinker](core/relinker) that converts executable to the target system's native format and implementations of [system prx libraries](core/libs/prx) suitable for dynamic linking. No emulation or separate runtime process.

[Usage](docs/user/USAGE.md), [Build instructions](docs/dev/BUILD.md), [Architecture](docs/dev/ARCHITECTURE.md), [Technical debt of the project](docs/dev/TechnicalDebt.md), [code style conventions](docs/dev/CONVENTIONS.md), [contributing](CONTRIBUTING.md)

## Status

[![libraries](https://boykopovar.github.io/AnyPS5/badge-libraries.svg)](https://boykopovar.github.io/AnyPS5/progress.html) [![shaders](https://boykopovar.github.io/AnyPS5/badge-shaders.svg)](https://boykopovar.github.io/AnyPS5/progress.html)

[![progress map](https://boykopovar.github.io/AnyPS5/progress.svg)](https://boykopovar.github.io/AnyPS5/progress.html)

[Progress history](https://boykopovar.github.io/AnyPS5/history.html)

<sub>* System libraries: percentage of the functions known to the project so far (declared in [core/libs/prx](core/libs/prx)), not of every PS5 system function. The total grows as more functions are declared.</sub>

Unsupported or unexpected states strictly throw `std::runtime_error`. `what()` is printed to stderr and the process terminates.

The [shader recompiler](core/shader/recompiler/Recompiler.cpp) successfully produces SPIR-V (validated via [Spirv-Tools](3rdparty/SPIRV-Tools) when built with `ANYPS5_ENABLE_SPIRV_TOOLS`).

## Compatibility

Per-game progress (stage reached, open blockers, libraries and GPU coverage) is tracked on the [blocker map](https://alxcbtw.github.io/anyps5-roadmap/), a community project built from public GitHub data ([source](https://github.com/AlxcBTW/anyps5-roadmap)). Test reports go in the [Game bring-up](https://github.com/boykopovar/AnyPS5/discussions/categories/game-bring-up) discussions, one thread per title.

## Input mapping

SDL-mapped game controllers are supported, including analog sticks and triggers. Keyboard and mouse controls can be configured with an `anyps5-input.ini` file. See [input mapping](docs/user/INPUT_MAPPING.md) for the supported devices and configuration format.

## Disclaimer

This project is intended for interoperability, research, preservation, and compatibility purposes. It does not include, distribute, or require copyrighted software, firmware, cryptographic keys, or proprietary libraries. Users are responsible for ensuring that any binaries used with this project are obtained and used in accordance with applicable laws and their respective license terms.

## License

This project is licensed under the GNU General Public License version 2 only.
