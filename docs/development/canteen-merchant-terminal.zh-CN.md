# ESP32-P4 食堂商家终端实现状态

本文记录基于 MicroPixel 的商家终端第一阶段实现，硬件基线是 ESP-IDF 6.1、`esp32-p4-function-ev` profile、1024×600 EK79007、GT911 和板载 ESP32-C6。

## 数据链与职责

```text
K230 -> FastAPI -> Java -> HTTPS -> Host Network Service -> Guest Scene
```

P4 不直连 K230。Java 是菜单、订单、预约和余量的唯一业务入口；K230 百分比只展示，不自动修改库存或上下架。

## Network Service 1.0

Service ID 19 提供 `GET_INFO`、`START`、`READ`、`CANCEL` 和 `CLOSE`。`START` 复制最多 4 KiB 请求体后立即返回句柄，两个 FreeRTOS worker 执行 HTTPS，完成后通过有界 Guest 事件队列发送 `REQUEST_COMPLETE`。单响应上限 64 KiB，同时最多两个活动请求和四个排队请求。

安全约束：

- Guest 只可提交以 `/` 开头的相对路径，拒绝 scheme、网络路径、反斜杠、控制字符和 `..` 路径段。
- Origin、CA、Token、storeId 和 allowed AppId 由 USB MPX1 配置，Guest 不能读取或替换。
- 仅 `canteen.merchant-terminal` 可打开已绑定凭据；生产构建要求可信 UTC 时间并进行完整 TLS 校验。
- Session 关闭会取消请求、断开回调并释放响应缓冲；请求响应缓冲位于 PSRAM。

缓存策略：

- 所有选定 GET 可使用四项内存缓存。
- 菜单、订单和预约可写容量更大的 `sys_store` 持久缓存；余量只用内存缓存。终端 Profile 单独保存在 `runtime_nvs`。
- 内容未变化且距上次写入不足五分钟时不写 Flash。写请求永不离线排队。

## USB 终端配置

`tools/micropixel terminal configure/status/clear` 使用分块上传传送 MNT1 Profile。Token 只从文件读取；状态输出只含 Origin、证书指纹、storeId、allowed AppId 和 Token 尾部掩码。

目前 Demo Profile 保存在 `runtime_nvs`。Java 已实现持久化终端配对、短期访问令牌、刷新凭据旋转和单设备撤销；Host 目前仍只保管通过 USB 下发的访问令牌。量产前还必须让 Host 接入刷新流程，并启用 Flash/NVS 加密或改用硬件保护的凭据存储。

## 已验证结果（2026-09-23）

- Function EV Host 完整构建通过：`micropixel.bin = 0x31ab20`。
- 两个 OTA 槽均为 `0x380000`；最小槽剩余 `0x654e0`，约 11%。
- 商家 Guest 使用 WASI SDK 33 和 RISC-V ILP32F `wamrc` 构建成功；release Bundle 327,680 字节且校验通过，SHA-256 为 `e5c663877b9408ef093a428621a745d951ba1d5be63adf41f828be558a789a1f`。
- Java 21 后端测试：26 个测试全部通过，包含配对、刷新旋转、旧令牌失效和单终端撤销。
- JSON Reader 和网络路径策略原生单元测试通过；Python Bundle/发布元数据测试 21 个通过。

## 尚需真机完成

- 使用真实 Java HTTPS Origin、CA 和商家 Token 完成端到端联调。
- 将 Java 终端刷新凭据接入 Host 的受保护存储和自动旋转流程。
- 列表选择/分页、设备 ID 受限键盘、绑定/换绑与六位数字核销键盘已实现，仍需真机触摸验收。
- 用 USB 截图及触摸注入覆盖六页，并做实体触摸四角、滑动和键盘验收。
- 执行断网、错误证书、错误主机名、401/403、Java/FastAPI 分别故障和恢复测试。
- 连续运行 24 小时，监控请求句柄、PSRAM/内部堆、任务栈、UI 响应和 C6 链路。
