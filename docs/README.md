# Android / FEX 开发资料索引

- **最新直接实现（2026-09-13）**：[WP1复核与原生Turnip／Session图形接入](validation/android-native-host/wp2-native-turnip-runtime-review-2026-09-13.md)。普通APK实际Turnip/WSI、guest VideoOut与会话寿命已接通；修复provider/信号/发布准入及线程退出竞态。真实选取TMNT的新边界为RegisterBuffers；非可玩/Swan验收。该记录优先于下文历史“CPU smoke／尚无图形接入”描述。


**最新直接修复（2026-09-13）：[guest libc 启动、时间/mutex/cond/TSD 与真机验证](validation/android-native-host/libc-runtime-repair-2026-09-13.md)。** 修复漏调_malloc_init和getspecific误绑，真实六模块初始化完成；普通APK16代、CLI10组×3、services43/0、FEX242/0。TMNT当前是具名sysmodule缺口，尚无游戏画面/Swan验收；继续同一份[TMNT整版spec](specs/android-native-host-tmnt-after-runtime.md)，不再重做已接通部分。

**最新（2026-09-13）：[生产运行时复核、修复与 APK 验证](validation/android-native-host/wp1-mechanism-review-2026-09-13.md) → [交给 Opus4.8 的 TMNT 整版 spec](specs/android-native-host-tmnt-after-runtime.md)。** 生产 Module/Linker/VM、guest线程/TLS、HleScope/可取消回调已在本轮接通，普通APK12代通过；下一步补游戏必需Orbis HLE、Turnip和平台服务，推进真实游戏。复现用[canonical证据入口](validation/android-native-host/2026-09-13-wp1-review/README.md)。

下列“最新/当前”均是对应日期的历史记录；旧“未接输入/仍SDL/CPU smoke only/缺生产运行时”状态由上述入口取代，勿据此重复实施。

- **最新施工顺序（用户指定）：[Foundation 输入优先实施补充](specs/android-foundation-input-first.md) → Android SDL 清理 → 生产 Runtime/真实 PKG。** [本次复核](validation/android-native-host/foundation-input-review-2026-09-12.md)确认手柄和JNI尚未接通，并纠正“设置依赖不可拆”和静音麦克风假设。共用输入拟下沉Foundation `modules/input`，Android采集采用可注入Kotlin library，JNI与Orbis语义留主仓；当前仅规划，下一位AI连续实施，不按控件交回微型任务。

- **最新交付：[host `.so` 里程碑 / `1149e948`](validation/android-native-host/host-library-milestone-2026-09-12.md)。** 真实共享STL/Foundation ON的NDK host链接成功；AYN61/0、macOS42/0。复用正式构建脚本和window/control接口，保持已修异常/timeout/Android显式目录初始化。下一位AI执行[生产Runtime整块spec](specs/android-native-host-production-runtime.md)：最终JNI/FEX/VM/HLE/callback、Turnip、Android平台去SDL、**从citron迁移手柄**、输入音频及真实PKG APK。手柄本轮未实施；现APK仍CPU smoke，host库尚含SDL且不含FEX/session接线。[原始证据](validation/android-native-host/2026-09-12-host-library/README.md)区分首次装载失败和修复后通过，不作为APK/Swan验收。

- 已有架构与依赖：[bionic 迁移两工作包方案](specs/android-native-host-full-link-plan-2026-09-12.md)，[依赖前置与 citron FFmpeg 核查](validation/android-native-host/bionic-prerequisites-2026-09-12.md)、[三方库归属与复用清单](validation/android-native-host/bionic-third-party-audit-2026-09-12.md)。

- 当前进展：[COMMON/bionic复核与Git交付](validation/android-native-host/pkg-v2-common-review-2026-09-12.md)：主线已推送，两个COMMON边界已修复，正式真机回归7+7+50项通过；继续PKG v2的正式host全链接与Turnip/游戏整合。

- 最新增量：[Vulkan/NDK 复核与修复（2026-09-12）](validation/android-native-host/vulkan-review-2026-09-12.md)：104个ARM64图形对象、acquire/脚本修复、Turnip默认策略；继续[PKG v2整版](specs/android-native-host-pkg-v2.md)。

**当前整版目标：[完整 PKG 最新复核（0f4fd74b）](validation/android-native-host/full-pkg-review-2026-09-12.md) → [PKG v2 执行 spec](specs/android-native-host-pkg-v2.md)**。连续补齐真实 TMNT 本体＋1.08更新的生产加载、FEX/Orbis、图形/输入/音频，推进到真机可操作场景、十分钟和同进程 Stop/重启。该次复核测试生命周期767/0、runner23/23、JVM109/0；当时边界反例见 [证据](validation/android-native-host/2026-09-12-review/README.md)。现有 APK 仍是 CPU smoke；早期小阶段计划不再作为交付终点。

当前目标：Swan / Android 16 / ARM64 / **4 KiB**，原生 shadPS4 host + FEXCore 执行 PS4 x86 guest。用户于 2026-09-08 将 16 KiB 工作后置。尚未完成完整 NDK backend/app 验收。

**历史入口（2026-09-11）：[Android 原生 host 评估](android-native-host-assessment-2026-09-11.md) → [执行 spec](specs/android-native-host-v1.md) → [本次证据](validation/android-native-host/2026-09-11/README.md)**。被审 `9ac6c300`，origin `0e10defc`，领先36提交。现有主仓 Kotlin/PKG native/JNI FEX 冒烟 APK，尚未接真实 loader/HLE/renderer。runner23/23；Android JVM tests 编译失败；真实 PKG 导入和最终 UI 启动未闭环。先修会话/allocator/API profile，再交付真 ELF→主仓 loader→真实 Orbis HLE→return；后续完成线程/callback、原生 WSI、guest renderer、input/audio 和 Swan 验收。

**二周目仍在进行：[进度记录](validation/round2/progress.md)**。此前 [N4复核](validation/round2/g3-n4-review-2026-09-11.md) 基于 `e0693faa`；其基础 runner/gate、RCX、native FP/Invoke catch 已有后续修复。完整 [E1/E2/E3/A1](specs/android-fex-round2-n4-to-apk.md)、[G3/H3](specs/android-fex-round2-g3-repair-h3.md)、G2 Q1–Q3 和 Swan 普通 APK G4 尚未整体验收。新的 host graphics 里程碑不改写旧 Round 2 范围。

**历史一周目状态：[一周目已提交并推送 / `85b57cb2` / V0_IN_PROGRESS](baselines/2026-09-08-round1-closeout.md)**。修复及证据见[事务加固报告](validation/v0/transaction-hardening-2026-09-08.md)：Swan contract 34/34、guest 45/45，runner 测试 6/6。历史 runner 输出 11 PASS / 0 FAIL / 46 NOT_RUN（57 在范围内、3 延期），仍需按环境和覆盖校正，尤其 B02 的独立 ELF 不是 APK 全库验收。该历史里程碑没有 app/ART 验收。

**固定基础版本：[2026-09-07 / `a7128893`](baselines/2026-09-07-android-fex-foundation.md)**。该记录包含主仓与七个 references 的精确提交、验证结果、未完成项、未纳入基线的本地改动及恢复方法；后续里程碑以它为起点。

**下一个实施目标：[二周目 spec](specs/android-fex-round2.md)**，直接转交[执行 AI 任务书](specs/android-fex-round2-handoff.md)。G0–G4：证据/构建、运行控制、跨线程事务、真实 HLE/callback、最小普通 APK，共 24 项验收。有限 Step、Vulkan 显示与完整游戏集成后续推进。二周目是规范要求，局部 CLI 通过不是完整验收状态；[V0 总 spec](specs/android-fex-v0.md)、[CPU API 契约](specs/android-fex-v0-api.md)和[验收矩阵](specs/android-fex-v0-acceptance.md)保留完整目标。

## 既有阶段记录

- [最初实施报告](validation/v0/implementation-report.md) 的 `V0_BLOCKED`、21 PASS / 0 FAIL / 39 NOT_RUN 是旧轮次输出；部分 host 单测被映射到完整验收项，不能作为当前完成比例。
- [FEX host page 适配](fex-host-page-size-adaptation.md) 记录早期 guard/InterruptFaultPage 调整。用户已将 16 KiB 工作后置；host 探针和页大小数学检查不代表 Android 16 KiB app 验收。
- [Android bionic 构建](fex-android-bionic-build.md) 记录初始化与线程生命周期；其“未执行 guest / adapter 未实现”是当时状态。
- [Guest 执行接入](fex-guest-execution-bringup.md) 记录后续真实执行、配置顺序及共享缓存失效修复，以最新复核证据为准。

## 推荐阅读顺序

实施前先读 [子仓归属与开发分支](subrepository-ownership.md) 和 [Foundation 接入记录](foundation-integration.md)。
Foundation 最小构建入口已接入；反射/网络闭包与 Android 16 运行仍待 V0 验证。

| 文档 | 解决的问题 |
|---|---|
| [Android 与 ARM64 C++ 整合审计](android-arm64-integration-audit.md) | 哪些接口已有实现、哪些需要适配、哪些阻碍目标运行；优先读 |
| [Android 16 原生 guest/host 方案](fex-android16-native-guest-host-plan.md) | 架构、16 KiB、地址空间、HLE、回调、原子访存、GPU 与实施门槛 |
| [Android 基线来源与选择](android-foundation-selection.md) | Android 工程出处、版本和源码不一致、构建入口 |
| [FEXCore 与 Dynarmic 源码比较](fexcore-dynarmic-source-comparison.md) | 源码规模及统计口径、API 与依赖差异 |
| [FEX host page 适配](fex-host-page-size-adaptation.md) | 三种页大小的区分、两处必经 4 KiB 假设的故障机制与修复、16 KiB 可行而 64 KiB 越界的原因 |
| [FEX Android bionic 构建](fex-android-bionic-build.md) | NDK r29 的 `atomic_ref` 硬前置、五处平台适配、两个未文档化的嵌入方义务、实机初始化证据与边界 |
| [Guest debugger 可行性](fex-guest-debugger-feasibility.md) | FEX 现有调试能力、协议缺口与执行状态接口 |
| [LLDB host → guest 工作流](fex-lldb-host-guest-workflow.md) | 安全点/异步 stop、寄存器来源、地址和反汇编关联 |
| [Winlator / WinNative / GameNative 开源项目审计](winlator-winnative-gamenative-audit.md) | ARM64EC、Wine/FEX 分层、UnixLib 的实际范围、Android 平台复用及游戏兼容性证据 |
| [Vortek / Gladio 图形桥接审计](vortek-gladio-graphics-bridge-audit.md) | Vulkan 与 GL→GLES 分工、client/server、AHB 呈现、BC 解码边界及原生 renderer 的复用取舍 |

这些文档以 2026-09-07 检出的源码为依据；版本、行号和能力判断需要随代码更新复核。仓内链接可随 checkout 使用；跨子模块文件在 GitHub 上若无法直接展开，可从 [references 索引](../references/README.md) 的精确提交进入。

## 源码与可复现检查

- [references/README.md](../references/README.md)：固定版本与初始化说明。
- [Android 基线锁](data/android-foundation.lock.json)：来源与实际验证边界。
- [C++ 传输测试脚本](../scripts/analysis/run_android_arm64_contract_tests.py)：18 项 host C++ 测试；不替代 Android/FEX/GPU 实机测试。
- [源码计量脚本](../scripts/analysis/compare_cpu_core_size.py)：使用固定 Git revision 的源码文本统计。
- [证据索引](data/README.md)：远端快照、差异与测试记录。

## 既有相关资料

- [shadPS4 / citron 架构对比](shadps4-citron-architecture-comparison.md)
- [PSVR 游戏列表](psvr-games-list.md)
- [PKG → ZAR 工作流](pkg-to-zar.md)

Beat Saber 的 PS4/PSVR 兼容性是独立后续目标。先验证非 VR guest 的执行、显示、输入、音频和生命周期，再推进 tracking、双眼呈现及 VR 时序。

- [2026-09-13：guest 图形提交／存档直接集成、设备回归与 TMNT 真实边界](validation/android-native-host/graphics-storage-integration-2026-09-13.md)
