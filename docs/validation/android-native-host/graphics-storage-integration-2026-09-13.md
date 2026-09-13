# Guest 图形提交、存档和信号量直接集成（2026-09-13）

本轮在 `7a62511b` 上直接实现，没有新增执行 spec。已贯通真实 x86-64 测试 guest → checked GNM/VideoOut → native Turnip → Android Surface 的四次翻页，并在同一普通 APK 进程完成三轮。存档已验证跨进程、APP_VER 更新读回及实际 Compose Cancel。**TMNT 仍未出游戏画面：已越过原 VideoOutRegisterBuffers 边界和本轮发现的 sem_init，当前三次稳定停在 `pthread_attr_init` (`wtkt-teR1so#libScePosix#1#libkernel`, op280)。不能称为完整游戏运行或全部图形兼容通过。**

证据：[最终 APK manifest](2026-09-13-graphics-storage/apk-final/manifest.json)、[host](2026-09-13-graphics-storage/host-final/result.json)、[CPU](2026-09-13-graphics-storage/cpu-final/manifest.json)。测试发生在提交前，身份保留 `7a62511b + dirty`、逐文件 SHA、DSO Build ID；不回填成提交后的源码身份。FEX 子仓 `385a0cc4`、Foundation `5388ef45` 未改。原有 dirty Bachata-S4 和未跟踪 dear_imgui 保留。

## 完成的实现

- `guest_graphics_hle` 使用显式 NID/参数政策，校验 guest 输出容量、寄存器输入、字符串、DCB/CCB 数组与命令范围。补齐常用 GNM 编码、四种提交、compute queue、event queue、VideoOut 注册/标签/状态/翻页/关闭；未覆盖的入口继续具名拒绝。嵌入 shader 和 VideoOut labels 使用会话 guest 分配，命令不携带 native shader/label 地址。
- 提交时复制命令并将所有权交给 Liverpool 协程；任务句柄由 RAII 释放。native HLE 在入队前取得执行准入，VM transaction 在 CPU quiesce 后再 drain GPU，防止空闲检查后发生晚到提交并与 backing 替换竞争。renderer 初始化会重放已经映射的数据区域。
- GPU watch 与 guest VM 的基础权限分别记录；tracking 不恢复已解除映射的页、不授予执行权限，也不把 readonly 变成 writable。parent FEX 仅在自己的 interrupt-page 分发后转交受管 ACCERR，保留 errno 和原 ART/FEX 转发。passive SignalDispatch 不安装新的 OS handler。回调撤销等待在途 reader；未改 FEX 子仓。
- Session 持有 VideoOut/IRQ/equeue，关闭后的 equeue 分配保留到 worker 退休，避免 IRQ 持有悬空对象。Stop 唤醒提交、帧获取、vblank/事件等待；异步 GPU 错误传回 session。修复 pitch、固定 IRQ 槽的并发注册，以及成功/suboptimal present 与“swapchain 是否可复用”混淆。
- `GuestStorage`、checked storage HLE、`GuestSaveDialog` 实接生产 Session/JNI/Compose。存档按稳定 user/title 保存，使用注入的 SaveInstance/mounts；guest fd、路径、nested pointer 不直接透传 desktop 全局服务。覆盖 Mount/参数/IO、同步 backup/check/restore/delete/event 和实际 TMNT 导入的 save 函数；输出在变更前校验，备份恢复以 staging + 原子 exchange 发布。元数据写入同步；异常保留损坏标记，read-only 不回写。PSF 解析改为有边界的事务式失败，坏文件不再 ASSERT 终止进程。
- 新增 session-owned POSIX/pthread semaphore 域，校验句柄、计数溢出、超时、取消和 errno；guest handle 指向 guest 分配而非 native semaphore。等待不保留 pin，活动 waiter 阻止销毁。它实际越过 TMNT 的 `sem_init` 边界。

## 验证结果与边界

设备是 **AYN Thor / API33 / ARM64 / 4096-byte pages / 普通 APK UID10157**，不是 Swan/API36。

| 测试 | 最终结果 | 证明范围 |
|---|---|---|
| APK synthetic graphics | 6 同 PID generations；3 个 gpu-flip 各 `guest_presents=4` | FEX 写双缓冲、native command/prepareFlip、GPU watch 后再次 CPU 写入、实际 Surface present、flip event、关闭/重启；夹杂坏地址/坏格式正确拒绝 |
| APK storage write | 3 cases 返回 `0xcafe` | 保存/backup/修改/restore/read、真实 guest save dialog；点击生产 Compose 按钮 |
| APK storage read | 不同 PID，2 cases 返回 `0xcafe` | APP_VER01.00→01.08、稳定 title 存档跨进程读回、真实 UI Cancel 结果返 guest；仅清理测试专属 CUSA99991 |
| TMNT selected-content | 3 同 PID generations，均结构化 Faulted 于 op280 | 边界观察；`guest_presents=0`，**不是游戏 PASS** |
| host native DSO | dlopen + **511/0** | 保存/PSF/对话/信号回调退休/嵌入 shader 地址/信号量等 host 契约 |
| FEX/API 回归 | **244/0**、CPU **46/46**、ABI **14/14**、registry **13/0**、veneer **19/0**、services **70/0**、Session **840/0** | 新的 parent fault 分发未破坏这些已有 CPU/运行时契约；CLI 辅助证据 |

最终 host Build ID `e0d787b7bfe414927bebf0d963c55a673b2dc28b`；APK JNI `9d7ccfcfb7eebc3b36823beb123d6eb3370fd6da`。完整 hashes、PID、generation 和测试出口在 manifest/raw log 中。

图形 fixture 是 guest 填色缓冲加真实 GNM submit-and-flip/事件，不是完整 draw/shader 游戏负载；只有 API 源码绑定的编码器/compute 路径不能自动算真机覆盖。尚未承诺任意损坏 PM4 的进程隔离或挂死 Vulkan driver 的有界析构，原 timeline/waitIdle 仍有驱动依赖。

存档 fixture 模拟同 title 更新内容，不等于 UI base+patch 安装事务验收。部分 SaveDataDialog system message 类型、图标展示/初始焦点仍未完整覆盖，拒绝的模式不能报支持。文件复制循环可取消，底层 fsync 不提供硬实时取消保证。

TMNT 使用[十个文件的选取集](2026-09-13-graphics-storage/selected-content.json)，包括原本体/更新 eboot、模块、param.sfo，没有全量 assets。无游戏字节、APK、DSO 入库。测试专属部署目录已[清理](2026-09-13-graphics-storage/cleanup.json)。

## 失败证据保留与下一实际入口

[historical](2026-09-13-graphics-storage/historical/) 保留首次呈现计数失败、存档 malformed SFO/fixture/root 故障、accessibility 按钮定位失败与 TMNT sem_init 边界。首次图形失败是把 Present 的“无需重建”当作“已经呈现”：suboptimal 下实际成功但计数为0；没有放宽 `>=4` 判据。存档 UI 使用实际 Compose 节点点击后通过；先前直接 JNI respond 不能代替 UI 证据。旧 root 失败没有足够异常日志证明是 Android 父目录 symlink，不能传播这个未证实解释。

继续现有整版目标，直接从 **guest pthread attribute 域及 create 的属性消费**推进：当前 `pthread_create` 明确对非默认 attr 返回 ENOTSUP，不能只实现 attr_init 返回空成功。随后根据实际执行推进其余 Orbis/完整 assets/音频/交互与十分钟运行；原“所有非图形启动完成”不成立。不要重做已通的初始 Turnip/Surface，也不要把临时测试边界包装为新 spec 或游戏验收。

复现入口仍为 `scripts/android/build-host-android`、现有 Gradle hostLoaderConfig 配置。仪器测试 selectors、精确参数与判据见归档的 [APK runner](2026-09-13-graphics-storage/run-delivery-apk.py)；它是本次环境记录，含已清理内容路径，重跑须按 manifest 重新准备自己的选取内容。正式 CPU/host 命令与输出保留在 build/、cpu-final/、host-final/。
