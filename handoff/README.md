# MicroPixel 当前开发交接

更新时间：2026-11-13
当前分支：`codex/function-ev-board`
当前保存版本：`handoff-2026-11-13`

这是给下一位智能体的临时交接文件。开始工作前必须先阅读仓库根目录的
[`AGENTS.md`](../AGENTS.md)，其中包含架构边界、真机安全规则、历史验证记录和更详细的命令。

## 可直接复制给下一个智能体的提示词

```text
你正在继续 MicroPixel 的 ESP32-P4 Function EV / Bear 多媒体开发工作。

仓库位于当前工作区的 micropixel 目录。先完整阅读 AGENTS.md 和 handoff/README.md，然后检查
git status、当前进程、/dev/ttyACM0 是否存在以及 fuser -v /dev/ttyACM0。当前分支已将这一阶段的源码和文档
已保存到 `handoff-2026-11-13`；继续开发时不要 reset、clean、checkout、覆盖或丢弃任何文件，也不要仅按 Host/Guest
目录机械拆分提交。`handoff/diagnostics/` 下的固件、ELF、截图和串口记录只保留在本机，按 `.gitignore`
规则不提交。

项目当前已经具备 Function EV 板级和 Ubuntu 构建基线；正在交付 Voice/System 端到端 ABI、Bear、中文
字体和设置，以及音频、Direct Surface、Wi-Fi、PSRAM、屏幕休眠稳定性修复。Voice 使用服务 ID 24，
System 已升级到 1.1 并增加 OpenSettings。ABI、Device contracts、Host、Runtime、Guest SDK 和 FirmwareApp
组合根必须作为完整合同审核。Voice Gateway 可以作为独立部署单元测试。

当前最重要的未关闭风险是联网语音回复：已配置的 Voice Gateway 在最近一次真机测试中连接超时，没有
产生回复音频，因此真人听音、屏幕蓝闪观察、playback low-water/overflow 和多轮回复测试仍未完成。
Bear 启动后的该次测试出现 3 次 AFE 空环警告。五分钟屏幕休眠/恢复后的完整 Bear 生命周期序列、
语音状态触摸取消和唤醒提示音规则也尚未完成。

不要把短进程反复打开 USB 串口造成的 CHIP_USB_UART_RESET 当作产品崩溃。后续真机测试必须让一个
长连接进程独占真实串口，所有控制命令复用该连接；不要让多个 tools/micropixel 进程反复直接打开
/dev/ttyACM0。如果发生真实异常，先保存匹配的 ELF、原始完整串口日志、panic 和当前固件哈希，再进行
任何重建或刷机。扬声器即将出声时必须先通知用户。

先复核 Voice Gateway 可达性，再执行联网多轮语音回复；同时记录播放 low-water、overflow、AFE 空环、
看门狗、重启和实体屏表现。之后在同一连续 uptime 中完成屏幕休眠/唤醒、Bear 启动、回 Hall、关闭和
再次启动。Wi-Fi 至少再做数次独立启动，记录关联、无 IP 超时、station restart、拿到 IP 的时间。

Codex 负责架构、ABI、并发和内存生命周期、代码集成、构建、真机和最终判断。DeepSeek 只能通过已安装
的 deepseek-delegator Skill 接收经过筛选且不含秘密的文本上下文，用于局部审查、测试设计或文档草稿；
不要让它直接操作文件、串口、硬件、部署或秘密。

完成工作时更新 handoff/README.md 和 AGENTS.md 的临时交接段，明确区分本轮实际执行的测试、历史证据、
仍未验证的内容和所有工具诱发的复位。
```

## 项目目标与架构边界

最终产品是运行在轮腿机器人上的交互式多媒体平台。当前代码主要完成屏幕宠物 Bear、触摸、语音、音频、
设置和底层稳定性；底盘通信、运动控制及与 Bear 的协同行为尚未形成完整合同，不能把“Bear 能运行”描述
成整个机器人项目完成。

依赖方向必须保持为：

```text
Runtime → Device contracts ← Platform
```

- `FirmwareApp` 是唯一组合根。
- Host 独占硬件、系统 UI 和应用生命周期。
- Guest 只能通过 Service ABI 使用能力，不能依赖 ESP-IDF、LVGL、硬件或自行创建平台线程。
- 任何时刻只有一个 `AppSession` 拥有 Guest 运行时。

## 当前工作区状态

- 2026-11-13 已将当前阶段源码、Voice/Bear/System 功能和交接文档推送到
  `origin/codex/function-ev-board`，并用 `handoff-2026-11-13` 标签固定本次交接基线。
- 提交包含 167 个源码和文档文件；固件、ELF、截图和串口记录没有提交，仍在本机的
  `handoff/diagnostics/`。
- 推送后工作区无未提交改动；提交前 `git diff --check` 通过。
- `.env`、pytest 缓存、`__pycache__`、构建目录和硬件诊断产物由 `.gitignore` 忽略。

建议按交付检查点审核，而不是机械规定每个目录一个提交：

1. Function EV 板级与构建基线。该基线已经在分支历史中；当前分区、sdkconfig 和构建文件分别服务于
   Voice、屏幕休眠或稳定性功能。
2. Voice/System 的端到端合同。Voice 服务 ID 24、完整方法/事件表和 System 1.1 OpenSettings 不能拆散。
3. Bear、中文字体与设置行为。
4. 音频/AFE、Direct Surface、Wi-Fi、PSRAM 与屏幕休眠稳定性。只有中间状态能独立构建和测试时才继续拆分。
5. 正式文档与最终交接。

## 已完成或已有验证证据

### Function EV 与构建

- ESP32-P4-Function-EV-Board 板级、构建 profile 和 Ubuntu 迁移基线已经存在。
- 当前固件版本为 `0.9.6`，使用 ESP-IDF `v6.1-1-g812f3e98ca`。

### Voice/System/Bear

- ABI 已增加 Voice 服务 ID `24`、Voice 1.1 方法与事件、System 1.1 `OpenSettings`。
- Host VoiceEngine、Runtime VoiceService、Guest SDK/runtime、CLI Voice Profile 和 Voice Gateway 源码均已存在。
- Bear 已安装到真机，并包含触摸、语音状态、设置页和中文 UI。
- 持续按压约 6.2 秒可以进入 Bear 设置；精确 5 秒释放没有进入。
- 按压时移动约 100 像素会取消设置入口。
- 动画强度从 75% 改为 100% 后跨 Host 重启仍保持 100%，随后已恢复为 75%。
- Bear About 页面可以显示“系统设置”入口。
- 使用单一长连接串口代理时，Host 系统信息页正常显示，没有出现 panic。

### 稳定性

- AFE 持续采集转发问题已修复并做过真机验证。
- Guest PSRAM 最大连续块为零时的保守回退已验证，曾完成 15 次 Bear 启停。
- Direct Surface Destroy 采用 sticky retiring/drain 所有权状态，已经完成 40 次真机生命周期循环：
  20 次直接启停、10 次立即挂起到 Hall 后关闭、10 次前台一秒后挂起到 Hall 再关闭。
- ESP-Hosted Wi-Fi 已增加全信道重试、连接超时和 station restart 恢复。
- 最近的 Wi-Fi 测试中，两个有效启动样本约 35 秒自动联网；另一个在 48 秒时确认已联网。至少一个样本
  经历无 IP 超时、第二次超时后 station restart，最终自动取得 IP。

这些结果只证明所列路径，不证明所有蓝屏、音频或网络原因均已解决。

## 当前问题与下一步

### 1. Voice Gateway 不可达

最近一次联网语音测试中，已配置的网关连接超时，没有下行回复音频。先检查 Gateway 服务、路由和配置
是否实际可达；状态和日志必须保持脱敏，不能输出 URL、令牌、CA、设备身份或 `.env` 内容。

网关恢复后执行至少三轮真人对话，并同时记录：

- 真人听到的卡顿、断音和延迟。
- 实体屏蓝闪、冻结或动画停顿。
- playback low-water 和 overflow。
- AFE 空环警告。
- task watchdog、panic、abort 和 uptime 是否连续。

### 2. AFE 空环仍需解释

最近一次 Bear 启动和失败的网关连接期间出现 3 次 AFE 空环警告。需要判断它们是启动瞬态、音频所有权
切换还是仍存在的供给不足；不能因为此前 AFE 修复通过就忽略新证据。

### 3. 屏幕休眠后的旧崩溃路径未完成本轮回归

等待设置中的真实休眠超时，确认面板关闭并唤醒，然后在同一连续 uptime 中执行：

1. 启动 Bear。
2. 回到 App Hall。
3. 关闭 Bear。
4. 再次启动 Bear。
5. 检查 Direct Surface timeout、panic、watchdog、heap/PSRAM 和 App last-error。

本轮测试在五分钟休眠超时前被中断，不能记为通过或失败。

### 4. 2026-11-12 Bear 启动期间再次观察到堆遍历异常

- 用户说明操作的是 Bear 界面外 Host 提供的停止图标。长连接串口期间观察到一次 Bear 正常启动；另一次
  启动在 `launching selected App: micropixel.bear` 后发生 Core 0 Load access fault 并重启。
- 串口报告的固件 ELF 前缀 `3ec915422` 与 Function EV ELF
  `build/host-esp32p4-function-ev/micropixel.elf`（SHA-256
  `3ec91542255081d8842bbede2dd7ff8f11475b5f82227c77a64a68c5c60cf52d`）匹配。第一次 ESP-IDF monitor
  加载了默认 `build/host-esp32p4/micropixel.elf`，并报告 flashed/built checksum mismatch；它显示的
  `gdma_new_link_list` 符号化是错误的，不可用于诊断。
- 用匹配 ELF 对 panic 地址重新符号化：MEPC `0x402d7220` 位于 TLSF `block_size`，RA `0x402d7238`
  位于 `tlsf_walk_pool`。这与历史 `heap_caps_get_largest_free_block` 堆遍历时触发的异常同类，说明堆元数据
  损坏或堆遍历遇到无效元数据；实际写坏堆的来源仍未知。崩溃发生在 Bear 启动日志之后，符合 Host 启动
  阶段堆快照发现损坏的时序，但尚未从完整调用栈确认调用者。
- 另一次捕获到停止路径：Bear 被挂起到 Hall 后，Host 请求 Guest 合作停止；3 秒后因 Guest 未接受停止而
  强制终止，随后 Direct Surface 销毁、WAMR Guest 清理完成并返回 Hall。当前日志未证明这一步是堆损坏
  的写入源；停止与启动 panic 的先后关联仍不清楚，不能据此归因。
- 未改代码、未构建或刷机。监视器已关闭并释放串口。原始完整 panic 日志未保存为本地文件；下次重现时应在
  同一个长连接会话中将日志写入受限临时文件，且 monitor 必须显式使用 Function EV 的匹配 ELF。

### 4.1 2026-11-12 堆检查诊断固件的后续结果与回退

- 在 `/tmp` 构建并刷入带轻量堆检查的诊断固件后，第一次启动在 `WAMR initialized` 检查点失败重启；第二次
  启动通过 WAMR 初始化及 Hall 绘制检查，但在 `before Hall launch cover retention` 检查点执行堆遍历时，
  `tlsf_walk_pool` 读到无效块头并发生 Load access fault。Bear Guest 尚未开始执行。Hall 绘制后的检查曾通过，
  因而目前只能把损坏窗口缩小到 Hall 绘制之后、启动遮罩/Guest 启动之前的 Host 或异步后台路径；真正的
  写坏者仍未定位。
- 在 `wamr_runtime.cpp` 增加了仅供 `CONFIG_MICROPIXEL_HEAP_CHECKPOINTS` 使用的 WAMR 完整初始化前后检查点。
  这是工作区中的诊断性源码改动，尚未合并；本次也没有改动生产代码或生成新的产品修复。
- 后续全面堆检查诊断版在 Guest 服务调用周围频繁遍历并校验堆，导致 IDLE1 长时间停在
  `multi_heap_check -> verify_fill_pattern`，并触发 Task WDT。用户观察到的连续蓝屏/恢复循环与此诊断版的
  看门狗重启相符。这是诊断开销造成的次生重启，不能作为原始 Bear 崩溃根因的证据。
- 已停止该诊断监视进程，并用 `python3 tools/firmware.py esp32-p4-function-ev flash-built --port /dev/ttyACM0`
  恢复项目原 Function EV 构建固件；esptool 对写入内容的 hash 校验通过并执行硬复位。该操作会覆盖主固件及
  随构建 profile 写入的固件资源分区，但保留 App Store 分区。刷机后没有继续打开 Bear，也没有验证设备启动或
  原始问题是否再次出现；当时串口已释放，没有留下监视器。
- 当前判断：全面检查版引起的看门狗循环已通过回退移除；原有 TLSF 堆元数据损坏和 PSRAM 非法/未对齐读取
  问题仍未解决。下一轮应基于低开销的分阶段记录或更窄的检查点继续缩小损坏窗口，避免在每次 Guest ABI
  调用中扫描整片堆；保留与固件匹配的 ELF 和单一长连接串口记录。

### 4.2 2026-11-12 Bear 打开/暂停时蓝闪复测

- 用户报告：Bear 可以打开且不一定重启，但打开和暂停时仍会短暂蓝闪。v4 诊断串口在 Bear 启动、到
  `WaitEvent` 安全挂起点、Direct Surface 销毁这几段没有对应的 panic；同一诊断固件随后仍出现 TLSF 遍历
  崩溃和看门狗复位，因此不能用它判断蓝闪是否存在于正常负载。
- 已用当前 Function EV 源码构建未启用 `MICROPIXEL_HEAP_CHECKPOINTS`、独立堆追踪或 Light heap poisoning 的
  普通固件。48 MHz 镜像大小 `0x3ef760`，可放入 OTA App 分区。一次 USB 芯片探测复位后的启动，在 Bear 未启动时
  于 `AppRuntime::Initialize()` 的 PSRAM `heap_caps_get_largest_free_block()` 遍历触发 TLSF Load access fault；
  堆栈为 `tlsf_walk_pool -> heap_caps_get_info -> heap_caps_get_largest_free_block -> micropixel_log_heap_state`。
  设备自动软件复位后第二次启动正常到 Hall。这是启动期单次记录，不证明与 Bear 转场有关，也没有定位损坏写入者。
- 将 DPI 像素时钟从 48 降到 44 MHz 作为带宽 A/B 候选。按当前总时序，刷新约从 55.7 Hz 降到 51.1 Hz；44 MHz
  仍高于 EK79007 1024x600 DE 时序资料列出的 40.8 MHz DCLK 下限。Espressif FAQ 将蓝帧/underrun 与外部内存带宽
  联系起来，并建议降低 `dpi_clock_freq_mhz`；这是候选解释，不是本机已捕获的 underrun 证据：
  [Espressif LCD FAQ](https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/peripherals/lcd.html)，
  [EK79007 datasheet](https://dl.espressif.com/AE/esp-iot-solution/EK79007.pdf)。
- 44 MHz 常规镜像构建通过，大小 `0x3ef760`，小于 `0x440000` App 分区；SHA256 为
  `eebd0bb99d96066e4a8de33f7370af696471c51a2143cd95cb55c07c175a7ad5`。只在复核芯片身份后写入 `0x30000` Host
  App，esptool 回读 hash 校验通过；没有写其他分区。启动后到达含 6 个应用的 Hall，没有启动期 panic/WDT。
- 44 MHz 下用 USB 控制启动 Bear 成功；留存串口显示 `surface created: 1024x600`，随后从底部上滑返回 Hall，Guest
  在 `WaitEvent` 安全点挂起，Hall 成功呈现。该次打开/暂停日志没有 panic、Task WDT 或复位。暂停后的 USB 帧缓冲
  截图显示 Hall 中 Bear 为“已暂停”；它不能捕获只发生在物理面板输出路径上的 DSI 蓝帧。

### 4.3 2026-11-12 44 MHz 实体屏确认与 TLSF 故障复现

- 用户在实体屏亲自打开并暂停 Bear，确认这轮两处均没有看到蓝闪。44 MHz 在本轮可见闪屏复测中通过；这是有限次数的
  观察，尚不能排除偶发闪屏。
- 同一长连接串口捕获在 uptime `152599 ms` 记录到 Bear 启动，随即发生 Core 0 Load access fault。匹配 ELF 的符号化
  显示故障指令在 `tlsf_walk_pool -> block_is_last -> block_size`，返回路径经过 `multi_heap_get_info_tlsf`；`MTVAL`
  为无效地址 `0xed87b75c`。部分后续地址落在 LVGL `lv_cache_acquire_or_create` 和 Tiny TTF 字形描述路径，但 panic
  解码未能取得完整回溯，因此不能据此归因到字体缓存或 Bear。
- panic 后串口记录到 `rst:0xc (SW_CPU_RESET)`。设备随后重新启动；之后数轮 Bear 启动、暂停和停止都回到 Hall，直到
  本次检查停止，未再观察到 panic、Task WDT 或 ESP-IDF DSI underrun 日志。TLSF 堆元数据损坏的写入源仍未定位；这次
  重启与蓝闪是两个尚未证明有关联的问题。
- 当前设备运行 44 MHz 普通固件；源码改动在
  `firmware/espressif/main/platform/boards/esp32-p4-function-ev-board/board_hardware.cpp`，仅 Host App 分区 `0x30000`
  被写入。为下次接续保留固件 BIN、压缩匹配 ELF、脱敏串口记录和帧缓冲截图于
  `handoff/diagnostics/bear-44mhz-2026-11-12/`。BIN SHA256：
  `eebd0bb99d96066e4a8de33f7370af696471c51a2143cd95cb55c07c175a7ad5`；解压 ELF 后可用于同版本 panic 符号化。
  这些硬件诊断产物按仓库规范只保留在本机，不提交到 GitHub；README、源码和本段结果会同步。
- 已按要求停止本轮检查并关闭串口 monitor；当前没有持续监测进程。下一轮从上述 ELF 和串口记录继续，优先定位 TLSF
  无效元数据的写入来源，同时用实体屏重复确认 44 MHz 的转场表现。

### 5. 其余 UI/行为回归

- 语音活跃状态下触摸取消/打断。
- 睡眠时第一次触摸可播放唤醒提示；已经醒着时触摸屏幕或 Bear 不能重复播放提示音。
- About → System Settings 的完整 Guest 停止和 Host 设置呈现路径。
- 中文 UI、设置入口取消条件、动画强度持久化和无看门狗/重启。
- 诊断录音、下行音频保存、实时麦克风诊断、许可证详情，以及触摸音效和流式回复偏好是否真正生效。

### 5. 后续安全项

Voice Profile 目前仍以明文存储在 NVS。它是后续安全加固项，不要与 AFE 或生命周期修复混在同一改动中。

## 开发工具

### 固件与 Guest

- ESP-IDF 6.1：`${PROJECT_PARENT}/.toolchains/esp-idf`
- Function EV 辅助脚本：`tools/p4.sh`
- Profile 构建/刷机：`tools/firmware.py`
- 设备、App、日志、截图和输入控制：`tools/micropixel`
- WASI SDK：`${PROJECT_PARENT}/.toolchains/wasi-sdk-33.0-x86_64-linux`
- RISC-V WAMRC：`${PROJECT_PARENT}/.toolchains/wamrc-riscv32-ilp32f/wamrc`
- Host 测试：`bash tools/tests/test_firmware_host.sh`
- Guest/AOT 一致性：`bash tools/build_guest_p4.sh`
- 架构与格式：`bash tools/check_firmware_style.sh --format-only`
- Python 测试：`python3 -m unittest ...`
- Voice Gateway：Python、FastAPI、pytest、WebSocket、Opus；说明见
  [`services/voice-gateway/README.md`](../services/voice-gateway/README.md)。

典型环境变量：

```bash
PROJECT_PARENT="$(cd .. && pwd)"
source "${PROJECT_PARENT}/.toolchains/esp-idf/export.sh"
export WASI_SDK_PATH="${PROJECT_PARENT}/.toolchains/wasi-sdk-33.0-x86_64-linux"
export WAMRC="${PROJECT_PARENT}/.toolchains/wamrc-riscv32-ilp32f/wamrc"
```

若普通 `export.sh` 指向错误的 Python 环境，优先使用 `tools/p4.sh` 已建立的项目工具链环境，不要全局安装
或替换 ESP-IDF 依赖。

### DeepSeek 委派

仓库外已安装 `deepseek-delegator:deepseek-delegation` Skill，工作流是“本地 MCP + Skill”。适合交给
DeepSeek 的任务包括选定 diff 的第二意见、局部测试设计、文档草稿和受限 patch 草稿。Codex 必须：

- 先选择最小必要上下文，绝不发送 API key、token、`.env`、设备标识或原始私密日志。
- 独立核对 DeepSeek 的每个结论。
- 自己应用修改、运行测试和真机验证。
- 保持对 ABI、架构、并发、内存生命周期、硬件和发布的最终责任。

此前 DeepSeek 建议拆成 15 个提交，该建议已被拒绝；也曾误判固定服务注册表和 `.gitignore` 缺失。

## 开发结果与证据位置

### 仓库内构建产物

```text
build/host-esp32p4-function-ev/micropixel.elf
build/host-esp32p4-function-ev/micropixel.bin
build/host-esp32p4-function-ev/micropixel_flashed.bin
build/apps/bear/bear.bundle.bin
```

当前构建/烧录 ELF 的 SHA-256（2026-11-05 18:35 构建，18:46 烧录）：

```text
3ec91542255081d8842bbede2dd7ff8f11475b5f82227c77a64a68c5c60cf52d
```

`micropixel.bin` 的 SHA-256 为
`088e4021fab49e5ba2e5484ac9e88953ef5acf548fbe6d79b08519afd62e49d0`，`micropixel_flashed.bin`
已与之一致。esptool 对写入数据做了哈希校验并硬复位；尚未读取设备启动报告，因此“已烧录”不等于
“已确认启动到 App Hall”。旧的 2026-10-19 固件 ELF
`703f4f9684e24f7d0b30d4b1498ee05f31dd7b6a1b2772d5bf4aa4a0947e26e7` 仅保存在用户私有证据目录，
用于旧日志/旧 panic 符号化。Bear Bundle 仍为 196608 字节（本轮未重建）。

### 用户私有证据

```text
${HOME}/.codex/private/micropixel-risk-20261105/
```

其中保存的是 2026-10-19 旧固件的匹配 ELF、应用镜像、已刷镜像、哈希清单和 `SUMMARY.md`。该目录不能提交。
最近一次测试的原始日志最初存放在 `/tmp/micropixel-risk-20261105`，中断后临时目录消失，因此不能声称
原始日志仍被保存。

历史交接曾引用 `/tmp/bear-dialog-monitor/`，但临时路径可能已不存在；任何使用前必须重新检查。

## 真机操作规则

- 活动板卡：ESP32-P4-Function-EV-Board。
- 常用串口：`/dev/ttyACM0`，每次使用前必须重新确认存在，并运行 `fuser -v /dev/ttyACM0`。
- 真实串口只能有一个所有者。不要同时运行 monitor、logs、短进程 CLI 或另一套自动化。
- 当前环境中，短进程反复直接打开 USB 串口可能触发：

  ```text
  rst:0x17 (CHIP_USB_UART_RESET)
  ```

  这类复位必须标为测试工具诱发，不能当作产品崩溃。连续生命周期测试应复用一个长连接串口会话。
- 扬声器测试即将出声前必须立即通知用户。
- 发生真实异常时停止后续重建，先保存匹配 ELF、完整原始串口日志、panic、截图、uptime 和哈希。
- 不得提交 API key、token、`.env`、Wi-Fi 信息、MAC、设备身份、Voice Profile 或原始串口日志。

## 已完成的软件检查记录

这些是历史记录，不等于本轮重新执行。输入发生变化后运行受影响的检查：

- `bash tools/tests/test_firmware_host.sh`：通过。
- `python3 -m unittest tools.tests.test_font_tools`：18 项通过。
- `python3 -m unittest tools.tests.test_micropixel_cli tools.tests.test_cli_json`：78 项通过。
- `bash tools/build_guest_p4.sh`：Guest conformance 和 RISC-V AOT 通过。
- Bear release AOT/Bundle 构建通过。
- Voice Gateway：19 项测试通过。
- `bash tools/check_firmware_style.sh --format-only`：架构与格式检查通过。
- Function EV 固件曾完成构建、刷写和哈希验证。

在本交接文档创建时，仅重新运行了 `git diff --check`，结果通过；没有重新构建、刷机或播放音频。

## 2026-11-05 维护批次（7 项 + 1 项新发现）

本轮软件/构建/主机测试期间未刷机；2026-11-05 18:46 已按用户要求在 `/dev/ttyACM0` 烧录新版本（见下）。
未播放音频、未访问 Voice Gateway；除烧录与 esptool 校验外，没有观察启动、Wi-Fi 或 Bear 行为。

### 已修改

1. **conformance 与 ABI 版本漂移**：`guest/tests/conformance/dynamic_texture.cpp` 现在依次验证
   过高 minor、过高 major 均返回 `VERSION_MISMATCH`，再用当前 Resource 1.0 打开并继续原有用例
   （新增退出码 109）。删除 conformance/SDK 注释中过时的 Graphics 1.5/1.6/1.7、Audio 1.2 标签。
2. **Bear 设置接线**：`kTouchSounds` 已驱动 listen/curious/excited 三种轻提示，只在 mood 实际变化
   时播放；睡眠/困倦首次触摸仍只唤醒，唤醒音只由 `kWakeCue` 在 Waking 转换播放。`kStreamingResponse`
   改为只读“Host 固定”，不再提供无效果开关；旧存档位仍被接受。README 已同步。
3. **文档**：Network 19 改为“1.0 已实现”，补充方法/事件、能力位、上限和 Profile/AppId/时间前提；
   `docs/README*.md` 索引补齐 4 篇；requirements 预检表述按 `micropixel_app_runtime_compatible`
   修正；架构文档中“Host-owned 纹理和 surface 同样检查安全水位”的旧表述已改为“直接分配、不经过
   Guest 准入”；中英文 SDK 能力目录补上 Network/Voice。
4. **PSRAM 准入审计**：结论是 Host surface/PCM 缓冲使用 Host reserve 属于设计选择，不是 Guest
   准入违反；补充所有权注释，新增 PCM 容量取整测试，并在 `AllocateHostBuffers()` 增加
   `pitch * height` 32 位回绕防护。
5. **DirectSurface 截屏生命周期**：`CaptureFront` 不再把调用者栈上的 `FrontCapture` 放进队列；
   presenter 使用自有 panel 尺寸 staging，调用者超时后可立即释放目标帧，消除 UAF。首次截图会多占
   约一个 panel 帧 PSRAM，直到 presenter Shutdown。
6. **Voice 并发**：`HandleText`/`HandleAudio`/`CancelSession`/`FinishPendingLocalCommand`/`SendAudio`
   都在入口持 `mutex_` 快照 session identity；`cancel_message_` 改为栈上局部数组；encoder/decoder
   在 WS 任务 join 后销毁。剩余窄窗口（`CancelSession` 快照 WebSocket 句柄后 send_text 与会话线程
   `DestroyTransport` 竞争、`DestroyTransport` 不主动 `StopPcm`、AFE 任务未 join）已记录，未扩大改动。
7. **启动图死代码**：删除 `AppSessionError::kLaunchBitmap` 及两处映射；设备层
   `ShowLaunchBitmap/DismissLaunchBitmap` hooks 保持不动。实际启动图仍由 Host UI/Hall 路径实现。
8. **新发现：`RasterService::UploadWarp` 溢出（已修）**：`row_bytes * height` 在 uint32 中回绕，
   可让 Guest 用 32768×32769 的 warp map 只分配约 262 KB，随后 `fill_n` 越界写约 4 GiB。现在所有
   尺寸与 `length` 校验都在 uint64 中完成，并拒绝无法放入 uint32 分配接口的 map；新增回归用例。

### 本轮实际执行的检查

- `bash tools/tests/test_firmware_host.sh`：完整通过。
- `raster_service_test`（ASan+UBSan，含新溢出回归）：通过。
- `direct_surface_service_test`、`pcm_stream_service_test`：通过。
- `bash tools/check_firmware_style.sh --format-only`：架构检查通过，537 文件格式通过。
- `bash tools/build_guest_p4.sh`（输出到 `/tmp/micropixel-guest-p4-check`）：25 个 Guest 构建通过。
- Bear release AOT/Build 到 `/tmp/bear-build-check`：通过；`test_analyze_sfx` 13 项通过。
- `python3 tools/firmware.py esp32-p4-function-ev build`：通过；`micropixel.bin` 0x3ef990 字节，
  app 分区剩余 7%。
- `python3 tools/firmware.py esp32-p4-function-ev flash-built --port /dev/ttyACM0`：exit 0；
  bootloader、分区表、srmodels 和 C6 companion 校验为未变化，`ota_data_initial` 重写，app 分区的
  变更扇区写入并 `Hash of data verified`；最后硬复位。原始日志在
  `/tmp/micropixel-flash-20261105.log`（临时路径，可能消失）。
- `git diff --check`：通过。

### 构建产物与设备状态

- 2026-11-05 18:35 重建、18:46 烧录的 ELF SHA-256 为
  `3ec91542255081d8842bbede2dd7ff8f11475b5f82227c77a64a68c5c60cf52d`；`micropixel.bin` 为
  `088e4021fab49e5ba2e5484ac9e88953ef5acf548fbe6d79b08519afd62e49d0`，
  `micropixel_flashed.bin` 与 bin 一致。
- esptool 已校验写入数据，设备硬复位并重新枚举；尚未通过串口读取启动报告。
- 旧 2026-10-19 固件 ELF
  `703f4f9684e24f7d0b30d4b1498ee05f31dd7b6a1b2772d5bf4aa4a0947e26e7` 仅保存在私有证据目录，
  用于旧日志/旧 panic 符号化；不要用新 ELF 解释旧固件日志。
- （截至 2026-11-05 的历史状态）当时工作区没有暂存内容或新提交；`.env`、pytest 缓存和构建目录仍由既有
  `.gitignore` 忽略。

### 仍未验证

- 已烧录但未打开串口观察启动、Wi-Fi 关联、App Hall 或 Bear；未验证新固件真机启动/稳定性。
- 触摸音效听感、Mosaico 截屏、Voice 并发修复、UploadWarp 修复的真机路径仍未验证。
- Voice Gateway 可达性、联网多轮语音、AFE 空环、屏幕休眠后的完整 Bear 生命周期、语音中触摸取消、
  唤醒提示音规则均保持原交接文档中的未完成状态。
- `RasterService::UploadWarp` 修复只在 Host 测试和 P4 编译中验证，未在真机用实际 Guest 负载复测。
