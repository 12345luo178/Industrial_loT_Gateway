# ============================================================================
#  ESP32 侧构建脚本（gateway_bridge）—— 2026-10-14 调通时固化
#
#  背景：D:\Espressif\esp-idf 是**浅克隆 + gitee 镜像**，装的时候：
#    · python 环境装到了 D:\Espressif\tools\python_env\idf5.5_py3.13_env（不是默认的
#      C:\Users\acer\.espressif\...\idf5.5_py3.14_env）=> 必须显式指定 IDF_TOOLS_PATH
#    · 一批子模块（esp-mqtt / mbedtls / tlsf / lwip / cJSON / spiffs / unity …）没检出
#      => 必须 `git submodule update --init --recursive`
#    · export.bat 因为 python 版本对不上而失败 => 本脚本直接调用环境里的 python.exe
#
#  用法：  powershell -File tools\esp32_build.ps1            # 构建
#          powershell -File tools\esp32_build.ps1 -Clean     # 清理后构建
#          powershell -File tools\esp32_build.ps1 -Flash COM5  # 构建并烧录+监视
# ============================================================================
param(
    [switch]$Clean,
    [string]$Flash = ""
)

$ErrorActionPreference = "Continue"

# ---- 环境（这几个值就是"装在哪"的事实，改机器时只需改这里）----
$IDF            = "D:\Espressif\esp-idf"
$IDF_TOOLS      = "D:\Espressif\tools"
$PY_ENV         = "$IDF_TOOLS\python_env\idf5.5_py3.13_env"
$ROM_ELF_DIR    = "$IDF_TOOLS\tools\esp-rom-elfs\20241011"
$XTENSA_BIN     = "$IDF_TOOLS\tools\xtensa-esp-elf\esp-14.2.0_20241119\xtensa-esp-elf\bin"
$Proj           = Join-Path $PSScriptRoot "..\esp32"

$env:IDF_PATH            = $IDF
$env:IDF_TOOLS_PATH      = $IDF_TOOLS
$env:IDF_PYTHON_ENV_PATH = $PY_ENV
$env:ESP_ROM_ELF_DIR     = $ROM_ELF_DIR
$env:PATH                = "$XTENSA_BIN;$env:PATH"

# ---- 前置检查：缺什么就明确报出来，而不是让 CMake 抛一串看不懂的错 ----
$problems = @()
if (-not (Test-Path "$PY_ENV\Scripts\python.exe")) { $problems += "python 环境不存在: $PY_ENV（需要跑 $IDF\install.bat，并把 IDF_TOOLS_PATH 设成 $IDF_TOOLS）" }
if (-not (Test-Path "$XTENSA_BIN\xtensa-esp32-elf-gcc.exe")) { $problems += "xtensa 工具链不存在: $XTENSA_BIN" }
if (-not (Test-Path "$ROM_ELF_DIR")) { $problems += "ROM ELF 目录不存在: $ROM_ELF_DIR" }
foreach ($m in @("components/mqtt/esp-mqtt", "components/mbedtls/mbedtls", "components/heap/tlsf")) {
    $p = Join-Path $IDF ($m -replace '/', '\')
    if (-not (Test-Path $p) -or -not (Get-ChildItem $p -Force -ErrorAction SilentlyContinue)) {
        $problems += "子模块没检出: $m  => 修法: git -C `"$IDF`" submodule update --init --recursive $m"
    }
}
if ($problems.Count -gt 0) {
    Write-Host "[环境不完整]" -ForegroundColor Red
    $problems | ForEach-Object { Write-Host "  - $_" }
    exit 2
}

$py  = "$PY_ENV\Scripts\python.exe"
$idfpy = "$IDF\tools\idf.py"

Write-Host "[1/3] ESP-IDF 环境已就绪（$IDF）" -ForegroundColor Green
if ($Clean) {
    Write-Host "[2/3] 清理 build/ …"
    & $py $idfpy -C $Proj fullclean 2>&1 | Select-Object -Last 3
}

Write-Host "[2/3] 构建 gateway_bridge …"
& $py $idfpy -C $Proj build 2>&1 | Tee-Object -Variable out | Select-Object -Last 20
$rc = $LASTEXITCODE

# 构建失败时把真正的错误行捞出来（tail 里通常只剩调用栈）
if ($rc -ne 0) {
    Write-Host ""
    Write-Host "[构建失败] 关键错误行：" -ForegroundColor Red
    $out | Select-String -Pattern ': error:|CMake Error|undefined reference|fatal error' |
        Select-Object -First 15 | ForEach-Object { Write-Host "  $($_.Line.Trim())" -ForegroundColor Red }
    Write-Host ""
    Write-Host "  完整日志：$(Join-Path $Proj 'build\log\')"
    exit $rc
}

Write-Host "[3/3] 构建成功 ✓ 产物：" -ForegroundColor Green
foreach ($f in @("build\gateway_bridge.bin", "build\bootloader\bootloader.bin", "build\partition_table\partition-table.bin")) {
    $full = Join-Path $Proj $f
    if (Test-Path $full) {
        "{0,-46} {1,9:N0} B" -f $f, (Get-Item $full).Length | Write-Host
    }
}

if ($Flash -ne "") {
    Write-Host "烧录到 $Flash 并打开监视（Ctrl+] 退出）…"
    & $py $idfpy -C $Proj -p $Flash flash monitor
}
