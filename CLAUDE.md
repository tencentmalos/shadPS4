# shadPS4 Android / FEX development context

@AGENTS.md

`AGENTS.md` 是共享事实与工程约束；本文件只提供 Claude/Opus 入口。

## 当前交接（2026-09-13）

最新读[图形／存档实际集成与设备证据](docs/validation/android-native-host/graphics-storage-integration-2026-09-13.md)，它覆盖下方旧状态：GNM→VideoOut→Turnip→Surface 已由 FEX synthetic guest 三轮各呈现四帧；会话 GPU 页保护/FEX fault 分发、命令所有权/VM drain 已接。存档跨进程及 APP_VER 更新读回、生产 Compose Cancel 已过；信号量已接。host511/0、FEX244/0、Session840/0。**真实 TMNT 仍未出画面，当前 op280 pthread_attr_init，不能说所有非图形启动已完成**。下一实际工作是 attr 域和 pthread_create 消费属性，继而完整内容/实际游戏负载；不要重做初始图形接线，不另拆 spec。以下旧“RegisterBuffers 边界／保存未接”的说法作为历史保留，以此为准。

用户补充：本版必须正确支持存档。缺失的 libSceSaveDataDialog 是交互层，libSceSaveData 是持久化服务；二者现已接入，范围与未覆盖模式见最新记录。缺失错误不是完成或延期理由。随真实游戏推进接入会话隔离、guest 指针/文件挂载、稳定用户/标题存档目录与真实 dialog 状态；验证保存→退出→重启读回、更新保留存档及失败/取消，不能假成功或把存档放进临时内容目录。详见当前复核记录的存档补充，不另拆 spec。

最新先读[WP1复核与原生Turnip／Session图形接入](docs/validation/android-native-host/wp2-native-turnip-runtime-review-2026-09-13.md)。本轮直接修复并推进，没有新增 spec：普通 APK 已加载固定 bionic Turnip（实测 shaderInt64=1），四个 hook 仅打包、不静态变成 JNI DT_NEEDED；Session generation 持有窗口/Presenter/VideoOut/IRQ，输入桥接完成后才 PlatformReady。VideoOutOpen/Resolution/SetBufferAttribute/Vblank 已过实际 guest；SetBufferAttribute 正确解第七个栈参数。真实选取 TMNT 的新边界是 sceVideoOutRegisterBuffers（w3BY+tAEiQY, op153），并非游戏画面或可玩验收。

已修 provider readiness、每代用户/系统队列、库准入绕过、PageManager覆盖FEX信号、VM发布与继续执行的准入竞态、线程销毁与发布冲突、内部Pause污染HLE取消源。G48强制33次交错和token下Cancel；FEX子仓保持385a0cc4。passive SignalDispatch只解决OS信号归属，GPU tracking与guest VM/FEX fault delivery仍需真正组合；不得仅扩大allow-set透传桌面缓冲/命令指针。精确测试计数和产物身份见交付manifest。旧“所有非图形启动已完成／尚未绑定Turnip窗口”两种描述都不再作为当前事实。用户要求继续在当前实现推进，不另拆微型spec。

先读[libc 启动与服务直接修复](docs/validation/android-native-host/libc-runtime-repair-2026-09-13.md)：已修 `_malloc_init` 顺序、错误 getspecific NID、session clock/errno、mutex/cond/TSD、heap/VM 启动族及通知后 cond 销毁竞态。最终 AYN services43/0、guest242/0、CLI10组×3、普通APK16代同PID；保留d499219e+dirty身份。真实选取内容已完成六模块DT_INIT，原停在sceSysmoduleLoadModule（现已被上面 WP1 族扩展越过），仍非可玩/Swan验收。不能重复原先“clock_gettime已正确、0x445c5f是精确fault RIP、两libc是同一库”的结论。继续下列整版spec，session sysmodule/provider与平台服务仍需实接；不要再从基础运行时或libc bootstrap重做。

先读[本轮运行时复核与修复](docs/validation/android-native-host/wp1-mechanism-review-2026-09-13.md)，然后连续执行[TMNT整版spec](docs/specs/android-native-host-tmnt-after-runtime.md)。用户明确要求生产 Linker、VM、线程/TLS、可取消HLE回调由当前复核轮完成，**这部分已经实现，不再交给下一位从头接线**。

生产 `GuestRuntime` 通过 `FexSessionBackend`、JNI 和普通 Service 执行 ELF/SELF；Module/Linker/VM/backing、guest pthread/TLS/errno/stack guard、HleScope/两层InvokeGuest/WaitingHle取消都已组成运行时。AYN/API33/4KiB：guest242/0、contract46/46、ABI14/0、registry13/0、veneer19/0；普通APK12个同PID generation覆盖双模块DT_INIT/TLS、子线程运行期间VM、正常/取消/坏指针/未知import/初始化取消及恢复。host dlopen+85/0、macOS现代LLVM contract45+1SKIP/46、Session807/0。原始证据保留07ce52ae+dirty身份；不能把它们说成TMNT已可玩或Swan已验。

下一阶段扩真实游戏的Orbis函数族与session sysmodule/provider，集成Turnip/Surface/AAudio/FEX-origin pad/内容版本事务，最后UI本体+更新、交互场景、十分钟、Stop、同进程三轮游戏。已完成的guest libc启动直接复用，连续推进两个工作包，不在每个NID/库/首帧处重新交回微型规划。

## 必须保留

- 目标Swan/Android16/API36/ARM64/**4KiB**；AYN为辅助，Swan不在线标NOT_RUN；16KiB和PSVR后置。FEX只跑PS4 x86 guest，host是NDK/bionic原生ARM64。
- host DSO唯一提供GuestAddressSpace和Foundation InputHub/OrbisPadAdapter；JNI/FEX导入生成SDK。无需重做单库。Android已无SDL和冲突JNI_OnLoad，桌面SDL保留。
- Foundation owned `codex/shadps4-android-fex-v0` / `5388ef45313d6c32cb5f4bb5b07f1246ee381370` 已push；FEX维持 `385a0cc4`，不绕过子仓指令改动。依赖修改先查owned ref和[归属](docs/subrepository-ownership.md)，子仓先push再pin。
- FEX使用实际rpmalloc+普通mmap/munmap hooks；不steal ART高VA、不替换bionic malloc、不用Windows HookPtrs。guest owns4MiB–256MiB、4GiB–120GiB，ART hole不在账本。exact reserve碰撞可回收失败，不扫描maps后MAP_FIXED。
- 保留主仓EntryBackedgePass：pinned FEX局部条件回边会跳过entry interrupt poll，G47以真实热循环复现。pass只改guest EntryPoint边；不改REP/原子内部循环、不用MAXINST=1掩盖。
- NON-spill退出JIT后执行HLE，scope控制回调；native fenv/errno、outer continuation、fault/cancel/exit归属保留。VM内部park必须真实退出JIT、释放资格；不能清用户Cancel或带pin跨任意callback。
- SELF原ELF节表offset不是容器offset；模块依赖支持modules/与sce_module/。guard是显式guest数据；未知对象拒绝，未知函数具名fault，不写native函数或global地址到guest GOT。
- 通用guest指令异常、非默认pthread/动态TLS/API覆盖仍有实际边界；新调用沿已接好的runtime完善，不把合成check数当全部HLE语义已经完成。
- 默认Android/bionic Turnip，无静默系统driver fallback；实际loader handle/shaderInt64/namespace/Surface寿命必须验。FFmpeg是独立owned子仓，不塞Foundation。
- TMNT01.08是更新；[本体/更新身份](docs/validation/android-native-host/2026-09-12-review/pkg-set.json)。本轮选择了base后overlay的eboot/模块/param.sfo，不含完整游戏assets。不能拿旧01.00 eboot-only冒充完整1.08启动。
- 不提交游戏/PKG/driver/APK/DSO或凭据；保留无关`references/Bachata-S4`和`externals/dear_imgui/`本地工作。所有失败和旧NOT_RUN保留，不能倒写历史Build ID或源码身份。

## 复现与历史资料

[本轮命令与证据](docs/validation/android-native-host/2026-09-13-wp1-review/README.md)；构建入口`scripts/android/build-host-android`、匹配profile FEX、Gradle的`fexBuildDir`/`hostLoaderConfig`；验证入口`scripts/android/validate-production-runtime-android`。源host与APK剥离符号后的Build ID必须相同。

[输入接通复核](docs/validation/android-native-host/runtime-input-review-2026-09-13.md)保留Foundation49/0+Android5、pad45/0、APK input6等证据。旧Stage0“crt到首个HLE已过”已撤回；prologue harness仅LOAD_AUDIT，不重新用作执行门槛。

[完整资料索引](docs/README.md)、[基础版本](docs/baselines/2026-09-07-android-fex-foundation.md)、[V0](docs/specs/android-fex-v0.md)、[Foundation](docs/foundation-integration.md)、[host→guest LLDB](docs/fex-lldb-host-guest-workflow.md)。历史allocator BLOCKED/无HleScope/未接runtime等描述是旧状态，当前实现以本轮复核和AGENTS为准。
