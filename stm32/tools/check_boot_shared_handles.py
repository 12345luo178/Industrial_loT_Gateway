#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""CI 检查：引导程序可达的代码，不得引用"只有应用才初始化"的 HAL 句柄。

============================================================================
 为什么需要这一步（2026-10-11 缺陷 O 的根因就是这个检查缺失）
============================================================================

引导程序是**独立构建**的镜像（`tools/build_ota_images.ps1`），它只初始化
极少的外设：`HAL_Init` / `SystemClock_Config` / `MX_GPIO_Init` / `MX_USART1_UART_Init`。
但 `src/` 与 `port/` 是**两个镜像共用**的代码 —— 一旦共用代码里出现
`HAL_xxx(&h...)`，那个句柄在引导程序里就是 BSS 里的**全 0**：

    hiwdg.Instance == NULL  ->  HAL_IWDG_Refresh(&hiwdg)  写地址 0x00000000
                            ->  不精确数据总线错误 (CFSR=0x400)
                            ->  HardFault -> 故障处理程序复位 -> 死循环

2026-10-10 就是这样：`port/gw_ota_port.c` 里有

    #if defined(HAL_IWDG_MODULE_ENABLED)
    extern IWDG_HandleTypeDef hiwdg;
    ...
    (void)HAL_IWDG_Refresh(&hiwdg);

它看起来**很安全**（有宏开关护着），但那个宏只能保证"类型存在"，
**保证不了"句柄被初始化过"**。结果板子进了 39Hz 复位循环，
引导程序永远走不到"跳转应用"那一步，而现象（串口每 25ms 打一次
`[BOOT] start`）看起来像硬件/供电问题，白排查了一轮。

============================================================================
 检查内容
============================================================================

1) 扫下面 `BOOT_SHARED` 列出的"引导程序可达"源文件，
   任何 `h<外设>` 形式的 HAL 句柄引用都判**失败**（宏开关不算豁免）。
   规则：喂狗走 `gw_fault_port_iwdg_feed()`（直接写寄存器，无句柄），
   打印走 `boot_puts()`（引导程序自己的局部实现）——都不碰句柄。

   ⚠️ 唯一的放宽口是 `ALLOWED` 白名单：**只有"引导程序自己初始化过"
      的句柄**才能进白名单，而且必须逐条写清"谁初始化它"。
      引导程序自己的 main 里用 `huart1` 是合法的（它调了
      `MX_USART1_UART_Init()`）；而 `hiwdg` 不合法 ——
      引导程序从不调 `MX_IWDG_Init()`，那正是缺陷 O。
      加白名单就是放宽检查，所以理由必须回答"谁初始化它"，
      且**过期的白名单会被报出来**（句柄已经不用了就该删掉这条）。

2) 断言 `build_ota_images.ps1` 仍把 `gw_rtos.c` 排除在引导程序之外。
   那个文件里有 `HAL_IWDG_Init(&hiwdg)` 和 FreeRTOS，进了引导程序
   不只是句柄问题，而是直接编不过 / 起不来。

⚠️ 这份清单是**手工维护**的，所以脚本会检查每个文件是否存在 ——
   文件改名/挪走后本检查会**报错**，而不是静默跳过（静默跳过比没有检查更糟）。
============================================================================
"""

import os
import re
import sys

# ---- 引导程序可达的源文件（相对仓库根） ----
# 依据：templates/bootloader_main.c 的调用链 + gw_ota_port/gw_boot_port/
#       gw_flash_stm32/gw_fault_port 这四条 port 层实现。
BOOT_SHARED = [
    "templates/bootloader_main.c",
    "templates/bootloader_glue.c",
    "port/gw_ota_port.c",
    "port/gw_boot_port.c",
    "port/gw_flash_stm32.c",
    "port/gw_fault_port.c",
]

# ---- 必须留在引导程序之外的源文件（含反证：它确实用了句柄） ----
MUST_BE_EXCLUDED = [
    ("port/gw_rtos.c", "HAL_IWDG_Init"),
]

# ---- 白名单：允许出现的句柄 + **谁初始化它** ----
# ⚠️ 进白名单的条件只有一个：这个句柄在引导程序里**确实被初始化过**。
#    理由必须回答"谁初始化它"，答不出来就不该在这里。
ALLOWED = {
    "templates/bootloader_main.c": {
        # 引导程序 main() 自己调 MX_USART1_UART_Init()，huart1 是有效的。
        "huart1": "MX_USART1_UART_Init()（引导程序 main 自己调用）",
    },
}

# ---- "应用专有 HAL 句柄"的模式 ----
# 只匹配句柄标识符本身（huart1 / hiwdg / hcan / hdma_usart2_rx / htim4 …），
# 所以 HAL_Init、hal_errors、html 之类都不会误伤。
HANDLE_RE = re.compile(
    r"\bh(?:iwdg|wwdg|can|uart[0-9]+|usart[0-9]+|spi[0-9]+|i2c[0-9]+|"
    r"adc[0-9]*|dac|tim[0-9]+|rtc|flash|dma[a-z0-9_]*)\b"
)

# 允许出现的例外：注释里提到句柄名字（解释"为什么不能这么写"）不算违规。
# 但代码里出现就算 —— 所以只把明显的注释行排除掉。
COMMENT_LINE_RE = re.compile(r"^\s*(/\*|\*|//)")


def strip_comment_only_lines(text):
    """逐行返回 (行号, 内容)，跳过纯注释行。

    注意：**不**做完整的注释剥离 —— 行尾注释里出现句柄名是要放过的，
    但为了让"这行有代码吗"的判断简单可靠，这里把行尾注释也切掉：
    `code();  /* 用 hiwdg */` -> `code();`
    内联块注释（`/* ... */` 在同一行中间）也一并去掉。
    """
    out = []
    for i, line in enumerate(text.splitlines(), 1):
        if COMMENT_LINE_RE.match(line):
            continue
        code = re.sub(r"/\*.*?\*/", "", line)      # 同行块注释
        code = re.sub(r"//.*$", "", code)          # 行尾行注释
        if code.strip():
            out.append((i, line))
    return out


def scan_file(path):
    """返回 [(行号, 行内容, 命中的句柄名)]。"""
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    hits = []
    for lineno, line in strip_comment_only_lines(text):
        m = HANDLE_RE.search(line)
        if m:
            hits.append((lineno, line.rstrip(), m.group(0)))
    return hits


def main():
    root = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else \
        os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    fail = 0
    print("  仓库根: " + root)

    # ---- 1) 引导程序可达的代码里不许有句柄（白名单除外） ----
    used_allowed = {}
    for rel in BOOT_SHARED:
        full = os.path.join(root, rel.replace("/", os.sep))
        if not os.path.isfile(full):
            print("  [FAIL] 清单里的文件不存在：%s" % rel)
            print("         （文件改名/挪走了 -> 请更新本脚本的 BOOT_SHARED 清单，"
                  "不要让它静默跳过）")
            fail += 1
            continue

        allow = ALLOWED.get(rel, {})
        hits = scan_file(full)
        bad = [h for h in hits if h[2] not in allow]
        good = [h for h in hits if h[2] in allow]
        for _, _, name in good:
            used_allowed.setdefault(rel, set()).add(name)

        if bad:
            fail += 1
            print("  [FAIL] %s 引用了引导程序**没有初始化**的 HAL 句柄：" % rel)
            for lineno, line, name in bad:
                print("           %s:%d: %s" % (rel, lineno, line.strip()))
                print("             ^ %s：引导程序从不初始化它 -> Instance 为 NULL"
                      " -> 写地址 0 总线错误 -> 复位循环（缺陷 O）" % name)
            print("         修法：喂狗走 gw_fault_port_iwdg_feed()"
                  "（无句柄，直接写 IWDG->KR）")
        else:
            if good:
                names = "、".join(sorted(set(h[2] for h in good)))
                print("    [ OK ] %s（白名单句柄 %s：已确认引导程序自己初始化）"
                      % (rel, names))
            else:
                print("    [ OK ] %s" % rel)

    # ---- 1b) 白名单不能过期：不用了就该删，否则等于给以后放行 ----
    for rel, names in ALLOWED.items():
        for name, why in names.items():
            if name not in used_allowed.get(rel, set()):
                print("  [WARN] 白名单过期：%s 里已不再出现 %s（理由：%s）"
                      % (rel, name, why))
                print("         请删掉这条白名单，否则它会给将来的误用放行")

    # ---- 2) 用了句柄的文件必须仍被排除在引导程序之外 ----
    bscript = os.path.join(root, "tools", "build_ota_images.ps1")
    if not os.path.isfile(bscript):
        print("  [FAIL] 找不到 tools/build_ota_images.ps1")
        fail += 1
    else:
        with open(bscript, "r", encoding="utf-8", errors="replace") as f:
            btext = f.read()
        for rel, needle in MUST_BE_EXCLUDED:
            fpath = os.path.join(root, rel.replace("/", os.sep))
            if os.path.isfile(fpath):
                with open(fpath, "r", encoding="utf-8", errors="replace") as f:
                    if needle not in f.read():
                        print("  [WARN] %s 里已没有 %s —— 例外清单可能过时了"
                              % (rel, needle))
            if "gw_rtos.c" not in btext:
                print("  [FAIL] build_ota_images.ps1 不再排除 gw_rtos.c："
                      "它会被编进引导程序")
                fail += 1
            else:
                print("    [ OK ] build_ota_images.ps1 仍排除 gw_rtos.c")

    if fail:
        print("  ==> 引导程序共享层句柄检查未通过（%d 处）" % fail)
        return 1
    print("  ==> 引导程序可达代码无应用专有句柄引用")
    return 0


if __name__ == "__main__":
    sys.exit(main())
