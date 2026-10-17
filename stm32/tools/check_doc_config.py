# -*- coding: utf-8 -*-
"""
检查「文档里写的硬件配置」与「代码里的配置常量」是否一致

============================================================================
 为什么需要这个检查

这个项目交付的是**源码 + 文档**，用户要照着文档在 CubeMX 里配外设。

一旦文档与代码不一致，后果是**静默的**：

  - 文档说 PA2/PA3，代码常量是别的引脚 ->
    `gw_config.h` 里那条 `#define` 只用于代码内部（DE 控制、DHT22 读取），
    **不会去改 CubeMX 生成的 UART 引脚配置** ——
    于是代码以为在用某个引脚，硬件实际是另一个。
    现象是"串口完全没数据"，而两边看起来都对。

  - 文档说 9600，代码说别的 ->
    波特率切换的初值是从 BRR 反算的，所以这里不会直接出错；
    但 `GW_RS485_UART_BAUD` 被用作"读不出来时的兜底值"，
    不一致时兜底就兜错了。

这种错误**编译器查不出、单元测试也查不出**（它们都在 PC 上跑，
不碰真实引脚）。所以只能靠这个脚本比对。

============================================================================
 判定方式与边界

做法：从 `port/gw_config.h` 抽出 `#define GW_xxx` 的值，
      在 `doc/CubeMX配置清单.md` 里找对应的描述，比对。

  ⚠️ 这是**文本比对**，不是语义分析。它依赖文档里用了可识别的表述
     （例如引脚写成 `PA2`、波特率写成 `9600`）。
     文档改写成别的表述方式时，本脚本会漏检 —— 那时应同步更新本脚本。

  它的定位是"**防止单边修改**"：改了代码忘了改文档（或反之）会被它抓到。
============================================================================
"""
import os
import re
import sys

CONFIG = "port/gw_config.h"
DOC = "doc/CubeMX配置清单.md"

# 每条规则： (代码常量名, 期望值, 文档里应出现的字符串, 说明)
RULES = [
    ("GW_RS485_UART_BAUD",  "115200", "115200", "RS485 波特率"),
    ("GW_DEBUG_UART_BAUD",  "115200", "115200", "调试串口波特率"),
    ("GW_ESP32_UART_BAUD",  "115200", "115200", "ESP32 串口波特率"),
    ("GW_CAN_BAUDRATE",     "500000", "500",    "CAN 位速率"),
    ("GW_RS485_TX_PIN",     "PIN_2",  "PA2",    "RS485 TX 引脚"),
    ("GW_RS485_RX_PIN",     "PIN_3",  "PA3",    "RS485 RX 引脚"),
    ("GW_RS485_DE_PIN",     "PIN_1",  "PA1",    "RS485 DE 引脚"),
    ("GW_ESP32_TX_PIN",     "PIN_10", "PB10",   "ESP32 TX 引脚"),
    ("GW_ESP32_RX_PIN",     "PIN_11", "PB11",   "ESP32 RX 引脚"),
    ("GW_ESP32_EN_PIN",     "PIN_12", "PB12",   "ESP32 EN 引脚"),
    ("GW_CAN_RX_PIN",       "PIN_8",  "PB8",    "CAN RX 引脚"),
    ("GW_CAN_TX_PIN",       "PIN_9",  "PB9",    "CAN TX 引脚"),
    ("GW_DHT22_PIN",        "PIN_11", "PG11",   "DHT22 数据脚"),
]

# 模块级定义（在 port/*.h 里，不在 gw_config.h）
#
# ⚠️ 这些地址的"文档位置"与引脚不同：引脚写在 doc/CubeMX配置清单.md，
#    而 Flash 分区地址写在 README.md（分区表）与 doc/最小上板验证包.md。
#    所以查找时要把这几份文档合起来搜。
EXTRA_DOCS = ["README.md", "doc/CubeMX配置清单.md", "doc/最小上板验证包.md"]

EXTRA_RULES = [
    ("port/gw_flash_map.h", "GW_PARAM_FLASH_ADDR", "0x0807C000", "0x0807C000",
     "参数区地址"),
    ("port/gw_flash_map.h", "GW_APP_A_ADDR",       "0x08009000", "0x08009000",
     "应用 A 分区地址"),
    ("port/gw_flash_map.h", "GW_APP_B_ADDR",       "0x0802B000", "0x0802B000",
     "应用 B 分区地址"),
    # ⚠️ 2026-10-11 分区表 v2：B 槽从 0x0803D000 搬到 0x0802B000（让出空间给 golden）。
    #    这条检查的价值在这次搬家上体现得很直接：它会强制文档跟着代码一起改 ——
    #    否则"文档说 0x0803D000、代码烧 0x0802B000"会害人按错的地址去操作 Flash。
    ("port/gw_flash_map.h", "GW_APP_GOLDEN_ADDR",  "0x0804D000", "0x0804D000",
     "golden 出厂镜像地址"),
]


def _eval_expr(text, expr, depth):
    """递归求值一个宏表达式，支持 + - * / 与括号（仅整数）"""
    if depth > 6:
        return None
    expr = expr.strip().strip("()").strip()
    if not expr:
        return None

    # 字面量
    lit = re.match(r"^(0[xX][0-9A-Fa-f]+|\d+)[uUlL]*$", expr)
    if lit:
        s = lit.group(1)
        return int(s, 16) if s.lower().startswith("0x") else int(s)

    # 括号内先求值，避免优先级问题：找最外层的 + / -（最低优先级）
    depth_paren = 0
    for i in range(len(expr) - 1, -1, -1):
        c = expr[i]
        if c == ")":
            depth_paren += 1
        elif c == "(":
            depth_paren -= 1
        elif depth_paren == 0 and c in "+-" and i > 0:
            left = _eval_expr(text, expr[:i], depth + 1)
            right = _eval_expr(text, expr[i + 1:], depth + 1)
            if left is None or right is None:
                return None
            return left + right if c == "+" else left - right

    # 再看 * /
    depth_paren = 0
    for i in range(len(expr) - 1, -1, -1):
        c = expr[i]
        if c == ")":
            depth_paren += 1
        elif c == "(":
            depth_paren -= 1
        elif depth_paren == 0 and c in "*/" and i > 0:
            left = _eval_expr(text, expr[:i], depth + 1)
            right = _eval_expr(text, expr[i + 1:], depth + 1)
            if left is None or right is None:
                return None
            if c == "*":
                return left * right
            return left // right if right else None

    # 最后当成宏名去查
    name = re.sub(r"[uUlL]+$", "", expr.strip())
    if re.match(r"^[A-Za-z_]\w*$", name):
        return resolve_define(text, name, depth + 1)
    return None


def resolve_define(text, name, depth=0):
    """取 #define 的值，并**解析它引用的其它宏与运算**

    ⚠️ 为什么需要解析：这些宏的定义是**表达式**而不是字面量：
           GW_APP_SLOT_SIZE  = (136u * 1024u)
           GW_APP_GOLDEN_ADDR = (GW_APP_B_ADDR + GW_APP_SLOT_SIZE)
       直接文本匹配 `0x0804D000` 会失败 —— 但它的实际值就是它。
       不解析就会误报，而**误报会让人不再相信检查**（那比没有检查更糟）。
    """
    if depth > 6:
        return None
    m = re.search(r"#define\s+" + re.escape(name) + r"\s+([^\n]+)", text)
    if not m:
        return None
    # ⚠️ 先剥掉行尾注释再解析表达式。
    #    例如 GW_APP_B_ADDR 的定义是：
    #        (GW_APP_A_ADDR + GW_APP_SLOT_SIZE)  /* 0x0802B000 */
    #    不剥注释的话，解析器会去查找一个叫 "0x0802B000" 的宏而失败。
    expr = re.sub(r"/\*.*?\*/", " ", m.group(1))
    expr = re.sub(r"//.*$", " ", expr)
    expr = expr.strip()
    if not expr:
        return None
    return _eval_expr(text, expr, depth)


def read(path):
    return open(path, encoding="utf-8", errors="replace").read()


def find_define(text, name):
    m = re.search(r"#define\s+" + re.escape(name) + r"\s+([^\s/]+)", text)
    return m.group(1) if m else None


def main():
    if not os.path.exists(CONFIG) or not os.path.exists(DOC):
        print("  [跳过] 找不到 %s 或 %s" % (CONFIG, DOC))
        return 0

    cfg = read(CONFIG)
    doc = read(DOC)
    # 合并多份文档：引脚在配置清单，Flash 地址在 README / 验证包
    alldocs = "\n".join(read(p) for p in EXTRA_DOCS if os.path.exists(p))

    problems = []
    checked = 0

    for name, expect, doc_str, desc in RULES:
        val = find_define(cfg, name)
        if val is None:
            problems.append("代码里找不到 %s（%s）" % (name, desc))
            continue
        checked += 1

        if expect not in val:
            problems.append("%s 的代码值是 %s，但本脚本期望含 %s（%s）"
                            % (name, val, expect, desc))

        if doc_str not in doc:
            problems.append("文档里找不到 %s（%s 的文档表述）" % (doc_str, desc))

    for path, name, expect, doc_str, desc in EXTRA_RULES:
        if not os.path.exists(path):
            problems.append("找不到文件 %s（%s）" % (path, desc))
            continue

        src = read(path)
        checked += 1

        # ⚠️ 解析宏表达式（GW_APP_B_ADDR = A + SIZE），不能直接文本匹配
        actual = resolve_define(src, name)
        want = int(expect, 16)

        if actual is None:
            problems.append("%s 的值无法解析（%s）" % (name, desc))
        elif actual != want:
            problems.append("%s 的实际值是 0x%08X，期望 0x%08X（%s）"
                            % (name, actual, want, desc))

        if doc_str.lower() not in alldocs.lower():
            problems.append("文档里找不到 %s（%s）—— 应出现在 %s 中"
                            % (doc_str, desc, " / ".join(EXTRA_DOCS)))

    # 反向检查：文档里提到的引脚，代码里是否都有定义
    doc_pins = set(re.findall(r"\bP([A-G])(\d{1,2})\b", doc))
    code_pins = set()
    for m in re.finditer(r"#define\s+GW_\w*_PIN\s+GPIO_PIN_(\d+)", cfg):
        code_pins.add(m.group(1))
    # 只检查"被显式声明为某个功能"的引脚，避免误报（例如 PA9/PA10 是
    # CubeMX 自动分配的调试串口，代码里没有对应常量）
    KNOWN_UNMAPPED = {"9", "10"}      # USART1 调试口，由 CubeMX 决定

    for port, num in doc_pins:
        if num in KNOWN_UNMAPPED:
            continue
        if num not in code_pins:
            # 只有当这个引脚在文档里带有 GW_ 风格的功能名时才报
            pass    # 暂不检查：doc 里引脚很多，误报率高

    # ------------------------------------------------------------------
    #  IDLE+DMA 开关是否与用户的 HAL 版本匹配
    #
    #  ⚠️ 为什么需要这一项（真实踩过的坑）：
    #
    #     `GW_HAL_HAS_UARTEX_IDLE` 原来默认 0（按 V1.8.0 保守编译）。
    #     CubeIDE 工程里没定义它 -> 走 0 的路径 ->
    #     用 `HAL_UART_Receive_IT` 逐字节中断收帧。
    #     而工程实际是 V1.8.7（有 IDLE+DMA）
    #     -> **主方案被静默绕过**，跑的是回退方案。
    #
    #     后果不是"不能用"，而是"用的不是设计的那条路"：
    #       逐字节中断在 115200 下每字节约 87us 一次中断；
    #       而且回退路径少了 `__HAL_DMA_DISABLE_IT(HT)`，
    #       "一帧超过半缓冲被切成两帧"的风险没被消除。
    #
    #     这类"配置漏了 -> 静默降级"的问题，编译器不会提醒。
    # ------------------------------------------------------------------
    RTL = r"D:\stm32cubeIDE\gateway2"
    hal_uart_h = os.path.join(
        RTL, "Drivers", "STM32F1xx_HAL_Driver", "Inc",
        "stm32f1xx_hal_uart.h")

    if os.path.isfile(hal_uart_h):
        hal_src = read(hal_uart_h)
        hal_has_idle = "HAL_UARTEx_ReceiveToIdle_DMA" in hal_src

        m = re.search(r"#define\s+GW_HAL_HAS_UARTEX_IDLE\s+(\d+)", cfg)
        macro_val = int(m.group(1)) if m else None

        user_def = None
        cproj = os.path.join(RTL, ".cproject")
        if os.path.isfile(cproj):
            us = read(cproj)
            m2 = re.search(r"GW_HAL_HAS_UARTEX_IDLE=(\d+)", us)
            if m2:
                user_def = int(m2.group(1))

        checked += 1
        effective = user_def if user_def is not None else macro_val

        if hal_has_idle and effective == 0:
            problems.append(
                "用户工程的 HAL 有 HAL_UARTEx_ReceiveToIdle_DMA，"
                "但 GW_HAL_HAS_UARTEX_IDLE 的有效值是 0"
                "（用户工程显式定义=%s，代码默认=%s）-> "
                "主方案 IDLE+DMA 被静默绕过，实际跑逐字节中断的回退路径。"
                "修法：CubeIDE 预处理器里加 GW_HAL_HAS_UARTEX_IDLE=1，"
                "或删掉显式的 0 以使用新的默认值 1"
                % (user_def, macro_val))
        elif (not hal_has_idle) and effective == 1:
            problems.append(
                "用户工程的 HAL 没有 HAL_UARTEx_ReceiveToIdle_DMA，"
                "但 GW_HAL_HAS_UARTEX_IDLE 的有效值是 1 -> 会编译失败。"
                "修法：定义 GW_HAL_HAS_UARTEX_IDLE=0")
        else:
            print("  [info] IDLE+DMA 开关与 HAL 匹配：HAL 支持=%s，"
                  "有效值=%s（用户工程显式定义=%s）"
                  % (hal_has_idle, effective, user_def))
    else:
        print("  [info] 未找到用户工程的 stm32f1xx_hal_uart.h，跳过该项比对")

    # ------------------------------------------------------------------
    #  CAN 位时序的自洽性检查
    #
    #  ⚠️ 为什么要单独查这一项（这是一个真实漏检的教训）：
    #
    #     本脚本原来只比对引脚号与波特率**数值**（"有没有 500000"），
    #     而 CAN 的位时序是一组**互相约束**的参数：
    #
    #         波特率 = APB1 / (Prescaler × (SYNC + BS1 + BS2))
    #         BS1 <= 16, BS2 <= 8, SJW <= 4（bxCAN 硬件限制）
    #
    #     本项目文档曾经推荐 `Prescaler=3, BS1=20` ——
    #     **BS1=20 超出硬件上限 16，CubeMX 里根本选不出来**，
    #     而且那个组合与本项目代码里的值还不一致。
    #
    #     引脚和波特率数值当时都是"对的"，所以本脚本放过了它。
    #     是用户在实际配置时撞到"下拉框里没有 20"才发现的。
    #
    #     现在补上：范围校验 + 波特率自洽 + 文档与代码一致。
    # ------------------------------------------------------------------
    can_src = read(os.path.join("port", "gw_can_port.c"))
    can_hdr = read(os.path.join("port", "gw_can_port.h"))
    can_doc = read(os.path.join("doc", "CubeMX配置清单.md"))

    def get_can_def(text, name):
        m = re.search(r"#define\s+" + name + r"\s+(\d+)u?", text)
        return int(m.group(1)) if m else None

    c_presc = get_can_def(can_src, "CAN_PRESCALER")
    c_bs1   = get_can_def(can_src, "CAN_BS1")
    c_bs2   = get_can_def(can_src, "CAN_BS2")
    c_sjw   = get_can_def(can_src, "CAN_SJW")
    c_apb1  = get_can_def(can_src, "CAN_APB1_HZ")
    c_rate  = get_can_def(can_src, "CAN_TARGET_BITRATE")

    if None in (c_presc, c_bs1, c_bs2, c_sjw, c_apb1, c_rate):
        problems.append("CAN 位时序宏缺失（gw_can_port.c 里应有 "
                        "CAN_PRESCALER/CAN_BS1/CAN_BS2/CAN_SJW/"
                        "CAN_APB1_HZ/CAN_TARGET_BITRATE）")
    else:
        checked += 1

        # (a) 硬件范围
        if not (1 <= c_bs1 <= 16):
            problems.append("CAN_BS1 = %d 超出 bxCAN 允许的 1..16 —— "
                            "CubeMX 里选不出来" % c_bs1)
        if not (1 <= c_bs2 <= 8):
            problems.append("CAN_BS2 = %d 超出 bxCAN 允许的 1..8" % c_bs2)
        if not (1 <= c_sjw <= 4):
            problems.append("CAN_SJW = %d 超出 bxCAN 允许的 1..4" % c_sjw)
        if not (1 <= c_presc <= 1024):
            problems.append("CAN_PRESCALER = %d 超出 1..1024" % c_presc)

        # (b) 波特率自洽
        tq_total = 1 + c_bs1 + c_bs2
        actual_rate = c_apb1 // (c_presc * tq_total)
        if actual_rate != c_rate:
            problems.append(
                "CAN 位时序算出来是 %d bps，目标 %d bps"
                "（Prescaler=%d, BS1=%d, BS2=%d, 总TQ=%d）"
                % (actual_rate, c_rate, c_presc, c_bs1, c_bs2, tq_total))

        # (c) 总 TQ 落在工程建议区间
        if not (8 <= tq_total <= 25):
            problems.append("CAN 总 TQ = %d 不在建议的 8..25 内"
                            "（采样点精度/容忍度折中）" % tq_total)

        # (d) 文档里推荐的值必须与代码一致
        #
        # ⚠️ 正则必须收得很紧。文档里有**两张**表都含这些词：
        #     范围表：  | `Time Quanta in BS1` | **1..16** | ...
        #     推荐表：  | Time Quanta in BS1 | **15** |
        #   第一版用 `BS1[^|\n]*\|\s*\**\s*(\d+)` 会先匹配到**范围表**，
        #   把 "1..16" 里的 1 当成推荐值 —— 于是永远报"不一致"。
        #   现在要求：闭合的 `|` 紧邻取值，且取值是**单个数字**（不是 `1..16`）。
        def doc_table_value(label_pattern):
            m = re.search(
                r"\|\s*`?" + label_pattern + r"`?\s*\|\s*\**\s*(\d+)\s*\**\s*\|",
                can_doc)
            return int(m.group(1)) if m else None

        doc_presc = doc_table_value(r"Prescaler")
        doc_bs1   = doc_table_value(r"(?:Time Quanta in\s*)?BS1")
        doc_bs2   = doc_table_value(r"(?:Time Quanta in\s*)?BS2")

        if doc_presc is None:
            problems.append("文档里找不到 CAN Prescaler 的推荐值（表格行）")
        elif doc_presc != c_presc:
            problems.append("文档推荐 CAN Prescaler=%d，代码是 %d"
                            % (doc_presc, c_presc))

        if doc_bs1 is None:
            problems.append("文档里找不到 CAN BS1 的推荐值（表格行）")
        elif doc_bs1 > 16:
            problems.append("文档推荐的 CAN BS1=%d 超出硬件上限 16"
                            % doc_bs1)
        elif doc_bs1 != c_bs1:
            problems.append("文档推荐 CAN BS1=%d，代码是 %d"
                            % (doc_bs1, c_bs1))

        if doc_bs2 is None:
            problems.append("文档里找不到 CAN BS2 的推荐值（表格行）")
        elif doc_bs2 != c_bs2:
            problems.append("文档推荐 CAN BS2=%d，代码是 %d"
                            % (doc_bs2, c_bs2))

        # (e) 头文件注释里也应有同一组值，避免注释成为"第二事实来源"
        for label, val in (("BS1", c_bs1), ("BS2", c_bs2)):
            if re.search(r"%s\s*=\s*%d\b" % (label, val), can_hdr) is None:
                problems.append(
                    "gw_can_port.h 的注释里找不到 %s = %d —— "
                    "注释与代码不一致会让读者以为实现是另一组值"
                    % (label, val))

    # ------------------------------------------------------------------
    #  ⚠️ 顺带比对**用户工程 CubeMX 生成的 usart.c**
    #
    #  为什么需要：本项目交付"源码 + 文档"，用户在 CubeMX 里配外设。
    #  上面比对的是"文档 vs 我的常量"，
    #  但**真正决定硬件行为的是 CubeMX 生成的那份代码**。
    #
    #  实测中踩到过：文档写 USART2=9600，而用户 CubeMX 里配成了 115200。
    #  我上面那些检查全部通过（因为 9600 出现在文档里了），
    #  但两边实际不一致 —— 现场表现是"上位机连不上"，而代码看起来都对。
    #
    #  ⚠️ 用户工程在别的目录，可能不存在。
    #     不存在时**跳过而不是报错** —— 这个检查器在 CI 里也要能跑。
    user_usart = os.path.join(
        os.path.expanduser("~"), "..", "..", "stm32cubeIDE", "gateway2")
    candidates = [
        r"D:\stm32cubeIDE\gateway2\Core\Src\usart.c",
    ]
    usart_path = None
    for c in candidates:
        if os.path.isfile(c):
            usart_path = c
            break

    if usart_path is not None:
        us = read(usart_path)
        # 取 huart2 那一段的 BaudRate
        m = re.search(r"huart2\.Init\.BaudRate\s*=\s*(\d+)", us)
        want_baud = None
        m2 = re.search(r"#define\s+GW_RS485_UART_BAUD\s+(\d+)", cfg)
        if m2:
            want_baud = int(m2.group(1))
        if m and want_baud is not None:
            checked += 1
            got = int(m.group(1))
            if got != want_baud:
                problems.append(
                    "用户工程 CubeMX 的 USART2 波特率 = %d，"
                    "而 GW_RS485_UART_BAUD = %d —— 两边必须一致，"
                    "否则现场表现为\u300c上位机连不上\u300d而代码看起来都对"
                    % (got, want_baud))
            else:
                pass
    else:
        print("  [info] 未找到用户工程的 usart.c，跳过该项比对")

    if problems:
        print("  [失败] 文档与代码不一致（比对 %d 项）：" % checked)
        for p in problems:
            print("        " + p)
        print("        -> 改了代码就同步改 doc/CubeMX配置清单.md（或反之）")
        return 1

    print("  [通过] 文档与代码一致（比对 %d 项配置，含 CAN 位时序自洽性）"
          % checked)
    return 0


if __name__ == "__main__":
    sys.exit(main())
