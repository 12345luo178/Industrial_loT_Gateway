# -*- coding: utf-8 -*-
<#
.SYNOPSIS
    按 deps.lock 钉死的版本，把外部构建依赖取到本机。

.DESCRIPTION
    本仓库**刻意不 vendoring** 第三方源码（见 deps.lock 开头的说明）。
    代价是"干净克隆缺依赖、构建 LVGL 变体直接失败"。这个脚本就是补这一步：
    下载 -> 校验 SHA256 -> 只保留需要的路径 -> 落盘 -> 记录精简树的聚合哈希。

    设计原则与仓库里其它工具一致：
      * 校验不通过就**失败**，绝不"先用了再说"（同 check_*.py 的风格）
      * 每一步都打印"做了什么、拿到了什么"，不静默改本机状态
      * 离线也能用：-CacheDir 复用已下载的发布包

.PARAMETER Dep
    要处理的依赖名（可重复）：lvgl / freertos。默认两个都处理。

.PARAMETER LvglDir
    LVGL 落盘目录，覆盖 deps.lock 里的 dest。

.PARAMETER CacheDir
    发布包缓存目录（默认 C:\Users\<你>\Downloads）。放对了就跳过下载。

.PARAMETER Offline
    只用缓存，缺包就失败，不联网。

.PARAMETER UpdateHashes
    把算出来的 sha256 / src_sha256 写回 deps.lock（首次钉版本、或刻意换版本时用）。

.PARAMETER Force
    目标目录已存在也重新拉取覆盖（默认存在就只校验、跳过下载）。

.EXAMPLE
    pwsh -File tools\fetch_deps.ps1
    按 deps.lock 取全部依赖；已存在的只校验不重下。

.EXAMPLE
    pwsh -File tools\fetch_deps.ps1 -Dep lvgl -Force -UpdateHashes
    强制重取 LVGL，并把新哈希写回 deps.lock。

.NOTES
    ⚠️ 本文件**必须带 UTF-8 BOM**：PS 5.1 无 BOM 时按 ANSI 解码，中文注释会乱码。
       tools/check_bom.py 会检查这一点，别把它去掉。
#>
[CmdletBinding()]
param(
    [string[]]$Dep = @(),
    [string]$LvglDir = "",
    [string]$CacheDir = "",
    [switch]$Offline,
    [switch]$UpdateHashes,
    [switch]$Force
)

$ErrorActionPreference = "Stop"

# ---- 路径：脚本在 tools\ 下，仓库根是它的上级 ----
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Split-Path -Parent $ScriptDir
$LockPath  = Join-Path $RepoRoot "deps.lock"

if (-not (Test-Path $LockPath)) {
    throw "找不到 deps.lock：$LockPath"
}
if ($CacheDir -eq "") {
    $CacheDir = Join-Path $env:USERPROFILE "Downloads"
}

# ============================================================
#  deps.lock 解析（极简 INI：[section] + key = value）
# ============================================================
function Read-DepsLock {
    param([string]$Path)

    $sections = @{}
    $current  = $null
    $n = 0

    foreach ($raw in (Get-Content $Path -Encoding UTF8)) {
        $n++
        $line = $raw.Trim()
        if ($line -eq "" -or $line.StartsWith("#") -or $line.StartsWith(";")) { continue }

        if ($line -match '^\[(.+)\]$') {
            $current = $Matches[1].Trim()
            if (-not $sections.ContainsKey($current)) { $sections[$current] = @{} }
            continue
        }
        $eq = $line.IndexOf("=")
        if ($eq -lt 1) {
            Write-Warning ("deps.lock 第 {0} 行看不懂，已跳过：{1}" -f $n, $line)
            continue
        }
        if ($null -eq $current) {
            Write-Warning ("deps.lock 第 {0} 行不在任何 [节] 里，已跳过：{1}" -f $n, $line)
            continue
        }
        $key = $line.Substring(0, $eq).Trim()
        $val = $line.Substring($eq + 1).Trim()
        $sections[$current][$key] = $val
    }
    return $sections
}

# ============================================================
#  哈希
# ============================================================
function Get-FileSha256 {
    param([string]$Path)
    return (Get-FileHash -Path $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

<#
    源码树聚合哈希：逐文件 SHA256，按"相对路径(小写、正斜杠)"排序后拼起来再哈希一次。
    这样得到的是一个**与文件系统枚举顺序无关**的稳定值：
      * 换个盘符、换个时间解压，结果一样
      * 少一个文件 / 多一个文件 / 改一个字节，结果都会变
#>
function Get-TreeSha256 {
    param([string]$Root, [string[]]$Keep)

    if (-not (Test-Path $Root)) { throw "目录不存在：$Root" }

    $rootFull = (Resolve-Path $Root).Path.TrimEnd("\")
    $entries  = New-Object System.Collections.Generic.List[string]

    foreach ($k in $Keep) {
        $rel = $k.TrimEnd("/")
        $full = Join-Path $rootFull ($rel -replace "/", "\")

        if (Test-Path $full -PathType Leaf) {
            $relPath = $rel
            $h = Get-FileSha256 $full
            $entries.Add(("{0}`n{1}" -f $relPath.ToLowerInvariant(), $h))
        } elseif (Test-Path $full -PathType Container) {
            $files = Get-ChildItem -Path $full -Recurse -File | Sort-Object FullName
            foreach ($f in $files) {
                $relPath = $f.FullName.Substring($rootFull.Length + 1).Replace("\", "/")
                $h = Get-FileSha256 $f.FullName
                $entries.Add(("{0}`n{1}" -f $relPath.ToLowerInvariant(), $h))
            }
        } else {
            throw ("keep 里声明的路径在源码树里不存在：{0}（看 {1}）" -f $rel, $rootFull)
        }
    }

    $joined = [string]::Join("`n", $entries)
    $sha    = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($joined)
        $hash  = $sha.ComputeHash($bytes)
        return @{
            Hash  = (($hash | ForEach-Object { $_.ToString("x2") }) -join "")
            Count = $entries.Count
        }
    } finally {
        $sha.Dispose()
    }
}

# ============================================================
#  下载 / 解压
# ============================================================
function Get-Archive {
    param([string]$Url, [string]$FileName, [string]$Sha256)

    if (-not (Test-Path $CacheDir)) {
        New-Item -ItemType Directory -Path $CacheDir -Force | Out-Null
    }
    $cached = Join-Path $CacheDir $FileName

    if (Test-Path $cached) {
        $got = Get-FileSha256 $cached
        if ($Sha256 -ne "" -and $got -ne $Sha256.ToLowerInvariant()) {
            Write-Host ("  [警告] 缓存里的包哈希对不上，准备重新下载") -ForegroundColor Yellow
            Write-Host ("         期望 {0}" -f $Sha256.ToLowerInvariant())
            Write-Host ("         实际 {0}" -f $got)
            Remove-Item $cached -Force
        } else {
            Write-Host ("  [缓存] 命中 {0}（{1:N1} MB）" -f $cached, ((Get-Item $cached).Length / 1MB))
            return $cached
        }
    }

    if ($Offline) {
        throw ("离线模式且缓存里没有：{0}（放到 {1} 再来）" -f $FileName, $CacheDir)
    }

    Write-Host ("  [下载] {0}" -f $Url)
    $tmp = "$cached.part"
    # ⚠️ 老版本 PowerShell 的 Invoke-WebRequest 默认走 IE 引擎、极慢；
    #    用 -UseBasicParsing 并显式给 TLS 1.2（GitHub 会拒 TLS 1.0）
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    Invoke-WebRequest -Uri $Url -OutFile $tmp -UseBasicParsing
    $sw.Stop()
    Move-Item -Path $tmp -Destination $cached -Force
    Write-Host ("  [完成] {0:N1} MB / {1:N1} s" -f ((Get-Item $cached).Length / 1MB), $sw.Elapsed.TotalSeconds)

    return $cached
}

<#
    解压到临时目录，再按 keep 挑文件拷到目标。
    ⚠️ 用 tar 而不是 Expand-Archive：PS 5.1 的 Expand-Archive 对
       GitHub release 包里某些文件名/深路径有兼容问题，且慢得多。
#>
function Expand-KeepOnly {
    param([string]$Archive, [string[]]$Keep, [string]$DestDir)

    $scratch = Join-Path ([System.IO.Path]::GetTempPath()) ("fetchdeps_" + [Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $scratch -Force | Out-Null

    try {
        Write-Host ("  [解压] -> 临时目录（整包解开，随后只拷 keep 内的路径）")
        <#
            ⚠️ 两个踩过的坑，别改回去：

            1) **不要**在 tar 后面加路径过滤参数（`lvgl.h src ...`）。
               Windows 自带的 bsdtar 会报 `lvgl.h: Not found in archive` 并返回非零
               —— 实测过，路径过滤不可靠。整包解开再挑文件，多花的是**临时磁盘**，
               不是时间（解 24 MB 的包是秒级）。

            2) **必须**把 $ErrorActionPreference 临时调回 Continue：
               本脚本开头设的 `Stop` 会把原生命令写到 **stderr 的任何一行**都变成
               终止性错误 —— 而 tar 正常工作时也会往 stderr 写东西。所以在调用点
               局部放开，再靠 $LASTEXITCODE 判成败。
        #>
        $savedEap = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            $tarArgs = @("-x", "-f", $Archive, "-C", $scratch)
            $tarOut  = & tar @tarArgs 2>&1
            $tarCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $savedEap
        }
        if ($tarCode -ne 0) {
            $tail = @($tarOut | Select-Object -Last 8) -join "`n"
            throw ("tar 解压失败（exit {0}）：{1}`n{2}" -f $tarCode, $Archive, $tail)
        }

        # 包里通常有一层根目录（lvgl-release-v8.2/），也可能没有
        $probe = Join-Path $scratch ($Keep[0].TrimEnd("/") -replace "/", "\")
        $root  = $scratch
        if (-not (Test-Path $probe)) {
            $subs = @(Get-ChildItem $scratch -Directory)
            if ($subs.Count -eq 1) { $root = $subs[0].FullName }
        }

        New-Item -ItemType Directory -Path $DestDir -Force | Out-Null

        $copied = 0
        foreach ($k in $Keep) {
            $rel  = $k.TrimEnd("/")
            $from = Join-Path $root ($rel -replace "/", "\")
            $to   = Join-Path $DestDir ($rel -replace "/", "\")

            if (Test-Path $from -PathType Leaf) {
                Copy-Item $from $to -Force
                $copied++
            } elseif (Test-Path $from -PathType Container) {
                if (-not (Test-Path $to)) { New-Item -ItemType Directory -Path $to -Force | Out-Null }
                Copy-Item (Join-Path $from "*") $to -Recurse -Force
                $copied += (Get-ChildItem $from -Recurse -File).Count
            } else {
                throw ("包里没有 {0}（keep 声明与实际发布包不符）" -f $rel)
            }
        }
        Write-Host ("  [落盘] {0}（{1} 个文件）" -f $DestDir, $copied)
        return $copied
    } finally {
        Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# ============================================================
#  deps.lock 写回
# ============================================================
function Set-LockValue {
    param([string]$Path, [string]$Section, [string]$Key, [string]$Value)

    $lines = Get-Content $Path -Encoding UTF8
    $out   = New-Object System.Collections.Generic.List[string]
    $cur   = $null
    $done  = $false

    foreach ($line in $lines) {
        $t = $line.Trim()
        if ($t -match '^\[(.+)\]$') { $cur = $Matches[1].Trim() }

        if (-not $done -and $cur -eq $Section -and $t -match ("^{0}\s*=" -f [Regex]::Escape($Key))) {
            # 保留原有缩进风格：key 左对齐到 12 列
            $out.Add(("{0,-11} = {1}" -f $Key, $Value))
            $done = $true
        } else {
            $out.Add($line)
        }
    }
    if (-not $done) { throw ("deps.lock 里找不到 [{0}] 的 {1}" -f $Section, $Key) }

    # 写回时保持 LF（仓库是 core.autocrlf=true，由 git 管换行）
    $text = [string]::Join("`n", $out) + "`n"
    [System.IO.File]::WriteAllText($Path, $text, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host ("  [lock] {0}.{1} = {2}" -f $Section, $Key, $Value)
}

# ============================================================
#  主流程
# ============================================================
$lock = Read-DepsLock -Path $LockPath
if ($Dep.Count -eq 0) { $Dep = @("lvgl", "freertos") }

Write-Host ("=" * 70)
Write-Host " 取外部依赖：按 deps.lock 钉死的版本"
Write-Host (" 仓库   : {0}" -f $RepoRoot)
Write-Host (" 缓存   : {0}" -f $CacheDir)
Write-Host (" 清单   : {0}" -f $LockPath)
Write-Host (" 依赖   : {0}" -f ($Dep -join ", "))
Write-Host ("=" * 70)

$failed = @()

foreach ($name in $Dep) {
    Write-Host ""
    Write-Host ("--- {0} ---" -f $name) -ForegroundColor Cyan

    if (-not $lock.ContainsKey($name)) {
        Write-Warning ("deps.lock 里没有 [{0}] 这一节，跳过" -f $name)
        continue
    }
    $cfg = $lock[$name]

    # ---- FreeRTOS 这类"非独立发布包"的依赖：只校验存在性 ----
    if ([string]::IsNullOrWhiteSpace($cfg["url_zip"])) {
        $dest = $cfg["dest"]
        if (Test-Path $dest) {
            $n = (Get-ChildItem $dest -Recurse -File -ErrorAction SilentlyContinue).Count
            Write-Host ("  [ OK ] 已存在：{0}（{1} 个文件）" -f $dest, $n) -ForegroundColor Green
            Write-Host ("         来源：{0}" -f $cfg["note"])
        } else {
            Write-Warning ("  缺失：{0}" -f $dest)
            Write-Host  ("         这个依赖没有发布包可下载 —— {0}" -f $cfg["note"])
            $failed += $name
        }
        continue
    }

    # ---- 可下载的依赖：下载 -> 校验 -> 精简落盘 -> 记录哈希 ----
    $dest = $cfg["dest"]
    if ($name -eq "lvgl" -and $LvglDir -ne "") { $dest = $LvglDir }

    $keep = @($cfg["keep"].Split(" ") | Where-Object { $_ -ne "" })
    if ($keep.Count -eq 0) { throw ("deps.lock 的 [{0}] 没写 keep" -f $name) }

    Write-Host ("  版本   : {0}" -f $cfg["version"])
    Write-Host ("  目标   : {0}" -f $dest)
    Write-Host ("  保留   : {0}" -f ($keep -join ", "))

    $already = Test-Path (Join-Path $dest ($keep[0].TrimEnd("/") -replace "/", "\"))
    if ($already -and -not $Force) {
        Write-Host "  [跳过] 目标已存在（要重取加 -Force）" -ForegroundColor Yellow
        $tree = Get-TreeSha256 -Root $dest -Keep $keep
        Write-Host ("  [校验] 精简树 {0} 个文件，聚合 SHA256 {1}" -f $tree.Count, $tree.Hash)
        if ($cfg["src_sha256"] -ne "" -and $cfg["src_sha256"] -ne "PENDING" -and
            $cfg["src_sha256"] -ne "N/A") {
            if ($tree.Hash -eq $cfg["src_sha256"].ToLowerInvariant()) {
                Write-Host "  [ OK ] 与本机锁定的源码哈希一致" -ForegroundColor Green
            } else {
                Write-Warning "  源码哈希**不一致** —— 这份 LVGL 被人改过，或被换成了别的版本"
                Write-Host  ("         锁定 {0}" -f $cfg["src_sha256"].ToLowerInvariant())
                Write-Host  ("         实际 {0}" -f $tree.Hash)
                $failed += $name
            }
        } elseif ($UpdateHashes) {
            Set-LockValue -Path $LockPath -Section $name -Key "src_sha256" -Value $tree.Hash
        }
        continue
    }

    $zipName = Split-Path -Leaf ([Uri]$cfg["url_zip"]).AbsolutePath
    $archive = Get-Archive -Url $cfg["url_zip"] -FileName $zipName -Sha256 $cfg["sha256"]

    Write-Host "  [校验] 发布包 SHA256"
    $gotZip = Get-FileSha256 $archive
    if ($cfg["sha256"] -ne "" -and $cfg["sha256"] -ne "PENDING") {
        if ($gotZip -ne $cfg["sha256"].ToLowerInvariant()) {
            throw ("发布包哈希不匹配！`n  期望 {0}`n  实际 {1}`n  （包可能被替换/损坏，已中止）" -f
                   $cfg["sha256"].ToLowerInvariant(), $gotZip)
        }
        Write-Host ("  [ OK ] {0}" -f $gotZip) -ForegroundColor Green
    } elseif ($UpdateHashes) {
        Set-LockValue -Path $LockPath -Section $name -Key "sha256" -Value $gotZip
    }

    # 重取时先清掉旧的目标内容，避免"新文件混着旧文件"这种最难查的状态
    foreach ($k in $keep) {
        $p = Join-Path $dest ($k.TrimEnd("/") -replace "/", "\")
        if (Test-Path $p) { Remove-Item $p -Recurse -Force }
    }

    [void](Expand-KeepOnly -Archive $archive -Keep $keep -DestDir $dest)

    $tree = Get-TreeSha256 -Root $dest -Keep $keep
    Write-Host ("  [哈希] 精简树 {0} 个文件，聚合 SHA256 {1}" -f $tree.Count, $tree.Hash)
    if ($UpdateHashes -or $cfg["src_sha256"] -eq "PENDING") {
        Set-LockValue -Path $LockPath -Section $name -Key "src_sha256" -Value $tree.Hash
    }
}

Write-Host ""
Write-Host ("=" * 70)
if ($failed.Count -gt 0) {
    Write-Host (" 有依赖没就绪：{0}" -f ($failed -join ", ")) -ForegroundColor Red
    Write-Host " 按上面的提示放好源码后可重跑本脚本；check_deps.py 也会报同样的结论。"
    exit 1
}
Write-Host " 全部就绪。下一步：python tools\check_deps.py" -ForegroundColor Green
Write-Host ("=" * 70)
exit 0
