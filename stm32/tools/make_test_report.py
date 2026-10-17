# -*- coding: utf-8 -*-
"""
生成《物联网数据采集终端与 Modbus 工业网关 —— 测试报告》DOCX

数据来源：本仓库 test/ 下 24 套测试的实际运行结果（1659 项全部通过）。
未上板的测量项在文档里**明确标注为"待填"** —— 不编造数据。
"""
from docx import Document
from docx.shared import Pt, Cm, RGBColor
from docx.enum.text import WD_ALIGN_PARAGRAPH
from docx.enum.table import WD_TABLE_ALIGNMENT
from docx.oxml.ns import qn
from docx.oxml import OxmlElement

# ---------------------------------------------------------------- 测试数据
SUITES = [
    ("test_regmap",    55, "寄存器表契约 + Modbus 从站协议（03/04/06/16、异常码、广播、越界读）"),
    ("test_dht22",     57, "DHT22 位解码 / 校验和 / 滑动滤波 / 时序状态机"),
    ("test_master",    91, "主站轮询 / 超时 / 重试 / 离线退避 / 跨引擎对测 / 从机表容量"),
    ("test_pipeline",  74, "时间累加 / 状态位 / 32 位量拆分 / 寄存器同步"),
    ("test_app",       52, "应用层集成：编排顺序、全链路、降级路径"),
    ("test_app_bus",   39, "总线仲裁接入：抢占 / 推迟 / 不泄漏 / 不饿死"),
    ("test_app_uplink",47, "上云集成：上报节拍 / 打包 / 帧分派 / 状态上报"),
    ("test_app_can",   46, "CAN 接入：通道同步 / 状态位语义 / 单一真相"),
    ("test_app_sched", 28, "★ 任务化等价性：调度模式与顺序模式寄存器表逐字节一致"),
    ("test_app_cmd",   89, "★ 指令执行：令牌保护 / 副作用 / 端到端 ACK-NACK"),
    ("test_arbiter",   36, "总线仲裁决策表穷举（3×3 全覆盖）+ 抢占语义"),
    ("test_param",     53, "Flash 磨损均衡 / 掉电安全 / 损坏槽回退"),
    ("test_frame",     82, "串口帧：粘包 / 半包 / 噪声重同步 / 坏帧不连锁"),
    ("test_uplink",    62, "断线补传 / 退避时间语义 / 溢出策略 / ACK 重发"),
    ("test_devinfo",  116, "参数范围 / 寄存器双向转换 / 线格式 / 省寿命比较 / 启动参数四道关卡"),
    ("test_can",      104, "CAN 报文取值 / 滤波 / 通道映射 / Bus-Off 恢复时间语义 / RTR 拒绝"),
    ("test_log",       88, "★ 分级日志（丢最旧）+ 故障升级（冷启动随机 RAM 不被误判）"),
    ("test_timing",    60, "时间预算 / 最坏情况 / 负载估算 / 优先级建议"),
    ("test_sched",     67, "任务划分 / 周期 / 看门狗 / 挂起恢复 / 硬超时"),
    ("test_ota",      111, "★ OTA 双分区：回滚 / 断电 / 元数据自保护（200 组随机）"),
    ("test_boot",      89, "★ Bootloader：向量表 / Thumb 位 / 整包 CRC / 与 OTA 联动"),
    ("test_cmd",      113, "★ 下行指令解析：令牌不可绕过 / 畸形输入 / 溢出"),
    ("test_selfcheck", 83, "★ 上板自检框架：依赖阻断 / SKIP 递归 / 根因定位"),
    ("test_fuzz",      17, "★ 对抗性输入：随机数据 + 金丝雀边界检测"),
]

BENCH = [
    ("CRC16 over 256 bytes",        "1875 ns",   "逐位算法，7.3 ns/字节，与字节数线性相关"),
    ("CRC 错误帧（丢弃）",           "41 ns",     "仅正常帧的 38% —— 总线有干扰时 CPU 负担反而更低"),
    ("读 16 个保持寄存器",           "289 ns",    "比读 4 个多 182 ns，约 15 ns/寄存器"),
    ("广播帧（执行但不应答）",        "109 ns",    "与正常读同价：省下的是发送时间，CPU 侧一样没少"),
    ("组帧（遥测 28 字节载荷）",      "21.5 ns",   "帧构造开销可忽略"),
    ("解析帧（整帧一次喂入）",        "99.6 ns",   ""),
    ("解析帧（逐字节喂）",           "168 ns",    "串口中断的真实模式，比整帧喂慢 1.7 倍"),
    ("CAN 收帧（映射命中）",         "2.4 ns",    "O(1)，与已收帧数无关"),
    ("CAN 收帧（未映射）",           "6.6 ns",    "反直觉但合理：未命中要走完整张表"),
    ("管道同步到寄存器（48 个）",     "5.1 ns",    ""),
    ("日志写入（DEBUG 通过）",       "3.7 ns",    "验证了「先写 RAM 环形缓冲」的设计达到目的"),
    ("日志写入（被等级过滤）",       "2.2 ns",    "等级判断放最前面生效"),
    ("CRC32 查表法 128 字节",       "195 ns",    "比逐位法快 5.0 倍，支撑了 OTA 用查表法的决定"),
    ("CRC32 逐位法 128 字节",       "969 ns",    ""),
]

# 需上板实测的项目（本次未测）
ONBOARD = [
    ("M1",  "HSE 实际频率",            "SystemCoreClock / MCO + 频率计",  "72 MHz ±1%"),
    ("M2",  "DWT 微秒精度",            "gw_dwt_delay_us(1000000) 对比秒表", "1 s 误差 < 1%"),
    ("M3",  "DHT22 采样总耗时",        "采样前后读 gw_dwt_us()",          "< 6 ms"),
    ("M4",  "DHT22 关中断时长",        "GPIO 翻转 + 示波器",              "< 6 ms"),
    ("M5",  "Modbus 从站响应延迟",     "收完请求到开始发送（GPIO 翻转）",   "< 5 ms"),
    ("M6",  "从站硬超时",              "gw_sched_stat(SLAVE)->deadline_miss", "必须为 0"),
    ("M7",  "一轮 poll 耗时",          "gw_timing_loop(&tm)->max_us",     "< 10000 µs"),
    ("M8",  "CPU 负载",                "gw_timing_load_permil(&tm)",      "< 500‰"),
    ("M9",  "CRC16 单帧耗时",          "帧处理前后 DWT 差值",              "< 50 µs（256 字节帧）"),
    ("M10", "Flash 单页擦除耗时",      "擦除前后 HAL_GetTick 差值",        "20~40 ms"),
    ("M11", "Flash 参数保存总耗时",    "保存前后 DWT",                    "< 100 ms"),
    ("M12", "CAN 收帧吞吐（1000 帧）", "连续发帧测总耗时",                 "rx_drop == 0"),
    ("M13", "上云遥测往返延迟",        "STM32 发 → EMQX 收到",            "< 2 s"),
    ("M14", "断线补传恢复时间",        "断网 N 秒后测补传完成时刻",         "补传 512 条 < 60 s"),
]

INJECT = [
    ("F1",  "拔掉 RS485 线",                "从站收帧停止但设备不崩；主站从机转离线"),
    ("F2",  "拔掉 DHT22 数据线",            "DHT_OK 位清除；温度保持旧值（不清零）"),
    ("F3",  "CAN_H/CAN_L 短接",             "Bus-Off → 状态位清除 → 退避后自愈"),
    ("F4",  "拔掉 ESP32 电源",              "上云转 DISCONNECTED；遥测进环形缓冲（积压数增长）"),
    ("F5",  "断开 EMQX（docker stop）",     "同上；offline_depth 增长"),
    ("F6",  "重新连接 EMQX",                "补传积压遥测，顺序正确，无重复"),
    ("F7",  "升级中途拔电",                 "重启后仍是旧固件（active 未变）"),
    ("F8",  "写入会崩溃的固件",             "试 3 次后自动回滚到旧固件"),
    ("F9",  "人为触发 HardFault",           "记录 PC/HFSR；复位后 boot_count +1；状态位置位"),
    ("F10", "参数区 Flash 人为位翻转",       "校验失败 → 用默认值启动；PARAM_OK 位清除"),
]

# ---------------------------------------------------------------- 工具
def set_cjk(run, name="宋体"):
    run.font.name = name
    r = run._element.rPr.rFonts
    r.set(qn('w:eastAsia'), name)

def add_heading(doc, text, level):
    h = doc.add_heading(text, level=level)
    for r in h.runs:
        set_cjk(r, "黑体" if level <= 2 else "宋体")
        r.font.color.rgb = RGBColor(0, 0, 0)
    return h

def add_body(doc, text, bold=False, size=10.5):
    p = doc.add_paragraph()
    r = p.add_run(text)
    r.bold = bold
    r.font.size = Pt(size)
    set_cjk(r)
    return p

def add_table(doc, headers, rows, widths=None):
    t = doc.add_table(rows=1, cols=len(headers))
    t.style = "Table Grid"
    t.alignment = WD_TABLE_ALIGNMENT.CENTER

    # ⚠️ 三件事必须一起做，只做一件 LibreOffice/Word 仍会按内容重算列宽
    #    （渲染验证时表格超出右边距就是漏了后两件）：
    #      1) tblLayout = fixed      —— 声明使用固定布局
    #      2) tblW = 指定总宽        —— 否则总宽是 auto
    #      3) tblGrid 的 gridCol 宽度 —— 列网格才是布局的真正依据
    t.autofit = False
    tblPr = t._tbl.tblPr

    for tag in ('w:tblLayout', 'w:tblW'):
        for el in tblPr.findall(qn(tag)):
            tblPr.remove(el)

    layout = OxmlElement('w:tblLayout')
    layout.set(qn('w:type'), 'fixed')
    tblPr.append(layout)

    total_cm = sum(widths) if widths else 14.8
    tblW = OxmlElement('w:tblW')
    tblW.set(qn('w:w'), str(int(total_cm * 567)))   # 1 cm = 567 twips
    tblW.set(qn('w:type'), 'dxa')
    tblPr.append(tblW)

    hdr = t.rows[0].cells
    for i, h in enumerate(headers):
        hdr[i].text = ""
        p = hdr[i].paragraphs[0]
        r = p.add_run(h)
        r.bold = True
        r.font.size = Pt(9.5)
        set_cjk(r)
        p.alignment = WD_ALIGN_PARAGRAPH.CENTER
    for row in rows:
        cells = t.add_row().cells
        for i, v in enumerate(row):
            cells[i].text = ""
            p = cells[i].paragraphs[0]
            r = p.add_run(str(v))
            r.font.size = Pt(9)
            set_cjk(r)

    if widths:
        # 重写列网格
        grid = t._tbl.find(qn('w:tblGrid'))
        if grid is not None:
            t._tbl.remove(grid)
        grid = OxmlElement('w:tblGrid')
        for w in widths:
            gc = OxmlElement('w:gridCol')
            gc.set(qn('w:w'), str(int(w * 567)))
            grid.append(gc)
        t._tbl.insert(list(t._tbl).index(tblPr) + 1, grid)

        for row in t.rows:
            for i, w in enumerate(widths):
                row.cells[i].width = Cm(w)
    return t

# ---------------------------------------------------------------- 文档
doc = Document()

# 全局字体
style = doc.styles["Normal"]
style.font.name = "宋体"
style.font.size = Pt(10.5)
style.element.rPr.rFonts.set(qn('w:eastAsia'), "宋体")

for s in doc.sections:
    # ⚠️ python-docx 默认是 Letter（21.59 × 27.94 cm）。
    #    中文报告应显式设为 A4（21.0 × 29.7 cm）——
    #    否则渲染/打印时会与 A4 预期不符（页宽差 0.6 cm，
    #    表格看起来会溢出右边距）。这个坑是渲染验证时发现的。
    s.page_width  = Cm(21.0)
    s.page_height = Cm(29.7)
    s.left_margin = Cm(2.5); s.right_margin = Cm(2.5)
    s.top_margin = Cm(2.5);  s.bottom_margin = Cm(2.5)

# 封面
title = doc.add_paragraph()
title.alignment = WD_ALIGN_PARAGRAPH.CENTER
r = title.add_run("\n\n物联网数据采集终端与 Modbus 工业网关\n设计与实现")
# ⚠️ 18pt 而非 22pt：22pt 时"物联网数据采集终端与 Modbus 工业网关"
#    会超出右边距（渲染验证发现）
r.bold = True; r.font.size = Pt(18); set_cjk(r, "黑体")

sub = doc.add_paragraph()
sub.alignment = WD_ALIGN_PARAGRAPH.CENTER
r = sub.add_run("\n\n测 试 报 告")
r.bold = True; r.font.size = Pt(26); set_cjk(r, "黑体")

info = doc.add_paragraph()
info.alignment = WD_ALIGN_PARAGRAPH.CENTER
r = info.add_run(
    "\n\n\n目标平台：STM32F103ZET6\n"
    "开发环境：STM32CubeIDE + arm-none-eabi-gcc 13.3.1\n"
    "测试方式：主机端单元/集成测试 + 交叉编译验证\n"
)
r.font.size = Pt(12); set_cjk(r)

doc.add_page_break()

# ---------------- 1 概述
add_heading(doc, "1  测试概述", 1)
add_body(doc,
    "本报告记录物联网数据采集终端与 Modbus 工业网关的测试过程与结果。"
    "被测系统在 STM32F103ZET6 上实现了 Modbus RTU 主从共存、CAN 现场总线接入、"
    "DHT22 环境采集、Flash 参数掉电保存、ESP32 串口帧协议与 MQTT 上云、"
    "断线重连与数据补传、OTA 双分区升级与自动回滚、分级日志与异常处理。")

add_body(doc, "架构上的关键约束是「协议逻辑与硬件解耦」："
    "所有业务逻辑只依赖抽象接口（vtable），不包含任何 HAL 调用，"
    "因此可以在 PC 上完整跑单元测试。这条约束是本次测试能覆盖 1659 项的直接前提。")

add_heading(doc, "1.1  测试层次", 2)
add_table(doc,
    ["层次", "方法", "覆盖内容", "本次结果"],
    [
        ["L1 单元测试", "主机端 gcc，纯逻辑模块", "协议解析、状态机、算法", "17 套 / 覆盖全部模块"],
        ["L2 集成测试", "主机端 gcc，链接完整应用层", "模块间协作、端到端链路", "5 套 / 覆盖 6 条链路"],
        ["L3 对抗性测试", "随机输入 + 金丝雀边界检测", "畸形输入、内存安全", "1 套 / 数十万轮随机"],
        ["L4 编译验证", "arm-none-eabi-gcc 交叉编译", "目标平台可编译性", "30 个模块 / 零警告"],
        ["L5 静态验证", "反汇编检查关键指令", "跳转序列、中断控制", "Bootloader 跳转已确认"],
        ["L6 上板验证", "真机 + DWT + 示波器", "时序、电气、长期稳定性", "★ 待执行"],
    ],
    widths=[2.2, 3.4, 5.4, 5.0])

add_body(doc, "本报告的第 2~5 章为已完成部分（L1~L5），第 6 章为待执行的上板验证方案（L6）。"
    "未实测的数据一律标注「待填」，不作推测。", bold=True)

# ---------------- 2 测试环境
add_heading(doc, "2  测试环境", 1)
add_table(doc,
    ["项目", "配置"],
    [
        ["目标芯片", "STM32F103ZET6（Cortex-M3，512 KB Flash / 64 KB RAM，72 MHz）"],
        ["调试器", "ST-Link V2"],
        ["交叉编译器", "arm-none-eabi-gcc 13.3.1（STM32CubeIDE 1.19.0 附带）"],
        ["HAL 固件包", "STM32Cube FW_F1 V1.8.7"],
        ["主机编译器", "MinGW-W64 GCC 16.2.0（x86-64）"],
        ["主机端编译选项", "-std=c99 -Wall -Wextra（基准测试加 -O2）"],
        ["外设", "RS485(USART2) / DHT22(PG11) / CAN(TJA1051T/3) / ESP32(USART3)"],
        ["上云", "ESP32 侧运行 MQTT 客户端，STM32 仅通过串口帧交互"],
    ],
    widths=[3.2, 12.8])

add_heading(doc, "2.1  内存与代码规模", 2)
add_table(doc,
    ["项目", "数值", "说明"],
    [
        ["静态 RAM 占用", "≈ 23 KB / 64 KB（36%）", "主体为 gw_app_t 15.8 KB + gw_uplink_t 14.5 KB（含于前者）"],
        ["任务栈预算", "≈ 5.5 KB", "FreeRTOS 迁移用，见 FreeRTOS 迁移指南"],
        ["源文件", "30 个 .c + 34 个 .h", "不含测试"],
        ["交叉编译产物", "≈ 116 KB", "30 个模块目标文件合计（含 2 KB 对齐填充）"],
        ["测试代码", "26 个 .c", "24 套测试 + 边界检测工具 + 基准测试"],
    ],
    widths=[3.2, 4.4, 8.4])

# ---------------- 3 功能测试
add_heading(doc, "3  功能测试（L1 + L2）", 1)
add_body(doc, "24 套测试全部通过，合计 1659 项。带 ★ 的是覆盖出错路径或对抗性场景的套件。")

add_heading(doc, "3.1  测试套件明细", 2)
add_table(doc,
    ["套件", "项数", "覆盖内容"],
    [[n, str(c), d] for n, c, d in SUITES],
    widths=[3.0, 1.2, 11.8])

total = sum(c for _, c, _ in SUITES)
p = doc.add_paragraph()
r = p.add_run(f"合计：24 套 / {total} 项，失败 0 项。")
r.bold = True; r.font.size = Pt(10.5); set_cjk(r)

# ---------------- 4 关键场景
add_heading(doc, "4  关键测试场景", 1)
add_body(doc, "以下场景是「正常路径能跑」之外、真正决定产品可靠性的部分。"
    "每一项都对应一个具体的现场故障模式。")

add_heading(doc, "4.1  掉电与数据完整性", 2)
add_table(doc,
    ["场景", "验证内容", "结论"],
    [
        ["参数保存中途断电", "逐次注入「第 N 次写入失败」，每次仍能读回完整有效参数", "通过（test_param）"],
        ["OTA 升级中途断电", "元数据未被修改，重启后仍启动旧固件", "通过（test_ota）"],
        ["坏固件反复启动", "试 3 次后自动回滚到旧分区，且回滚后持续稳定", "通过（test_ota）"],
        ["坏固件连跳转都过不去", "boot_tries 在跳转前落盘，不会无限重试", "通过（test_ota）"],
        ["待确认固件损坏", "整包 CRC 校验失败 → 拒绝启动 → 留在旧固件", "通过（test_boot）"],
    ],
    widths=[3.2, 9.2, 3.6])

add_heading(doc, "4.2  冷启动与随机内存", 2)
add_body(doc,
    "故障记录存放在 .noinit 段以跨复位存活，但冷启动时该段内容是随机的。"
    "若不校验就当成崩溃记录，会得到一个随机的 PC 与随机的连续计数（可能为 65535），"
    "导致设备一上电就进入安全模式。测试覆盖三种形态：")
add_table(doc,
    ["输入形态", "期望行为", "结论"],
    [
        ["全 0x00", "识别为冷启动，计数清零", "通过"],
        ["全 0xFF", "识别为冷启动（该形态 consec=65535，最易误判）", "通过"],
        ["200 组伪随机位模式", "全部识别为冷启动，不误进安全模式", "通过"],
    ],
    widths=[3.6, 9.6, 2.8])

add_heading(doc, "4.3  任务化等价性", 2)
add_body(doc,
    "将单一轮询函数拆分为 6 个独立任务后，必须证明行为未改变。"
    "测试用同一套输入分别跑「顺序模式」与「调度模式」，逐字段比对："
    "从站收发计数、主站在线位图、DHT 采样次数、从站响应逐字节、"
    "以及寄存器表 48 个寄存器的逐字节一致性。")
p = doc.add_paragraph()
r = p.add_run("结论：两条路径的寄存器表逐字节相同，任务化未改变任何可观测行为。")
r.bold = True; r.font.size = Pt(10.5); set_cjk(r)

add_heading(doc, "4.4  下行指令的令牌保护", 2)
add_body(doc,
    "save / restore / reboot 三条指令会改变设备状态，其中 restore 会清除现场标定参数。"
    "测试覆盖 21 个令牌攻击用例，验证的核心断言是：")
p = doc.add_paragraph()
r = p.add_run("令牌错误时，移植层回调一次都不会被调用。")
r.bold = True; r.font.size = Pt(10.5); set_cjk(r)
add_body(doc, "这条断言能发现「先执行再验令牌」这类写反顺序的实现错误 —— "
    "那种错误下，一条误发的 restore 已经把参数清掉了。")

# ---------------- 5 对抗性测试
add_heading(doc, "5  对抗性输入测试（L3）", 1)
add_body(doc,
    "功能测试验证的是「正常路径正确」，本项验证的是「任何输入都不会崩」。"
    "现场会有设计者想不到的输入（电磁干扰产生的乱码、第三方设备的畸形帧、"
    "电平抖动产生的半个字节），这类输入若越界写内存，症状是数小时后才出现的偶发异常。")

add_heading(doc, "5.1  测试方法", 2)
add_body(doc, "本机 MinGW 无 ASan/UBSan 运行库，故采用金丝雀哨兵做边界检测：")
p = doc.add_paragraph()
r = p.add_run("    [ 前哨 16 字节 ][ 被测缓冲 512 字节 ][ 后哨 16 字节 ]")
r.font.name = "Consolas"; r.font.size = Pt(9.5)
add_body(doc, "操作前后各校验一次哨兵，被改即判定越界。"
    "该手段弱于 ASan（查不出 use-after-free），但恰好覆盖嵌入式最要害的一类问题，"
    "且零依赖、上板也能用。")

add_heading(doc, "5.2  覆盖范围与结果", 2)
add_table(doc,
    ["测试对象", "轮数", "不变量检查", "结果"],
    [
        ["串口帧解析器", "200 000", "取出的帧长度不超过上限；随机数据不误同步", "通过"],
        ["Modbus 从站", "50 000", "响应不越界；越界读一律返回异常", "通过"],
        ["下行指令解析器", "200 000", "ID 与结果码始终在枚举范围内；令牌不可绕过", "通过"],
        ["CAN 收帧", "200 000", "扩展帧 / 远程帧 / 非法 DLC 一律被拒", "通过"],
        ["长时随机操作序列", "20 000", "6 条系统不变量全程成立", "通过"],
    ],
    widths=[3.6, 2.0, 9.0, 1.4])

add_heading(doc, "5.3  本项测试发现的缺陷", 2)
add_body(doc, "对抗性测试发现并修复了一个真实缺陷：", bold=True)
add_table(doc,
    ["项", "内容"],
    [
        ["缺陷", "CAN 远程帧（RTR）未被拒绝"],
        ["根因", "帧校验函数只检查 DLC 与 ID 范围，未检查 RTR 位"],
        ["后果", "远程帧是「请求他人发数据」，自身无数据段。放它进入通道映射后，"
                 "会从复用的 data[] 中取到上一个报文的过期值，"
                 "上报到云端后无人能辨真伪 —— 这比「通道无数据」危险得多"],
        ["修复", "gw_can_frame_valid() 增加 RTR 检查（返回 -4），并补 3 项回归测试"],
        ["验证", "test_can 由 101 项增至 104 项；fuzz 复测 20 万轮通过"],
    ],
    widths=[2.6, 13.4])

p = doc.add_paragraph()
r = p.add_run("该缺陷说明：功能测试（给定输入断言输出）无法覆盖这类问题，"
              "必须用随机输入才能暴露。")
r.bold = True; r.font.size = Pt(10.5); set_cjk(r)

# ---------------- 6 基准测试
add_heading(doc, "6  独立协议验证（不依赖被测代码）", 1)
add_body(doc,
    "第 3~5 章的测试期望值都来自 C 代码自身（硬编码的期望字节或结构性断言）。"
    "那只能证明\u300c代码前后一致\u300d，不能证明\u300c发出的字节符合 Modbus 规范\u300d。"
    "本章记录三类**独立**验证：它们的判据不来自被测代码。")

add_heading(doc, "6.1  端到端字节流验证（146 项）", 2)
add_body(doc,
    "做法：C 侧只跑一个真实运行场景并把经过两个串口的每个字节 dump 出来"
    "（**不做任何断言**）；Python 侧用**独立实现**的 CRC 与 Modbus 规范知识逐帧核对。")
add_body(doc,
    "Python 的 CRC16/CRC8 用逐位算法（C 侧用查表），方法不同才能交叉验证。")
add_table(doc,
    ["核对项", "内容"],
    [
        ["CRC16 / CRC8", "用与 C 侧不同的算法交叉验证"],
        ["响应与请求的匹配", "功能码一致、数量一致、字节数公式正确"],
        ["异常响应格式", "恰好 5 字节，异常码在规范范围内"],
        ["广播（地址 0）", "必须**不应答**"],
        ["地址不匹配", "必须**不应答**"],
        ["CRC 错误的请求", "必须**不应答**"],
        ["主站请求帧", "长度 8、CRC 正确、读取数量 1..125"],
        ["串口帧 HEAD/LEN/CRC8", "总长 = 长度字段 + 4、载荷长度自洽"],
        ["遥测 28 字节布局", "温湿度在传感器量程内"],
    ],
    widths=[4.0, 12.0])
add_body(doc, "结论：146 项全部通过。", bold=True)

add_heading(doc, "6.2  ⚠️ 交互式边界测试（56 项）", 2)
add_body(doc,
    "6.1 是固定剧本，能验证规范符合性，但证明不了\u300c没被写进剧本的边界\u300d。"
    "6.2 用 Python 当**活的主站**逐条出题、逐条核对：")
add_table(doc,
    ["能力", "为什么固定剧本做不到"],
    [
        ["确认\u300c不该应答时确实没有字节\u300d",
         "要等超时才能确认。剧本只能数响应总数，而多一个少一个可能互相抵消 —— 那是间接证据"],
        ["主动构造任意畸形帧", "剧本里的帧是预先想好的"],
        ["快速连发做压力", "剧本是一次性的"],
    ],
    widths=[5.6, 10.4])
add_body(doc,
    "覆盖：数量边界（1/32/33/0/125/126）、**地址 16 位回绕陷阱**、"
    "未实现功能码、三条静默规则、**广播写\u300c执行但不答\u300d**、"
    "畸形帧、写路径、连发 200 次 + 交替 60 次。")
add_body(doc, "结论：56 项全部通过。其中\u300c广播写\u300d一项同时验证了两件相反的事"
              "（有效果 + 无响应），这是固定剧本很难做到的。", bold=True)

add_heading(doc, "6.3  ⚠️ 主机端性能基准", 2)
add_body(doc,
    "在 x86 主机上测量各模块的算法效率，作为回归基线。"
    "每个条目累计计时不少于 30 ms（量化误差 < 1%），编译选项 -O2。")

add_table(doc,
    ["操作", "单次耗时", "说明"],
    [[a, b, c] for a, b, c in BENCH],
    widths=[5.0, 2.2, 8.8])

add_heading(doc, "6.3.1  测量方法的三个陷阱", 3)
add_body(doc, "本节记录测量过程中发现并修正的三个问题，它们决定了上述数字是否可信：")
add_table(doc,
    ["陷阱", "现象", "修正"],
    [
        ["优化器删除被测代码", "CRC32 实测输出 0.0 ns —— 循环结果未被使用，整段被删",
         "用 volatile 黑盒承接结果"],
        ["计时分辨率不足", "2 ns 的操作跑 100 万次仅 2 ms（2 个计时单位，误差可达 50%）",
         "自适应加倍批量，保证累计 ≥ 30 ms"],
        ["测了异常路径却当成正常路径",
         "「处理 125 寄存器请求」显示 55 ns，反而快于「处理 4 个」的 106 ns",
         "查出保持寄存器表仅 16 个，该请求立即命中非法地址异常返回。"
         "改为读满 16 个后为 289 ns > 107 ns，数字自洽"],
    ],
    widths=[2.8, 7.6, 5.6])

p = doc.add_paragraph()
r = p.add_run("第三个陷阱的教训是通用的：「太快了」往往不是优化好，"
              "而是根本没走到要做的事。基准测试必须先确认测的是哪条代码路径。")
r.bold = True; r.font.size = Pt(10.5); set_cjk(r)

add_heading(doc, "6.3.2  ⚠️ 数据适用范围（重要）", 3)
add_body(doc,
    "上述数字来自 x86（约 3 GHz、乱序执行、多级缓存），"
    "不能直接作为 STM32（72 MHz、顺序执行、Flash 取指有等待周期）的耗时。"
    "可用的是两点：")
add_body(doc, "（1）横向比较 —— 同一平台上 A 比 B 快几倍的比值，在目标平台通常同样成立；")
add_body(doc, "（2）回归基线 —— 改动前后各跑一次，某操作慢一个量级即说明引入了性能退化。")
p = doc.add_paragraph()
r = p.add_run("真正要写入报告的 STM32 数据必须上板用 DWT 实测，见第 7 章。")
r.bold = True; r.font.size = Pt(10.5); set_cjk(r)

# ---------------- 7 待执行
add_heading(doc, "7  上板验证（L6，待执行）", 1)
p = doc.add_paragraph()
r = p.add_run("说明：本章所有表格均为空白待填。本报告不包含任何未经实测的上板数据。")
r.bold = True; r.font.size = Pt(10.5); set_cjk(r)

add_heading(doc, "7.1  量化测量项", 2)
add_table(doc,
    ["编号", "测量项", "测量方法", "合格判据", "实测值"],
    [[i, n, m, c, ""] for i, n, m, c in ONBOARD],
    widths=[1.0, 3.0, 5.0, 3.6, 3.4])

add_heading(doc, "7.2  异常注入测试", 2)
add_table(doc,
    ["编号", "注入的故障", "期望行为", "实测"],
    [[i, f, e, ""] for i, f, e in INJECT],
    widths=[1.0, 3.4, 9.4, 2.2])

add_heading(doc, "7.3  72 小时稳定性测试记录表", 2)
add_body(doc, "测试条件：主站每 100 ms 轮询；DHT22 每 2.5 s 采样；遥测每 10 s 上报；"
    "CAN 总线每 50 ms 一帧；串口日志开到 INFO 级。每小时记录一行。")
hdr = ["时刻", "复位", "CRC 错误", "响应超时", "DHT 成功率", "Bus-Off",
       "上云成功", "补传", "丢弃", "一轮最大耗时", "CPU 负载", "空闲内存"]
add_table(doc, hdr,
    [["0 h"] + [""] * (len(hdr) - 1),
     ["24 h"] + [""] * (len(hdr) - 1),
     ["48 h"] + [""] * (len(hdr) - 1),
     ["72 h"] + [""] * (len(hdr) - 1)],
    widths=[1.2, 0.9, 1.2, 1.2, 1.5, 1.2, 1.2, 0.9, 0.9, 2.2, 1.2, 1.4])

add_heading(doc, "7.4  稳定性合格判据", 2)
add_table(doc,
    ["项目", "判据"],
    [
        ["复位次数", "0（boot_count 不增长）"],
        ["HardFault", "0（HARDFAULT 状态位未置起）"],
        ["从站 CRC 错误率", "< 0.1%"],
        ["从站响应超时", "0（deadline_miss == 0）"],
        ["DHT 采样成功率", "> 99%"],
        ["CAN Bus-Off 次数", "0，或 ≤1 且能自愈"],
        ["数据完整性", "补传条数 + 直接上报条数 = 采样条数（不丢数据）"],
        ["一轮最大耗时", "< 10 ms（不超节拍）"],
        ["CPU 负载", "< 500‰"],
        ["空闲内存", "无单调下降趋势（排除越界破坏）"],
    ],
    widths=[4.0, 12.0])

# ---------------- 8 缺陷汇总
add_heading(doc, "8  开发过程中发现并修复的缺陷", 1)
add_body(doc, "以下缺陷均在 PC 测试阶段发现并修复，未流入上板调试环节。"
    "列出它们是为了说明测试设计的有效性。")
add_table(doc,
    ["#", "缺陷", "根因", "发现方式"],
    [
        ["1", "CAN 远程帧未被拒绝，通道出现过期值",
         "帧校验漏检 RTR 位", "对抗性随机测试"],
        ["2", "参数写入成功但读回校验永远失败",
         "槽头字段重叠：len(偏移8..11) 与 version(偏移10..11) 冲突", "单元测试"],
        ["3", "参数前 2 字节被清零，后续写入被拒",
         "把「头部长度 20」与「数据偏移 18」混为一谈", "单元测试"],
        ["4", "粘包时只有第一帧被解析",
         "收满一帧后继续向同一缓冲追加", "单元测试"],
        ["5", "坏帧之后的好帧被连带丢弃，丢失量恰为一帧长度",
         "重同步时把状态机位置硬设为 1，覆盖了刚搬移的数据", "单元测试"],
        ["6", "上云缓冲积压后补传永久停滞",
         "批次计数器未在发送路径重置", "单元测试"],
        ["7", "总线被主站永久占用，整个 RS485 网络瘫痪",
         "释放条件在超时重发路径下永不成立", "集成测试"],
        ["8", "主站永远收不到响应（上一条的过度修正）",
         "无条件释放导致主站等待期间丢失持权", "集成测试"],
        ["9", "任务化后 CAN 状态位恒为 0",
         "状态字读的是 can_ok 字段，却写入了 sticky 位", "集成测试"],
        ["10", "直接写寄存器表被静默覆盖为 0",
         "管道同步是轮询最后一步，会无条件回写寄存器表", "集成测试"],
        ["11", "改单个参数被「其他字段未初始化」误拒",
         "整表校验替代了按字段校验", "集成测试"],
        ["12", "Bootloader 静默跳过整包 CRC 校验",
         "误用 meta_valid 的返回值约定（成功返回 1 而非 0）", "集成测试"],
        ["13", "连续掉电会耗尽所有参数槽，参数永久丢失",
         "pick_target_slot 跳过损坏槽；而磨损均衡每轮用下一个槽，"
         "8 次\u300c写中途掉电\u300d就占满所有槽，退化分支擦掉唯一有效槽",
         "★ 新增的参数断电穷举（275 个半字级断电点）"],
        ["14", "指令确认与遥测 ACK 复用同一帧类型，互相污染",
         "上云链路只看帧类型不判载荷，一次\u300c指令令牌错误\u300d"
         "被当成\u300c遥测被拒\u300d并误触发重传",
         "集成测试的帧分派用例"],
    ],
    widths=[0.9, 4.5, 7.4, 3.2])

# ---------------- 9 结论
add_heading(doc, "9  测试结论", 1)
add_body(doc, "（1）功能完整性：报告要求的功能已全部实现，除 Web 前端与小程序（按方案裁剪外）。", )
add_body(doc, "（2）功能正确性：24 套测试 1659 项全部通过，覆盖 6 条端到端链路；"
              "其中 12 项缺陷在测试阶段被发现并修复。")
add_body(doc, "（3）内存安全性：数十万轮随机输入未破坏金丝雀哨兵，未出现越界写。")
add_body(doc, "（4）可移植性：30 个模块在 arm-none-eabi-gcc 13.3.1 下零警告通过交叉编译；"
              "Bootloader 跳转序列经反汇编确认。")
add_body(doc, "（5）性能：主机端基准建立了可用于回归比对的基线数据。")

p = doc.add_paragraph()
r = p.add_run("（6）尚未验证的部分：所有与真实硬件时序、电气特性、长期稳定性相关的指标"
              "（第 7 章 M1–M14、F1–F10、72 小时稳定性）均未实测。"
              "PC 测试能够证明逻辑正确，但不能替代上板验证 —— "
              "这是本报告最主要的能力边界。")
r.bold = True; r.font.size = Pt(10.5); set_cjk(r)

add_heading(doc, "9.1  已知限制", 2)
add_table(doc,
    ["项目", "状态", "说明"],
    [
        ["Modbus 主站功能码", "仅读（0x03/0x04）", "写操作待扩展；最多管理 16 台从机"],
        ["CAN", "只收不发", "按方案有意如此"],
        ["IWDG 看门狗", "未使能", "裸机下有长阻塞路径（Flash 擦除、DHT22 时序），"
                                  "FreeRTOS 化后放入空闲任务钩子才可靠"],
        ["OTA 签名校验", "未实现", "当前仅 CRC32（防传输错误），不防篡改"],
        ["Flash 日志落盘", "未实现", "分区表已预留 44 KB"],
        ["安全模式退出", "未实现", "进入后持续到下次复位"],
        ["FreeRTOS 迁移", "业务代码已就绪", "任务入口签名与 TaskFunction_t 一致，"
                                            "xTaskCreate 即可；实际建任务需在 CubeMX 工程完成"],
    ],
    widths=[3.0, 3.0, 10.0])

doc.save("测试报告.docx")
print("已生成：测试报告.docx")
