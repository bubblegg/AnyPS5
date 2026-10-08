# Architecture

How a PS5 executable becomes a native Linux or Windows program. Nothing is emulated: the converted executable runs as a normal process and calls native implementations of the system libraries.

## Overview

```mermaid
flowchart LR
    subgraph input["PS5 game"]
        elf["input.elf"]
        mods["sce_module/*"]
    end

    subgraph relinker["relinker (core/relinker)"]
        direction TB
        intel["--to-intel:<br/>lower AMD-only instructions<br/>(codegen)"]
        pipeline["RelinkerPipeline:<br/>read imports by NID,<br/>check syscalls, filter unused NIDs,<br/>build SysV dynamic section"]
        guest["GuestModuleBuilder:<br/>convert bundled modules"]
        patcher["LinuxElfPatcher / WindowsPePatcher"]
        intel -.-> pipeline --> guest --> patcher
    end

    subgraph build["build (core/libs)"]
        direction TB
        prx["core/libs/prx/*<br/>shared libraries"]
        nid["nid_patcher:<br/>rename exports to their NIDs"]
        prx --> nid
    end

    subgraph out["Native program"]
        app["app.elf / app.exe"]
        app0["app0/sce_module/*"]
        libs["libs/*.prx"]
    end

    elf --> intel
    mods --> guest
    patcher --> app
    guest --> app0
    nid --> libs
    app -- "OS loader binds imports by NID" --> libs
    app0 -- "OS loader binds imports by NID" --> libs
```

- [`core/relinker/main.cpp`](../../core/relinker/main.cpp) runs the steps in this order. `--to-intel` is optional, see [USAGE.md](../user/USAGE.md).
- Each library in [`core/libs/prx`](../../core/libs/prx) builds as a shared library. After the build, `nid_patcher` ([`core/libs/nid`](../../core/libs/nid)) renames every export to its NID, computed from the function name. `APS5_EXPORT("<nid>", func)` sets the NID directly when the name is unknown.

## Graphics

```mermaid
flowchart TB
    subgraph init["Game initialization"]
        direction LR
        create["libSceAgc:<br/>create shader,<br/>link stages,<br/>primitive state"] --> reg["libSceAgcDriver:<br/>RegisterShader,<br/>Resolve*Abi"]
    end

    subgraph recompiler["core/shader/recompiler"]
        direction LR
        dec["RdnaDecoder"] --> cf["ControlFlow:<br/>graph + structurize"]
        cf --> tr["Translation:<br/>RDNA to IR"]
        tr --> opt["Optimization:<br/>SSA, resources,<br/>bindings"]
        opt --> spv["SpirvBackend:<br/>emit SPIR-V"]
    end

    subgraph frame["Each frame"]
        direction LR
        game["Game:<br/>command buffers"] --> submit["libSceAgcDriver/Submit<br/>DCB / ACB"]
        submit --> pm4["Execution/Pm4:<br/>state, draws,<br/>dispatches"]
        pm4 -- "user data +<br/>descriptors" --> inv["Invocation:<br/>specialization constants,<br/>descriptor heaps, BDA"]
        inv --> vk["libSceAgcDriver/Graphics:<br/>Vulkan pipeline"]
    end

    init -- "code + static ABI" --> disk{"Compiled artifact<br/>on disk?"}
    disk -- no --> recompiler
    disk -- yes --> art["Prepared artifact:<br/>SPIR-V with no<br/>resource values"]
    recompiler --> art
    art -- "looked up at<br/>draw / dispatch" --> frame
```

- Shaders are compiled when the game registers them, not when they are first drawn. [`ShaderRegistry.cpp`](../../core/libs/prx/libSceAgcDriver/Execution/src/Driver/Shaders/ShaderRegistry.cpp) runs `Driver::RegisterShader` for every shader the game creates, and `Resolve*Abi` when libSceAgc links stages or sets the primitive state. Each one compiles the artifact for that static ABI (stage, wave size, push constant layout, linked stages).
- An artifact does not depend on the resources bound at draw time. The shader reads them at runtime, as on the PS5: buffers, images and samplers from descriptor arrays and typed heaps indexed at runtime, and user data from the shader data block defined in [`RuntimeAbi.hpp`](../../core/shader/recompiler/RuntimeAbi.hpp).
- At a draw or dispatch, the driver looks up the prepared artifact and fails if there is none. Only compute shaders the game never registered are compiled at that point. The draw supplies the invocation: the specialization constants listed in [`PipelineSpecialization.hpp`](../../core/shader/recompiler/PipelineSpecialization.hpp), the descriptors and the push constants. Each distinct set of constants produces one specialized SPIR-V module, kept in memory.
- [`Recompiler.cpp`](../../core/shader/recompiler/Recompiler.cpp) runs the stages in this order. With `ANYPS5_ENABLE_SPIRV_TOOLS`, the SPIR-V is also validated and optimized with SPIRV-Tools. `ShaderDiskCache` stores the artifacts on disk so later runs skip the recompilation.
