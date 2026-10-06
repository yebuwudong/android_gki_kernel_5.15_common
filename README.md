# android_gki_kernel_5.15_common

基于 **hfdem** 内核基线，合并 Android GKI 上游源码的自定义 Android 内核。内核版本命名保持 GKI 官方默认格式（无定制后缀），便于与原厂镜像兼容及规避基于版本串的特征识别。

- 基线分支：`android13-5.15-lts-2026-07`
- 上游：Android Common Kernel `android13-5.15-lts`（R7.6 完整合并上游 57 提交）
- 当前版本：Linux 5.15.216（Android 13 GKI）
- 内核版本串：`5.15.216-android13-8-g<hash>`（GKI 原生格式，带构建 commit 短哈希，可区分版本且无第三方标识）
- 构建：LLVM=1（Clang），ThinLTO

## Droidspaces 容器支持

按 [Droidspaces-OSS 官方 GKI 配置指南](https://github.com/ravindu644/Droidspaces-OSS/blob/main/Documentation/Kernel-Configuration.md) 配置：

1. **SYSVIPC kABI 补丁**（必须）— 已合入 `include/linux/sched.h`：`task_struct` 的 `sysvsem`/`sysvshm` 从原位迁入 `ANDROID_KABI_RESERVE(6)`/`RESERVE(7)+(8)`，开 `CONFIG_SYSVIPC`/`CONFIG_IPC_NS`/`CONFIG_POSIX_MQUEUE` 不改变既有字段偏移，原厂 vendor 模块可继续加载。本树 `RESERVE(1)` 已被 user_dumpable 占用，补丁使用的 6/7/8 未占用，官方 `001.GKI-below-6.12-fix_sysvipc_kabi_6_7_8.patch` 可直接套用。
2. **gki_defconfig 直改**（GKI 不走 fragment）— 已按官方清单启用：`SYSVIPC`、`POSIX_MQUEUE`、`IPC_NS`、`PID_NS`（原为 not set）、`DEVTMPFS`、`NETFILTER_XT_MATCH_ADDRTYPE`，及推荐项 `USER_NS`、`NETFILTER_XT_SET`、`TMPFS_XATTR`、`TMPFS_POSIX_ACL`。`IP_NF/IP6_NF_TARGET_REJECT`（UFW）上游已内建。注意官方文档中 `NETFILTER_XT_TARGET_REJECT` 是旧内核符号名，5.15 的实际符号为 `IP_NF_TARGET_REJECT`/`IP6_NF_TARGET_REJECT`。
3. **验证** — 刷机后在 Droidspaces 应用 Settings → Requirements → Check Requirements，或终端 `su -c droidspaces check`。
4. **不要开 `CFS_BANDWIDTH` / `CGROUP_PIDS`（会导致 bootloop）** — 官方指南的 CPU/进程限额组做过实测：这两个选项会重排调度器与 cgroup 结构体，改变 **4101 个导出符号的 CRC**。原厂 vendor 模块按旧 CRC 编译，随即以 `disagrees about version of symbol` 拒绝加载，显示/存储/Wi-Fi 驱动缺失导致设备 bootloop（连 recovery 都进不去）。与 `SYSVIPC` 不同，新字段塞不进 kABI 预留空间，**没有补丁可解**。唯一安全路径是从同一源码树重编**全部**内核模块并同时刷 `boot.img`+`vendor_boot`+`vendor_dlkm`+`system_dlkm`——本设备 vendor 模块绝大多数为预编译，做不到。也**不要**靠关闭 `CONFIG_MODVERSIONS` 或强制加载绕过：结构体确实变了，模块会读到错误偏移。代价是 `--cpus` 与 `--pids-limit` 不可用（Droidspaces 会把这两个开关置灰），其余功能不受影响。

## 修改与移植特性

### 定制压缩算法
- **zstdh** — 定制 zstd（1.5.7 魔改，符号重命名 ZSTD_→ZSTDH_），5 项优化：
  - 压缩哈希表复用（zram 逐页提速约 26%）
  - 解压 decodeSequence AArch64 优化（上游 #4418 + #4509）
  - Huffman 解压 4-way（#4413）
  - COPY8 优化（#4414）
  - get1BlockSummary 4 路累加（#4429）
  - zram 压缩率 ≈75%（逐页 4KB 场景），比 lz4/lz4hc 好 20-35 个百分点

### 移植的特性（内建）
- **kext_alloc_adjust** — 高阶 DMA-IOMMU 分配标志优化（`android_vh_adjust_alloc_flags` + `android_vh_kvmalloc_node_use_vmalloc` 注册，order > 3 去掉 `__GFP_RECLAIM`，缓解高负载掉帧）。运行时开关 `/sys/kernel/kext_alloc_adjust/enabled`
- **kext_mmap_bypass** — direct-reclaim 节流旁路（`android_vh_throttle_direct_reclaim_bypass`，PF_MEMALLOC 之外放行；收窄到四个 cpuset 组）
- **mi_sw_sync** — 内建 `/dev/mi_sw_sync` misc 设备
- **binder_sched_opt** — binder 调度优化（uclamp 方案）：`android_vh_binder_transaction_received` 触发时为 binder 线程提升 `uclamp_min` 频率下限，`android_vh_binder_restore_priority` 成对恢复。运行时参数 `/sys/module/binder_sched_opt/parameters/{enabled,mode,uclamp_min}`：mode 0 关闭 / 1 uclamp floor（默认）/ 2 RT 提升（历史行为 A/B），uclamp_min 默认 256；诊断接口 `/proc/binder_sched_opt_status`
- **kshrink_slabd** — 异步 slab 回收（VIP 回调合并 + 频率感知强度，默认关闭）
- **kshrink_lruvecd** — 异步 lruvec 回收（15 项重构，按 THP 页数硬上限）
- **kext_audit LSM** — 模块加载阻断（前缀匹配 binder_prio/moon_/kshrink_，避免与内建功能重复注册 hook）
- **mi_rmap_efficiency** — 高 mapcount 页保护
- **unionpower** — 帧卡顿检测引擎
- **ipset** — netfilter 集合框架（由 fragment 改入 gki_defconfig 持久化）
- **BBR** — TCP 拥塞控制（系统默认，同上持久化）

### 可选模块（默认关闭）
OS4 移植 wave1+wave2 合入，全部 `default n`，需手动开启 CONFIG：
- **kext_yield_penalty** — yield 换帧边界睡眠（uid 护栏）
- **kext_qos_inherit** — futex 等待链 QOS 继承（uclamp 兜底）
- **kext_rtload** — RT 策略请求统计（kprobe）
- **kext_unfairmem** — SF/场景任务水位放宽（默认 LOWER 方向）
- **kext_mi_reclaim** — 回收路径纯观测（零行为修改）
- **kext_scene_swappiness** — 场景 swappiness（默认 passthrough，不覆盖附加模块）
- **kext_dynamic_readahead** — uid 预读窗口
- **kext_rss_monitor** — RSS 探测
- **kext_bootmonitor** — boot 锚点 + console-ramoops 持久化
- **MI_BOOT_TIME** — 开机耗时统计（开源树逐字移植）
- 另含 4 个新 vendor hook（mm/sched/vmscan）与 `task_work_add` 新增 EXPORT

### 修复与调整
- binder_alloc `kcalloc` → `kvcalloc`（8G 大 binder 缓冲避免 OOM）
- 修复 boeffla wakelock blocker 三处 1 字节越界写（崩溃审计发现：`len >` → `>=` 两处 + 匹配缓冲 52→53）
- 移除 DRM encoder clone 校验（修显示黑屏）
- 修复 LZ4 MIN/MAX 宏冲突（Clang 兼容）
- MLGO regalloc advisor release→default（Debian clang 19 兼容）
- should_be_protected hook 签名升级
- 关闭 F2FS lz4hc（`/data` 分区未用压缩，lz4hc 变体无意义，lz4 保留）

## 命名说明

所有自研/移植功能模块统一使用 `kext_` 前缀（kernel extension），配置符号统一 `CONFIG_KEXT_*`，文件名 `mm/kext_*.c`、`kernel/sched/kext_*.c`、`security/kext_audit_lsm.c`，无定制内核品牌字样；版本串保持 GKI 官方默认 `5.15.216-android13-8`（`CONFIG_LOCALVERSION` 未设置）。历史 revision 标签（R6-R8）仅存在于 git tag/分支命名，不进入内核镜像。

## 来源说明
- **参考社区方案**：binder_sched_opt（社区 Binder 调度思路为启发起点，独立实现并经多轮演进为 uclamp 方案）
- **小米 MiCode piano 移植**：kshrink_slabd、mi_rmap、unionpower
- **OnePlus kswapd_opt 移植**：kext_alloc_adjust（alloc_adjust_flags + kvmalloc_adjust_flags）
- **OS4 移植**：kext_yield_penalty 等 8 模块 + kext_bootmonitor + MI_BOOT_TIME（默认关闭）
- **Droidspaces-OSS**：容器支持的 GKI 配置清单与 SYSVIPC kABI 补丁
- **原生改造**：mi_sw_sync
- 详细分类见 `kernel-customizations-docs` 分支

## 构建

```bash
make O=out ARCH=arm64 LLVM=1 -j$(nproc) Image
```

工具链：Android Clang。刷机包使用 AnyKernel3，仅替换 boot 分区 Image.gz。

## 致谢
- **@hfdem** — Android 13 GKI 基线维护者
- **@Amktiao（酷安）** 及模块来源贡献者
- **小米 MiCode piano** — 特性移植来源
- **OnePlus OSS** — kswapd_opt 优化来源
- **OS4** — 可选模块移植来源
- **ravindu644/Droidspaces-OSS** — 容器内核配置参考
- Android Common Kernel 上游
