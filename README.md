<picture>  
<source media="(prefers-color-scheme: dark)" srcset="https://i.imgur.com/8qsV6MH.png">  
<source media="(prefers-color-scheme: light)" srcset="https://i.imgur.com/4cpzGnB.png">  
<img src="https://i.imgur.com/8qsV6MH.png" width="200">  
</picture>  

*Part of the GBAStation ecosystem* — https://www.GBAStationverse.com

**Flycast** is a widely used emulator for the Sega Dreamcast, NAOMI, Atomiswave, and other related arcade systems, known for its accuracy, performance, and active development.

This port is exclusive to GBAStation, adapted to work with its frontend and runtime, and provided as a standalone build for the Nintendo Switch. It focuses on integration, consistency, and predictable behavior within the GBAStation ecosystem.

----------

## Summary

This port focuses on making Flycast fit naturally within GBAStation, rather than behaving as a separate application.

It adds:

-   Custom overlay matching GBAStation design, including time, date, user avatar, and game title
-   Explicit control over display (integer scaling and aspect ratios)
-   Runtime-selectable rendering filters
-   Built-in save and load state support
-   Controller mapping aligned with GBAStation input conventions, including VMU and arcade stick emulation

----------

## Credits

This port is built on top of the official Flycast emulator project.
All core emulation work belongs to the Flycast team and its contributors.

- **Official Flycast repository** — https://github.com/flyinghead/flycast
- **Flycast website** — https://flyinghead.github.io/flycast-builds/

----------

## A Note

A lot of work in this scene disappears over time — not because it lacked value, but because it was never shared.
If you are building something, consider releasing it. Even small contributions can help others move forward.
----------

## Build (Nintendo Switch)

Requires a sibling `switchVK` checkout and devkitPro (devkitA64). The output
`GBAStationFlycastStub.nro` is written to the repository root.

```bash
# 普通完整编译
bash build_local.sh

# 清理后完整编译
bash build_local.sh --clean

# 指定线程数
bash build_local.sh -j 8
```

`build_local.sh` picks the SDK from `SWITCH_NVK_ROOT` (or the sibling
`switchVK/nvk-switch-26.1.4` directory) and drives `build_flycast_nro.sh`.
The same build can be run through CI by pushing a `v*` tag, which builds the
release NRO against the latest published switchVK SDK.