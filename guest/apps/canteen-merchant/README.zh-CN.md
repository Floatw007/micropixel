# 食堂商家终端 Guest

应用 ID 固定为 `canteen.merchant-terminal`，面向 ESP32-P4-Function-EV-Board 的 1024×600 横屏。应用只持有业务模型，不持有 Origin、CA 或 Bearer Token；这些配置由 Host 的 Network Service 保管。

## 构建

```powershell
$env:WASI_SDK_PATH = "$env:LOCALAPPDATA\MicroPixel\environments\50bc5f1b910d382bdb87c70a\wasi"
$env:WAMRC = "$env:LOCALAPPDATA\MicroPixel\packages\2408ac3f5eb861836cf0777e\wamrc\wamrc.exe"
python tools/micropixel package guest/apps/canteen-merchant --aot-target riscv32-ilp32f --force
python tools/micropixel bundle validate build/apps/merchant-terminal/merchant-terminal.bundle.bin
```

不要把上面的缓存目录写入 CI；CI 应通过已验证的 SDK 清单注入 `WASI_SDK_PATH` 和 `WAMRC`。

## 配置与安装

```powershell
python tools/micropixel --port COM5 terminal configure `
  --origin https://java-api.example.com `
  --ca-file C:\secure\java-api-ca.pem `
  --token-file C:\secure\merchant-token.txt `
  --store-id 1 `
  --app-id canteen.merchant-terminal

python tools/micropixel --port COM5 terminal status
python tools/micropixel --port COM5 app install `
  build/apps/merchant-terminal/merchant-terminal.bundle.bin --start
```

Token 只能通过文件输入，CLI 和 Host 日志不会打印完整值。Demo Token 因 Java 重启失效时，应用会禁用写操作并提示重新配置。

## 当前行为

- 前台每 2 秒同步订单与 K230 余量，健康检查 10 秒，菜单和预约 60 秒。
- GET 可回退 Host 缓存；余量只使用内存缓存，菜单、订单和预约允许持久缓存。
- 写请求带 `Idempotency-Key`。收到 2xx 后立即回读；超时也只回读确认，不盲目重放写请求。
- 订单、菜品、余量设备和预约均支持行选择与分页。支持接单、固定理由拒单、出餐、六位数字键盘核销、上下架、售罄、数字键盘库存调整、绑定/换绑/解绑和预约启停。
- 设备号键盘仅能输入 `A-Z/a-z/0-9/._-`，最长 64 字节。
- 固定边界为 8 分类、32 菜品、64 订单、16 预约和 16 余量设备，越界数据只显示截断提示。

## 安全边界

Guest 只能提交相对路径。Host 将请求限定到配置的 HTTPS Origin，并验证 SNTP 时间、主机名、证书链和有效期。只有配置绑定的 AppId 可以使用商家凭据；App 停止、Trap、切换或暂停会关闭整个请求 Session。
