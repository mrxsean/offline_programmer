# RL78 离线编程器（ESP32-C3）

基于 ESP32-C3 的 Renesas RL78 系列 MCU 脱机烧写器。设备创建 WiFi 热点并提供
HTTP 服务，通过浏览器即可完成固件文件的上传、管理与目标芯片的擦除、编程、校验操作，
无需连接电脑或专用烧录器。

## 功能特性

- RL78 串行编程（Protocol A，1-wire UART 模式），适用于 RL78/G10/G11/G12/G13/G14 等
  大多数 RL78 系列芯片
- 支持固件格式：Intel HEX（.hex）、Motorola S-Record（.mot/.srec/.s19 等）、原始二进制（.bin）
- WiFi SoftAP 热点 + 内嵌 Web 页面，手机/电脑浏览器直接操作
- 固件文件管理：上传（带进度）、列表、下载、删除，存储于片内 LittleFS（约 2.5MB）
- 烧录流程：按块空白检查 → 按需擦除 → 编程 → 校验，实时进度与日志
- 读取硅签名（设备名称、代码/数据闪存大小、固件版本）
- 全片擦除模式（仅勾选"擦除"时）
- 网页选定固件保存到 NVS，断电保留；实体按键启动擦除＋编程＋校验
- 蓝色 LED 指示状态，SSD1315 72×40 OLED 显示固件名、进度和结果

## 硬件连接

| ESP32-C3 引脚 | 连接目标 RL78 | 说明 |
| --- | --- | --- |
| GPIO0（TOOL0_TX） | TOOL0 | 经约 1kΩ 电阻串联 |
| GPIO1（TOOL0_RX） | TOOL0 | 直接连接（与 TX 在目标侧汇合） |
| GPIO2（RESET） | RESET | 直接连接 |
| GND | GND | 共地必须连接 |

| ESP32-C3 引脚 | 面板连接 | 说明 |
| --- | --- | --- |
| GPIO8 | 蓝色 LED | 低电平点亮 |
| GPIO9 | 启动按键 | 按下接地，启用内部上拉，40ms 消抖 |
| GPIO5 | OLED SDA | SSD1315，72×40，I2C 100kHz |
| GPIO6 | OLED SCL | 自动探测 7 位地址 0x3C / 0x3D |

- TX 与 RX 在 TOOL0 处汇合构成单线半双工；串联电阻用于避免双方同时驱动时的总线冲突。
- ESP32-C3 为 3.3V 电平：目标板工作电压为 3.3V 时可直连；目标为 5V 供电时，
  RL78 的输入高电平阈值约为 0.7×VDD（3.5V），建议加电平转换电路。
- 目标板可由本设备 3.3V 供电（电流允许时），或独立供电（烧录前需上电）。
- 引脚可在 `idf.py menuconfig` → `Offline Programmer Configuration` 中修改。

## 软件架构

```
main/
├── app_main.c       # 入口：NVS / 存储 / UART / WiFi / HTTP 初始化
├── rl78_prog.c/h    # RL78 Protocol A：模式进入、帧收发、擦除/编程/校验命令
├── image_parser.c/h # Intel HEX / S-Record / 二进制固件解析（段式存储）
├── storage.c/h      # LittleFS 文件系统（/storage 分区）
├── wifi_ap.c/h      # WiFi SoftAP
├── http_server.c/h  # HTTP 服务（REST API + 烧录任务调度）
├── front_panel.c/h  # 按键消抖、低有效 LED、SSD1315 显示
└── index.html       # 内嵌 Web 管理页面
```

烧录协议参考 Renesas 应用笔记 R01AN0815《RL78 Microcontrollers (RL78 Protocol A)
Serial Programming Guide》。

## 构建与烧写

```bash
source ~/esp/v5.3/esp-idf/export.sh
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

分区布局（4MB Flash）：factory 1.5MB + LittleFS 存储 2.5MB，见 `partitions.csv`。

## 使用说明

1. 设备上电后创建热点：`RL78-Flasher`（默认密码 `12345678`）。
2. 手机/电脑连接该热点，浏览器访问 `http://192.168.4.1`。
3. 在"固件文件"页上传 .hex/.mot/.bin 文件，并点击"选择"，看到“已保存”后即已写入设备；重启或重新打开网页会恢复选择。
4. 接好目标板连线，可先在"目标芯片"页点击"连接/读取芯片信息"验证通信。
5. 在"烧录控制"页勾选操作（默认 擦除+编程+校验），点击"开始烧录"，
   实时查看进度条与运行日志。
6. 脱机时短按 GPIO9 按键，对保存的固件执行擦除＋编程＋校验。按原有流程逐块空白检查、按需擦除；网页复选框不影响实体按键。

实体按键长按只触发一次，松开后才可再次启动；烧录中按键忽略，不排队。
请在正常上电后按键；GPIO9 同时是 ESP32-C3 启动模式引脚，上电/复位时按住会影响正常启动，
见 [Espressif 启动模式说明](https://docs.espressif.com/projects/esptool/en/latest/esp32c3/advanced-topics/boot-mode-selection.html)。
没有选定固件或文件已丢失时，不启动任务，OLED 提示 `SELECT WEB`，LED 快闪。
删除选定文件会清除设备保存的选择。烧录期间拒绝切换固件、上传、删除和再次启动。

| 状态 | 蓝色 LED | OLED |
| --- | --- | --- |
| 待机 | 熄灭 | 固件名、`READY` / `PRESS KEY` |
| 烧录中 | 慢闪（亮/灭各 500ms） | 当前阶段、百分比及进度条 |
| 成功 | 常亮 | `SUCCESS`、100% |
| 失败 | 快闪（亮/灭各 125ms） | `FAILED` / `CHECK LOG`，详情查看网页日志 |
| 取消 | 熄灭 | `CANCELLED` |

OLED 使用大小写英文字母、数字及文件名符号，每行可容纳 12 字符；固件名单行显示，
超过屏幕宽度时沿 X 轴向左循环滚动，不换行。每 100ms 移动 2 像素，循环间留 3 字符空白，
每次回到开头时停留约 1 秒；切换固件后从开头显示，短文件名保持静止。
状态、百分比和进度条保持原位，结果保持到下次启动或重新选择固件。
OLED 启动时先清理 I2C 总线，再同步命令并重新初始化、重画屏幕，处理 ESP32-C3 手动复位而 OLED 仍通电的情况。
OLED 未连接或通信失败不会阻止按键烧录和 LED 指示；初始化或刷新失败后每约 1 秒重试，通信恢复后自动重画并继续刷新。
显示窗口采用列 28～99、5 页，参考 [M5Stack 72×40 SSD1315 面板配置](https://github.com/m5stack/M5GFX/blob/master/src/M5UnitMiniOLED.h)
及 [SSD1315 数据手册](https://files.seeedstudio.com/wiki/Grove-OLED-Yellow%26Blue-Display-0.96-%28SSD1315%29_V1.0/res/SSD1315.pdf)。

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
| POST | `/api/select?file=<f>` | 保存实体按键使用的固件，断电保留；`GET /api/files` 的 `selected` 字段返回该选择 |
| POST | `/api/program?file=<f>&erase=1&program=1&verify=1` | 启动烧录任务 |
| POST | `/api/cancel` | 取消进行中的任务 |
| GET | `/api/status` | 任务状态、进度、芯片信息与日志 |

## 验证

```bash
python3 tests/run_host_tests.py
idf.py build
git diff --check
```

主机测试编译实际 C 源码，模拟 NVS、任务调度、HTTP 和目标芯片，覆盖选择恢复、保存失败、
非法/缺失文件、忙时互斥、完整任务及错误/取消、按键消抖/长按、LED 极性、文件名横向滚动、I2C 显示范围及 OLED 失败恢复。
启用 AddressSanitizer / UndefinedBehaviorSanitizer；若在 ptrace 沙箱中运行而 LeakSanitizer 不可用，
使用 `ASAN_OPTIONS=detect_leaks=0 python3 tests/run_host_tests.py`（保留地址和未定义行为检查）。

主机测试和编译不能代替实板验收：需检查断电后选择恢复、OLED 方向/窗口、ESP32-C3 连续手动复位后的显示恢复、按键单次触发，
并接真实 RL78 验证成功、断线失败和网页取消时的显示及 LED。

## 注意事项

- 烧写期间目标 RESET 由本设备控制；烧写完成后设备会复位目标并运行其应用程序。
- 仅支持代码闪存的烧写/校验，不支持数据闪存（Data Flash）与安全位设置。
- 若目标芯片 RESET 引脚被配置为普通 IO，需先断电，按住目标板复位拉低后上电再
  进行连接（即上电时序进入编程模式）。
