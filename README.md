# door_access · 双核智能门禁

ESP32-S3 负责「看与说」——摄像头人脸识别、LVGL 交互、后端上报；
GD32F303ZE 负责「做与守」——帧校验、状态机判决、继电器开锁、掉电保持、独立看门狗。
两板之间只有一根 UART（115200，IO10/IO11 ↔ PA3/PA2）。

> 核心设计：**决策权分离**。ESP32 只能"申请"开门，GD32 是唯一有权让门打开的模块——
> 它被刷坏、拔掉、断电，门都保持锁定（fail-secure）。

## 文档

| 文档 | 内容 |
|---|---|
| `docs/architecture.md` | 两板分工、帧协议、安全模型、数据流、引脚表 |
| `docs/test-report.md` | 每个模块的实测记录（全部来自真实测试） |
| `docs/troubleshooting.md` | 33 条踩坑记录：现象 → 根因 → 解法 |
| `docs/项目总览.html` | 立项时的方案文档（历史） |

## 模块进度

| 编号 | 模块 | 状态 | 提交（ESP32 仓库） |
|---|---|---|---|
| M0 | 工程骨架（编译 + 启动自检） | ✅ | `6c84ab0` |
| M1 | 屏 BSP（ST7796 竖屏 320×480 + LVGL 9.6 + 中文） | ✅ | `d5f92be` |
| M2 | 触摸 BSP（FT6336 轮询，释放 IO10） | ✅ | `86be421` |
| M3 | 摄像头预览（GC0308 QVGA，旋转 90° 显示） | ✅ | `f46169b` |
| M4 | 人脸检测（MSRMNP_S8_V1 + 四角括号 + 稳定帧） | ✅ | `e34d754` |
| M5 | 人脸识别 + 人脸库（MFN_S8_V1，FAT 掉电保持） | ✅ | `50db67d` |
| M6 | UI 八界面状态机 + 管理员体系（密码/成员/删除/设置） | ✅ | `0d208f1` |
| M7 | 协议库 link_proto + PC 模拟器 | ✅ | `8f8904c` |
| M8 | UART 链路任务（心跳 / 开锁请求 / 重传） | ✅ | `6678bc1` |
| M9 | GD32 侧固件（另一仓库 `GD32F303_1012/18-door-access`） | ✅ | `1bd197e`…`4fa35b5` |
| M10 | 活体检测 | 已实现，**默认关闭**（`LIVENESS_ENABLE 0`） | `9a9b397` |
| M11 | 端到端联调（刷脸 → 继电器 → 回执） | ✅ | `fd77e05` |
| M12 | 后端上报 + 人员管理 + 网页 | ✅ | `715b16e` `9e323d8` |
| M13 | 交付文档 | ✅ | 本仓库 docs/ |

GD32 侧提交：`1bd197e` M9a 帧解析 → `5d896a5` M9b 继电器 → `71dceb0` M9c-1 状态机 →
`976462c` M9c-2a 键盘 → `8c0ef8d` M9c-2b 掉电保持 → `4fa35b5` M9d 看门狗。

## 快速上手（演示脚本）

1. **启动后端**（PC，零依赖）：

   ```bash
   python3 tools/backend/server.py --port 8080
   # 浏览器打开 http://<PC局域网IP>:8080/          通行记录（5 秒自动刷新）
   #              http://<PC局域网IP>:8080/members  人员管理（填姓名，支持中文）
   ```

2. **设备上电**：ESP32 自动连 WiFi（凭据在 `components/net/include/wifi_creds.h`，
   模板 `.example`）；GD32 上电即进入锁定态。

3. **刷脸通行**：站到屏前 → 四角括号 → 识别 → 绿屏显示姓名 + 继电器"咔哒" 2 秒 →
   网页出现一条记录（带姓名）。

4. **密码开锁**（刷脸失败时的备用方式）：待机页右下角「密码」→ 输 `1234` → 确认。

5. **管理员**：待机页长按 1.5 秒 → 密码 `1234` → 菜单：
   录入人脸（10 帧采集 + 查重 + 命名去后端网页改）/ 查看成员 / 删除成员 / 阈值与锁定设置。

## 协议

帧格式：`AA55 | ver | type | seq | len | payload | CRC16 | MAC32`

- **CRC16/MODBUS** 覆盖 ver..payload——只保证传输没错
- **MAC32**（SipHash-2-4 折叠，16 字节共享密钥）覆盖 ver..CRC——防伪造；
  替代了总览里的 HMAC-SHA256（GD32 无加密库，SipHash 只要 60 行可移植 C）
- **时间戳窗口 ±5s + nonce 去重** 在 GD32 状态机里做——防重放
- 消息类型：1 心跳 / 2 ACK / 3 开锁请求 / 4 开锁应答 / 5 按键事件 / 6 状态

自测：`python3 tools/test_proto.py`（39 项：向量/往返/篡改/粘包/去重/重放 + C↔Python 交叉验证）。

## 构建

```bash
# ESP32（IDF v6）
. /mnt/data/esp/esp-idf/export.sh
idf.py build && idf.py -p /dev/ttyACM0 flash monitor

# GD32（仓库 /home/dcz/GD32F303_1012/18-door-access）
make            # arm-none-eabi-gcc + Makefile（与 EIDE 并存）
make flash      # ST-Link + OpenOCD
```

N16R8：16MB Flash + Octal 8MB PSRAM。分区表 `partitions.csv`：
factory 6M（app）+ storage 2M（FAT，人脸库 `/storage/face.db`）。

## 串口

| 设备 | VID:PID | 用途 |
|---|---|---|
| `/dev/ttyUSB0` | 1a86:**7522** | ESP32 板（CH343，控制台） |
| `/dev/ttyUSB1` | 1a86:**7523** | GD32 麒麟板（CH340C，USART0 调试口） |
| `/dev/ttyACM0` | 303a:1001 | ESP32 内置 USB-JTAG（烧录最稳） |

两个 CH34x 会抢编号，插拔后用
`udevadm info -q property -n /dev/ttyUSBx | grep ID_MODEL_ID` 确认。
**注意**：打开 GD32 的调试口（CH340C 带 DTR/RTS 下载电路）会复位整板，
做时序测试时别同时抓它的日志。

## 硬件清单

| 件 | 型号/接口 | 说明 |
|---|---|---|
| 主控 | ESP32-S3 N16R8 开发板 | 识别 + UI + 上报 |
| 执行 | GD32F303ZE 麒麟板 | 判决 + 继电器 |
| 摄像头 | GC0308（DVP，XCLK 必须 24MHz） | QVGA RGB565 |
| 屏幕 | ST7796U 320×480 + FT6336 触摸 | 竖屏 |
| 继电器 | ESP-01S Relay v4.0（**低电平触发**，PC817 光耦） | 拔掉板上 ESP-01S，PC0 接 IO0 |
| EEPROM | AT24C02（I2C0，0x50） | 失败次数/锁定/PIN 掉电保持 |
| 键盘 | 4×4 薄膜（**默认不用**，密码已在触摸屏） | 可选维护口 |
| 调试 | ST-Link V2 + OpenOCD | GD32 烧录 |
