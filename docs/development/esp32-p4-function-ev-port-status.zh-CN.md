# MicroPixel 移植到 ESP32-P4-Function-EV-Board：工程状态记录

本文记录 MicroPixel 0.9.4 在 ESP32-P4-Function-EV-Board 上的移植范围、实现状态、实机验证、遗留风险和下一步计划。它是开发状态快照，不替代
[板级构建与接线说明](esp32-p4-function-ev-board-bring-up.zh-CN.md)。

最后更新：2026-09-22  
工作区：`C:\Users\FLOAT\Documents\ChatGPT\esp32p4\micropixel`  
开发分支：`codex/function-ev-board`  
固件基线：MicroPixel `0.9.4`、ESP-IDF `6.1`  
目标 profile：`esp32-p4-function-ev`

## 1. 结论摘要

当前已经完成 MicroPixel 核心交互路径的板级移植：系统可从 16 MiB Flash 启动，点亮 EK79007 1024×600 MIPI-DSI 显示，读取 GT911 触摸，通过板载 ESP32-C6 提供 Wi-Fi，并通过 ESP32-P4 USB Serial/JTAG 提供 MPX1 本地控制。WAMR、系统大厅、双 OTA 槽和 6 MiB BundleFS App Store 均进入正常启动路径。

低功耗第一阶段也已完成：LVGL 按需刷新、GT911 自适应轮询、C6 minimum modem sleep、CPU 动态频率 40–360 MHz，以及空闲 30 秒后的受控 MIPI-DPI 挂起/恢复。受控挂起会真正删除 DPI panel、DBI IO 和 DSI bus，释放 ESP-IDF 私有 `dsi_dpi` 最高频率锁，而不只是关背光。

Automatic Light Sleep 已完成代码接入、全量冷构建、烧录和 USB 连线状态验证，但尚未完成“脱离 USB 后真实进入 Light Sleep”的电流与长稳实测。因此它当前属于**实现完成、最终硬件验收未完成**，不能宣称整机低功耗已经量产就绪。

按不同完成定义判断：

- 只以 MicroPixel 核心体验为范围（显示、触摸、Wi-Fi、USB、本地 App、OTA、空闲功耗）：约 **80%–85%**。
- 以 ESP32-P4-Function-EV-Board 全部外设为范围（再含音频、摄像头、SD 卡等）：约 **60%–70%**。
- 距离可发布版本，主要差距不是“能否点亮”，而是脱机低功耗、OTA/恢复、网络长稳、App 兼容与自动化回归。

## 2. 当前架构

```text
MicroPixel Host / WAMR Guest Apps
        │
        ├─ System UI + LVGL + Direct Surface + PPA/DMA2D
        │                    │
        │                    └─ EK79007 / MIPI-DSI / 1024×600 RGB888
        │
        ├─ Input bridge ───────── GT911 / I²C1 / 自适应轮询
        │
        ├─ Network manager ────── ESP-Hosted / SDIO ── ESP32-C6 / Wi-Fi
        │
        ├─ Local control ──────── MPX1 / USB Serial-JTAG
        │
        └─ Storage ────────────── 双 OTA + sys_store + C6 image + BundleFS
```

板级入口位于 `firmware/espressif/main/platform/boards/esp32-p4-function-ev-board/`。当前 `BoardRegistration` 发布以下能力：

- Graphics
- Input
- Wi-Fi
- Local Control
- System UI

当前没有发布 Audio Output、Battery、Power、Cellular、Sensor 或 Haptics。摄像头和 SD 卡也没有接入 MicroPixel 服务层。

## 3. 硬件与固件映射

| 功能 | 当前实现 |
| --- | --- |
| 主控 | ESP32-P4，双核，运行频率 40–360 MHz |
| Flash | 16 MiB，QIO 80 MHz |
| PSRAM | 32 MiB，实机以 200 MHz 初始化 |
| 显示 | EK79007，1024×600，RGB888，2-lane MIPI-DSI，lane 1000 Mbps |
| 显示时钟 | DPI 52 MHz |
| 显示复位 | GPIO27 |
| 背光 | GPIO26，LEDC 5 kHz、10 bit |
| 触摸 | GT911，I²C1，SDA GPIO7、SCL GPIO8，地址 `0x5d` |
| 触摸中断/复位 | LCD 子板未连接，均为 `GPIO_NUM_NC` |
| Wi-Fi | 板载 ESP32-C6，ESP-Hosted SDIO 4-bit/40 MHz |
| SDIO | CLK18、CMD19、D0–D3 为 GPIO14–17，C6 reset GPIO54 |
| 本地控制 | ESP32-P4 USB Serial/JTAG，MPX1 协议 |

16 MiB 分区布局：

| 分区 | 偏移 | 大小 | 用途 |
| --- | ---: | ---: | --- |
| `ota_0` | `0x030000` | `0x380000` | Host 固件 A |
| `ota_1` | `0x3b0000` | `0x380000` | Host 固件 B |
| `sys_store` | `0x730000` | `0x0d0000` | Host 设置/NVS |
| `slave_fw` | `0x800000` | `0x200000` | C6 ESP-Hosted 镜像 |
| `app_store` | `0xa00000` | `0x600000` | BundleFS / MicroPixel App |

## 4. 已完成并验证的工作

### 4.1 上游同步与独立 profile

- 已同步 MicroPixel 固件 `0.9.4` 基线。
- 已增加 `esp32-p4-function-ev` profile，Host 与 C6 companion 镜像由同一入口构建。
- `flash`/`flash-built` 同时处理 P4 Host、分区表、OTA data 和 C6 `network_adapter.bin`。
- 已建立独立板级目录，未把 Function EV 的引脚和策略混入 Metalio-Claw4 公共路径。

对应阶段提交：

- `5eb56e6 feat(p4): support ESP32-P4 Function EV Board`
- `62a292e Merge remote-tracking branch 'origin/main' into codex/function-ev-board`

### 4.2 显示与图形

- EK79007 MIPI-DSI 初始化、双 framebuffer、RGB888 扫描输出可用。
- GPIO26 PWM 背光可控，启动亮度 80%，系统保存亮度可以恢复。
- LVGL、系统大厅、Direct Surface、PPA/DMA2D copy 均进入实际运行路径。
- USB 截图与注入触摸已经接入显示唤醒请求。
- 固件启动与运行中未出现 framebuffer 分配失败。

### 4.3 GT911 触摸

- 已处理 LCD 子板没有 INT/RST 接线的实际硬件条件，没有配置虚假 GPIO。
- 坐标采用本板局部镜像参数，未污染公共输入层。
- 轮询策略为：按下 `10 ms`、普通空闲 `50 ms`、显示已挂起 `100 ms`。
- 显示恢复期间使用输入 gate，首次有效触摸既触发恢复，也在恢复完成后继续传递，避免“第一次点击只亮屏、不产生按键”。

### 4.4 ESP32-C6 Wi-Fi

- ESP-Hosted SDIO 能识别 ESP32-C6，从机固件与 Host 组件同版本构建。
- P4 可从 `slave_fw` 检查/更新 C6 镜像，版本匹配后不会重复升级。
- Wi-Fi manager 可以启动保存网络扫描和连接流程。
- 已启用 `WIFI_PS_MIN_MODEM`，不改变断线重连与扫描策略。

对应提交：`c6149a9 perf(p4): enable C6 modem sleep`。

### 4.5 USB 本地控制

- MPX1 本地控制服务可以通过 USB Serial/JTAG 启动。
- 已实现状态、截图、触摸注入和 App/固件管理所需的统一通道。
- 烧录工具会先探测芯片类型，避免把错误目标当作 Function EV。
- Windows 端已验证 COM12 可用于烧录和监视；文档和命令必须使用实际枚举出的 `COMx`。

### 4.6 16 MiB 存储与 OTA 基础

- 16 MiB Flash 几何、双 `0x380000` Host OTA 槽、C6 镜像区和 6 MiB App Store 已落地。
- 全量冷构建生成的 Host binary 大小为 `0x2e9120`，最小 App 槽剩余 `0x96ee0`，约 17%。
- BundleFS 在空白 App Store 上能挂载/初始化，Host 设置位于独立 `sys_store`。
- 运行镜像可在平台初始化成功后确认 OTA image。

### 4.7 第一阶段低功耗

- LVGL 从固定持续刷新改为按需唤醒，保留低频兜底刷新。
- LVGL 外部 full sleep 会释放自身 `NO_LIGHT_SLEEP` PM lock。
- GT911 自适应轮询减少 I²C 和任务唤醒。
- C6 使用 minimum modem sleep。
- FreeRTOS tickless idle 和 ESP-IDF DFS 生效，配置范围 40–360 MHz。
- 三个启动检查点输出 PM lock 表和频率驻留，采样结束后任务自删除。

对应提交：

- `249bef5 perf(p4): reduce Function EV idle wakeups`
- ESP-IoT-Solution 子模块 `a24c7d98 fix(lvgl): release PM lock during external display sleep`

### 4.8 受控 MIPI-DPI 挂起/恢复

显示工作时，ESP-IDF 的 MIPI-DPI 驱动持有私有 `dsi_dpi` `CPU_FREQ_MAX` 锁，CPU 不能真正降到 40 MHz。当前实现会在 30 秒没有前台显示活动后：

1. 退出 Direct Surface 独占扫描。
2. 等待 LVGL flush 完成并把 display 从 adapter 脱离。
3. 关闭背光和 EK79007 输出。
4. 停用 DPI DMA2D。
5. 删除 DPI panel、DBI IO 与 DSI bus，释放 `dsi_dpi` 锁。
6. 进入 100 ms 低功耗触摸轮询。

恢复时按相反顺序重建总线、panel 和 framebuffer 绑定，完成全屏刷新后恢复亮度。此前已连续完成 6 次真实挂起/恢复循环，恢复耗时约 176–177 ms，随后又完成最终启动 smoke test。

对应提交：

- `56ddaad feat(p4): suspend MIPI-DPI display while idle`
- `b7ebff2 perf(p4): deepen suspended display power savings`

## 5. 正在进行：Automatic Light Sleep

### 5.1 已完成的实现

- 板级初始化读取已有 `esp_pm_config_t`，保留 40–360 MHz 范围，只把 `light_sleep_enable` 设为 `true`。
- `CONFIG_PM_ENABLE=y`、`CONFIG_FREERTOS_USE_TICKLESS_IDLE=y` 已在最终生成的 sdkconfig 中生效。
- 启用 `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION=y`：USB 主机连接时由 ESP-IDF 持有 `usb_serial_jtag` `NO_LIGHT_SLEEP` 锁，保护调试串口。
- 保持 `CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP` 关闭，短时自动睡眠暂不主动断掉外设电源域，优先降低 SDIO/I²C/显示恢复风险。

### 5.2 2026-09-22 实机结果

| 项目 | 结果 |
| --- | --- |
| Host + C6 全量冷构建 | 通过 |
| P4 主固件链接与 16 MiB size check | 通过 |
| COM12 烧录与校验 | 通过 |
| 启动日志报告 `Light sleep: ENABLED` | 通过 |
| EK79007 / GT911 / C6 / WAMR / 系统大厅启动 | 通过 |
| 30 秒后释放 MIPI-DPI | 通过 |
| LVGL `NO_LIGHT_SLEEP` 锁在挂起后 Active=0 | 通过 |
| USB 连接保护锁 Active=1 | 通过，符合设计 |
| USB 连线时 `light_sleep_counts` | 0，符合设计，不代表失败 |
| 挂起后的 USB 截图唤醒 | 本轮未完成，monitor 退出后 Windows 报 COM12 busy/设备不可用 |
| 拔掉 USB 后真实 automatic light sleep | 未验证 |
| 脱机睡眠电流、长稳和 Wi-Fi 共存 | 未验证 |

USB 连着电脑时，ESP32-P4 USB Serial/JTAG 不能跨 Light Sleep 保持正常工作，因此保护锁必须阻止真正睡眠。此时能够验证 PM 配置、锁策略、显示挂起和 CPU DFS，但不能以 `light_sleep_counts=0` 判定自动睡眠无效。

### 5.3 仍需完成的 Light Sleep 验收

1. 使用独立 5 V 供电并断开 USB Serial/JTAG，至少运行 30 分钟。
2. 使用电流表或功耗分析仪分别记录：亮屏活动、亮屏空闲、显示挂起、automatic light sleep 四档电流。
3. 用独立 UART、JTAG 或离线计数器确认真实 `light_sleep_counts` 增长，避免 USB 连接本身改变结论。
4. 覆盖触摸唤醒、系统定时器唤醒、Wi-Fi DTIM/收包、掉线重连和显示重建。
5. 重复插拔 USB，确认设备重新枚举后 MPX1 能恢复；USB 本身不是 Light Sleep 唤醒通道。
6. 做 8–24 小时离线 soak，观察 SDIO timeout、GT911 I²C 错误、WAMR watchdog 和内存低水位。

ESP-Hosted 当前没有可直接依赖的 P4 Host/C6 协同 Light Sleep 流程。现方案依赖短时 automatic light sleep、外设域保留以及 C6 自身 modem sleep，不能等同于“P4 与 C6 协同深睡”。

## 6. 测试状态

| 测试层级 | 当前状态 | 说明 |
| --- | --- | --- |
| `git diff --check` | 通过 | 当前 Auto Light Sleep 改动无 whitespace 错误 |
| 固件架构检查 | 通过 | 板级依赖边界未被破坏 |
| ESP32-C6 冷构建 | 通过 | companion image 正常生成 |
| ESP32-P4 冷构建 | 通过 | 本轮 2162 个 Ninja 步骤完成 |
| 固件烧录/Hash 校验 | 通过 | P4 与 C6 image 均完成校验 |
| 启动 smoke | 通过 | 显示、触摸、USB service、C6、WAMR、BundleFS、System Shell 均启动 |
| 受控显示循环 | 通过 | 已完成 6 次 suspend/resume 实机循环 |
| PM lock/DFS | 通过 | DPI/LVGL 锁释放，存在 40 MHz 驻留 |
| Automatic Light Sleep（USB 连接） | 通过 | 正确被 USB `NO_LIGHT_SLEEP` 锁保护 |
| Automatic Light Sleep（脱离 USB） | 待测试 | 需要独立供电和测量通道 |
| Python 工具测试（Windows） | 受阻 | 运行 107 项：2 failure、7 error、1 skipped；失败集中在 Bash 接收 Windows 临时路径、符号链接权限、POSIX `0600` 权限和路径分隔符断言 |
| Host shell 测试（Windows） | 受阻 | `tools/tests/test_firmware_host.sh` 依赖 POSIX `fcntl`，不能直接在当前 Windows Python 下运行 |
| OTA A/B + rollback | 待完整测试 | 分区与基础确认已完成，破坏性/回滚场景未覆盖 |
| Wi-Fi 长稳/吞吐/重连 | 待测试 | 已完成启动与省电模式，不等于长期可靠性验收 |
| App 安装/启动兼容矩阵 | 待测试 | BundleFS/WAMR 已启动，仍需真实 App 集合回归 |

## 7. 尚未完成的移植范围

### P0：发布前必须完成

- 脱离 USB 的 automatic light sleep、功耗测量和触摸/Wi-Fi 唤醒闭环。
- 修复或复现 Windows 在 monitor 退出后偶发 COM 端口仍 busy/设备不可用的问题。
- Wi-Fi 扫描、关联、DHCP、断线重连、minimum modem sleep 的长稳测试。
- USB MPX1 截图、触摸、App 安装、固件更新的完整回归。
- 双 OTA 升级、失败回滚、断电恢复，以及 C6 镜像升级失败场景。
- 至少一组真实 WAMR AOT App：安装、启动、触摸、网络、存储、退出、重启恢复。
- 8–24 小时 soak，检查 heap/PSRAM 水位、display suspend/resume、SDIO 和 I²C 错误。

### P1：完整 MicroPixel 产品体验

- 把 Function EV 板载音频接入 `AudioOutput`，验证混音、系统音量和 Guest PCM service。
- 如果目标产品需要，接入摄像头、SD 卡；当前 MicroPixel 板级注册和 Guest service 没有发布它们。
- 建立 Function EV 的 CI compile gate、固件产物和硬件在环 smoke test。
- 制作 Windows 一键环境检查、烧录包和恢复流程，减少 ESP-IDF/Anaconda 环境混用。
- 完成性能基线：触摸到首帧延迟、动画帧时间、Direct Surface 帧率、Wi-Fi 并发时抖动。

### P2：板上全部外设覆盖

- 评估并接入官方板上的其他外设；是否需要电池、传感器、振动、蜂窝网络应由产品需求决定。
- Function EV 本身不是带电池计量和电源键的 Metalio-Claw4，不能机械复制 `Battery`/`Power` 注册。没有对应硬件时应保持能力缺失，而不是伪造状态。
- 若追求更低功耗，需要等待或实现 P4 Host 与 C6 的协调休眠协议，再评估外设域断电和更深睡眠。

## 8. 已遇到并解决的问题

### 8.1 ESP-IDF Python 环境不存在

症状：执行 `export.ps1` 时提示 `idf6.1_py3.13_env\Scripts\python.exe` 不存在，随后 dot-source 表达式失败。

原因：ESP-IDF 目录存在，但对应 Python virtual environment 尚未安装或 `IDF_TOOLS_PATH` 指向了另一套工具目录。

处理：先用该 ESP-IDF 自带安装脚本建立环境，再保证 `IDF_TOOLS_PATH` 和 `export.ps1` 属于同一套安装。不能用 PowerShell ExecutionPolicy 代替环境安装。

### 8.2 Anaconda Python 没有 `esptool`

症状：目标探测时报 `D:\Anaconda\python.exe: No module named esptool`。

原因：shell 中的 `python` 指向 Conda，而 ESP-IDF 工具安装在 IDF virtual environment。

处理：先执行正确的 `export.ps1`，或显式使用 IDF Python。构建、烧录、monitor 应使用同一 Python 环境。

### 8.3 命令拼写错误

症状：`buildm` 被 argparse 拒绝。

处理：合法 action 是 `build`、`flash`、`flash-built`、`monitor`、`fullclean`、`port`。先读 usage，不要把工具链问题和参数拼写问题混在一起排查。

### 8.4 `fullclean` 后 companion 输入缺失

症状：清理后 P4 configure 找不到 C6 image 或 managed component。

原因：Function EV 是双镜像工程，P4 构建依赖 C6 companion 产物；component manager 也需要先恢复 managed components。

处理：使用 profile 命令重新构建，不直接假设单独执行 P4 `idf.py build` 就具备全部输入。profile 会先生成 C6，再生成 P4。

### 8.5 CPU 一直维持 360 MHz

症状：开启 DFS 后仍看不到有效 40 MHz 空闲驻留。

原因：MIPI-DPI 驱动持有 `dsi_dpi` `CPU_FREQ_MAX` 锁；只关背光不会释放它。LVGL external sleep 早期也仍持有自己的 `NO_LIGHT_SLEEP` 锁。

处理：建立受控显示生命周期，先 detach LVGL，再安全删除 panel/IO/bus；同时修正 LVGL adapter，在 external full sleep 时释放 PM lock。

### 8.6 触摸轮询与流畅度冲突

症状：固定高频轮询浪费功耗，固定低频轮询又造成滑动阶梯感。

处理：把轮询分成按下、空闲、显示挂起三个状态，分别为 10/50/100 ms；只在拿到有效样本后请求显示/LVGL 唤醒。

### 8.7 显示恢复容易丢第一次触摸

原因：MIPI-DPI 重建约需 176–177 ms，若输入事件直接进入 UI，panel 尚未准备好。

处理：有效触摸先请求恢复，输入分发等待恢复 gate；panel 重建、LVGL rebind、全屏刷新和亮度恢复完成后再继续该触摸。

### 8.8 Windows 串口不可用不是单一问题

已区分三类症状：

- 完全没有 COM：通常是线材、USB 口选择、驱动或供电问题。
- `No module named esptool`：Python 环境问题，不是硬件端口问题。
- `port is busy` / `Cannot configure port`：端口被 monitor/其他进程占用，或 USB 设备刚复位后 Windows 状态未恢复。

当前尚有一个待复现问题：本轮退出 IDF monitor 后，进程列表未看到残留 monitor，但 COM12 仍短暂报告 busy/设备不可用，导致挂起后的截图唤醒没有完成。后续需用 Process Explorer/Handle、设备管理器事件和 USB 重新枚举日志定位。

### 8.9 测试工具的 Windows 可移植性

在 `PYTHONUTF8=1` 下运行 `python -m unittest tools.tests.test_firmware tools.tests.test_micropixel_cli`，共 107 项，结果仍为 2 failure、7 error、1 skipped。已确认的 Windows 差异包括：

- Git Bash 脚本收到 `C:\...` 临时目录后不能按 POSIX 路径正确创建 build 目录。
- 非开发者模式/非管理员进程不能创建测试所需的符号链接。
- Windows 文件 mode 不能满足测试硬编码的 POSIX `0600` 断言。
- 测试期望 `/`，实际命令参数使用 `\`。

这些结果应在 WSL/Linux CI 中复核；当前不能把 Windows runner 的失败标记为固件通过，也不能直接归因于 Function EV 的 Auto Light Sleep 改动。

## 9. 可复用的处理经验

1. **配置文件不是验证结果。** 写入 defaults 后必须检查最终生成的 `sdkconfig.release`，再通过启动日志和 PM lock 表确认运行时行为。
2. **把“连线调试”和“脱机低功耗”分开测试。** ESP32-P4 USB Serial/JTAG 连线会主动阻止 Light Sleep，这是安全机制，不是功耗代码失败。
3. **关屏不等于显示域休眠。** 背光为 0 只减少 LCD 光源功耗，不会释放 MIPI-DPI 时钟锁或 framebuffer/driver 资源。
4. **先建立可逆生命周期，再谈省电。** suspend 必须有严格配对的 resume；任何一步失败都要保持对象句柄和状态一致，不能留下“指针已空但硬件未删”的半状态。
5. **测频率看累计驻留，不看单次瞬时值。** 采样任务本身可能唤醒 CPU 或持有 RTOS 锁，`Mode stats` 比任务内读一次时钟更可信。
6. **无中断触摸需要状态化轮询。** 活跃、空闲、显示挂起使用不同周期，才能兼顾滑动流畅度与空闲唤醒次数。
7. **双芯片构建必须锁定版本关系。** P4 Host、ESP-Hosted component 和 C6 image 应由同一 profile 生成并一起烧录，避免 RPC/协议版本漂移。
8. **Windows 上不要混用 Conda 与 IDF Python。** 端口探测、esptool、idf.py 和 monitor 最好全部来自同一 ESP-IDF environment。
9. **先读原始错误的第一层。** `buildm`、缺模块、端口忙、芯片不匹配属于不同层，逐层处理能避免把简单问题扩大成硬件故障。
10. **不要把一次 smoke test 当长稳。** 显示能恢复一次、Wi-Fi 能启动一次，只证明路径存在；发布前仍需要循环、断电、回滚和 soak。

## 10. 标准构建、烧录与验证入口

PowerShell：

```powershell
Set-Location C:\Users\FLOAT\Documents\ChatGPT\esp32p4\micropixel
$env:IDF_TOOLS_PATH = 'D:\esp\tools'
. D:\esp\frameworks\esp-idf-v6.1\export.ps1

git submodule update --init --recursive
python tools\firmware.py esp32-p4-function-ev build
python tools\firmware.py esp32-p4-function-ev flash-built --port COM12
python tools\firmware.py esp32-p4-function-ev monitor --port COM12 --reset
```

USB 本地控制验收必须在 monitor 释放端口后执行：

```powershell
python tools\micropixel --transport usb --port COM12 device status
python tools\micropixel --transport usb --port COM12 screenshot --output function-ev.jpg
```

关键日志应包含：

```text
automatic light sleep enabled: CPU=40..360 MHz, USB connection protected by NO_LIGHT_SLEEP lock
touch controller has no interrupt line; adaptive polling active=10000 idle=50000 low_power=100000 us
controlled MIPI-DPI suspend armed after 30000 ms of foreground inactivity
C6 Wi-Fi minimum modem sleep enabled
MIPI-DPI panel detached in ... ms; dsi_dpi frequency lock released
```

## 11. 推荐的下一步顺序

1. 先解决/复现 COM monitor 退出后的 Windows 端口占用，完成休眠后 USB 截图唤醒回归。
2. 用独立供电和测量通道完成 automatic light sleep 的真实入睡、唤醒和电流曲线。
3. 做 Wi-Fi + 显示 suspend/resume + GT911 的 8–24 小时并发 soak。
4. 完成 USB MPX1、真实 App、OTA A/B、rollback 和断电恢复矩阵。
5. 接入板载音频；摄像头和 SD 卡按产品需求决定是否进入本轮“完整移植”定义。
6. 把 profile 冷构建、size gate 和硬件 smoke 纳入 CI/发布清单。

满足以上 P0 项目后，可以把 Function EV 版本定义为“MicroPixel 核心功能完整移植”；完成音频以及产品要求的摄像头/SD 卡后，才适合称为“ESP32-P4-Function-EV-Board 全外设移植”。
