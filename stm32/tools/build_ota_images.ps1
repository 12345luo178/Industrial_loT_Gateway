<#
.SYNOPSIS
    构建 OTA 镜像：引导程序（0x08000000）、槽 A、槽 B、golden 出厂镜像

.DESCRIPTION
    为什么要这个脚本（2026-10-10 做 OTA 槽位化时写的）：

      CubeMX 工程（gateway2）只会产出**一个**镜像：链接地址 0x08000000、
      整块 512KB Flash。但 OTA 的 A/B 布局要求（**v2**，2026-10-11）：

          0x08000000  32 KB  引导程序        <- bootloader.ld
          0x08008000   4 KB  元数据（双页）
          0x08009000 136 KB  应用槽 A        <- app_slot_a.ld
          0x0802B000 136 KB  应用槽 B        <- app_slot_b.ld
          0x0804D000 136 KB  golden 出厂镜像 <- app_golden.ld（-Golden）

      ⚠️ v1 是 208 KB 槽且**一个字节余量都没有**，加 golden 只能缩小 A/B 槽
         （另一条路是吃掉"日志区"那个已规划但未实现的功能区 —— 那正是同一天
         在输入寄存器 28~31 上踩过的"借用"坑，所以没选）。
         代价：单槽上限 208 -> 136 KB（当前应用约 39 KB，仍有 3 倍余量）。

      所以需要按**不同链接脚本 + 不同向量表偏移**重建同一个应用。
      本脚本用 arm-none-eabi-gcc 直接产出两个镜像，不依赖 CubeMX/IDE。

    ⚠️ 为什么不能只改链接脚本：
       链接地址变了，向量表偏移也必须跟着变（SystemInit 用
       FLASH_BASE + VECT_TAB_OFFSET 设 SCB->VTOR）。
       本脚本会**自动生成**改好偏移的 system_stm32f1xx.c 副本，
       不去动工程里的那个文件（它还要给 0x08000000 的直接启动镜像用）。

    ⚠️ 产出会做**自证检查**（这一步不能省）：
       ① 每个镜像不得超过它的槽位大小
       ② 镜像头 8 字节 = 栈顶 + 复位向量，必须落在 RAM / 本槽范围内
       —— 链接脚本写错、偏移忘了改，都会在这两条上立刻暴露，
          而不是等烧进去上板才发现"跳过去就 HardFault"。

.EXAMPLE
    pwsh -File tools/build_ota_images.ps1
    pwsh -File tools/build_ota_images.ps1 -SlotB            # 顺带产出槽 B 镜像
    pwsh -File tools/build_ota_images.ps1 -SlotB -Golden    # 产出一整套（含 golden）
    pwsh -File tools/build_ota_images.ps1 -OutDir D:\ota
#>
param(
    # CubeMX 工程目录（提供 Core/ 与 Drivers/）
    [string]$Project = "D:\stm32cubeIDE\gateway2",
    # 产出目录（默认仓库下 build\ota，已被 .gitignore 忽略）
    [string]$OutDir = "",
    # 工具链 bin 目录（默认自动查找 STM32CubeIDE 自带的 GNU 工具）
    [string]$Toolchain = "",
    # 是否顺带产出槽 B 镜像（默认只产出引导程序 + 槽 A）
    [switch]$SlotB,
    # 是否顺带产出 golden 出厂镜像
    # ⚠️ 它是"某个已知良好版本"的第三份拷贝：量产/交付前对**已验证通过**的那一版
    #    跑一次并烧进 golden 区。平时改代码不必每次重建它。
    [switch]$Golden,
    # ★ 只做一次"FreeRTOS 整镜像链接检查"（产出到 build\rtos_link）
    #
    #  ⚠️ 为什么需要它：RTOS 落地前有**一整类问题只在链接期暴露** ——
    #     缺符号（某个 hook 没实现）、重复定义（内核与工程各定义了一个 handler）、
    #     **装不下**（RAM 或槽位 Flash 超了，链接脚本直接报错）。
    #     而这一步**不需要烧板**，所以可以在浸泡期间随便跑。
    #  ⚠️ 它**不会碰 build\ota**（那里是浸泡测试的冻结基线）。
    #  ⚠️ 产出物**不是可烧录镜像**：用的 main 是 templates\main_template.c 的
    #     RTOS 分支，不是 CubeMX 工程的 main.c。真上板走正式槽位流程。
[switch]$RtosLink,
    # ★ 产出**真正的 RTOS 槽位镜像**（2026-10-11 第 31 轮加）：
    #   普通路径产出的镜像**不带** GW_USE_FREERTOS -> 是裸机版；
    #   而 -RtosLink 那只做链接检查、且排除了整个 it.c（会丢中断）。
    #   落地之后烧板子要用这个开关。
    [switch]$Rtos,
    # FreeRTOS 内核源码目录（默认按 GW_FW_ROOT / STM32Cube 仓库自动找）
    [string]$Freertos = "",
    # 优化档位：默认 "0"（与工程的 Debug 配置一致，便于单步）。
    # ⚠️ 引导程序实测占槽 94.5%，加功能前先用 -Optimize s 换余量。
    [string]$Optimize = "0",

    # ---- LVGL（可选，2026-10-14 加）----
    # ⚠️ 为什么是"可选开关"而不是默认编进来：
    #    ① LVGL 会显著改变体积（探针实测 text ≈ +53KB，含中文字库子集后更多），
    #       而 build\ota 是**浸泡基线** —— 默认行为必须一个字节都不变；
    #    ② LVGL 源码不在仓库里（在 $LvglDir），默认打开会让别的机器构建直接失败。
    # 实测依据见 doc/上板调试交接.md 与 lvgl_slim_probe.py。
    [switch]$Lvgl,
    [string]$LvglDir = "D:\lvgl\lvgl-release-v8.2"
)

$ErrorActionPreference = "Stop"

$repo = Split-Path $PSScriptRoot -Parent
if ($OutDir -eq "") { $OutDir = Join-Path $repo "build\ota" }

# ---------------- 工具链 ----------------
function Find-Toolchain {
    param([string]$Explicit)
    if ($Explicit -ne "") { return $Explicit }
    if ($env:GW_ARM_GCC) { return $env:GW_ARM_GCC }
    $cands = @()
    $cands += Get-ChildItem "D:\stm32cubeIDE","C:\ST","$env:USERPROFILE\STM32Cube" -Recurse -Depth 6 `
              -Filter "arm-none-eabi-gcc.exe" -ErrorAction SilentlyContinue |
              Select-Object -ExpandProperty DirectoryName
    if ($cands.Count -eq 0) {
        $c = Get-Command arm-none-eabi-gcc -ErrorAction SilentlyContinue
        if ($c) { return (Split-Path $c.Source) }
    }
    if ($cands.Count -eq 0) { throw "找不到 arm-none-eabi-gcc（用 -Toolchain 或 GW_ARM_GCC 指定）" }
    return $cands[0]
}

$tc = Find-Toolchain -Explicit $Toolchain
$gcc      = Join-Path $tc "arm-none-eabi-gcc.exe"
$objcopy  = Join-Path $tc "arm-none-eabi-objcopy.exe"
$sizeTool = Join-Path $tc "arm-none-eabi-size.exe"
foreach ($x in @($gcc, $objcopy, $sizeTool)) {
    if (-not (Test-Path $x)) { throw "工具链里缺 $x" }
}

# ---------------- 路径检查 ----------------
$coreInc = Join-Path $Project "Core\Inc"
$coreSrc = Join-Path $Project "Core\Src"
$halSrc  = Join-Path $Project "Drivers\STM32F1xx_HAL_Driver\Src"
$startup = Join-Path $Project "Core\Startup\startup_stm32f103zetx.s"
$sysSrc  = Join-Path $coreSrc "system_stm32f1xx.c"
foreach ($p in @($coreInc, $coreSrc, $halSrc, $startup, $sysSrc)) {
    if (-not (Test-Path $p)) { throw "找不到 $p（-Project 指对了没？）" }
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$inc = @(
    "-I$(Join-Path $Project 'Core\Inc')",
    "-I$(Join-Path $Project 'Drivers\STM32F1xx_HAL_Driver\Inc')",
    "-I$(Join-Path $Project 'Drivers\CMSIS\Device\ST\STM32F1xx\Include')",
    "-I$(Join-Path $Project 'Drivers\CMSIS\Include')",
    "-I$(Join-Path $repo 'inc')",
    "-I$(Join-Path $repo 'port')",
    "-I$(Join-Path $repo 'drivers')",
    "-I$(Join-Path $repo 'app')"
)

# ⚠️ 只开 -Wall（与 CubeMX 生成的工程一致）。
#    加 -Wextra 会在**CubeMX 生成的文件**里报 unused-parameter（assert_failed 的
#    file/line），那不是我们的代码 —— 与其压噪不如与工程保持同一套开关。
$cflags = @("-mcpu=cortex-m3", "-mthumb", "-std=gnu11", "-O$Optimize", "-g3",
            "-ffunction-sections", "-fdata-sections",
            "-Wall",
            "-DUSE_HAL_DRIVER", "-DSTM32F103xE") + $inc

$ldflags = @("-mcpu=cortex-m3", "-mthumb",
             "--specs=nosys.specs", "--specs=nano.specs",
             "-Wl,--gc-sections", "-static", "-mfloat-abi=soft",
             "-Wl,--start-group", "-lc", "-lm", "-Wl,--end-group")

# ---------------- 公共源文件 ----------------
# ⚠️ gw_rtos.c 必须排除：它 #include "FreeRTOS.h"，而没有内核头文件就编不过。
#    （CI 里也是这么排除的 —— 这条规则两处必须一致。）
$coreObjs = Get-ChildItem $coreSrc -Filter *.c |
    # ⚠️ 还要排除 CubeMX 新建的 freertos.c（2026-10-11 实测）：
    #    它是默认任务 + CMSIS 包装层调用，对本项目是**死代码**
    #    （任务由 port/gw_rtos.c 创建，落地 main 不调 MX_FREERTOS_Init），
    #    但会被当成 Core 源文件编 —— 连 bootloader 都会因缺 FreeRTOS.h 而失败。
            Where-Object { $_.Name -ne "main.c" -and $_.Name -ne "system_stm32f1xx.c" -and $_.Name -ne "freertos.c" } |
            Select-Object -ExpandProperty FullName
$halObjs  = Get-ChildItem $halSrc -Filter *.c | Select-Object -ExpandProperty FullName
$repoObjs = @()
foreach ($d in @("src", "app", "drivers", "port")) {
    $repoObjs += Get-ChildItem (Join-Path $repo $d) -Filter *.c |
                 Where-Object { $_.Name -ne "gw_rtos.c" } |
                 Select-Object -ExpandProperty FullName
}

# ---------------- LVGL（可选，见 param 里的说明）----------------
# 只在 -Lvgl 时收集；关掉时这三个数组是空的，**对现有构建零影响**。
$lvglSrc = @(); $lvglInc = @(); $lvglDef = @()
if ($Lvgl) {
    if (-not (Test-Path $LvglDir)) {
        throw "找不到 LVGL 目录：$LvglDir（用 -LvglDir 指定，例如 D:\lvgl\lvgl-release-v8.2）"
    }
    $lvRoot = $LvglDir
    $lvglSrc += Get-ChildItem (Join-Path $lvRoot 'src') -Filter *.c -Recurse |
        Where-Object {
            # ⚠️ 跳过 src/extra（扩展控件）—— 本工程 LV_USE_* 全关，编进来只会带来
            #    大量"隐式声明"警告把真正的错误淹没。
            # ⚠️ 但 src/extra/lv_extra.c 必须留：lv_init() 会调 lv_extra_init()。
            $rel = $_.FullName.Substring($lvRoot.Length)
            (($rel -notmatch '\\extra\\') -or ($_.Name -eq 'lv_extra.c'))
        } | Select-Object -ExpandProperty FullName
    # 头文件路径：LVGL 根（lvgl.h）+ src；lv_conf.h 由已有的 -I<repo>\port 找到。
    $lvglInc += @("-I$lvRoot", "-I$(Join-Path $lvRoot 'src')")
    # ⚠️ LV_CONF_INCLUDE_SIMPLE 让 LVGL 直接 #include "lv_conf.h"（从 -I 路径找）
    $lvglDef += @("-DLV_CONF_INCLUDE_SIMPLE", "-DGW_USE_LVGL=1")
    Write-Host ("    LVGL 已启用：{0} 个源文件，来自 {1}" -f $lvglSrc.Count, $lvRoot) -ForegroundColor Cyan
}

# ⚠️ 生成"改好向量表偏移"的 system 文件副本（不动工程里的原件）
function New-SystemStub {
    param([string]$Offset, [string]$OutPath)
    $t = [System.IO.File]::ReadAllText($sysSrc, [System.Text.Encoding]::UTF8)
    $new = [regex]::Replace($t, '(?m)^#define\s+VECT_TAB_OFFSET\s+\S+',
                            "#define VECT_TAB_OFFSET  $Offset /* 由 build_ota_images.ps1 按槽位生成 */")
    if ($new -eq $t -and $Offset -ne "0x00000000U") {
        throw "system_stm32f1xx.c 里没找到 VECT_TAB_OFFSET 宏，偏移改不了"
    }
    [System.IO.File]::WriteAllText($OutPath, $new, (New-Object System.Text.UTF8Encoding($false)))
    return $OutPath
}

# ---------------- 单个镜像的构建 ----------------
$results = @()

function Build-Image {
    param(
        [string]$Name,            # bootloader / app_a / app_b
        [string]$LdFile,
        [string]$VecOffset,
        [int]   $SlotBytes,
        [string[]]$ExtraSrc,
        # 额外的 include / 宏定义（RTOS 链接检查用：要加内核头文件路径）
        [string[]]$ExtraInc = @(),
        [string[]]$ExtraDef = @(),
        # 额外的链接参数（RTOS 检查用 `-Wl,-u,...` 强制保留入口：
        # 否则 --gc-sections 会把没被 main 引用的 RTOS 层整段丢掉，
        # 于是"缺符号"根本不报 —— 检查就变弱了）
        [string[]]$ExtraLd = @(),
        # 要从工程 Core/Src 里**排除**的文件名。RTOS 链接检查用它排掉
        # `stm32f1xx_it.c` —— 原因见下面 RTOS 段（那三个 handler 在 CubeMX
        # 启用 FreeRTOS 时会被删掉，这里按"删掉之后"的样子链）。
        [string[]]$ExcludeCore = @()
    )

    Write-Host ""
    Write-Host "==== 构建 $Name （$LdFile, VECT_TAB_OFFSET=$VecOffset）====" -ForegroundColor Cyan

    $sysCopy = Join-Path $OutDir "$Name`_system.c"
    # ⚠️ 别写成 C 风格的 (void)(...) —— PowerShell 里那不是合法语句
    $null = New-SystemStub -Offset $VecOffset -OutPath $sysCopy

    $elf = Join-Path $OutDir "$Name.elf"
    $map = Join-Path $OutDir "$Name.map"
    $bin = Join-Path $OutDir "$Name.bin"

    $coreUse = $coreObjs
    if ($ExcludeCore.Count -gt 0) {
        $coreUse = $coreObjs | Where-Object { $ExcludeCore -notcontains (Split-Path $_ -Leaf) }
        Write-Host ("    排除 Core/Src: " + ($ExcludeCore -join ", ")) -ForegroundColor Yellow
    }
    $srcs = @($startup, $sysCopy) + $coreUse + $halObjs + $repoObjs + $ExtraSrc
    $args = @($cflags) + @($ExtraDef) + @($ExtraInc) + @($srcs) + @("-T", (Join-Path $repo $LdFile),
             "-Wl,-Map=$map", "-Wl,--print-memory-usage", "-o", $elf) + $ExtraLd + $ldflags

    # ⚠️ 调本地 exe 前必须把 ErrorActionPreference 放回 Continue：
    #    在 Stop 下，gcc 写到 stderr 的**编译错误**会被 PowerShell 当成终止性
    #    错误抛出 —— 真正的错误信息反而被异常吞掉，只剩一句 NativeCommandError。
    #    （第一版就是这样，白看了一轮。）
    $eap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $out = & $gcc @args 2>&1 | Out-String
    $code = $LASTEXITCODE

    if ($code -ne 0) {
        Write-Host $out
        # ⚠️ 回显实际命令行（只看 -D/-I/-Wl/--specs）：
        #    这样"-D 参数被拆坏 / 数组嵌套"这类问题一眼可见。
        Write-Host "---- gcc 参数（仅 -D/-I/-Wl/--specs）----" -ForegroundColor Yellow
        $args | Where-Object { $_ -match '^-D|^-I|^-Wl|^--specs' } |
            ForEach-Object { Write-Host ("    [" + $_ + "]") -ForegroundColor Yellow }
        $ErrorActionPreference = $eap
        throw "$Name 编译/链接失败（退出码 $code）"
    }
    $warn = ($out -split "`n") | Where-Object { $_ -match 'warning:|error:' }
    if ($warn) { $warn | Select-Object -First 12 | ForEach-Object { Write-Host "    $_" } }
    if (-not (Test-Path $elf)) { $ErrorActionPreference = $eap; throw "$Name 没有产出 elf" }

    & $objcopy -O binary $elf $bin
    $ErrorActionPreference = $eap
    if ($LASTEXITCODE -ne 0) { throw "$Name objcopy 失败" }

    # ---- 自证 ①：不得超过槽位 ----
    $sz = (& $sizeTool $elf) | Select-Object -Last 1
    $parts = ($sz -split '\s+') | Where-Object { $_ -ne "" }
    $flashBytes = [int]$parts[0] + [int]$parts[1]      # text + data
    $binSize = (Get-Item $bin).Length
    if ($binSize -gt $SlotBytes) {
        throw ("$Name 镜像 $binSize 字节 > 槽位 $SlotBytes 字节 —— 链接脚本或代码超了")
    }

    # ---- 自证 ②：向量表前 8 字节 = 栈顶 + 复位向量，必须落在合法范围 ----
    $b = [System.IO.File]::ReadAllBytes($bin)
    if ($b.Length -lt 8) { throw "$Name 镜像太小" }
    $sp = [BitConverter]::ToUInt32($b, 0)
    $pc = [BitConverter]::ToUInt32($b, 4)
    $ramLo = 0x20000000; $ramHi = 0x20010000
    if ($sp -lt $ramLo -or $sp -gt $ramHi -or ($sp % 8) -ne 0) {
        throw ("$Name 的栈顶 0x{0:X8} 不在 RAM 内或未 8 字节对齐" -f $sp)
    }
    # 复位向量的地址必须落在"镜像自身"覆盖的区间里
    $imgBase = $null
    if ($Name -eq "bootloader") { $imgBase = 0x08000000 } 
    elseif ($Name -eq "app_a")  { $imgBase = 0x08009000 }
    elseif ($Name -eq "app_b")  { $imgBase = 0x0802B000 }
    elseif ($Name -eq "app_golden") { $imgBase = 0x0804D000 }
    # RTOS 链接检查用槽 A 的地址（它只是链接检查，不烧录）
    elseif ($Name -eq "app_rtos_link") { $imgBase = 0x08009000 }
    if ($imgBase -eq $null) { throw "$Name 没有登记基址（改分区表时同步本函数）" }
    $imgEnd = $imgBase + $binSize
    if ($pc -lt $imgBase -or $pc -ge $imgEnd) {
        throw ("$Name 的复位向量 0x{0:X8} 不在镜像 0x{1:X8}..0x{2:X8} 内 —— 链接地址与向量表偏移不匹配" -f $pc, $imgBase, $imgEnd)
    }

    Write-Host ("    [OK] {0,-11} {1,7} 字节（槽 {2} 字节，占 {3:P1}）  栈顶=0x{4:X8}  复位=0x{5:X8}" -f `
                $Name, $binSize, $SlotBytes, ($binSize / $SlotBytes), $sp, $pc) -ForegroundColor Green

    $script:results += [pscustomobject]@{
        Name = $Name; Elf = $elf; Bin = $bin; Bytes = $binSize
        Slot = $SlotBytes; Base = ("0x{0:X8}" -f $imgBase)
    }
}

# ==================================================================
#  ★ FreeRTOS 整镜像链接检查（-RtosLink）—— 只链接，不烧板
#
#  为什么要有这一步（"只写不烧"阶段最值钱的一步）：
#    RTOS 落地前有一整类问题**只在链接期暴露**：
#      · 缺符号      —— 某个 hook / API 没实现（缺陷 Q 就是这一类，只是它更早一步）
#      · 重复定义    —— 内核与工程各定义了一个 handler / 向量
#      · **装不下**  —— RAM 或槽位 Flash 超了，链接脚本当场报错
#    这些都**不需要烧板**，所以浸泡期间可以随便跑，把上板当天的意外提前消掉。
#
#  ⚠️ 产出固定写 build\rtos_link，**绝不碰 build\ota** ——
#     后者是 72 小时浸泡测试的冻结基线（SHA256 记在交接文档 §二·八）。
#  ⚠️ 产出物**不是可烧录镜像**：这里用的 main 是 templates\main_template.c
#     的 RTOS 分支，不是 CubeMX 工程的 main.c。真上板走正式槽位流程。
# ==================================================================
if ($RtosLink -or $Rtos) {
    Write-Host ""
    if ($RtosLink) {
        Write-Host ""
        Write-Host "==== FreeRTOS 整镜像链接检查（只链接，不烧板）====" -ForegroundColor Cyan
    }
    if ($Rtos) {
        Write-Host ""
        Write-Host "==== 构建 RTOS 槽位镜像（带内核 + GW_USE_FREERTOS）====" -ForegroundColor Cyan
    }

    if ($Freertos -eq "") {
        $fwRoots = @()
        if ($env:GW_FW_ROOT) { $fwRoots += $env:GW_FW_ROOT }
        $fwRoots += @("$env:USERPROFILE\STM32Cube\Repository", "D:\STM32Cube\Repository",
                      "C:\STM32Cube\Repository")
        foreach ($r in $fwRoots) {
            if (-not (Test-Path $r)) { continue }
            $hit = Get-ChildItem $r -Directory -Filter "STM32Cube_FW_F1*" -ErrorAction SilentlyContinue |
                   Sort-Object Name -Descending | Select-Object -First 1
            if ($hit) {
                $cand = Join-Path $hit.FullName "Middlewares\Third_Party\FreeRTOS\Source"
                if (Test-Path $cand) { $Freertos = $cand; break }
            }
        }
    }
    if ($Freertos -eq "" -or -not (Test-Path $Freertos)) {
        throw ("找不到 FreeRTOS 内核源码。用 -Freertos <...\Middlewares\Third_Party\" +
               "FreeRTOS\Source> 指定，或设 GW_FW_ROOT")
    }
    Write-Host "  内核源码 : $Freertos"

    $rtInc = @("-I$(Join-Path $Freertos 'include')",
               "-I$(Join-Path $Freertos 'portable\GCC\ARM_CM3')")

    # ⚠️ 用的 main 是**工程的真实 main.c**（像普通镜像一样），不是模板 ——
    #    因为模板是"合并到 main.c 里的片段"，它依赖 CubeMX 在 main.c 里
    #    定义的句柄（huart1 等），单独当 main 编译会报 huart1 undeclared。
    #    ⚠️ 但真实 main.c 的 main() 不会去启动调度器，那 RTOS 层就没被引用 ——
    #       `--gc-sections` 会把整段丢掉，"缺符号"也就不会报。
    #       所以下面用 `-Wl,-u,gw_rtos_create_tasks` **强制保留** RTOS 入口，
    #       让它连带的依赖（内核 API、hook、HAL 句柄）全部必须解析出来。
    $rtSrc = @(
        (Join-Path $Freertos "tasks.c"),
        (Join-Path $Freertos "queue.c"),
        (Join-Path $Freertos "list.c"),
        (Join-Path $Freertos "portable\GCC\ARM_CM3\port.c"),
        (Join-Path $Freertos "portable\MemMang\heap_4.c"),
        (Join-Path $repo "port\gw_rtos.c"),                  # 普通镜像里排除的那一个
        (Join-Path $coreSrc "main.c")                        # 工程真实 main（提供句柄与 MX_*_Init）
    )
    # ⚠️ 要 root **全部** RTOS 入口，不能只 root create_tasks ——
    #    否则 `--gc-sections` 会把 `vTaskStartScheduler` 与调度器核心
    #    （tick 处理、上下文切换、hook）当成"没人用"整段丢掉，
    #    于是"内核能不能链上"这件事**根本没被验证**。
    #    实测证据：只 root create_tasks 时 text 只比裸机版大 476 字节 —— 太小了，
    #    正说明核心被丢了（现在三个入口都 root，体积才对得上）。
    $rtLd = @("-Wl,-u,gw_rtos_create_tasks",
              "-Wl,-u,gw_rtos_start",
              "-Wl,-u,gw_rtos_iwdg_enable")

    # ⚠️⚠️ 必须排除工程的 stm32f1xx_it.c —— 这是**实测出来的落地步骤**：
    #
    #    `port/FreeRTOSConfig.h` 里那三行重定向宏（缺陷 R 的修法）让 port.c 也
    #    定义了 `SVC_Handler` / `PendSV_Handler` / `SysTick_Handler`，
    #    而工程的 stm32f1xx_it.c **里这三个 handler 还在** -> 链接直接报
    #    `multiple definition of 'SysTick_Handler'`（第一次跑这个检查就撞上了）。
    #
    #    CubeMX 在启用 FreeRTOS 时会**自动**从它生成的 it.c 里删掉这三个 ——
    #    所以这里按"已经删掉"的样子链（排除整个文件）。
    #    ⚠️ 排掉之后向量表里那些 IRQ 由**启动文件的弱定义**兜底，链接不受影响；
    #       但真实工程里 it.c 仍然要有其它中断服务函数，别整个文件删掉。
    $rtExclude = @("stm32f1xx_it.c")

    if ($RtosLink) {
    $OutDir = Join-Path $repo "build\rtos_link"
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

    Build-Image -Name "app_rtos_link" -LdFile "ld\app_slot_a.ld" -VecOffset "0x00009000U" `
                -SlotBytes (136 * 1024) `
                -ExtraSrc $rtSrc -ExtraInc $rtInc -ExtraLd $rtLd -ExcludeCore $rtExclude

    Write-Host ""
    Write-Host "  [OK] RTOS 整镜像链接通过（内核 + 调度器核心 + RTOS 层 + 工程 main）" -ForegroundColor Green
    Write-Host "       ⚠️ 这里用的是**工程真实 main.c** + -u 强制保留 RTOS 入口；" -ForegroundColor Yellow
    Write-Host "          模板里那段 RTOS 分支由 CI 的「模板类型检查」另编一遍。" -ForegroundColor Yellow
    Write-Host "  ⚠️ 这不是可烧录镜像：真上板走正式槽位流程（迁移指南第 6 步）。" -ForegroundColor Yellow
    Write-Host "  ⚠️ build\ota 未被触碰（浸泡基线完好）。" -ForegroundColor Yellow
    exit 0
    }
}

# ---------------- app 镜像的源文件/包含/宏（扁平数组，勿写成数组嵌套）----------------
# ⚠️⚠️ 2026-10-14 踩过的坑：写成
#       -ExtraDef $(if ($Rtos) { @("-D...=1") } else { @() }) + $lvglDef
#     会变成"数组里套数组"，PowerShell 传给 gcc 时用空格拼成一个字符串
#     => gcc 报 `token "=" is not valid in preprocessor expressions`。
#     所以这里先逐项 += 算平，再整体传入。
$appSrc = @()
if ($Rtos) { $appSrc += $rtSrc } else { $appSrc += (Join-Path $coreSrc "main.c") }
$appSrc += $lvglSrc

$appInc = @()
if ($Rtos) { $appInc += $rtInc }
$appInc += $lvglInc

$appDef = @()
if ($Rtos) { $appDef += "-DGW_USE_FREERTOS=1" }
$appDef += $lvglDef

# ---------------- 四个镜像（v2 分区表：三个镜像区等大 136 KB） ----------------
Build-Image -Name "bootloader" -LdFile "ld\bootloader.ld" -VecOffset "0x00000000U" `
            -SlotBytes (32 * 1024) `
            -ExtraSrc @((Join-Path $repo "templates\bootloader_main.c"),
                        (Join-Path $repo "templates\bootloader_glue.c"))

Build-Image -Name "app_a" -LdFile "ld\app_slot_a.ld" -VecOffset "0x00009000U" `
            -SlotBytes (136 * 1024) `
            -ExtraSrc $appSrc `
            -ExtraInc $appInc `
            -ExtraDef $appDef

if ($SlotB) {
    Build-Image -Name "app_b" -LdFile "ld\app_slot_b.ld" -VecOffset "0x0002B000U" `
                -SlotBytes (136 * 1024) `
            -ExtraSrc $appSrc `
            -ExtraInc $appInc `
            -ExtraDef $appDef
}

if ($Golden) {
    # ⚠️ golden 用的就是**同一份应用源码**，只换链接地址 —— 不是另写恢复固件。
    #    这样"校验 + 跳转"路径与 A/B 完全共用，不引入新的代码路径与失败模式。
    #    它的内容含义是"某个已知良好版本"，所以量产后应把**已验证通过**的那一版
    #    重新用本开关构建一次，烧进 golden 区（见 README 的分区表说明）。
    Build-Image -Name "app_golden" -LdFile "ld\app_golden.ld" -VecOffset "0x0004D000U" `
                -SlotBytes (136 * 1024) `
            -ExtraSrc $appSrc `
            -ExtraInc $appInc `
            -ExtraDef $appDef
}

Write-Host ""
Write-Host "==================== 产出 ====================" -ForegroundColor Cyan
$results | Format-Table Name, Base, Bytes, Slot, Bin -AutoSize | Out-String | Write-Host
Write-Host "烧录（地址必须与上表一致）：" -ForegroundColor Yellow
foreach ($r in $results) {
    $addr = $r.Base
    Write-Host ("  STM32_Programmer_CLI -c port=SWD -w `"{0}`" {1} -v" -f $r.Bin, $addr)
}
Write-Host ""
Write-Host "⚠️ 首次从「单镜像 0x08000000」切到槽位化时：元数据两页里残留的是旧镜像的代码，" -ForegroundColor Yellow
Write-Host "   会被判为无效记录（magic/CRC 不符）-> 引导程序回退启动槽 A，这是预期的。"
Write-Host "   随后**第一次真实写盘**会把目标页擦掉、把残留清理干净 —— 不需要手工擦 Flash。"
Write-Host "   （⚠️ 不要在单镜像运行时用 CLI 去擦 0x08008000：那地址属于正在运行的应用代码，"
Write-Host "     会把设备擦挂 —— 这个坑本项目真实踩过一次。）"
