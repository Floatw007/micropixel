# Ubuntu 上开发 ESP32-P4 Function EV

本文说明如何把 Windows 上的 MicroPixel Function EV 开发分支迁移到原生 Ubuntu，并完成 ESP32-P4 Host、ESP32-C6 ESP-Hosted companion、烧录、串口监视和本地控制验证。

迁移包对应分支 `codex/function-ev-board`。Windows 端没有 `78/micropixel` 的写权限，因此使用离线 Git bundle 保存完整可达历史；这比复制工作目录可靠，也不会把 Windows 的构建缓存和 Python 环境带到 Linux。

## 1. 推荐环境

- 原生 Ubuntu 24.04 x86_64；Ubuntu 22.04 也可使用。
- 不建议把 WSL2 或虚拟机作为主要真机环境。它们可以编译，但 USB 烧录、设备重枚举和脱离 USB 的低功耗测试需要额外转发。
- Function EV 使用两个目标工具链：ESP32-P4 Host 和 ESP32-C6 companion。
- 固件固定使用 ESP-IDF commit `812f3e98ca5da31ed4fc6be8b703019f7b6dcaa6`，不要直接跟随 ESP-IDF `master`。

## 2. 从迁移包恢复仓库

把以下两个 Windows 端生成的文件复制到 Ubuntu，例如放在 `~/transfer/`：

```text
micropixel-ubuntu-transfer.bundle
micropixel-ubuntu-transfer.bundle.sha256
```

先验证文件完整性：

```bash
cd ~/transfer
sha256sum -c micropixel-ubuntu-transfer.bundle.sha256
git bundle verify micropixel-ubuntu-transfer.bundle
```

再克隆迁移分支：

```bash
mkdir -p ~/work
git clone \
  --branch codex/function-ev-board \
  ~/transfer/micropixel-ubuntu-transfer.bundle \
  ~/work/micropixel

cd ~/work/micropixel
git log -3 --oneline
git status
```

bundle 会被登记为临时 `origin`。把它改名为 `transfer`，并把官方仓库登记为只读 `upstream`：

```bash
git remote rename origin transfer
git remote add upstream https://github.com/78/micropixel.git
git remote -v
```

当前账号没有官方仓库写权限。以后需要跨机器推送时，应创建个人 fork 或私有仓库，再登记为 `origin`：

```bash
git remote add origin https://github.com/<你的账号>/micropixel.git
git push -u origin codex/function-ev-board
```

不要把未经确认的访问令牌写进 remote URL、`.env` 或文档。

## 3. 初始化子模块

bundle 保存主仓库历史，不内嵌子模块对象。Ubuntu 需要联网获取固定的 WAMR 和 ESP-IoT-Solution 子模块：

```bash
cd ~/work/micropixel
git submodule sync --recursive
git submodule update --init --recursive
git submodule status
```

正常情况下，子模块应处于主仓库记录的精确 commit，而不是各自远端的最新分支。

## 4. 安装 Ubuntu 基础依赖

```bash
sudo apt update
sudo apt install -y \
  git curl wget \
  flex bison gperf \
  python3 python3-pip python3-venv \
  cmake ninja-build ccache \
  libffi-dev libssl-dev \
  dfu-util libusb-1.0-0
```

本项目不依赖 Conda。若 shell 自动进入 Conda，先退出：

```bash
conda deactivate
```

## 5. 安装固定 ESP-IDF

```bash
mkdir -p ~/esp
git clone https://github.com/espressif/esp-idf.git ~/esp/esp-idf
cd ~/esp/esp-idf

git checkout 812f3e98ca5da31ed4fc6be8b703019f7b6dcaa6
git submodule update --init --recursive

./install.sh esp32p4,esp32c6
```

每个新终端都要激活该环境：

```bash
source ~/esp/esp-idf/export.sh
```

确认没有混入系统或 Conda Python：

```bash
echo "$IDF_PATH"
command -v idf.py
command -v python
idf.py --version
```

项目 Python 依赖安装到当前激活的 IDF 环境：

```bash
cd ~/work/micropixel
python -m pip install -r requirements-dev.txt
```

## 6. 首次冷构建

不要复制 Windows 的 `build/`、`sdkconfig.release`、ESP-IDF Python virtual environment 或 `managed_components` 缓存。Ubuntu 应从干净 build 目录生成自己的产物。

Function EV 必须使用专用 profile；普通 `tools/p4.sh build-host` 默认不是该开发板：

```bash
cd ~/work/micropixel
source ~/esp/esp-idf/export.sh

python3 tools/firmware.py esp32-p4-function-ev build
```

profile 会先构建 C6 companion，再构建 P4 Host。成功后主要产物为：

```text
build/esp-hosted-c6-slave/network_adapter.bin
build/host-esp32p4-function-ev/micropixel.bin
```

第一次构建需要解析 managed components，耗时明显长于后续增量构建。

## 7. 配置 USB 串口权限

```bash
sudo usermod -aG dialout "$USER"
```

执行后注销并重新登录。重新登录后检查：

```bash
groups
ls -l /dev/ttyACM* 2>/dev/null
ls -l /dev/serial/by-id/ 2>/dev/null
```

连接开发板时可以观察内核枚举：

```bash
sudo dmesg --follow
```

出现 `Permission denied` 时先确认 `dialout` 已生效。出现端口忙时检查占用者：

```bash
lsof /dev/ttyACM0
```

若确认是 `ModemManager` 抢占，才临时停止它：

```bash
sudo systemctl stop ModemManager
```

不要在没有证据时永久禁用系统服务。

## 8. 探测、烧录和监视

```bash
cd ~/work/micropixel
source ~/esp/esp-idf/export.sh

python3 tools/firmware.py esp32-p4-function-ev port
```

假设探测到 `/dev/ttyACM0`：

```bash
python3 tools/firmware.py \
  esp32-p4-function-ev \
  flash-built \
  --port /dev/ttyACM0

python3 tools/firmware.py \
  esp32-p4-function-ev \
  monitor \
  --port /dev/ttyACM0 \
  --reset
```

使用 `Ctrl+]` 退出 monitor。monitor、esptool 和 MicroPixel USB 本地控制不能同时占用同一个端口。

## 9. USB 本地控制验收

退出 monitor 后执行：

```bash
python3 tools/micropixel \
  --transport usb \
  --port /dev/ttyACM0 \
  device status

python3 tools/micropixel \
  --transport usb \
  --port /dev/ttyACM0 \
  screenshot \
  --output function-ev.jpg
```

端口名称可能在复位或重新枚举后变化。自动化中优先使用 `/dev/serial/by-id/` 下的稳定符号链接，并在每次硬件操作前确认它仍指向目标板。

## 10. Linux 回归检查

Ubuntu 可以运行 Windows 原生环境下受 POSIX 路径、符号链接、文件权限或 `fcntl` 影响的检查：

```bash
cd ~/work/micropixel
source ~/esp/esp-idf/export.sh

bash tools/check_firmware_architecture.sh
bash tools/check_firmware_style.sh --format-only
bash tools/tests/test_firmware_host.sh
```

当前迁移提交包含较大范围的在开发功能。迁移成功不等于这些功能已经验收；至少还应重新执行 Function EV 全量构建、显示/触摸、C6 Wi-Fi、USB、本地 App、音频输入和 SD 卡相关测试。

## 11. Guest App 工具链

只修改 Host 固件时不需要 WASI SDK 或 WAMRC。编译 Guest App 时还需要：

- WASI SDK 33，通过 `WASI_SDK_PATH` 或 `WASI_CLANG` 指定；
- 当前仓库固定 MicroPixel WAMR fork 构建的 `RISCV32_ILP32F` WAMRC，通过 `WAMRC` 指定。

不要用发行版提供的任意 `wamrc` 代替。项目要求 AOT format v6；普通上游 WAMR 即使显示相同版本号，也可能产生不兼容的 AOT 文件。

环境变量示例：

```bash
export WASI_SDK_PATH=/opt/wasi-sdk
export WAMRC="$HOME/tools/micropixel-wamrc/wamrc"
```

然后可构建商家终端 Guest：

```bash
python3 tools/micropixel build \
  guest/apps/canteen-merchant \
  --aot-target riscv32-ilp32f
```

`bash tools/build_guest_p4.sh` 用于构建 P4 Guest conformance 集合，不接受单个 App 目录参数。

## 12. Automatic Light Sleep 验收边界

USB Serial/JTAG 连接电脑时，ESP-IDF 会持有 `usb_serial_jtag` `NO_LIGHT_SLEEP` 锁，以避免串口在 Light Sleep 中失效。此时可以验证显示挂起、PM lock 释放和 CPU DFS，但 `light_sleep_counts=0` 是预期行为。

真实 Automatic Light Sleep 测试应使用独立供电并断开 USB 数据连接，再通过功耗分析仪、独立 UART/JTAG 或离线统计确认入睡和唤醒。Ubuntu 不会改变这一硬件边界。

## 13. 日常 Git 工作流

获取官方更新：

```bash
git fetch upstream
git log --oneline --left-right HEAD...upstream/main
```

在当前开发未完成时，不要直接把 `upstream/main` 强制覆盖到工作分支。先创建安全分支或 tag，再 merge/rebase 并重新构建 P4 与 C6。

每次提交前至少执行：

```bash
git status --short
git diff --check
```

不要提交 `build/`、生成 sdkconfig、managed components、固件镜像、设备日志、访问令牌或机器专用 `.env`。
