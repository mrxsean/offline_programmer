# RL78 离线编程器（ESP32-S3）

> 当前分支 `esp32s3` 适配 ESP32-S3（8MB Flash + 2MB PSRAM）；
> ESP32-C3 版本请切换到 `master` 分支。

基于 ESP32-S3 的 Renesas RL78 系列 MCU 脱机烧写器。设备创建 WiFi 热点并提供
HTTP 服务，通过浏览器即可完成固件文件的上传、管理与目标芯片的擦除、编程、校验操作，
无需连接电脑或专用烧录器。

## 功能特性

- RL78 串行编程（Protocol A，1-wire UART 模式），适用于 RL78/G11/G12/G13/G14 等
  大多数 RL78 系列芯片
- 支持固件格式：Intel HEX（.hex）、Motorola S-Record（.mot/.srec/.s19 等）、原始二进制（.bin）
- WiFi SoftAP 热点 + 内嵌 Web 页面，手机/电脑浏览器直接操作
- **320×240 LCD（ST7789V）+ 6 个 I2C 触摸按键的本地操作界面（LVGL 8.3）**，
  与 Web 界面功能对等，可完全脱离浏览器使用
- 固件文件管理：上传（带进度）、列表、下载、删除，存储于片内 LittleFS（约 6MB）
- 烧录流程：按块空白检查 → 按需擦除 → 编程 → 校验，实时进度与日志
- 读取硅签名（设备名称、代码/数据闪存大小、固件版本）
- 全片擦除模式（仅勾选"擦除"时）

## 硬件连接

| ESP32-S3 引脚 | 连接目标 RL78 | 说明 |
| --- | --- | --- |
| GPIO42（TOOL0_TX） | TOOL0 | 经约 1kΩ 电阻串联 |
| GPIO44（TOOL0_RX） | TOOL0 | 直接连接（与 TX 在目标侧汇合） |
| GPIO43（RESET） | RESET | 直接连接 |
| GND | GND | 共地必须连接 |

- TX 与 RX 在 TOOL0 处汇合构成单线半双工；串联电阻用于避免双方同时驱动时的总线冲突。
- GPIO43/44 是 UART0 控制台默认引脚，本固件已将日志控制台切换到
  **USB Serial/JTAG**（GPIO19/20 的 USB 口），烧录/调试请使用该 USB 口；
  用 UART0 口烧录固件不受影响（esptool 直接使用 ROM 引导）。
- ESP32-S3 为 3.3V 电平：目标板工作电压为 3.3V 时可直连；目标为 5V 供电时，
  RL78 的输入高电平阈值约为 0.7×VDD（3.5V），建议加电平转换电路。
- 目标板可由本设备 3.3V 供电（电流允许时），或独立供电（烧录前需上电）。
- 引脚可在 `idf.py menuconfig` → `Offline Programmer Configuration` 中修改。

### LCD 与触摸按键

| 功能 | ESP32-S3 引脚 | 说明 |
| --- | --- | --- |
| LCD MOSI | GPIO48 | ST7789V，SPI2 @ 40MHz，横屏（MADCTL=0x60） |
| LCD CLK | GPIO14 | |
| LCD CS | GPIO47 | |
| LCD DC (RS) | GPIO21 | |
| LCD RST | GPIO13 | |
| LCD 背光 | GPIO12 | LEDC PWM 调光（13bit / 400Hz） |
| 触摸 SDA | GPIO6 | I2C1 @ 100kHz，触摸芯片地址 0x28（CAP1298/Cypress 自动识别） |
| 触摸 SCL | GPIO5 | |
| 触摸 INT | GPIO4 | 双边沿中断 |
| 蜂鸣器 | GPIO37 | 无源蜂鸣器，LEDC PWM（TIMER_1/CH_1） |

蜂鸣提示音：

| 场景 | 提示音 |
| --- | --- |
| 上电 | 1 声（120ms） |
| 按键按下 / 连发 | 短促按键音（40ms） |
| 烧录任务启动 | 2 短声 |
| 烧录成功 / 连接成功 / 删除成功 | 3 短声 |
| 烧录失败 / 连接失败 / 操作错误 | 1 长低音（400ms，1500Hz） |

触摸按键（2×3）功能映射：

| 按键 | 功能 |
| --- | --- |
| MODE（右上角） | **返回**：其他页面返回状态页；烧录中取消任务 |
| MENU | 切换页面（状态 → 文件 → 烧录，循环前进） |
| UP / DOWN | 移动光标（长按连发） |
| OK | 确认：连接芯片 / 选定文件 / 切换选项 / 开始烧录 |
| 长按 OK | 文件页：删除文件（3 秒内再按 OK 确认） |
| ONOFF | 任意页面直接返回主页（状态页） |

LCD 三个页面：**状态**（芯片签名、热点与存储信息、任务状态）、
**文件**（列表、选定、删除）、**烧录**（擦除/编程/校验选项、进度条、最新日志）。
与 Web 界面共享同一任务引擎（`prog_job`），两端操作状态互相同步。

## 软件架构

```
main/
├── app_main.c       # 入口：NVS / 存储 / UART / WiFi / HTTP / LCD / 触摸 初始化
├── rl78_prog.c/h    # RL78 Protocol A：模式进入、帧收发、擦除/编程/校验命令
├── image_parser.c/h # Intel HEX / S-Record / 二进制固件解析（段式存储）
├── storage.c/h      # LittleFS 文件系统（/storage 分区）
├── wifi_ap.c/h      # WiFi SoftAP
├── prog_job.c/h     # 烧录任务引擎（HTTP 与 LCD UI 共用：状态、日志、调度）
├── http_server.c/h  # HTTP 服务（REST API）
├── drv_st7789v.c/h  # LCD 驱动 + LVGL 端口层 + 背光 PWM
├── drv_touch_key.c/h# I2C 触摸按键（中断 + 长按/连发状态机）
├── ui_main.c/h      # LVGL 界面（状态/文件/烧录三页 + 按键导航）
└── index.html       # 内嵌 Web 管理页面
components/
└── esp_lcd_st7789/  # ST7789 esp_lcd panel 驱动（本地组件）
```

烧录协议参考 Renesas 应用笔记 R01AN0815《RL78 Microcontrollers (RL78 Protocol A)
Serial Programming Guide》。

## 构建与烧写

```bash
source ~/esp/v5.3/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor   # monitor 请用 USB Serial/JTAG 口
```

分区布局（8MB Flash）：factory 2MB + LittleFS 存储约 6MB，见 `partitions.csv`。

## 使用说明

1. 设备上电后创建热点：`RL78-Flasher`（默认密码 `12345678`）。
2. 手机/电脑连接该热点，浏览器访问 `http://192.168.4.1`。
3. 在"固件文件"页上传 .hex/.mot/.bin 文件，并点击"选择"。
4. 接好目标板连线，可先在"目标芯片"页点击"连接/读取芯片信息"验证通信。
5. 在"烧录控制"页勾选操作（默认 擦除+编程+校验），点击"开始烧录"，
   实时查看进度条与运行日志。

热点名称、密码、信道、烧录波特率（默认 115200）、目标电压（默认 3.3V）、
Flash 块大小（默认 1KB）等均可在 menuconfig 中配置。

## HTTP API

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/` | Web 管理页面 |
| GET | `/api/files` | 文件列表（含存储用量） |
| POST | `/api/upload?name=<file>` | 上传固件（请求体为文件内容） |
| GET | `/api/file?name=<file>` | 下载固件 |
| DELETE | `/api/file?name=<file>` | 删除固件 |
| POST | `/api/connect` | 进入编程模式并读取硅签名 |
| POST | `/api/program?file=<f>&erase=1&program=1&verify=1` | 启动烧录任务 |
| POST | `/api/cancel` | 取消进行中的任务 |
| GET | `/api/status` | 任务状态、进度、芯片信息与日志 |
| GET | `/api/ota/info` | 当前固件版本、编译时间、运行分区 |
| POST | `/api/ota` | 上传新固件（.bin）并 OTA 升级，成功后自动重启 |

## 自身 OTA 升级

设备固件可通过网页"固件升级 (OTA)"卡片在线更新：选择编译产物
`build/offline_programmer.bin`，上传后设备自动校验镜像（魔数、芯片 ID、SHA256）、
写入另一个 OTA 槽位并重启。

- **分区布局**：`ota_0`/`ota_1` 双槽各 2MB（共 8MB Flash，存储区约 3.9MB）
- **回滚保护**：新固件首次启动处于 `PENDING_VERIFY` 状态，所有服务正常初始化后
  自动标记为有效；若新固件无法启动，bootloader 下次复位自动回滚到旧固件
- **版本号**：在根 `CMakeLists.txt` 的 `PROJECT_VER` 中维护，
  网页 OTA 卡片与 LCD 顶栏均显示当前版本
- **注意**：从旧 factory 布局升级到 OTA 布局必须先用串口烧录一次
  （`idf.py flash` 会自动写入初始 otadata），此后即可全部在线升级

## 注意事项

- 烧写期间目标 RESET 由本设备控制；烧写完成后设备会复位目标并运行其应用程序。
- 仅支持代码闪存的烧写/校验，不支持数据闪存（Data Flash）与安全位设置。
- 若目标芯片 RESET 引脚被配置为普通 IO，需先断电，按住目标板复位拉低后上电再
  进行连接（即上电时序进入编程模式）。
