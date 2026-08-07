# GBAStation_flycast

GBAStation 生态的 Dreamcast / NAOMI / Atomiswave 模拟器核心（Nintendo Switch 移植）。

## 构建

需要 sibling `switchVK` 仓库和 devkitPro（devkitA64）。

```bash
# 普通完整编译
bash build_local.sh

# 清理后完整编译
bash build_local.sh --clean

# 指定线程数
bash build_local.sh -j 8
```

产物：`GBAStationFlycastStub.nro`（仓库根目录）。
