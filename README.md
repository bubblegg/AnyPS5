# Phasmophobia port

This branch (`phasmophobia-port`) is [AnyPS5](https://github.com/boykopovar/AnyPS5) `main` as of 14 October 2026 plus the changes that run Phasmophobia (PPSA09030) on Windows. It is a working branch, not a release: The upstream thread for the title is [Game bring-up discussion #4680](https://github.com/boykopovar/AnyPS5/discussions/4680).

## What works

- The title starts, reaches the lobby, loads saves and plays a contract.
- Voice recognition works with the default Windows microphone.
- Measured on Windows 11 with an RTX 5070 Ti and pipelined draws on: about 26 to 31 presents a second in the lobby (1,700 to 1,900 draws a frame) and about 11 to 12 in the truck (training level) (6,100 to 6,600 draws a frame).

## Known problems

- The frame rate falls with the number of draws in view; the truck being one of the worst cases.
- Brief colour blocks appear when the camera moves to the lobby whiteboard.
- One draw type is skipped (`multisampled image resources have no descriptor image shape`).
- Multisampled targets are drawn with one sample.

## What is in here

- Fixes the title needs to run: kernel waits and module loading, network, SSL and entitlement functions, save data sizes, depth and DCC handling.
- A port of the pipelined draws draft ([#1201](https://github.com/boykopovar/AnyPS5/pull/1201)): one thread prepares draws while another records them. Off unless `APS5_PIPELINED_DRAWS=1` is set.
- Draw path speed work on top of it: an allocator cache for the driver, a compiled resource walk, per-shape reuse of what a draw prepares and records, pooled per-draw storage, a cached draw key and only the changed Vulkan state recorded per draw. Each has an environment switch that turns it off, and most have one that checks it against the original path.

## Building and running

Build as upstream describes in the [build instructions](docs/dev/BUILD.md) and convert the title as in [Usage](docs/user/USAGE.md). This branch was built with WinLibs GCC 16.1.0; upstream supports 15.2.0. Run the converted executable with `APS5_PIPELINED_DRAWS=1` for the figures above.

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
