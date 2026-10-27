# RL78 离线编程器（ESP32-C3）

基于 ESP32-C3 的 Renesas RL78 系列 MCU 脱机烧写器。设备创建 WiFi 热点并提供
HTTP 服务，通过浏览器即可完成固件文件的上传、管理与目标芯片的擦除、编程、校验操作，
无需连接电脑或专用烧录器。

## 功能特性

- RL78 串行编程（Protocol A，1-wire UART 模式），适用于 RL78/G10/G11/G12/G13/G14 等
  大多数 RL78 系列芯片
- 支持固件格式：Intel HEX（.hex）、Motorola S-Record（.mot/.srec/.s19 等）、原始二进制（.bin）
- WiFi SoftAP 热点 + 内嵌 Web 页面，手机/电脑浏览器直接操作
- 固件文件管理：上传（带进度）、列表、下载、删除，存储于片内 LittleFS（分区 1.5 MiB）
- 烧录流程：按块空白检查 → 按需擦除 → 编程 → 校验，实时进度与日志
- 读取硅签名（设备名称、代码/数据闪存大小、固件版本）
- 全片擦除模式（仅勾选"擦除"时）
- 网页选定固件保存到 NVS，断电保留；实体按键启动擦除＋编程＋校验
- 蓝色 LED 指示状态，SSD1315 72×40 OLED 上电显示热点名称 3 秒，随后显示固件名、进度和结果
- 网页 OTA 升级编程器自身固件，校验成功后自动重启
- 按功能划分选项卡：RL78 烧录、UART、I2C、SPI、GPIO、ADC、固件升级
- GPIO20 RX / GPIO21 TX 的 UART0 文本与 HEX 收发；ESP32 系统日志改走 USB Serial/JTAG
- 网页配置软件 I2C、SPI 主机、GPIO 输入/输出和 ADC 采样，检查引脚保留及占用冲突

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
├── serial_debug.c/h # UART0 有界接收历史、波特率配置和收发
├── io_debug.c/h     # 引脚占用、软件 I2C、SPI、GPIO、ADC 调试
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

分区布局（4MB Flash）：两个 1,216 KiB OTA 应用分区 + 1.5 MiB LittleFS，见 `partitions.csv`。
`ota_0` 位于 `0x20000`，`ota_1` 位于 `0x150000`，各占 `0x130000` 字节；文件存储位于 `0x280000`，占 `0x180000` 字节。

从旧分区布局（包括此前双 1,088 KiB OTA 分区）迁移，须先在网页下载备份已有目标固件，再通过串口执行
`idf.py -p /dev/ttyACM0 flash`，同时写入新分区表、初始 OTA 数据和应用固件。
文件存储分区位置及容量已改变，首次启动会重新格式化，之后需重新上传目标固件。
不要仅向旧的 `0x10000` 地址写入新应用；新应用地址是 `0x20000`。
完成这次迁移后，后续可直接通过网页 OTA，无需再次修改分区表。网页 OTA 仅更新应用，不会更新分区表。

发布版本号统一在 `version.txt` 中维护。OLED 与网页读取当前运行应用的 ESP-IDF 固件描述信息，显示相同的版本和编译日期/时间。
编译时间采用构建机时区；本项目构建可使用 `TZ=Asia/Singapore idf.py build`。

系统日志使用 USB Serial/JTAG（GPIO18/19），UART0 专用于外部设备数据。
已有 `sdkconfig` 若仍选用 UART 控制台，须在 menuconfig 中将 `Channel for console output` 改为 `USB Serial/JTAG Controller`。
本次需要完整串口烧录（包含 bootloader、新分区表和应用），使引导程序日志也切到 USB；仅应用 OTA 不会更新旧 bootloader。
芯片 ROM 上电信息仍受芯片自身 UART 打印配置控制；本项目不修改 eFuse。

## 使用说明

1. 设备上电后创建热点：`RL78-Flasher-XXXX`（`XXXX` 为 SoftAP MAC 最后四位大写十六进制字符，默认密码 `12345678`）。
2. 手机/电脑连接该热点，浏览器访问 `http://192.168.4.1`。
3. 在"固件文件"页上传 .hex/.mot/.bin 文件，并点击"选择"，看到“已保存”后即已写入设备；重启或重新打开网页会恢复选择。
4. 接好目标板连线，可先在"目标芯片"页点击"连接/读取芯片信息"验证通信。
5. 在"烧录控制"页勾选操作（默认 擦除+编程+校验），点击"开始烧录"，
   实时查看进度条与运行日志。
6. 自身升级：在“编程器自身升级（OTA）”区域选择 `build/offline_programmer.bin`，点击“升级编程器”。上传、写入和校验成功后自动重启；重新连接原热点并刷新网页。不要上传 RL78 固件、bootloader、分区表或合并镜像。
7. 脱机时短按 GPIO9 按键，对保存的固件执行擦除＋编程＋校验。按原有流程逐块空白检查、按需擦除；网页复选框不影响实体按键。

短按松开时启动烧录；失败状态下短按清除失败并回到待机，保留固件，再次短按才重新烧录。
长按 1 秒按文件名顺序循环切换固件并保存到 NVS，每次长按只切换一次，松开后才可再次操作；烧录中按键忽略，不排队。
请在正常上电后按键；GPIO9 同时是 ESP32-C3 启动模式引脚，上电/复位时按住会影响正常启动，
见 [Espressif 启动模式说明](https://docs.espressif.com/projects/esptool/en/latest/esp32c3/advanced-topics/boot-mode-selection.html)。
没有选定固件或文件已丢失时，不启动任务，OLED 显示 `NO FILE`（未选择时）和 `FAILED`，LED 快闪。
删除选定文件会清除设备保存的选择。烧录期间拒绝切换固件、上传、删除和再次启动。

| 状态 | 蓝色 LED | OLED |
| --- | --- | --- |
| 待机 | 熄灭 | 固件名、芯片型号、`READY` |
| 烧录中 | 慢闪（亮/灭各 500ms） | 固件名、芯片型号、当前阶段和百分比 |
| 成功 | 常亮 | `SUCCESS`、100% |
| 失败 | 快闪（亮/灭各 125ms） | `FAILED`，短按回待机，详情查看网页日志 |
| 取消 | 熄灭 | `CANCELLED` |

上电时 OLED 使用 5×7 字形显示五行信息：热点名称两行、固件版本、编译日期、编译时间，持续 3 秒后进入状态界面。每行 12 个字符；自定义热点前缀较长时，第一行快速滚动，第二行保留 MAC 后缀。

OLED 状态界面使用 7×11 像素字形（字符间距 8 像素），显示三行：固件名、目标 RL78 芯片型号、烧录状态。
芯片型号在网页读取芯片信息或烧录连接成功后显示，此前显示 `NO CHIP`。
每行可容纳 9 字符，超长内容沿 X 轴向左循环滚动，不换行。每 100ms 移动 2 像素，
循环间留 3 字符空白，每次回到开头时停留约 1 秒；切换固件后从开头显示。
结果保持到下次操作，失败后可短按返回待机。
OLED 启动时先清理 I2C 总线，再同步命令并重新初始化、重画屏幕，处理 ESP32-C3 手动复位而 OLED 仍通电的情况。
OLED 未连接或通信失败不会阻止按键烧录和 LED 指示；初始化或刷新失败后每约 1 秒重试，通信恢复后自动重画并继续刷新。
显示窗口采用列 28～99、5 页，参考 [M5Stack 72×40 SSD1315 面板配置](https://github.com/m5stack/M5GFX/blob/master/src/M5UnitMiniOLED.h)
及 [SSD1315 数据手册](https://files.seeedstudio.com/wiki/Grove-OLED-Yellow%26Blue-Display-0.96-%28SSD1315%29_V1.0/res/SSD1315.pdf)。

热点名称前缀（最多 27 字节，自动追加 `-XXXX`）、密码、信道、烧录波特率（默认 115200）、目标电压（默认 3.3V）、
Flash 块大小（默认 1KB）等均可在 menuconfig 中配置。

## 网页调试工具

各选项卡切换时保留输入内容和设备配置。调试配置不保存到 NVS：重启后 UART 恢复 115200，I2C/SPI/GPIO/ADC 均未配置。
IO 配置由设备全局共享，多个浏览器共同使用；先选择引脚并点击“应用配置”，再执行操作，不再使用时点击“释放 IO”。
网页显示真实引脚占用，设备端也执行相同校验。当前接线下空闲 GPIO 为 3、4、7、10；ADC 可选 GPIO3、GPIO4。
RL78 的配置引脚、OLED GPIO5/6、LED GPIO8、按键 GPIO9、Flash GPIO11～17、USB GPIO18/19、UART0 GPIO20/21 禁止分配给其它调试功能。

| 功能 | 支持的操作与限制 |
| --- | --- |
| UART | GPIO20 RX 接外部 TX，GPIO21 TX 接外部 RX，GND 共地；3.3V TTL、8N1、无流控；9600～921600 的常用波特率；文本 UTF-8 / HEX、文本追加 LF/CR/CRLF；每次 1～256 字节 |
| I2C | 软件主机，SDA/SCL 网页选择，开漏输出；目标时钟 10/50/100 kHz，软件调度会影响实际频率；建议外接上拉；7 位地址扫描；独立读取/写入区域，各自配置设备地址、可选 8/16 位寄存器/存储地址和数据长度（1～64 字节，不含寄存器地址）；16 位地址高字节先发送，寄存器读取采用重复起始，写入 HEX 字节数必须与长度一致；时钟拉伸超时约 1 ms，不支持多主机仲裁；OLED 持续工作 |
| SPI | SPI2 主机，SCLK/MOSI/MISO/CS 网页选择且互不重复；10 kHz～10 MHz、模式 0～3、MSB 优先、CS 低有效；1～64 字节全双工，接收与发送长度相同；事务结束释放 CS |
| GPIO | 可依次配置多个引脚；输入或输出、无上下拉/上拉/下拉、初始电平、读电平、设置高低；输入模式拒绝写输出 |
| ADC | ADC1、12 位单次采样；0/2.5/6/12 dB 衰减；返回原始值，有芯片校准时返回 mV，校准不可用时不虚构电压；输入必须处于芯片所选衰减的测量范围 |

UART 保留最近 8 KiB 接收数据，网页分别按接收位置读取，互不抢走数据；显示区保留最近 32K 字符。
停止接收仅停止该网页轮询，设备仍接收并覆盖最旧历史；再次接收时会提示历史缓冲丢失字节数。
UART FIFO 溢出、驱动缓冲满、帧/奇偶错误计入“串口错误事件”，不把它当作精确丢失字节数。
网络中断或部分发送后请先确认对端状态，网页不会自动重发。OTA 期间暂停调试操作和 UART 网页轮询。

## HTTP API

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/` | Web 管理页面 |
| GET | `/api/info` | 编程器自身的版本、编译日期与时间 |
| GET | `/api/files` | 文件列表（含存储用量） |
| POST | `/api/upload?name=<file>` | 上传固件（请求体为文件内容） |
| GET | `/api/file?name=<file>` | 下载固件 |
| DELETE | `/api/file?name=<file>` | 删除固件 |
| POST | `/api/connect` | 进入编程模式并读取硅签名 |
| POST | `/api/select?file=<f>` | 保存实体按键使用的固件，断电保留；`GET /api/files` 的 `selected` 字段返回该选择 |
| POST | `/api/program?file=<f>&erase=1&program=1&verify=1` | 启动烧录任务 |
| POST | `/api/cancel` | 取消进行中的任务 |
| GET | `/api/status` | 任务状态、进度、芯片信息与日志 |
| POST | `/api/ota` | 编程器自身升级，请求体为原始应用 .bin；校验成功后重启 |
| GET | `/api/serial?cursor=<n>` | UART 二进制数据以 HEX 返回，同时返回 next/lost/errors/baud/more/reset |
| POST | `/api/serial/config?baud=<n>` | 设置 UART0 波特率 |
| POST | `/api/serial/send` | 发送原始字节，请求体 1～256 字节，返回 written 和是否完成 |
| GET | `/api/io` | 引脚保留/占用情况及 I2C/SPI/ADC 当前配置 |
| POST | `/api/io` | 执行 IO 调试 JSON 请求，最多 512 字节；检查响应 ok/error |

IO 请求操作：`i2c_config / i2c_scan / i2c_transfer / i2c_release`、
`spi_config / spi_transfer / spi_release`、`gpio_config / gpio_read / gpio_write / gpio_release`、
`adc_config / adc_read / adc_release`。HEX 字段为连续十六进制字符，单方向最多 64 字节。
例如：`{"op":"i2c_config","sda":3,"scl":4,"hz":10000}`，
随后读取寄存器：`{"op":"i2c_transfer","address":80,"hex":"","read":1,"register_bytes":1,"register":0}`。
写入寄存器：`{"op":"i2c_transfer","address":80,"hex":"AABB","read":0,"write":2,"register_bytes":2,"register":4660}`，发送顺序为寄存器 `12 34`、数据 `AA BB`。
省略 `register_bytes/register` 或设置 `register_bytes:0` 且不提供 `register` 时直接读写。
`write` 为期望写入的数据字节数，提供时必须等于 HEX 数据长度且 `read` 必须为 0；原始 `hex`＋`read` 的先写后读接口仍可使用。
同一 HTTP 服务任务串行执行配置和硬件操作，避免配置与传输同时使用一组引脚。

## 验证

```bash
source ~/esp/v5.3/esp-idf/export.sh
python3 tests/run_host_tests.py
node tests/test_serial_web.js
node tests/test_io_web.js
idf.py build
git diff --check
```

主机测试编译实际 C 源码，模拟 NVS、任务调度、HTTP 和目标芯片，覆盖选择恢复、保存失败、
非法/缺失文件、忙时互斥、完整任务及错误/取消、按键消抖/长按、LED 极性、文件名横向滚动、I2C 显示范围及 OLED 失败恢复。
OTA 测试覆盖空/超大文件、错误镜像头/项目名、分片接收、中断、写入/校验/启动分区失败，以及升级与烧录互斥；OLED 测试覆盖启动提示的 3 秒边界、长热点前缀滚动、MAC 后缀常显，以及版本/日期/时间的五行布局；HTTP 测试检查自身固件信息。
UART/IO 主机测试还覆盖二进制分片、发送中断/短写/超时、环形缓冲覆盖、独立读取、保留引脚与功能争用、非法请求、软件 I2C 的 ACK/NACK/重复起始/时钟拉伸超时、SPI/ADC 资源清理及校准缺失。网页测试执行实际页面 JavaScript，验证选项卡、表单保留、HEX 校验、IO 选项、错误处理和 OTA 互斥。IO 测试使用 `IDF_PATH` 下的实际 cJSON 源码。
启用 AddressSanitizer / UndefinedBehaviorSanitizer；若在 ptrace 沙箱中运行而 LeakSanitizer 不可用，
使用 `ASAN_OPTIONS=detect_leaks=0 python3 tests/run_host_tests.py`（保留地址和未定义行为检查）。

主机测试和编译不能代替实板验收：需检查断电后选择恢复、OLED 方向/窗口、ESP32-C3 连续手动复位后的显示恢复、按键单次触发，
并接真实 RL78 验证成功、断线失败和网页取消时的显示及 LED。还需实板确认热点 MAC 后缀、启动提示 3 秒，以及连续两次 OTA（双分区轮换）、上传中断后仍启动旧固件。

还需实板验证：UART GPIO21→GPIO20 回环和外部设备收发，USB 日志路由，I2C 扫描/读写及 OLED 同时运行，SPI MOSI→MISO 回环与 CS/时钟波形，GPIO 读写，以及 ADC 对照已知电压的测量。主机模拟不能证明真实总线时序或 ADC 精度。

## 注意事项

- 烧写期间目标 RESET 由本设备控制；烧写完成后设备会复位目标并运行其应用程序。
- 仅支持代码闪存的烧写/校验，不支持数据闪存（Data Flash）与安全位设置。
- 若目标芯片 RESET 引脚被配置为普通 IO，需先断电，按住目标板复位拉低后上电再
  进行连接（即上电时序进入编程模式）。
