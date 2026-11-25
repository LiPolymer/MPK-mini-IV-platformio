# MPK mini IV 应用开发模板（PlatformIO）

[English](README.md) | 简体中文

适用于 **Akai Professional MPK mini IV** 的 PlatformIO 裸机开发模板与板级支持包（`mpk4-bsp`），基于 [open-mpk4](https://github.com/giusmarci/open-mpk4) 中的逆向成果整理修补而成。目标芯片为 `AT32F405RCT7`（`216 MHz`、`256 KiB` Flash、`96 KiB` SRAM，USB VID/PID `09E8:005D`）。

默认工程在 [src/main.cpp](src/main.cpp) 里初始化板卡和看门狗，在 `160x80` 屏幕上居中画出一个 `hello world!`，并给出面板输入与 USB MIDI 的主循环骨架。

## 环境要求

- [PlatformIO](https://platformio.org/)
- Python 3（首次运行 `tools/mpk4_flash.py` 时如缺少 `hidapi`，或在 Linux/macOS 缺少 `mido`、`python-rtmidi`，脚本会自动通过 `pip` 安装）

## 编译与烧录

### 1. 编译应用包

```sh
pio run -e mpk4
```

默认使用 [platformio.ini](platformio.ini) 中的 `custom_firmware_version = 41.00`，编译后会生成 `.pio/build/mpk4/mpk4_app_package.bin`。这个文件里只有你自己的应用代码、版本号、存储区边界和尾部 SHA-256 校验，不包含任何 Akai 原厂数据。[release/mpk4_app_package.bin](release/mpk4_app_package.bin) 另附有一份预编译好的示例包。

### 2. 准备原厂固件（仅首次需要）

仓库本身不分发 Akai 二进制文件。刷机前，工具会在本地基于 Akai 原厂 `v1.41` 或 `v1.43` 固件（SHA-256 校验值见 [tools/mpk4_layout.py](tools/mpk4_layout.py)）拼出完整的 `256 KiB` 镜像。首次使用运行一次拉取即可：

```sh
# 通过 HTTP Range 仅从 Akai 公开安装包中下载约 80 KB 的固件数据
python tools/fetch_stock.py

# 如果本地已有 .pkg 安装包、Payload、cpio 或原厂 .bin，也可以直接离线提取
python tools/fetch_stock.py --from-file path/to/stock_or_installer
```

校验通过的镜像会缓存在 `~/.openmpk4/MPKminiIV_v1.41_stock.bin`（可通过 `$OPENMPK4_HOME` 修改目录；若当前目录存在 `backup/firmware/MPKminiIV_v1.41_stock.bin` 会优先读取）。

### 3. 烧录到键盘

**注意：** 刷机过程会擦除 `0x06000–0x3EFFF`（包含应用区与用户预设/存储区），有需要保留的预设请提前备份。出厂序列号、滑轮校准参数以及 `0x3F000–0x3FFFF` 区域不会受影响。

在 Windows 上刷机前，请先关闭正在占用 MPK mini IV 端口的 DAW 或 MIDI 软件，因为 `mpk4_flash.py` 会通过 Windows Kernel Streaming（`usbaudio.sys`）直接发送进入升级模式的 SysEx。

```sh
pio run -e mpk4 -t upload
# 或者直接调用刷写脚本：
python tools/mpk4_flash.py flash .pio/build/mpk4/mpk4_app_package.bin --write
```

### 离线检查与救砖

不连接硬件时，可以先做一次空跑检查，或直接导出完整的 `256 KiB` 镜像：

```sh
python tools/mpk4_flash.py flash .pio/build/mpk4/mpk4_app_package.bin
python tools/mpk4_flash.py flash .pio/build/mpk4/mpk4_app_package.bin --stock path/to/stock.bin
python tools/mkimage.py .pio/build/mpk4/mpk4_app_package.bin full_image.bin
```

其他常用命令：

```sh
python tools/mpk4_flash.py probe               # 查看设备当前状态（'app' 或 'bootloader <版本>'）
python tools/mpk4_flash.py enter-update --send # 发送升级 SysEx：F0 47 00 5D 74 00 00 F7
python tools/mpk4_flash.py flash stock --write # 刷回 Akai 原厂固件
```

万一自己写的固件卡死、不再响应 SysEx，只要拔掉 USB，按住 **SHIFT + UNDO** 再插回电脑，屏幕显示 `Upgrade` 后就能重刷。此外，[startup.c](lib/mpk4-bsp/src/hal/startup.c) 里的硬件异常和未处理中断也会直接重启进 Bootloader 升级模式，一般不用担心变砖。

## 编写自己的固件

应用代码写在 [src/main.cpp](src/main.cpp)，固件版本号在 [platformio.ini](platformio.ini) 的 `custom_firmware_version` 里修改（需为 `NN.NN` 两位 BCD 格式，编译时由 [scripts/build.py](scripts/build.py) 检查）。

使用 BSP 时有几个地方顺手提一下：

- **初始化与喂狗：** 启动时调用 `mpk4::Board::begin()` 和 `mpk4::Board::enableWatchdog()`，主循环每轮开头记得调 `mpk4::Board::poll()`。如果 `Board::begin()` 返回 `false`，说明外部 `12 MHz` 晶振没起振，芯片会退回 `8 MHz` 内部时钟运行，此时没有 USB，但键盘、旋钮和 DIN MIDI 还能照常工作。
- **别漏掉 `UsbMidi::read()`：** 就算你的固件不需要处理电脑发来的 MIDI 数据，主循环里也一定要持续调用 `mpk4::UsbMidi::read(packet)`。它会顺带监听 cable 0 上的 `F0 47 00 5D 74 00 00 F7` 指令，收到后自动重启进入 Bootloader。
- **屏幕 DMA 是非阻塞的：** `mpk4::Display::draw()` 通过 SPI2 DMA 刷屏（一帧约 `15.2 ms`），上一帧没发完时会直接返回 `false`，下一轮循环再试就行。[src/main.cpp](src/main.cpp) 里的 `drawGreeting()` 只内置了 `"hello world!"` 几个字模，写自己界面时可以直接删掉。
- **Flash 存储擦除停顿：** 如果用到了掉电保存槽位（[storage.h](lib/mpk4-bsp/include/mpk4/storage.h)），启动时调用 `storage_init()`，主循环每轮调用 `storage_poll(idle)`。擦除扇区会阻塞 Flash 读取约 `16 ms`（放在 SRAM 里的 `50 kHz` 定时器中断和 SysTick 不受影响），所以只在允许短暂停顿的时候传 `idle = true`，比如 `Controls::idleMillis() >= 200`。

### BSP 模块一览（[bsp.hpp](lib/mpk4-bsp/include/mpk4/bsp.hpp)）

| 模块 | 说明 |
| :--- | :--- |
| `mpk4::Board` | 时钟与外设初始化、`poll()`、看门狗、`millis()`、`ticks()`（`20 µs`）、`serial()`、`rebootToBootloader()` |
| `mpk4::Controls` | 事件队列 `read()`，实时读取琴键/打击垫/按键/ADC/旋钮/滑轮/编码器/踏板，切换力度曲线 `0..2` |
| `mpk4::Leds` | 32 级 PWM 调光：`pad(index, r, g, b)`、`button(btn, brightness, secondColour)`、`beatButton(...)` |
| `mpk4::Display` | `160x80` RGB565 `Canvas`（`clear`、`pixel`、`fillRect`）、`rgb565()`、非阻塞 DMA `draw()` |
| `mpk4::UsbMidi` | 5 路下行 / 4 路上行 USB-MIDI 1.0 端口（MIDI、DAW、Plugin、Software Control、DIN Out） |
| `mpk4::DinMidi` | `PB6` 引脚 `31250` 波特率 DIN MIDI 输出（`write()`、高优先级 `realtime()`、`queued()`） |
| `mpk4::MidiClock` | 运行在 `50 kHz` 中断里的 `20–300` BPM `24 PPQN` 时钟，支持转发到 DIN 与节拍灯同步 |
| [storage.h](lib/mpk4-bsp/include/mpk4/storage.h) | C 接口的掉电安全内部 Flash 存储，共 19 个槽位（单槽上限 `STORAGE_MAX_BLOB = 12144` 字节） |

## 输入事件（`mpk4::InputEvent`）

字段定义见 [bsp.hpp](lib/mpk4-bsp/include/mpk4/bsp.hpp)。`mpk4::Controls::read(event)` 每次弹出 `{ kind, index, value, delta, sample }`：

| `InputKind` | `index` | `value` | `delta` | `sample` | 说明 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `keyOn` | 琴键 `0..24`（`0` = 最低 C） | 力度 `1..127` | 奇数（第二）触点先闭合为 `1`，否则 `0` | 两触点时间差 `dt`（单位 `20 µs`，上限 `16383`） | 按当前 `keyCurve` 换算力度 |
| `keyOff` | 琴键 `0..24` | `0` | `0` | `0` | — |
| `buttonDown` | `uint8_t(Button)` | `0` | `0` | `0` | `10 ms` 按下消抖 |
| `buttonUp` | `uint8_t(Button)` | `0` | `0` | `0` | 松开立即上报 |
| `knob` | 旋钮 `0..7`（`K1..K8`） | `0` | 有符号步数（每步 `2°`） | 每步耗时（`ms`，上限 `200`） | 静止超过 `200 ms` 后需转 `6°` 唤醒（上报 `±1`） |
| `encoder` | `0` | `0` | 有符号格数 | `0` | 按压作为 `Button::encoder` 上报 |
| `padOn` | 打击垫 `0..7` | 力度 `1..127` | `0` | 10 位峰值 ADC（`pad_raw >> 2`） | 阈值 `209`，约 `20 ms` 窗口取峰值 |
| `padPressure` | 打击垫 `0..7` | 触后压力 `1..127` | `0` | 当前 10 位 ADC 采样 | `padOn` 后 `200 ms` 开启，有变化时每 `~50 ms` 上报 |
| `padOff` | 打击垫 `0..7` | `0` | `0` | `0` | 保持约 `20 ms` 后低于 `62` 时释放 |
| `wheel` | `0`（`pitch`）/ `1`（`modulation`） | `0` | 弯音 `-8192..8191`，调制 `0..127` | 迟滞滤波后的 12 位 ADC | 弯音轮中心有 `±300` 死区 |
| `pedal` | `0` | 引脚电平（`1` = 高电平/断开） | `0` | `0` | `PA7` 引脚，`5 ms` 消抖 |
| `matrix` | 驱动线 `0..10` | 闭合位掩码（`1` = 闭合） | `0` | 发生变化的位掩码 | 仅 `enableRawEvents(true)` 时输出 |
| `adc` | ADC 通道 `0..5` | 多路复用地址 `0..7` | `0` | 12 位 ADC 采样值 | 仅 `enableRawEvents(true)` 且变化 `>= 32` 时输出 |

`mpk4::Button` 枚举索引：`0` `octaveDown`、`1` `octaveUp`、`2` `arp`、`3` `latch`、`4` `noteRepeat`、`5` `tap`、`8` `bank`（双色灯）、`9` `minus`、`10` `plus`、`11` `shift`、`12` `daw`（双色灯）、`13` `undo`、`14` `encoder`（无灯）、`16` `loop`、`17` `play`、`18` `record`、`19` `overdub`。

## Flash 分区与目录结构

Flash 布局对应 [ld/app.ld](ld/app.ld) 和 [tools/mpk4_layout.py](tools/mpk4_layout.py)：

| 地址范围 | 大小 | 用途 |
| :--- | :--- | :--- |
| `0x08000000–0x08005FFF` | `24 KiB` | Akai Bootloader（从原厂镜像复制） |
| `0x08006000–0x0800BFFF` | `24 KiB` | Bootloader 暂存副本（须与 Bootloader 一致） |
| `0x0800C000–0x0802BFFF` | `128 KiB` | 应用区（`FLASH`，中断向量表在 `0x0800C000`） |
| `0x0802C000–0x080387FF` | `50 KiB` | 槽位存储区（`STORAGE`，25 个 `2 KiB` 扇区，刷机时擦除） |
| `0x08038800–0x08038FFF` | `2 KiB` | 应用有效标志扇区（`0x38FFE–0x38FFF` 来自原厂镜像） |
| `0x08039000–0x0803EFFF` | `24 KiB` | 原厂未使用的应用数据页（打包填充 `0xFF`） |
| `0x0803F000–0x0803F7FF` | `2 KiB` | 受保护备份页（刷机升级时不擦写） |
| `0x0803F800–0x0803FFFF` | `2 KiB` | 设置页与校验尾（序列号、校准数据、版本号、校验和） |

主要文件位置：
- [src/main.cpp](src/main.cpp)：应用入口
- [platformio.ini](platformio.ini) 与 [boards/mpk_mini_iv.json](boards/mpk_mini_iv.json)：PlatformIO 配置与板卡定义
- [lib/mpk4-bsp/](lib/mpk4-bsp/library.json)：板级支持包（`TinyUSB 0.21.0`、ArteryTek `AT32F402/405` 库 `v2.1.5`）
- [scripts/](scripts/package.py)：编译前后置与打包脚本（`build.py`、`package.py`）
- [tools/](tools/mpk4_flash.py)：原厂固件拉取与刷写工具（`fetch_stock.py`、`mkimage.py`、`mpk4_flash.py`、`mpk4_layout.py`）

## 许可协议

本项目源码采用 MIT 协议（[LICENSE](LICENSE)），第三方组件许可及商标声明见 [NOTICE](NOTICE)。本项目为独立开源项目，与 Akai Professional 及 inMusic Brands, Inc. 无关。
