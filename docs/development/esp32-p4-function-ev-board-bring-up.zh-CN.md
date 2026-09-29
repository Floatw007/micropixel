# ESP32-P4-Function-EV-Board 板级支持

本文对应 `esp32-p4-function-ev` 固件 profile，目标硬件是
ESP32-P4-Function-EV-Board v1.5.x 与官方 7 英寸 1024×600 LCD 子板。该 profile 启用
EK79007 MIPI-DSI 显示、GPIO26 PWM 背光、GT911 触摸、板载 ESP32-C6 Wi-Fi，以及通过
ESP32-P4 USB Serial/JTAG 提供的 MPX1 本地控制，并把板载 microSD 卡作为可移动的 BundleFS
扩展应用存储。板载 ES8311、NS4150 扬声器和模拟麦克风均接入 Audio Service；摄像头不在当前范围内。

官方硬件说明与连接图见
[ESP32-P4-Function-EV-Board 用户指南](https://docs.espressif.com/projects/esp-dev-kits/zh_CN/latest/esp32p4/esp32-p4-function-ev-board/user_guide.html)，
上游板级实现可对照
[Espressif ESP-BSP](https://github.com/espressif/esp-bsp/tree/master/bsp/esp32_p4_function_ev_board)。

## 硬件连接

1. 断电后连接 7 英寸 LCD 子板，MIPI-DSI 排线按官方说明反向插入并锁紧。
2. LCD 子板 `RST_LCD` 接主板 `GPIO27`。
3. LCD 子板 `PWM` 接主板 `GPIO26`。
4. LCD 子板通过 USB-C 供电，或把子板 `5V`、`GND` 接到主板对应电源；不要同时使用两种供电方式。
5. GT911 使用板载 I²C1：SDA 为 GPIO7，SCL 为 GPIO8。该 LCD 子板未连接触摸 INT/RST，固件在空闲时以
   50 ms 周期轮询，检测到按下后切换为 10 ms；显示挂起后使用 100 ms 低功耗周期。
6. C6 使用板载 SDIO，无需外接跳线：CLK GPIO18、CMD GPIO19、D0–D3 GPIO14–GPIO17、RESET GPIO54。
7. USB 本地控制连接主板标注为 USB Serial/JTAG 的 Type-C 口；只供电的数据线不会枚举端口。
8. microSD 卡槽使用 SDMMC Slot 0 四线模式：CMD GPIO44、CLK GPIO43、D0–D3 GPIO39–GPIO42，LDO4 提供 3.3 V。
   卡槽没有独立卡检测信号，需在上电或重启前插卡；运行时拔卡会使后续存储访问失败。
9. ES8311 与触摸共用 I²C1，I²S0 使用 MCLK GPIO13、BCLK GPIO12、WS GPIO10、DOUT GPIO9 和 DIN GPIO11；
   GPIO53 控制 NS4150 功放。扬声器输出与板载麦克风输入均为 16 kHz、16-bit、单声道。

本 profile 固定为 EK79007 1024×600。1280×800 ILI9881C 子板不能使用这份固件。

## 构建

先初始化子模块并激活 ESP-IDF 6.1：

```sh
git submodule update --init --recursive
source /path/to/esp-idf/export.sh
python3 tools/firmware.py esp32-p4-function-ev build
```

Windows PowerShell 已激活 ESP-IDF 时，可以直接运行：

```powershell
python tools/firmware.py esp32-p4-function-ev build
```

该命令会先在 `build/esp-hosted-c6-slave` 构建与 Host 组件同版本的 C6 从机镜像，再构建
`build/host-esp32p4-function-ev`。当前分区使用 16 MiB Flash，Host 有两个
`0x380000` OTA 槽；`0x800000` 起的 2 MiB `slave_fw` 保存 ESP32-C6 ESP-Hosted 镜像，
最后 6 MiB 为 `app_store`。空白或几何变化后的 `app_store` 会在首次启动时自动初始化为 BundleFS。

板载 C6 出厂固件通常早于当前 ESP-Hosted。`flash`/`flash-built` 会把匹配的 C6 镜像写入
`slave_fw`；P4 首次启动发现版本不匹配时通过 SDIO 自动升级 C6，并重启一次完成同步。版本匹配后
不会重复升级。Host 端使用 Function Board 预设，以 SDIO 4-bit/40 MHz 与 C6 通信，无需 ESP-Prog。

## 烧录与监视

先确认串口对应 ESP32-P4，再烧录已经构建的 Host：

```sh
python3 tools/firmware.py esp32-p4-function-ev port
python3 tools/firmware.py esp32-p4-function-ev flash-built --port /dev/ttyACM0
python3 tools/firmware.py esp32-p4-function-ev monitor --port /dev/ttyACM0 --reset
```

Windows 把端口替换为实际的 `COMx`：

```powershell
python tools/firmware.py esp32-p4-function-ev flash-built --port COM8
python tools/firmware.py esp32-p4-function-ev monitor --port COM8 --reset
```

正常启动时应看到以下关键日志：

```text
initializing ESP32-P4-Function-EV-Board display and touch
touch controller has no interrupt line; adaptive polling active=10000 idle=50000 us
controlled MIPI-DPI suspend armed after 30000 ms of foreground inactivity
automatic light sleep configured: CPU=40..360 MHz
USB local control protected by persistent NO_LIGHT_SLEEP lock; display suspend and DFS remain enabled
Function EV ES8311/NS4150 ready: 16000 Hz mono speaker + mono microphone
microphone probe passed: frames=128 min=... max=... peak=...
idle DFS checkpoint 1/3: configured=40..360 MHz light-sleep=automatic
MIPI-DPI panel detached in ... ms; dsi_dpi frequency lock released
idle DFS sampling complete; probe task is stopping
ready: EK79007 1024x600 RGB888 + GT911 touch + ES8311/NS4150 audio=ready + ESP32-C6 Wi-Fi + USB local control + SD=ready
```

## 音频与麦克风验收

音频后端使用同一条 I²S0 全双工链路：ES8311 DAC 经 NS4150 驱动扬声器，ES8311 ADC 接板载模拟麦克风。
固件初始化后先读取 128 帧输入并记录最小值、最大值和绝对峰值；出现 `microphone probe passed` 表示 I²C、
I²S RX 和 ADC 数据链路均已工作。Guest Audio 1.1 提供 `input_info()` 和 `ReadInput()`，每次最多读取
256 帧 signed int16 mono PCM；设备目录同时登记 `AUDIO_INPUT`。

自动验收可运行 `audio_input` conformance：它检查格式、请求边界、连续四块采样以及数据不是恒定值。
扬声器可通过 `audio_synth` 或带音效的 App 驱动，并从日志确认 Audio engine 与 I²S TX 成功启动。
听感、响度、失真和麦克风语音质量属于主观人工验收；无人值守流程应明确标记为跳过，而不能由日志代替。

## 第一阶段空闲功耗

Function EV profile 在 0.9.4 基线上启用以下空闲优化：

- Display refresh timer 从持续 16 ms 改为 1000 ms 兜底；Host UI、Guest frame 和有效触摸仍通过
  `RequestDisplayRefresh()` 立即唤醒，不以 1 秒为交互延迟。
- `esp_lv_adapter` 在 1 秒无 LVGL 工作后进入 pause，最长普通等待为 120 秒；新的显示或输入事件会显式唤醒。
- 无 INT 线的 GT911 使用 50 ms 空闲/10 ms 按下自适应轮询，减少大厅静置时的 I²C 与任务唤醒。
- ESP32-C6 station 启动后使用 `WIFI_PS_MIN_MODEM`；关联 AP 后按 DTIM 进入 modem-sleep，断线扫描与
  重连策略保持不变。
- 30 秒无前台显示活动后，板级任务先退出 Direct Surface 独占扫描，再通过
  `esp_lv_adapter_sleep_prepare()` 等待 flush 并脱离 panel，关闭背光和 EK79007，停用 DMA2D，依次删除
  DPI panel、DBI IO 与 DSI bus。`esp_lcd_panel_del()` 由驱动释放私有 `dsi_dpi` 最高频率锁。
- 有效 GT911 样本、USB 注入触摸、截图以及 Host/Guest 可见更新会请求恢复。恢复路径重新创建 bus/IO/panel，
  重新绑定 LVGL framebuffer，先完成一次全屏刷新，再恢复用户亮度；触摸分发在此期间等待，因此首次按下不会丢失。
- 大厅时钟/电量兜底属于 passive refresh：显示工作时照常更新，显示已挂起时只更新模型，不唤醒面板，也不重置
  30 秒计时。

启动后的 5、15、40 秒会各记录一次 DFS 检查点，第三次输出 ESP-IDF PM lock 与 CPU 频率驻留统计，随后测量
任务自删除，不形成永久周期唤醒。默认配置预期为 40–360 MHz、tickless idle 和 automatic light sleep
已配置。运行中的探针任务本身会持有 `rtos0` 最高频率锁，所以不能用任务内的瞬时读数判断空闲频率；应查看
`Mode stats` 中 40 MHz 档位的累计驻留时间。DPI panel 活跃时，ESP-IDF 的 `dsi_dpi` CPU 最高频率锁会阻止
40 MHz 驻留；若启动后没有前台活动，30 秒受控挂起会释放该锁，第三个检查点应显示非零的 40 MHz 驻留。
ESP32-P4 USB Serial/JTAG 不能跨 Light Sleep 保持正常工作，因此 USB 主机连接时，IDF connection monitor 会持有
`usb_serial_jtag` `NO_LIGHT_SLEEP` 锁。但 Windows 选择性挂起会停止 SOF，使 connection monitor 暂时误判主机已断开；
若此时释放锁并进入 light sleep，COM 端口将不能可靠恢复。因此该 profile 在 USB 本地控制的整个生命周期额外持有
板级 `micropixel_usb` `NO_LIGHT_SLEEP` 锁。息屏、面板断电、tickless idle 和 40 MHz DFS 仍正常工作，但
`light_sleep_counts=0` 是预期结果；如需验证脱离 USB 的 automatic light sleep，必须先提供可关闭 USB 本地控制的产品路径。

显示应先亮起 MicroPixel Host 界面，启动亮度为 80%。静置 30 秒后背光应关闭；首次触摸应先恢复完整画面并
继续传递该次按下。长按和连续滑动
不应出现 50 ms 的阶梯感。触摸验收至少覆盖四角、短按、横向滑动、
从底部上滑和从顶部下滑；坐标方向错误时不要修改公共输入层，应只调整本板
`board_hardware.cpp` 中 GT911 的 `swap_xy`、`mirror_x`、`mirror_y`。

## Wi-Fi 与 USB 本地控制验收

进入系统 Wi-Fi 设置后应能扫描、连接 2.4 GHz 网络并获得 IP。串口日志应能看到 ESP-Hosted
协处理器初始化、C6 版本信息和 `C6 Wi-Fi minimum modem sleep enabled`；Wi-Fi 失败不会阻止显示、触摸和
USB 本地控制启动。

USB 端口可用后，先查看状态，再验证截图和触摸注入：

```powershell
python tools\micropixel --port COM8 device status
python tools\micropixel --port COM8 screenshot --output function-ev.jpg
```

App 安装使用同一 MPX1 通道；没有已格式化的 SD 扩展区时写入板载 6 MiB `app_store`，有扩展区时优先写入
SD 卡。固件更新仍写入非活动 OTA 槽，完成后由 bootloader 切换；USB 安装不会直接开放原始 Flash 或 NVS。

插卡启动后，Host 直接探测原始扇区，不挂载 FAT。未写入 BundleFS 的新卡或原有 FAT 卡会在
系统设置 → Manage Apps 中显示扩展存储未格式化；MicroPixel 不会自动改写可移动介质。只有用户在该页面确认
格式化后，整张卡才成为 BundleFS 扩展应用区，下载的新 App 优先写入卡中。该操作会删除卡上原有分区和文件，
操作前必须备份。拔卡后重启会回退到板载 6 MiB app_store。

## 当前板端验收状态

以下结果来自 v1.5.x 开发板、1024×600 LCD 子板和一张已格式化的 microSD 卡；它们说明当前
`esp32-p4-function-ev` profile 的实测范围，不代表所有硬件版本或其他 SD 卡均已验收。

| 项目 | 已验证 | 尚待验证或处理 |
|---|---|---|
| 显示、触摸与 USB | 系统界面、USB 截图和触摸注入可用；30 秒显示挂起后可通过截图恢复，USB 本地控制仍可读取设备状态 | 实体触摸的四角、滑动与首按手感需要人工验收；显示已挂起时直接执行 `app start` 曾发生一次控制超时和重启，唤醒显示后启动成功，需继续定位 |
| SD 扩展应用区 | SDMMC 识别、设置页确认格式化、BundleFS 挂载与应用安装成功；重启后仍能列出全部应用 | 卡槽无独立卡检测，运行时热拔插不支持；其他卡型及断电恢复尚未在本板逐项验收 |
| Guest 应用 | 测试设备目录保有 SDK Demo、Jump Jump、Gravity Balls、Juicy Snake 和 Juicy Blocks；Snake、Blocks 均以 RISC-V AOT Bundle 安装，经 USB 触摸注入进入实际游戏画面，`app last-error` 为空，重启后仍在目录中 | 实体触摸操作手感与长时间游戏稳定性需要人工验收；这五个应用是当前设备目录状态，不是固件出厂预装清单 |
| 音频 | ES8311/NS4150 初始化与 I²S TX 启动成功；麦克风启动探针及 Guest 连续 PCM 读取通过 | 扬声器响度、失真和麦克风语音质量需要人工听测 |
| Wi-Fi 与功耗 | ESP32-C6 Wi-Fi 初始化、显示挂起、tickless idle 和 DFS 降频路径已观察到 | 实际 automatic light sleep 被 USB 本地控制的 `NO_LIGHT_SLEEP` 锁阻止；Wi-Fi 入网、待机电流和 Windows 选择性挂起场景尚未完成独立验收 |

当前 profile 尚未注册 MIPI CSI 摄像头、有线以太网、C6 蓝牙 LE、独立 USB FS/HS OTG 数据口或
1280×800 LCD 子板。它们属于后续可选移植，不应由上述板端验收结果推断为已支持。

> ESP32-P4-Function-EV-Board v1.4 的下载口由 CP2102N 转接 UART，不等同于 USB Serial/JTAG。
> 本 profile 的 USB MPX1 路径按 v1.5.x 验证；v1.4 若只连接 CP2102N 端口，只能获得串口日志与烧录能力。

## 常见问题

- 背光亮但无画面：先检查 MIPI 排线方向、锁扣和 GPIO27 复位线，再检查串口是否出现 EK79007 初始化错误。
- 完全不亮：检查 LCD 子板供电与 GPIO26 PWM 线；固件在面板初始化成功后才把背光提升到 80%。
- GT911 初始化失败：检查 GPIO7/GPIO8、子板供电和 I²C 地址 `0x5d`，不要为未连接的 INT/RST 引脚配置虚假 GPIO。
- C6 Wi-Fi 初始化失败：确认日志中的 SDIO 引脚为 18/19/14–17、复位为 GPIO54，并确认 C6 已烧录与 Host 组件兼容的 ESP-Hosted 从机固件。
- SD 卡未出现：确认卡在重启前已经插入，并检查启动日志中的 “SD card ready”；卡槽走 GPIO39–44 的
  SDMMC Slot 0，C6 走 GPIO14–19 的 SDIO Slot 1。已有 FAT 文件的卡显示“未格式化”是预期行为，不要在未备份时确认格式化。
- 首次启动发生一次自动重启：这是旧 C6 完成 SDIO OTA 后的预期行为；第二次启动应报告 C6 与 Host 版本兼容。
- Windows 看不到 COM 端口：确认连接 v1.5.x 的 USB Serial/JTAG 口、线材支持数据，随后在设备管理器检查“端口”和“通用串行总线设备”。
- 能点按但坐标相反：记录左上、右下两点的实际坐标，再只修改板级镜像参数。
- `no memory for frame buffer`：确认 PSRAM 已启用且为 200 MHz；本 profile 需要两张 1024×600 RGB888 帧缓冲。
