<#
.SYNOPSIS
    本地 CI：交叉编译 -> 单元测试 -> cppcheck 静态分析

.DESCRIPTION
    对应项目工程报告「7.2 工程化实践」要求的流水线：
        代码推送触发 -> arm-none-eabi-gcc 交叉编译 -> 执行单元测试 -> cppcheck 静态分析

    本脚本把这三步在本地一次跑完，退出码非 0 表示有任一步失败。
    同样的步骤可以直接搬到 GitHub Actions（见 .github/workflows/ci.yml）。

.NOTES
    ⚠️ 与报告的差异（如实记录）：
      报告写的是 CI 在代码推送时自动触发。本仓库是纯源码交付、
        没有托管到 GitHub，所以这里提供的是**可手动执行、也可直接
        用于 CI 的脚本**，并且附带了 GitHub Actions 配置。
      报告写的是 Unity/CMock 框架；本项目用了自建的极简框架
        （理由见 README「运行测试」章节：Unity 需要额外的 C 文件与
          submodule，而本项目的断言需求用 30 行宏就够了）。

.EXAMPLE
    pwsh -File tools/ci.ps1
    pwsh -File tools/ci.ps1 -SkipFuzz        # 跳过耗时长的 fuzz
#>
param(
    [switch]$SkipFuzz,
    [switch]$SkipCppcheck,
    # ⚠️ 优先取环境变量 GW_FW_ROOT —— 这样 CI 可以把固件包放在任意位置，
    #    而不必改脚本。本地开发时用下面的默认值（本机实测路径）。
    [string]$FwRoot = $(if ($env:GW_FW_ROOT) {
        $env:GW_FW_ROOT
    } else {
        "C:\Users\acer\STM32Cube\Repository\STM32Cube_FW_F1_V1.8.7"
    }),
    [string]$ArmGcc = ""
)

$ErrorActionPreference = "Continue"

# ⚠️ 平台差异：Windows 的可执行文件带 .exe，Linux 不带
#
# ⚠️⚠️ 变量名特意叫 $EXESUF 而不是 $EXE ——
#     **PowerShell 的变量名大小写不敏感**，
#     叫 $EXE 会与下面各步骤里的局部变量 $exe（完整路径）**撞名**：
#         $exe = Join-Path $PWD "tools\powerloss_ota$EXE"
#     这一行先把 "$EXE" 展开成后缀、再赋给 $exe ——
#     但赋值之后 $EXE 本身就变成了完整路径！
#     下一步再用 $EXE 就拼出 "...param" + "C:\...\powerloss_ota.exe"。
#     这个坑本项目真实踩过（第 12 步编译失败），改名根治。。
#    这个变量让同一份脚本能在两个平台上跑 ——
#    这样 GitHub Actions 可以直接调用它，
#    而不是维护第二套 CI 定义（两套一定会漂移）。
if ($IsWindows -or ($null -eq $IsWindows)) {
    $EXESUF = ".exe"          # Windows（PowerShell 5.1 没有 $IsWindows）
} else {
    $EXESUF = ""              # Linux / macOS
}

# ⚠️ 让所有 Python 辅助脚本用 UTF-8 输出。
#    Windows 的 Python 默认按控制台代码页（中文环境是 GBK）编码 stdout，
#    而我们的脚本里有 ⚠️ 之类的非 GBK 字符 —— 那会抛
#    UnicodeEncodeError 并让整步失败。
#    第一次就踩了这个：脚本单独跑（外层设了 PYTHONIOENCODING）通过，
#    在 CI 里却崩在 print 上。
$env:PYTHONIOENCODING = "utf-8"

# ⚠️⚠️ 让 PowerShell 按 UTF-8 解码**本机程序**（gcc 编出来的测试 exe）的输出。
#
#    这是一个真实踩过的坑（2026-10-15）：
#      gcc 编出来的测试 exe 用 printf 打中文（源文件是 UTF-8 字节），
#      而 PowerShell 默认按**控制台代码页**（中文环境是 936/GBK）解码
#      本机程序的 stdout —— 于是 "通过 239 项, 失败 0 项" 这行变成乱码，
#      下面第 2 步的 `Select-String -Pattern "通过 (\d+) 项"` **一个都匹配不到**。
#
#      后果极具误导性：32 套测试**全部**被报成
#        "[失败] test_xxx 有失败项："   （$line 是空串，所以冒号后面什么都没有）
#      然后汇总成 "CI 失败：33 处问题"。
#      看起来像"整个仓库全挂了"，实际上一行代码都没错 —— 只是解码不对。
#
#      ⚠️ 症状里最坑的一点：失败消息末尾是**空的**。看到
#         "有失败项：" 后面什么都没有，就该想到"这不是测试失败，是没解析到汇总行"。
#
#    ⚠️ 只设 $OutputEncoding 不够 —— 那个只管 PowerShell 往管道里**写**的编码，
#       管不到它**读**本机程序 stdout 时用的编码。必须设 [Console]::OutputEncoding。
#    ⚠️ 也不能靠"用户自己 chcp 65001"：CI 脚本应当在任何代码页下都给出正确结论。
try {
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
    $OutputEncoding = [System.Text.Encoding]::UTF8
} catch {
    Write-Host "  [警告] 无法把控制台输出编码设成 UTF-8，中文匹配可能失准" -ForegroundColor Yellow
}

$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$script:failed = 0
$script:steps  = 0

function Step($name) {
    $script:steps++
    Write-Host ""
    Write-Host ("=" * 66) -ForegroundColor Cyan
    Write-Host ("  [{0}] {1}" -f $script:steps, $name) -ForegroundColor Cyan
    Write-Host ("=" * 66) -ForegroundColor Cyan
}

function Fail($msg) {
    $script:failed++
    Write-Host ("  [失败] " + $msg) -ForegroundColor Red
}

function Pass($msg) {
    Write-Host ("  [通过] " + $msg) -ForegroundColor Green
}

# ------------------------------------------------------------------ 环境探测
function Find-ArmGcc {
    if ($ArmGcc -ne "" -and (Test-Path $ArmGcc)) { return $ArmGcc }

    $c = Get-Command arm-none-eabi-gcc -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }

    # CubeIDE 自带的路径（本机实测位置）
    $guess = Get-ChildItem "D:\stm32cubeIDE" -Recurse -Filter "arm-none-eabi-gcc$EXESUF" `
             -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($guess) { return $guess.FullName }
    return ""
}

$gccHost = (Get-Command gcc -ErrorAction SilentlyContinue).Source
$gccArm  = Find-ArmGcc

Step "环境检查"
Write-Host ("  主机编译器   : " + $(if ($gccHost) { $gccHost } else { "未找到" }))
Write-Host ("  交叉编译器   : " + $(if ($gccArm)  { $gccArm }  else { "未找到" }))
Write-Host ("  HAL 固件包   : " + $(if (Test-Path $FwRoot) { $FwRoot } else { "未找到" }))
if (-not $gccHost) { Fail "找不到 gcc，无法跑单元测试" }
if (-not $gccArm)  { Fail "找不到 arm-none-eabi-gcc，无法做交叉编译验证" }
if (-not (Test-Path $FwRoot)) { Fail "找不到 HAL 固件包：$FwRoot（交叉编译需要它的头文件）" }

# ------------------------------------------------------------------ 1) 单元测试
Step "单元测试（主机端）"

# 每套测试的源文件与 include 路径
$base = @("src/modbus_crc.c","src/modbus_rtu.c","src/modbus_master.c",
          "src/gw_pipeline.c","src/gw_frame.c","src/gw_uplink.c",
          "src/gw_can_core.c","src/gw_timing.c","src/gw_sched.c",
          "src/gw_cmd.c","src/gw_devinfo.c","drivers/dht22.c","app/gw_app.c")

$suites = @(
    @{n="test_regmap";    i=@("-Iinc");                       s=@("src/modbus_crc.c","src/modbus_rtu.c","test/test_regmap.c")},
    @{n="test_dht22";     i=@("-Idrivers");                   s=@("drivers/dht22.c","test/test_dht22.c")},
    @{n="test_master";    i=@("-Iinc");                       s=@("src/modbus_crc.c","src/modbus_rtu.c","src/modbus_master.c","test/test_master.c")},
    @{n="test_pipeline";  i=@("-Iinc","-Idrivers");           s=@("drivers/dht22.c","src/gw_pipeline.c","test/test_pipeline.c")},
    @{n="test_app";       i=@("-Iinc","-Idrivers","-Iapp");   s=$base+@("test/test_app.c")},
    @{n="test_app_bus";   i=@("-Iinc","-Idrivers","-Iapp");   s=$base+@("src/rs485_arbiter.c","test/test_app_bus.c")},
    # ⚠️ 主站响应派发 —— 守护一个上板才暴露的缺陷：
    #    mb_master_on_response() 在生产代码里从没被调用过，
    #    导致主站收不到响应、一直持总线、把整条总线锁死。
    #    原 2275 项测试全过却漏掉它，因为 test_app_bus 的 take_frame
    #    直接 return -1，而其它测试**手工调** on_response，都绕过了这段接线。
    @{n="test_master_dispatch"; i=@("-Iinc","-Idrivers","-Iapp"); s=$base+@("test/test_master_dispatch.c")},
    @{n="test_app_uplink";i=@("-Iinc","-Idrivers","-Iapp");   s=$base+@("test/test_app_uplink.c")},
    @{n="test_app_can";   i=@("-Iinc","-Idrivers","-Iapp");   s=$base+@("test/test_app_can.c")},
    @{n="test_app_sched"; i=@("-Iinc","-Idrivers","-Iapp");   s=$base+@("test/test_app_sched.c")},
    @{n="test_app_cmd";   i=@("-Iinc","-Idrivers","-Iapp");   s=$base+@("test/test_app_cmd.c")},
    @{n="test_arbiter";   i=@("-Iinc");                       s=@("src/rs485_arbiter.c","test/test_arbiter.c")},
    @{n="test_param";     i=@("-Iinc");                       s=@("src/gw_param.c","test/test_param.c")},
    @{n="test_frame";     i=@("-Iinc");                       s=@("src/gw_frame.c","test/test_frame.c")},
    @{n="test_uplink";    i=@("-Iinc");                       s=@("src/gw_frame.c","src/gw_uplink.c","test/test_uplink.c")},
    @{n="test_devinfo";   i=@("-Iinc");                       s=@("src/gw_devinfo.c","src/gw_param.c","test/test_devinfo.c")},
    @{n="test_can";       i=@("-Iinc");                       s=@("src/gw_can_core.c","test/test_can.c")},
    @{n="test_log";       i=@("-Iinc");                       s=@("src/gw_log.c","src/gw_fault.c","test/test_log.c")},
    @{n="test_timing";    i=@("-Iinc");                       s=@("src/gw_timing.c","test/test_timing.c")},
    # 相位切分对 loop 统计的影响 —— PC 上复现了"CPU 负载饱和 1000‰"那个缺陷
    # （真因：SYNC 相位把轮末空闲时间吸进了 busy，导致 busy≈total）
    @{n="test_timing_phasesplit"; i=@("-Iinc");               s=@("src/gw_timing.c","test/test_timing_phasesplit.c")},
    @{n="test_sched";     i=@("-Iinc");                       s=@("src/gw_sched.c","test/test_sched.c")},
    # 界面离线渲染：把 gw_ui.c 编到 PC 上，用桩件画进内存帧缓冲并断言内容。
    # ⚠️ 它不进 ARM 镜像，只是 host 侧回归（几何/脏区/残字这类编译查不出的问题）。
    @{n="test_ui_render"; i=@("-Iinc","-Iport","-Iapp","-Itest/ui_sim");
      s=@("app/gw_ui.c","port/lcd_font_8x16.c","test/test_ui_render.c")},
    @{n="test_ota";       i=@("-Iinc","-Iport");              s=@("src/gw_crc32.c","src/gw_ota.c","test/test_ota.c")},
    @{n="test_boot";      i=@("-Iinc","-Iport");              s=@("src/gw_crc32.c","src/gw_ota.c","src/gw_boot.c","test/test_boot.c")},
    @{n="test_cmd";       i=@("-Iinc");                       s=@("src/gw_cmd.c","test/test_cmd.c")},
    @{n="test_selfcheck"; i=@("-Iinc");                       s=@("src/gw_selfcheck.c","test/test_selfcheck.c")},
    @{n="test_baud";      i=@("-Iinc");                       s=@("src/gw_baud.c","test/test_baud.c")},
    @{n="test_ota_bridge";i=@("-Iinc","-Iport");
      s=@("src/gw_crc32.c","src/gw_ota.c","src/gw_ota_bridge.c","test/test_ota_bridge.c")}
)

# ---- ESP32 侧（纯逻辑部分；硬件相关层无法在 PC 上编译） ----
$suites += @{n="test_esp32_json";   i=@("-Iinc","-Iesp32/include");
             s=@("src/gw_frame.c","esp32/src/esp32_json.c","esp32/test/test_esp32_json.c")}
$suites += @{n="test_esp32_bridge"; i=@("-Iesp32/include");
             s=@("esp32/src/esp32_bridge.c","esp32/test/test_esp32_bridge.c")}
# 端到端对接：STM32 打包/封帧 -> 字节流 -> ESP32 解帧/转 JSON。
# ⚠️ 单独看前两个套件各测各的（payload 都是自己造的），
#    一旦两侧布局漂移，它们**都会通过** —— 这个套件才是那道闸。
$suites += @{n="test_link_e2e";     i=@("-Iinc","-Iesp32/include");
             s=@("src/gw_frame.c","esp32/src/esp32_json.c","esp32/test/test_link_e2e.c")}

if (-not $SkipFuzz) {
    $fuzzSrc = @("src/modbus_crc.c","src/modbus_rtu.c","src/modbus_master.c",
                 "src/gw_pipeline.c","src/gw_frame.c","src/gw_uplink.c",
                 "src/gw_can_core.c","src/gw_timing.c","src/gw_sched.c",
                 "src/gw_cmd.c","src/gw_devinfo.c","src/rs485_arbiter.c",
                 "src/gw_ota.c","src/gw_crc32.c","drivers/dht22.c",
                 "app/gw_app.c","test/gw_guard.c","test/test_fuzz.c")
    $suites += @{n="test_fuzz"; i=@("-Iinc","-Idrivers","-Iapp","-Iport","-Itest","-O2");
                 s=$fuzzSrc; arg="50000"}
}

$total = 0
$passedSuites = 0
foreach ($t in $suites) {
    $args = @("-std=c99","-Wall","-Wextra") + $t.i + @("-o","test/$($t.n)$EXESUF") + $t.s
    $build = & $gccHost @args 2>&1
    if ($LASTEXITCODE -ne 0) {
        Fail "$($t.n) 编译失败"
        $build | Select-Object -First 4 | ForEach-Object { Write-Host "        $_" }
        continue
    }
    if ($t.arg) { $out = & "./test/$($t.n)$EXESUF" $t.arg 2>&1 } else { $out = & "./test/$($t.n)$EXESUF" 2>&1 }
    $line = ($out | Select-String -Pattern "通过 (\d+) 项").Line
    # ⚠️ 先取数字再判"失败 0 项"：$Matches 会被后一次 -match 覆盖，
    #    写反了就会得到"合计 0 项"（这个坑我自己踩过一次）。
    $m = [regex]::Match([string]$line, "通过 (\d+) 项")
    if ($m.Success -and ([string]$line) -match "失败 0 项") {
        $total += [int]$m.Groups[1].Value
        $passedSuites++
        Write-Host ("  {0,-18} {1}" -f $t.n, ([string]$line).Trim())
    } else {
        # ⚠️ 汇总行**为空**与"汇总行说失败"是两回事，必须分开报。
        #    空 = 压根没匹配到那行 ⇒ 十有八九是输出编码不对
        #    （见文件开头的 [Console]::OutputEncoding）。
        #    合成一条报的话，现象是"32 套测试全部有失败项、且失败信息为空"，
        #    看起来像"整个仓库全挂了"，实际一行代码都没错。
        #    2026-10-15 真的这么查了十分钟。
        if ([string]::IsNullOrWhiteSpace([string]$line)) {
            Fail "$($t.n) 解析不到汇总行 —— 多半是输出编码问题，**不是**测试失败"
        } else {
            Fail "$($t.n) 有失败项：$line"
        }
    }
}
if ($passedSuites -eq $suites.Count) {
    Pass "$passedSuites 套测试全部通过，合计 $total 项"
} else {
    Fail "只有 $passedSuites / $($suites.Count) 套通过"
}

# ------------------------------------------------------------------ 2) 交叉编译
Step "交叉编译（目标平台 arm-none-eabi-gcc）"
if ($gccArm -and (Test-Path $FwRoot)) {
    $inc = @("-Iinc","-Iport","-Idrivers","-Iapp","-Itools/hal_conf_check",
             "-I$FwRoot/Drivers/STM32F1xx_HAL_Driver/Inc",
             "-I$FwRoot/Drivers/CMSIS/Device/ST/STM32F1xx/Include",
             "-I$FwRoot/Drivers/CMSIS/Include")
    $srcs = Get-ChildItem src -Filter *.c | ForEach-Object { $_.FullName }
    $srcs += (Get-ChildItem drivers -Filter *.c | ForEach-Object { $_.FullName })
    $srcs += (Get-ChildItem app     -Filter *.c | ForEach-Object { $_.FullName })
    # ⚠️ 排除 gw_rtos.c：它 #include "FreeRTOS.h"，只有带了内核头文件
    #    才能编译。它在下面的「FreeRTOS 内核 + RTOS 层」段里单独编译。
    $srcs += (Get-ChildItem port -Filter *.c |
              Where-Object { $_.Name -ne "gw_rtos.c" } |
              ForEach-Object { $_.FullName })

    $okN = 0; $badN = 0; $sum = 0
    $tmp = Join-Path $env:TEMP "gw_ci_obj"
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null

    foreach ($src in $srcs) {
        $obj = Join-Path $tmp ((Split-Path $src -Leaf) + ".o")
        $a = @("-mcpu=cortex-m3","-mthumb","-std=c99","-Wall","-Wextra","-c",
               "-DUSE_HAL_DRIVER","-DSTM32F103xE","-DGW_HAL_HAS_UARTEX_IDLE=1") + $inc + @($src,"-o",$obj)
        $r = & $gccArm @a 2>&1
        if ($r) {
            Fail "$(Split-Path $src -Leaf) 有告警/错误"
            $r | Select-Object -First 3 | ForEach-Object { Write-Host "        $_" }
            $badN++
        } elseif (Test-Path $obj) {
            $okN++; $sum += (Get-Item $obj).Length
        } else { $badN++ }
    }
    # ---- 附加：FreeRTOS 内核 + RTOS 层 ----
    # ⚠️ 这两部分只在固件包自带 FreeRTOS 时才能编译。
    #    它们验证的是"我们的 FreeRTOSConfig.h 能被内核接受"
    #    —— 这个坑很隐蔽（配错时报错会指向内核文件，看起来像内核有问题）。
    $rt = "$FwRoot\Middlewares\Third_Party\FreeRTOS\Source"
    if (Test-Path $rt) {
        $incR = $inc + @("-I$rt/include", "-I$rt/portable/GCC/ARM_CM3")
        $kern = @("$rt\tasks.c", "$rt\queue.c", "$rt\list.c",
                  "$rt\portable\GCC\ARM_CM3\port.c",
                  "$rt\portable\MemMang\heap_4.c",
                  "$PWD\port\gw_rtos.c")
        $kOk = 0; $kBad = 0; $kSum = 0
        foreach ($k in $kern) {
            $obj = Join-Path $tmp (("rtos_" + (Split-Path $k -Leaf)) + ".o")
            $a = @("-mcpu=cortex-m3","-mthumb","-std=c99","-Wall","-Wextra","-c",
                   "-DUSE_HAL_DRIVER","-DSTM32F103xE","-DGW_HAL_HAS_UARTEX_IDLE=1") + $incR + @($k,"-o",$obj)
            $r = & $gccArm @a 2>&1
            if ($r) {
                Fail "RTOS: $(Split-Path $k -Leaf) 有告警/错误"
                $r | Select-Object -First 3 | ForEach-Object { Write-Host "        $_" }
                $kBad++
            } elseif (Test-Path $obj) { $kOk++; $kSum += (Get-Item $obj).Length }
            else { $kBad++ }
        }
        if ($kBad -eq 0) {
            Pass "FreeRTOS 内核 + RTOS 层：$kOk 个通过，合计 $kSum 字节"
        } else {
            Fail "FreeRTOS 部分有 $kBad 处问题"
        }

        # ⚠️ 附加：确认三个中断向量**真的**被重定向了（工具链级，不是看文本）
        #
        #    port.c 里定义的是 vPortSVCHandler / xPortPendSVHandler /
        #    xPortSysTickHandler；只有 FreeRTOSConfig.h 里的重定向宏生效，
        #    符号名才会变成 CMSIS 标准名（启动文件的弱符号才会被覆盖）。
        #
        #    ⚠️ 漏了的症状：**第一次任务切换就 HardFault**，而且**链接不报错**
        #       （那几个名字只是没被向量表引用）。手写检查抓不住，nm 一抓就准。
        #    实测（2026-10-11）：有宏 -> port.o 里是 SVC_Handler/PendSV_Handler/
        #       SysTick_Handler；把宏去掉 -> 变回 vPortSVCHandler 三个。
        $portObj = Join-Path $tmp "rtos_port.c.o"
        $nmExe   = Join-Path (Split-Path $gccArm) "arm-none-eabi-nm.exe"
        if ((Test-Path $portObj) -and (Test-Path $nmExe)) {
            $syms = (& $nmExe $portObj 2>&1) -join "`n"
            $mapped = ($syms -match '\bSVC_Handler\b') -and
                      ($syms -match '\bPendSV_Handler\b') -and
                      ($syms -match '\bSysTick_Handler\b')
            $unmapped = $syms -match '\bvPortSVCHandler\b'
            if ($mapped -and -not $unmapped) {
                Pass "三个中断向量已重定向到 CMSIS 标准名（port.o 里就是 SVC_Handler 等）"
            } else {
                Fail "三个中断向量**没有**重定向 —— 症状是第一次任务切换就 HardFault"
                Write-Host "        （FreeRTOSConfig.h 里要有 #define vPortSVCHandler SVC_Handler 等三行）"
            }
        }
    } else {
        Write-Host "  跳过 FreeRTOS（固件包里没有 FreeRTOS 源码）" -ForegroundColor Yellow
    }

    # ---- 附加：模板类型检查 ----
    # ⚠️ 这一段的由来：两个模板（main_template.c / bootloader_main.c）
    #    是给用户复制进 CubeMX 工程的，依赖生成的 main.h，
    #    所以在 CI 里**长期没有被编译过** —— 而它们恰恰最容易写错。
    #    加了这个桩头文件之后，模板每次 CI 都会被真正编译一遍。
    #    加进来当天就抓出了 3 个真实问题（API 少传参数、
    #    io 接口混用、重复注释）。
    $stub = Join-Path $PWD "tools\template_check"
    if (Test-Path $stub) {
        $tInc = $inc + @("-I$stub")
        $tmpls = @(
            @{ n = "main_template.c";     f = "templates\main_template.c";
               d = "GW_HAL_HAS_UARTEX_IDLE=1" },
            @{ n = "bootloader_main.c";   f = "templates\bootloader_main.c";
               d = "GW_HAL_HAS_UARTEX_IDLE=1" },
            # ⚠️ RTOS 变体：同一条 main 的 **RTOS 分支**（GW_USE_FREERTOS=1）
            #    也必须被真正编译过。否则"建任务 + 使能 IWDG + 启动调度器"
            #    那段只存在于文档和模板里、没有任何东西编过它 ——
            #    缺陷 Q（钩子没人接线而无人发现）就是这一类。
            @{ n = "main_template_rtos.c"; f = "templates\main_template.c";
               d = "GW_HAL_HAS_UARTEX_IDLE=1";
               x = @("-DGW_USE_FREERTOS=1") }
        )
        $tOk = 0; $tBad = 0
        foreach ($t in $tmpls) {
            if (-not (Test-Path $t.f)) { continue }
            $obj = Join-Path $tmp ("tmpl_" + $t.n + ".o")
            $a = @("-mcpu=cortex-m3","-mthumb","-std=c99","-Wall","-Wextra","-c",
                   "-DUSE_HAL_DRIVER","-DSTM32F103xE","-D" + $t.d) + $t.x + $tInc + @($t.f,"-o",$obj)
            $r = & $gccArm @a 2>&1
            if ($r) {
                Fail "模板 $($t.n) 有问题"
                $r | Select-Object -First 5 | ForEach-Object { Write-Host "        $_" }
                $tBad++
            } elseif (Test-Path $obj) {
                $tOk++
            } else { $tBad++ }
        }
        if ($tBad -eq 0 -and $tOk -gt 0) {
            Pass "$tOk 个模板通过类型检查（含 CubeMX 句柄与初始化函数的桩声明）"
        } elseif ($tBad -gt 0) {
            Fail "$tBad 个模板有问题"
        }
    }

    if ($badN -eq 0) {
        Pass "$okN 个模块零警告通过，目标文件合计 $sum 字节"
    } else {
        Fail "$badN 个模块有问题"
    }
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
} else {
    Write-Host "  跳过（缺少交叉编译器或 HAL 固件包）" -ForegroundColor Yellow
}

# ------------------------------------------------------------------ 3) cppcheck
Step "静态分析（cppcheck）"
if ($SkipCppcheck) {
    Write-Host "  按参数要求跳过" -ForegroundColor Yellow
} else {
    $cpp = (Get-Command cppcheck -ErrorAction SilentlyContinue).Source
    if (-not $cpp) {
        Write-Host "  跳过（未安装 cppcheck）" -ForegroundColor Yellow
    } else {
        $srcs = Get-ChildItem src,drivers,app -Filter *.c | ForEach-Object { $_.FullName }
        $r = & $cpp --enable=warning,style,performance,portability --std=c99 `
                    --platform=unix64 --suppress=missingIncludeSystem --quiet `
                    -Iinc -Idrivers -Iapp @srcs 2>&1

        $errs = ($r | Select-String -Pattern ": (error|warning):").Count
        $styles = ($r | Select-String -Pattern ": style:").Count

        if ($errs -eq 0) {
            Pass "0 error / 0 warning（另有 $styles 条风格建议，见下）"
        } else {
            Fail "$errs 条 error/warning"
            $r | Select-String -Pattern ": (error|warning):" |
                Select-Object -First 8 | ForEach-Object { Write-Host "        $($_.Line)" }
        }
        if ($styles -gt 0) {
            Write-Host "  风格建议（不阻塞）：" -ForegroundColor DarkGray
            $r | Select-String -Pattern ": style:" | Select-Object -First 12 |
                ForEach-Object { Write-Host ("        " + $_.Line) -ForegroundColor DarkGray }
        }
    }
}

# ------------------------------------------------------------------ 4) .noinit 初始化器检查
Step ".noinit 段初始化器检查"
# ⚠️ 为什么需要这一步：
#    ARM GCC 拒绝 .noinit 里的非零初始化器
#      error: only zero initializers are allowed in section '.noinit'
#    但**主机 gcc 不报这个错** —— 而本项目的 PC 测试全用主机 gcc。
#    所以这个错误只在交叉编译时才暴露，离改动已经很远。
#    本步骤用源码扫描提前发现（定位是"廉价预警"，不替代编译器）。
# ⚠️⚠️ 这里必须是 `do { } while ($false)`，**不能**写成裸的 `{ }`。
#
#     PowerShell 有一个很坑的行为：**脚本顶层的裸块 `{ ... }`
#     不会执行 —— 它被当成字符串字面量输出到管道**。
#     也就是说脚本会"正常跑完、报告全部通过"，而那一段代码
#     一行都没执行。这比语法错误危险得多 —— 语法错误至少会报错。
#
#     我第一次就是写成裸块，结果 CI 输出里直接打印了这段源码，
#     却依然报"CI 全部通过"。
#
#     `do { } while ($false)` 是真正的语句块，执行且只执行一次。
do {
    # 找 Python：PATH -> 常见安装位置 -> DSH 自带运行时
    $py = (Get-Command python -ErrorAction SilentlyContinue).Source
    if (-not $py) { $py = (Get-Command python3 -ErrorAction SilentlyContinue).Source }
    if (-not $py) {
        $cands = @(
            "$env:LOCALAPPDATA\Programs\Python\Python3*\python$EXESUF",
            "C:\Python3*\python$EXESUF",
            "$env:USERPROFILE\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python$EXESUF"
        )
        foreach ($cand in $cands) {
            $hit = Get-ChildItem $cand -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($hit) { $py = $hit.FullName; break }
        }
    }
    if ($py) { Write-Host ("  python: " + $py) }
    $script = Join-Path $PWD "tools\check_noinit_init.py"
    if ($py -and (Test-Path $script)) {
        $r = & $py $script "src" "app" "port" "drivers" "inc" 2>&1
        $r | ForEach-Object { Write-Host "  $_" }
        if ($LASTEXITCODE -ne 0) {
            Fail ".noinit 初始化器检查未通过"
        } else {
            Pass ".noinit 段无非法初始化器"
        }
    } else {
        Write-Host "  跳过（未找到 python 或脚本）" -ForegroundColor Yellow
    }
} while ($false)

# ---------------------------------- 4.5) 引导程序共享层的 HAL 句柄检查
Step "引导程序共享层的 HAL 句柄检查"
# ⚠️ 为什么需要这一步（2026-10-11 缺陷 O：板上 39Hz 复位循环）
#
#    引导程序是**独立构建**的镜像（tools/build_ota_images.ps1），只初始化
#    GPIO 与 USART1；而 `src/` 与 `port/` 是**两个镜像共用**的代码。
#    共用代码里一旦出现 `HAL_xxx(&h...)`，那个句柄在引导程序里就是 BSS 的
#    全 0（`Instance == NULL`）：
#
#        HAL_IWDG_Refresh(&hiwdg)  ->  写地址 0x00000000
#        -> 不精确数据总线错误 (CFSR=0x400) -> HardFault
#        -> 故障处理程序复位 -> 39Hz 死循环，永远走不到"跳转应用"
#
#    ⚠️ 关键教训：`#if defined(HAL_IWDG_MODULE_ENABLED)` 这类**宏开关挡不住它** ——
#       宏只保证"类型存在"，保证不了"句柄被初始化过"。所以这个洞看起来
#       很安全（有开关护着），却只在真跳转时才暴露。
#
#    ⚠️⚠️ 这类缺陷的**现象极像硬件问题**（串口每 25ms 打一次 [BOOT] start、
#        `RCC_CSR` 里 POR/PIN 标志还都立着）—— 我们因此误判成"供电/复位脚问题"
#        白排查了一轮。真正的判据是**读引导程序自己的 `.noinit` 故障记录**
#        （`s_rec`，符号地址要从 `build/ota/bootloader.elf` 里 nm 出来，
#        不能用应用 ELF 的地址）。判读方法见 doc/上板调试交接.md §三·五。
#
#    ⚠️ 这里必须是 `do { } while ($false)`（原因见上面 .noinit 那一步）。
do {
    $script = Join-Path $PWD "tools\check_boot_shared_handles.py"
    if ($py -and (Test-Path $script)) {
        $r = & $py $script $PWD 2>&1
        $r | ForEach-Object { Write-Host "  $_" }
        if ($LASTEXITCODE -ne 0) {
            Fail "引导程序共享层引用了它没有初始化的 HAL 句柄"
        } else {
            Pass "引导程序可达代码无应用专有句柄引用"
        }
    } else {
        Write-Host "  跳过（未找到 python 或脚本）" -ForegroundColor Yellow
    }
} while ($false)

# ---------------------------------- 6.5) RTOS 让出钩子链路检查
Step "RTOS 让出钩子链路检查"
# ⚠️ 为什么需要这一步（2026-10-11 修掉的一个真实静默失效）：
#
#    `gw_ota_port.c` 每擦一页都调 `gw_ota_port_yield()`：裸机侧是**弱符号**
#    空实现，RTOS 侧由 `gw_rtos.c` 给**强定义**（vTaskDelay(1)）覆盖。
#    这条链断掉**不报任何错**：
#
#        OTA 擦除 136 KB（1.4~2.7 秒）期间不让出 CPU
#        -> 空闲任务跑不到 -> 喂狗被跳过 -> IWDG（1.07~2.13 秒）咬
#        -> **升级擦到一半被复位**
#
#    而它只在**真做一次 OTA 时**才暴露：平时串口正常、自检全绿、CI 全绿。
#    ⚠️ 以前那边写的是 `#if defined(GW_OTA_PORT_USE_RTOS)`，而**全工程没有
#       任何地方定义这个宏**、也没有任何强定义 —— 等于那条设计从未生效。
#       现在改成弱符号（RTOS 侧自动覆盖），这份检查守住"别改回去"。
#
#    ⚠️ 这里必须是 `do { } while ($false)`（原因见上面 .noinit 那一步）。
do {
    $script = Join-Path $PWD "tools\check_rtos_hooks.py"
    if ($py -and (Test-Path $script)) {
        $r = & $py $script 2>&1
        $r | ForEach-Object { Write-Host "  $_" }
        if ($LASTEXITCODE -ne 0) {
            Fail "RTOS 让出钩子链路不完整（弱占位 / 强覆盖 / 调用点）"
        } else {
            Pass "RTOS 让出钩子链路完整"
        }
    } else {
        Write-Host "  跳过（未找到 python 或脚本）" -ForegroundColor Yellow
    }
} while ($false)

# ---------------------------------------------- 6.6) FreeRTOS 整镜像链接检查
Step "FreeRTOS 整镜像链接检查（内核 + 调度器核心 + RTOS 层 + 工程 main）"
# ⚠️ 为什么需要这一步：RTOS 落地前有一整类问题**只在链接期暴露** ——
#    缺符号、重复定义、RAM/Flash 装不下。而它**不需要烧板**就能跑。
#
#    ⚠️ 实测抓到过（2026-10-11）：`port.c`（经 FreeRTOSConfig.h 的重定向宏）
#       与工程的 `Core/Src/stm32f1xx_it.c` **都定义了** SVC_Handler /
#       PendSV_Handler / SysTick_Handler -> `multiple definition`。
#       CubeMX 启用 FreeRTOS 时会自动从那三个删掉，迁移指南里有这条落地步骤。
#
#    ⚠️ 它需要 CubeMX 工程（Core/ 与 Drivers/）与 FreeRTOS 内核源码；
#       找不到就跳过（与其它依赖固件包的步骤同样处理）。
#    ⚠️ 必须是 do { } while ($false)（原因见 .noinit 那一步）。
do {
    $cube = $(if ($env:GW_CUBE_PROJECT) { $env:GW_CUBE_PROJECT } else { "D:\stm32cubeIDE\gateway2" })
    $bs   = Join-Path $PWD "tools\build_ota_images.ps1"
    if ((Test-Path (Join-Path $cube "Core\Src\main.c")) -and (Test-Path $bs)) {
        $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $bs -RtosLink -Optimize s 2>&1
        $code2 = $LASTEXITCODE
        $okLine = $out | Select-String -Pattern '\[OK\] app_rtos_link'
        if ($code2 -ne 0 -or -not $okLine) {
            Fail "FreeRTOS 整镜像链接失败"
            $out | Select-Object -Last 10 | ForEach-Object { Write-Host "        $_" }
        } else {
            Pass ("FreeRTOS 整镜像链接通过 -> " + $okLine.Line.Trim())
        }
    } else {
        Write-Host "  跳过（没找到 CubeMX 工程或构建脚本）" -ForegroundColor Yellow
    }
} while ($false)

# ---------------------------------------------- 6.7) FreeRTOS 落地预检
Step "FreeRTOS 落地预检（工程侧：时基 / 内核 / 三个 handler / 宏 / main）"
# ⚠️ 为什么需要它：落地条件分散在**仓库代码、CubeMX 工程、IDE 预处理宏**三处，
#    而"它们互相之间一致不一致"没人管 —— 本阶段已在这些交界处踩到 7 个问题
#    （O~V），且多数**只在链接或上板那一刻**才暴露。
#
#    ⚠️ 它只在发现"**互相矛盾**"时才算失败；"待办"是当前预期状态
#       （FreeRTOS 还没启用），所以这条检查**随时可以跑**，也就能进 CI。
#       例：工程里已加内核源码 + it.c 里三个 handler 还在 -> 必然 multiple definition。
#    ⚠️ 必须是 do { } while ($false)（原因见 .noinit 那一步）。
do {
    $clScript = Join-Path $PWD "tools\check_landing.py"
    $cubeDir  = $(if ($env:GW_CUBE_PROJECT) { $env:GW_CUBE_PROJECT } else { "D:\stm32cubeIDE\gateway2" })
    if ($py -and (Test-Path $clScript)) {
        $r = & $py $clScript --project $cubeDir 2>&1
        $r | ForEach-Object { Write-Host "  $_" }
        if ($LASTEXITCODE -ne 0) {
            Fail "FreeRTOS 落地预检发现互相矛盾的状态（见上面 [矛盾] 行）"
        } else {
            Pass "FreeRTOS 落地预检通过（工程侧无矛盾）"
        }

        # ★ 落地预演：把"生产模板 + 工程 CubeMX 尾巴 + 工程声明块"真的拼一次，
        #   并**真编一遍**（-Wall 要求 0 诊断）。
        #   ⚠️ 为什么必须机械化：2026-10-11 手工预演时，拼出来的第一版直接报
        #      'huart1 undeclared' 与 'implicit declaration of MX_GPIO_Init' —
        #      两处缺漏**只有真编才暴露**，读文档永远看不出来。做成工具后，
        #      模板或工程任何一侧漂移都会在 CI 当场暴露。
        $mkScript = Join-Path $PWD "tools\make_landing_main.py"
        if (Test-Path $mkScript) {
            $r = & $py $mkScript --project $cubeDir --check --verify-compile 2>&1
            $r | Select-Object -Last 2 | ForEach-Object { Write-Host "  $_" }
            if ($LASTEXITCODE -ne 0) {
                Fail "落地预演失败（生成的 main.c 前置条件或编译检查没过）"
            } else {
                Pass "落地预演通过（拼出的 main.c 编译 0 诊断）"
            }
        }
    } else {
        Write-Host "  跳过（未找到 python 或脚本）" -ForegroundColor Yellow
    }
} while ($false)

# ---------------------------------------------- 6.7b) 落地预检的"自证"
Step "落地预检自证（合成工程：7 种坏/好状态 —— 内务 / 主循环 / CubeMX 默认配置 / L0 版 main / 骨架 main / freertos.c 抢钩子）"
# ⚠️ 为什么连"检查的检查"也要进 CI：
#    本项目的纪律是**证据可复现** —— 一条检查如果只在某天本地跑过一次，
#    下一个人无法确认它到底能不能报错（"永远返回 0 的检查"是最坏的情况）。
#    这个脚本搭三种合成工程，验证 check_landing 的内务/主循环两项
#    确实会在这两种坏状态下报 [矛盾]。约 3 秒。
do {
    $spScript = Join-Path $PWD "tools\selfproof_landing.py"
    if ($py -and (Test-Path $spScript)) {
        $r = & $py $spScript 2>&1
        $r | Select-Object -Last 1 | ForEach-Object { Write-Host "  $_" }
        if ($LASTEXITCODE -ne 0) {
            Fail "落地预检自证未通过（检查本身可能失灵了）"
        } else {
            Pass "落地预检自证通过（7/7：含缺陷 R/S 回归、L0 版 / 骨架 main、freertos.c 抢钩子）"
        }
    } else {
        Write-Host "  跳过（未找到 python 或脚本）" -ForegroundColor Yellow
    }
} while ($false)

# ------------------------------------------------------------------ 5) BOM 检查
Step "源文件 BOM 检查"
# ⚠️ 为什么需要这一步：
#    UTF-8 BOM 在 .c/.h 里是有害的（GCC 会把它当内容，报错行列号偏移）。
#    但 .ps1 **需要** BOM（PS 5.1 无 BOM 时按 ANSI 解码，中文乱码）。
#    这种"按文件类型要求相反"的规则很容易出错 ——
#    我用 PowerShell 恢复一个 .c 文件时就给它带进了 BOM，
#    而且一连带进了 10 个文件都没发现。
do {
    $script = Join-Path $PWD "tools\check_bom.py"
    if ($py -and (Test-Path $script)) {
        $r = & $py $script 2>&1
        $r | ForEach-Object { Write-Host "  $_" }
        if ($LASTEXITCODE -ne 0) { Fail "BOM 检查未通过" }
        else { Pass "BOM 检查正常" }
    } else {
        Write-Host "  跳过（未找到 python 或脚本）" -ForegroundColor Yellow
    }
} while ($false)

# ------------------------------------------- 5.5) 工具脚本的控制台编码检查
# ---------------------------------------------- 12.0) 工具脚本编译检查
Step "工具脚本编译检查（语法错误当场暴露）"
# ⚠️ 为什么需要它：编码检查那一步只**静态检查** print 有没有守卫，
#    **不执行**脚本 —— 于是语法错的脚本能一路混到"有人真去跑它"。
#    而我在同一天里第 5 次犯了同一个错（Python 双引号串里又写 ASCII 双引号）。
do {
    $ckScript = Join-Path $PWD "tools\check_tools_syntax.py"
    if ($py -and (Test-Path $ckScript)) {
        $r = & $py $ckScript 2>&1
        $r | ForEach-Object { Write-Host "  $_" }
        if ($LASTEXITCODE -ne 0) {
            Fail "有工具脚本编译不过（见上面 [FAIL] 行）"
        } else {
            Pass "工具脚本全部能编译"
        }
    } else {
        Write-Host "  跳过（未找到 python 或脚本）" -ForegroundColor Yellow
    }
} while ($false)

Step "工具脚本的控制台编码检查"
# ⚠️ 为什么需要这一步（2026-10-10 的真实事故）：
#    两个上板验证脚本（pc_slave_sim.py / verify_master_dual.py）在
#    **默认中文控制台**里打印第一个 ⚠️ 就 UnicodeEncodeError 死掉，
#    一行 Modbus 都没发出去，表现成"PC 从机没启动 / 验证没做"，
#    白查了一整天。
#    ⚠️ 本文件上面设了 PYTHONIOENCODING=utf-8，所以 CI 里**永远复现不了**。
#       这类"只在用户手敲时才出现"的缺陷只能靠源文本检查。
#    它与上面一步（BOM 检查）是同一类：都是"源文件文本层面"的检查。
do {
    $script = Join-Path $PWD "tools\check_console_encoding.py"
    if ($py -and (Test-Path $script)) {
        $r = & $py $script 2>&1
        $r | ForEach-Object { Write-Host "  $_" }
        if ($LASTEXITCODE -ne 0) { Fail "工具脚本在 GBK 控制台会崩" }
        else { Pass "工具脚本都能在 GBK 控制台正常跑" }
    } else {
        Write-Host "  跳过（未找到 python 或脚本）" -ForegroundColor Yellow
    }
} while ($false)

Step "OneNET 工具自检（token 算法 / MQTT 报文编码 / 物模型合并）"
# ⚠️ 为什么需要它：这三个工具的自检**全部离线**（不连平台、不需要凭据），
#    所以能进 CI。而它们的失效方式很隐蔽：
#      - token 算法错一位 → 现象是 rc=4「鉴权失败」，看起来像密钥不对
#      - MQTT 剩余长度编码错 → 现象是"连不上"，看起来像网络问题
#      - 物模型合并丢字段 → 现象是导入被拒，且报错含糊
#    三者的共同点是"错在本地、怪在平台"。
do {
    $onenetTools = @(
        @{ f = "tools\onenet_token.py";     what = "token 生成（含 openssl 交叉验证向量）" },
        @{ f = "tools\onenet_probe.py";     what = "MQTT 报文编码 + token 向量" },
        @{ f = "tools\merge_onenet_tsl.py"; what = "物模型合并（10 组断言）" }
    )
    $onenetFail = 0
    foreach ($t in $onenetTools) {
        $tp = Join-Path $PWD $t.f
        if ($py -and (Test-Path $tp)) {
            $r = & $py $tp --selftest 2>&1
            $r | ForEach-Object { Write-Host "  $_" }
            if ($LASTEXITCODE -ne 0) { $onenetFail++ }
        } else {
            Write-Host "  跳过 $($t.f)（未找到 python 或脚本）" -ForegroundColor Yellow
        }
    }
    if ($onenetFail -gt 0) {
        Fail "OneNET 工具有自检未通过（见上面 [FAIL] 行）"
    } else {
        Pass "OneNET 工具自检全部通过"
    }
} while ($false)

Step "OneNET 物模型属性标识符一致性（固件宏 / 探针 / TSL / 合并文件）"
# ⚠️ 为什么需要它：标识符在这 4 处各写了一遍，任何一处拼错，
#    现象**完全一样** —— 平台回 {"code":2306,"identifier not exist"}，
#    而且**一次只报一个**。没有这个比对，就得手工逐字核对 4 份表，
#    或者来回十几轮"改一个试一次"。
#    它还兜住"宏定义了但没接上"（有人把宏写回字符串字面量）——
#    这类漂移宏自己不会报错，最难看。
do {
    $idsScript = Join-Path $PWD "tools\check_onenet_ids.py"
    if ($py -and (Test-Path $idsScript)) {
        $r = & $py $idsScript 2>&1
        $r | ForEach-Object { Write-Host "  $_" }
        if ($LASTEXITCODE -ne 0) {
            Fail "OneNET 标识符在固件/探针/TSL/物模型之间不一致"
        } else {
            Pass "OneNET 标识符 4 处一致"
        }
    } else {
        Write-Host "  跳过（未找到 python 或脚本）" -ForegroundColor Yellow
    }
} while ($false)

Step "外部依赖检查（deps.lock 钉死的 LVGL / FreeRTOS）"
# ⚠️ 为什么需要它：本仓库刻意**不 vendoring** 第三方源码（LVGL 走 -LvglDir、
#    FreeRTOS 走 -Freertos，都是外部路径）。代价是"干净克隆构建不了 LVGL 变体"，
#    而冻结基线（72h 浸泡）用的恰好就是 LVGL 版 ——
#    也就是说，**依赖版本漂了，冻结基线的 SHA256 就失去意义**。
#    没有这一步时的现象是 build_ota_images.ps1 抛一句"找不到 LVGL 目录"，
#    既不告诉你要哪个版本、也不告诉你放哪。
#    用 --fast：完整模式要遍历 325 个文件算哈希，那是手工排查用的，不该进每次 CI。
do {
    $depsScript = Join-Path $PWD "tools\check_deps.py"
    if ($py -and (Test-Path $depsScript)) {
        $r = & $py $depsScript --fast 2>&1
        $r | ForEach-Object { Write-Host "  $_" }
        if ($LASTEXITCODE -ne 0) {
            Fail "外部依赖缺失或版本与 deps.lock 不符"
        } else {
            Pass "外部依赖与 deps.lock 一致"
        }
    } else {
        Write-Host "  跳过（未找到 python 或脚本）" -ForegroundColor Yellow
    }
} while ($false)

# ------------------------------------------------------------------ 6) API 覆盖检查
Step "纯逻辑模块的 API 覆盖检查"
# ⚠️ 为什么需要这一步：
#    "测试数量多"不等于"覆盖全"。本步骤是在本项目里真正抓到过东西的 ——
#    它一次扫出 6 个公开 API 一个测试都没引用过，其中
#    mb_master_current_index 既没调用方也没测试、
#    gw_boot_err_str 被 Bootloader 用来打印失败原因却没测。
#    这类问题单看测试数字发现不了。
#
# ⚠️ 检查器本身也有过一次漏洞：它原先搜"测试源码的原始文本"，
#    于是**注释里提到函数名**就算"被引用"。故障注入才发现
#    （把测试里的调用改名后仍报通过）。现在先剥注释再搜。
do {
    $script = Join-Path $PWD "tools\check_api_coverage.py"
    if ($py -and (Test-Path $script)) {
        $r = & $py $script 2>&1
        $r | ForEach-Object { Write-Host "  $_" }
        if ($LASTEXITCODE -ne 0) { Fail "API 覆盖检查未通过" }
        else { Pass "所有公开 API 都有测试引用" }
    } else {
        Write-Host "  跳过（未找到 python 或脚本）" -ForegroundColor Yellow
    }
} while ($false)

# ------------------------------------------------------------------ 7) 文档-代码一致性
Step "文档与代码的硬件配置一致性"
# ⚠️ 为什么需要这一步：
#    本项目交付的是「源码 + 文档」，用户照着文档在 CubeMX 里配外设。
#    文档与代码不一致的后果是**静默的**：
#      gw_config.h 里的引脚宏只用于代码内部（DE 控制、DHT22 读取），
#      **不会去改 CubeMX 生成的 UART 引脚配置** ——
#      于是代码以为在用某个引脚，硬件实际是另一个，
#      现象是"串口完全没数据"，而两边看起来都对。
#    这种错误编译器查不出、单元测试也查不出（都在 PC 上跑，不碰真引脚）。
do {
    $script = Join-Path $PWD "tools\check_doc_config.py"
    if ($py -and (Test-Path $script)) {
        $r = & $py $script 2>&1
        $r | ForEach-Object { Write-Host "  $_" }
        if ($LASTEXITCODE -ne 0) { Fail "文档与代码的配置不一致" }
        else { Pass "文档与代码的配置一致" }
    } else {
        Write-Host "  跳过（未找到 python 或脚本）" -ForegroundColor Yellow
    }
} while ($false)

# ------------------------------------------------------------------ 8) 端到端字节流验证
Step "端到端 transcript 独立验证"
# ⚠️ 为什么需要这一步：
#    项目已有 2200+ 项测试，但集成测试是**"代码和自己比"** ——
#    期望字节是 C 代码自己算的。那只能证明"代码前后一致"，
#    **不能证明"发出的字节符合 Modbus 规范"**。
#
#    本步骤把验证完全交给外部：
#      C 侧只跑场景 + dump 字节流（不做断言）
#      Python 侧用**独立实现**的 CRC 与规范知识逐帧核对
#
#    Python 的 CRC16/CRC8 是逐位算法（C 侧可能用查表），
#    方法不同才能交叉验证，而不是"两边抄一遍"。
do {
    $scen = Join-Path $PWD "tools\e2e_scenario$EXESUF"
    $ver  = Join-Path $PWD "tools\e2e_verify.py"

    if (-not (Test-Path $scen)) {
        # 先编出来（它依赖 13 个模块）
        $scenSrc = Join-Path $PWD "tools\e2e_scenario.c"
        $mods = @("src\modbus_crc.c","src\modbus_rtu.c","src\modbus_master.c",
                  "src\gw_pipeline.c","src\gw_frame.c","src\gw_uplink.c",
                  "src\gw_can_core.c","src\gw_timing.c","src\gw_sched.c",
                  "src\gw_cmd.c","src\gw_devinfo.c","drivers\dht22.c",
                  "app\gw_app.c") | ForEach-Object { Join-Path $PWD $_ }
        $a = @("-std=c99","-Wall","-Wextra","-Iinc","-Idrivers","-Iapp",
               "-o", $scen, $scenSrc) + $mods
        $b = & $gccHost @a 2>&1
        if ($b) {
            Fail "端到端场景编译失败"
            $b | Select-Object -First 4 | ForEach-Object { Write-Host "        $_" }
        }
    }

    if ((Test-Path $scen) -and $py -and (Test-Path $ver)) {
        $trans = Join-Path $env:TEMP "gw_e2e_transcript.txt"
        & $scen 2>&1 | Out-File -FilePath $trans -Encoding UTF8
        $r = & $py $ver $trans 2>&1
        $r | Where-Object { $_ -notmatch "^\s*\[info\]" } | ForEach-Object {
            Write-Host "  $_"
        }
        if ($LASTEXITCODE -ne 0) { Fail "端到端验证未通过" }
        else { Pass "端到端字节流符合 Modbus 规范" }
    } else {
        Write-Host "  跳过（未找到 python 或脚本）" -ForegroundColor Yellow
    }
} while ($false)

# ------------------------------------------------------------------ 9) 交互式从站边界测试
Step "交互式 Modbus 从站边界测试（Python 当活的主站）"
# ⚠️ 与第 9 步（transcript 验证）的分工：
#    transcript 是**固定剧本**，能验证"规范符合性"，
#    但证明不了"没被写进剧本的边界"。
#    本步骤是**交互式**的：Python 逐条出题、逐条核对，能做到剧本做不到的事：
#      - 主动构造任意畸形帧
#      - **确认"不该应答时确实没有字节"**（要等超时；剧本只能数响应总数，
#        而多一个少一个可能互相抵消）
#      - 边界的数量值（1/125/126/0/65535）
#      - 快速连发做压力
do {
    $srv  = Join-Path $PWD "tools\e2e_slave_server$EXESUF"
    $mst  = Join-Path $PWD "tools\e2e_master.py"
    $srvSrc = Join-Path $PWD "tools\e2e_slave_server.c"

    if (-not (Test-Path $srv)) {
        $mods = @("src\modbus_crc.c","src\modbus_rtu.c") |
                ForEach-Object { Join-Path $PWD $_ }
        $a = @("-std=c99","-Wall","-Wextra","-Iinc","-o",$srv,$srvSrc) + $mods
        $b = & $gccHost @a 2>&1
        if ($b) {
            Fail "交互式从站服务端编译失败"
            $b | Select-Object -First 4 | ForEach-Object { Write-Host "        $_" }
        }
    }

    if ((Test-Path $srv) -and $py -and (Test-Path $mst)) {
        $r = & $py $mst $srv 2>&1
        $r | Where-Object { $_ -notmatch "^\s*\[info\]" } | ForEach-Object {
            Write-Host "  $_"
        }
        if ($LASTEXITCODE -ne 0) { Fail "交互式边界测试未通过" }
        else { Pass "从站的边界与静默行为符合规范" }
    } else {
        Write-Host "  跳过（未找到 python 或脚本）" -ForegroundColor Yellow
    }
} while ($false)

# ------------------------------------------------------------------ 11) OTA 断电穷举
Step "OTA 断电穷举（每一个 Flash 操作后断电）"
# ⚠️ 为什么需要这一步：
#    test_ota.c 里有一个断电测试，但它测的是**一个硬编码的时刻**。
#    不同时刻的后果完全不同：
#      断电在擦元数据页之后、写之前 -> magic 失效 -> 走兜底
#      断电在写元数据中途           -> CRC 失效 -> 也走兜底
#      断电在写完元数据之后         -> pending=1 -> 重试若干次后回滚
#    只测一个点，另外两条就是"没验证过"。
#
#    本步骤穷举**每一个**受影响的写/擦操作（26 个点），
#    每点都单独跑一次完整升级并在该点断电，然后验证：
#      设备必须能启动（含 Bootloader 的兜底路径）
#      最终分区号必须合法
#      boot_tries 必须有界
do {
    $exe = Join-Path $PWD "tools\powerloss_ota$EXESUF"
    $src = Join-Path $PWD "tools\powerloss_ota.c"
    if (-not (Test-Path $exe) -or -not (Test-Path $exe)) {   # 两边的条件相同是笔误，保留不影响正确性
        $mods = @("src\gw_crc32.c","src\gw_ota.c") |
                ForEach-Object { Join-Path $PWD $_ }
        $a = @("-std=c99","-Wall","-Wextra","-Iinc","-Iport",
               "-o",$exe,$src) + $mods
        $b = & $gccHost @a 2>&1
        if ($b) {
            Fail "OTA 断电穷举程序编译失败"
            $b | Select-Object -First 4 | ForEach-Object { Write-Host "        $_" }
        }
    }
    if (Test-Path $exe) {
        $r = & $exe 2>&1
        $r | Where-Object { $_ -match "穷举完成|回滚发生|FAIL|通过 |基准|共 \d+ 次" } |
            ForEach-Object { Write-Host "  $_" }
        if ($LASTEXITCODE -ne 0) { Fail "OTA 断电穷举未通过" }
        else { Pass "任意时刻断电都不会变砖" }
    }
} while ($false)

# ------------------------------------------------------------------ 12) 参数断电穷举
Step "参数保存断电穷举（半字粒度）"
# ⚠️ 为什么需要这一步：
#    test_param.c 的掉电测试只注入**前 3 次**写失败，
#    而一次 gw_param_save 有 **137 次**半字写（9 头部 + 128 数据）。
#    第 4 次之后的断电点从未被验证过。
#
#    ⚠️ 而且粒度很关键：本工具第一版把"一次 write 调用"当成原子操作，
#       只有 2 个断电点。但 F1 的编程单位是**半字**，
#       一次 18 字节的头部写入在真机上是 9 次独立编程 ——
#       改到半字粒度后断电点从 5 个变成 275 个。
#
#    ⚠️⚠️ 这个工具抓到了一个**真实缺陷**：
#       pick_target_slot 原本 `continue` 跳过损坏槽，
#       而"写中途掉电"会让一个槽变损坏。磨损均衡依次用下一个槽，
#       所以**每次掉电消耗一个槽** —— 8 次连续掉电之后就只剩
#       1 个有效槽，退化分支会**擦掉它**，参数永久丢失。
#       修复：复用损坏槽（按 seq 最小选，延续磨损均衡）。
do {
    $exe = Join-Path $PWD "tools\powerloss_param$EXESUF"
    $src = Join-Path $PWD "tools\powerloss_param.c"
    if (-not (Test-Path $exe)) {
        $pm = Join-Path $PWD "src\gw_param.c"
        $a = @("-std=c99","-Wall","-Wextra","-Iinc","-Iport","-o",$exe,$src,$pm)
        $b = & $gccHost @a 2>&1
        if ($b) {
            Fail "参数断电穷举程序编译失败"
            $b | Select-Object -First 8 | ForEach-Object { Write-Host "        $_" }
        }
    }
    if (Test-Path $exe) {
        $r = & $exe 2>&1
        $r | Where-Object { $_ -match "checked|power-loss points|FAIL|passed|consecutive" } |
            ForEach-Object { Write-Host "  $_" }
        if ($LASTEXITCODE -ne 0) { Fail "参数断电穷举未通过" }
        else { Pass "任意时刻断电都不会丢参数" }
    }
} while ($false)

# ------------------------------------------------------------------ 汇总
Write-Host ""
Write-Host ("=" * 66) -ForegroundColor Cyan
if ($script:failed -eq 0) {
    Write-Host "  CI 全部通过" -ForegroundColor Green
} else {
    Write-Host ("  CI 失败：$script:failed 处问题") -ForegroundColor Red
}
Write-Host ("=" * 66) -ForegroundColor Cyan
exit $(if ($script:failed -eq 0) { 0 } else { 1 })
