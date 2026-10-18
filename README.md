# 工业物联网网关（Modbus RTU / CAN / MQTT）

一个跑在 **STM32F103ZET6** 上的工业网关固件：对下讲 **Modbus RTU**（既是**从站**被上位机读写，也是**主站**轮询下游仪表），对现场收 **CAN**，对上通过 **ESP32** 走 **MQTT**（已对接中国移动 OneNET），本地带 **2.4" 触摸屏** UI，并支持 **A/B 双槽 OTA + 出厂 golden 镜像**。

> 设计取向：**分层解耦 + 可测试 + 可观测**。
> 纯逻辑层（协议栈、管道、参数、OTA、调度）**可在 PC 上编译并跑测试**，硬件层通过 vtable 注入；
> 所有"当前状态"都暴露成寄存器或全局符号，**能用 SWD/串口取证，而不是靠猜**。

---

## 1. 系统架构

```
                    ┌──────────────── 上位机 / 组态软件 ────────────────┐
                    │           Modbus RTU 主站（功能码 01/03/04/06/16）│
                    └───────────────────────┬──────────────────────────┘
                                            │ RS485（USART2 + DMA + IDLE）
┌───────────────────────────────────────────▼───────────────────────────────────────────┐
│                                  STM32F103ZET6                                        │
│                                                                                       │
│  ┌───────────── 纯逻辑层（PC 可编译，100% 主机测试覆盖）───────────────────────────┐   │
│  │  modbus_rtu   gw_frame   gw_pipeline   gw_param   gw_ota   gw_log   gw_sched   │   │
│  │  gw_cmd       gw_timing  rs485_arbiter gw_fault   gw_selfcheck                 │   │
│  └──────────────────────────────▲──────────────────────────────▲──────────────────┘   │
│                                 │ vtable 注入                    │                    │
│  ┌──────────────────────────────┴──────────┐   ┌───────────────┴────────────────┐     │
│  │  app/  模块编排（三张 vtable 定义）      │   │  port/  硬件层                 │     │
│  │  gw_app / gw_lvgl_ui / gw_ui            │   │  RS485 · CAN · Flash · DWT     │     │
│  └─────────────────────────────────────────┘   │  DHT22 · ESP32 · LCD · Touch   │     │
│                                                │  IWDG · FreeRTOS 适配          │     │
│  ┌─────────────────────────────────────────────┴───────────────────────────────┐      │
│  │  FreeRTOS：7 个任务（SLAVE/MASTER/CAN/UPLINK/SENSOR/HOUSE/UI）+ 逐任务心跳   │      │
│  └─────────────────────────────────────────────────────────────────────────────┘      │
└──────────┬──────────────────────┬─────────────────────────┬───────────────────────────┘
           │ CAN1                  │ USART3（帧协议）          │ SPI1
    ┌──────▼──────┐        ┌──────▼───────┐          ┌───────▼───────────┐
    │ CAN 现场总线 │       │    ESP32     │          │ ILI9341 + XPT2046 │
    │（只收不发）  │       │  MQTT 上云   │          │  2.4" 触摸屏      │
    └─────────────┘        └──────┬───────┘          └───────────────────┘
                                  │ WiFi / MQTT（OneJSON）
                           ┌──────▼──────────┐
                           │ OneNET / EMQX   │
                           └─────────────────┘
```

**为什么把 ESP32 单独拿出来做上云**：STM32F103 没有无线，而 TLS + MQTT 协议栈放在 F103 上会吃掉本就紧张的 64 KB RAM。
拆成"STM32 管实时控制、ESP32 管网络"之后，两边各自简单，中间用一条**带帧边界与重发**的串口协议连接。

---

## 2. 核心特性

### 2.1 Modbus RTU 主从共存

- **从站**：功能码 01/03/04/06/16，**40 路输入寄存器 + 16 路保持寄存器**
- **主站**：轮询下游从机，支持滤波/掩码、超时、重试、在线位图
- **总线仲裁**：主从共用一条 RS485，用仲裁器保证"主站提问时不插话"
- 大端序、CRC16 全程严格；异常响应码符合规范

### 2.2 数据管道与断网补传

- 统一采集 → 归一化 → 上报管道，**传感器与上云解耦**
- **断网缓存队列**：网断了不丢数据，但**溢出丢包计数可见**（`IREG_OFFLINE_DROP`）——
  不暴露丢包数的"不丢数据"是假话

### 2.3 A/B 双槽 OTA + 出厂 golden

| 区域 | 地址 | 大小 |
|---|---|---|
| Bootloader | `0x08000000` | 32 KB |
| 元数据（双页 ping-pong） | `0x08008000` | 4 KB（2 页） |
| 应用槽 A | `0x08009000` | 136 KB |
| 应用槽 B | `0x0802B000` | 136 KB |
| Golden 出厂镜像 | `0x0804D000` | 136 KB |

- 元数据**双页轮换**：任何时刻掉电，至少一份有效（35 项断电注入用例守护）
- 启动计数 + 回滚：新镜像连续启动失败自动回退
- 断电穷举验证：**26 个断电点 × 全部不变式**

### 2.4 可观测性（本项目的核心方法论）

所有关键状态都可从外部读取，而不是只在代码里：

- **Modbus 寄存器**：40 路输入寄存器把运行态（温度/湿度/状态位/各类计数/耗时）暴露给上位机
- **全局符号**：关键计数器都是 `volatile` 全局量，可用 SWD 直接读（`s_feed_ok`/`s_sched`/`g_touch_*` …）
- **故障记录**：`.noinit` 段跨复位保存最后异常类型
- 这条原则贯穿到工具链：`soak_monitor.py` 每 5 分钟采样一次并**对每一列给出明确判据**

### 2.5 本地 UI

- **LVGL v8.2.0** 版（生产/冻版）：4 页签（总览 / CAN / 诊断 / 配置）
- **自绘 UI** 版（精简，用于无 LVGL 环境）
- 电阻屏 XPT2046 触摸，已上板标定（轴互换 + 双轴反向）

---

## 3. 关键设计取舍（这部分比功能列表更能说明问题）

| 取舍 | 选择 | 理由 |
|---|---|---|
| 云端后端 | **编译期切换**（OneNET / EMQX） | 两套 topic 与报文格式不兼容；运行期切换会引入大量分支与 RAM |
| LVGL 对象 | 所有控件 `radius=0`，`LV_DRAW_COMPLEX=0` | 省 20 KB Flash（槽占用 99.8% → 84.2%）；代价是控件绘制语义变化，已用"轨道+填充"两个矩形替代 `lv_bar` |
| 绘制缓冲 | 320×10（6.4 KB），非官方推荐的 1/10 屏 | 省 8.6 KB RAM，代价是整屏分 24 块刷 |
| 第三方源码 | **不入库**，用 `deps.lock` 钉版本 + 哈希 | 仓库 `.git` 仅 2.9 MB；LVGL 必需部分就有 14.3 MB。`fetch_deps.ps1` + `check_deps.py` 保证可复现 |
| 上云协议栈 | 放 ESP32 | F103 的 64 KB RAM 撑不住 TLS + MQTT |
| 看门狗 | IWDG `PR=5/RLR=999`（**3.2 s** 窗口） | 独立于内核；喂狗成功/拒喂都计数，可判真伪 |

---

## 4. 验证结果（真实数字）

### 4.1 主机端测试与 CI

```
32 套测试  /  2609 项断言  /  0 失败
43 个模块零警告（-Wall -Wextra）
22 个 CI 步骤
148 个 C/H 文件，共 57,771 行
```

CI 覆盖：主机端单元测试、交叉编译零警告、**源文件 BOM 检查**、工具脚本编译与 GBK 控制台编码检查、
寄存器/标识符一致性、API 覆盖检查、OTA 断电穷举、参数断电穷举、文档与代码配置一致性。

### 4.2 上板实测（端到端）

| 环节 | 结果 |
|---|---|
| Modbus 主从共存 | ✅ 位图 / 保持寄存器 / 错误码 SWD 铁证 |
| DHT22 采集 | ✅ 28/28 次成功、0 错误 |
| 本地 UI 4 页签 | ✅ 可切换、颜色正确 |
| **上行** STM32→ESP32→OneNET | ✅ 15/15 属性 `code=200` |
| **下行** 云→ESP32→STM32 | ✅ `cmd done: … -> ACK`（100 ms 内） |
| OTA 断电穷举 | ✅ 26 断电点，0 不变式违反 |
| 参数断电穷举 | ✅ 275 个断电点 + 15 轮连续掉电，0 违反 |

### 4.3 浸泡测试

`soak_monitor.py` 每 5 分钟经 **SWD hotplug**（不占 RS485 总线）采样，判据包括：
uptime 单调性、`RCC_CSR` 复位标志、IWDG 配置漂移、元数据零擦写、`s_feed_reject==0`、逐任务心跳。

> **已完成的连续运行：8.34 小时 / 101 个采样点 / 0 异常 / 0 复位。**
> 72 小时完整浸泡**尚未完成**（过程中因设备调试中断），详见 `docs/04-已知问题与后续.md`。

---

## 5. 构建与烧录

### 5.1 取第三方依赖

```powershell
powershell -File tools\fetch_deps.ps1     # 按 deps.lock 下载并校验 LVGL
python tools\check_deps.py                # 校验版本与源码树哈希
```

### 5.2 构建 LVGL 

```powershell
tools\build_ota_images.ps1 -Rtos -Optimize s -Lvgl `
  -OutDir <repo>\build\ota `
  -Toolchain <arm-none-eabi-gcc 所在 bin> `
  -Freertos <FreeRTOS Source 目录>
```

产物：`app_a.bin` 117,260 B（槽占用 84.2%）、`bootloader.bin` 19,416 B。

### 5.3 烧录（⚠️ 必须用 `mode=UR`）

```powershell
# hotplug 模式下内核正在运行、要擦的是它自己所在的区，会报 failed to erase memory
STM32_Programmer_CLI -c port=SWD mode=UR `
  -w build\ota\bootloader.bin 0x08000000 -v `
  -w build\ota\app_a.bin      0x08009000 -v -rst
```

### 5.4 跑测试

```powershell
powershell -File tools\ci.ps1      # 全量 CI
python tools\check_deps.py --fast  # 只查依赖
python tools\soak_status.py        # 浸泡进度与健康摘要
```

---

## 6. 目录结构

### 6.1 本资料包

```
README.md                    本文件（项目展示页）
docs/                        技术文档
  01-架构设计.md
  02-Modbus寄存器映射.md
  03-验证与测试.md
source/                      ★ 与板载固件严格对应的完整源码快照（可复现）
  README.md                    快照说明 + 重建步骤 + 校验方法
  stm32/  cubemx/  esp32/      主仓库源码 / CubeMX 工程（构建输入）/ ESP32 源码
  image/                       板上运行的 app_a.bin + bootloader.bin
```

### 6.2 源码内部分层

```
inc/         纯逻辑层头文件（协议、管道、OTA、参数、日志、调度、仲裁、时间预算）
src/         纯逻辑层实现（可在 PC 编译，无硬件依赖）
app/         模块编排 + 三张 vtable 定义
port/        硬件层（RS485 / CAN / Flash / DWT / DHT22 / ESP32 / LCD / Touch / IWDG / RTOS）
drivers/     DHT22 解码（纯逻辑）+ 硬件抽象
templates/   main 模板（与 CubeMX 生成的 main.c 合并后才是工程真 main）
esp32/       ESP32 侧固件（MQTT / OneJSON / 串口帧协议）
test/        32 套主机端测试
tools/       PC 侧工具 + CI（探针、浸泡监控、OTA 发送、依赖检查、串口抓取…）
ld/          链接脚本（bootloader / 槽 A / 槽 B / golden）
doc/         设计文档与调试记录
deps.lock    第三方依赖版本与哈希锁定
---

本仓库**不含任何真实凭据**。WiFi / OneNET 的 SSID、密码、产品 ID、访问密钥、token
均以占位符形式给出，需要你在本地填写：

```c
/* esp32/src/main.c —— 用你自己的值替换 */
#define GW_WIFI_SSID   "<YOUR_WIFI_SSID>"
#define GW_WIFI_PASS   "<YOUR_WIFI_PASSWORD>"
#define GW_MQTT_USER   "<YOUR_PRODUCT_ID>"
#define GW_MQTT_PASS   "<GENERATED_TOKEN>"
```

token 生成：`python tools\onenet_token.py`（需要产品的访问密钥）。

---

## 7. 技术栈

**MCU**：STM32F103ZET6（Cortex-M3，64 KB RAM / 512 KB Flash）
**RTOS**：FreeRTOS（7 任务 + 逐任务心跳监控）
**显示**：ILI9341 240×320 + XPT2046 电阻触摸，LVGL v8.2.0
**协议**：Modbus RTU（主+从）、CAN 2.0B、自定义串口帧协议
**云**：MQTT + OneJSON（OneNET）/ 通用 topic（EMQX）
**工具链**：arm-none-eabi-gcc 13.3、STM32CubeIDE、PowerShell CI、Python 侧工具链
