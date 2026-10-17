# -*- coding: utf-8 -*-
"""
让 tools/ci.ps1 能在 Linux 上跑（供 GitHub Actions 复用同一份脚本）

============================================================================
 为什么要做这件事

项目原本有**两套** CI 定义：

    tools/ci.ps1                本地，11 个步骤
    .github/workflows/ci.yml    GitHub，5 个步骤

结果它们**漂移**了：Actions 里缺了第 5~11 步，
包括最有价值的「端到端独立验证」与「OTA 断电穷举」——
也就是说**推上 GitHub 跑的 CI 比本地弱很多**，而"CI 绿了"会给人错误的安心。

根治办法不是"把 Actions 补齐到 11 步"（那还是会漂移），
而是让 Actions **调用同一个 ci.ps1**。于是脚本成为唯一事实来源。

============================================================================
 需要处理的三处平台差异

  1) **可执行文件后缀**：Windows 需要 `.exe`，Linux 不需要。
     脚本里写死了 `test/test_x.exe`，在 Linux 上会找不到文件。

  2) **PowerShell 的 `&` 调用**：这个跨平台没问题。
     PowerShell 7 在 Linux 上可用（GitHub Actions 的 ubuntu 镜像预装了 pwsh）。

  3) **路径分隔符**：反斜杠在 PowerShell 里跨平台都能处理（它接受两种），
     所以不用改。

  4) **Python 定位**：脚本里有 Windows 专属的候选路径，Linux 上要靠 PATH。
     这也不用改 —— 脚本本来就有"PATH -> 候选路径"的降级顺序。

============================================================================
 做法

在脚本开头定义一个 `$EXE` 变量（Windows 是 ".exe"，其它平台是空），
然后所有 `.exe` 引用改成 `"name$EXE"`。

这个转换是**幂等**的：重复跑不会叠加。
============================================================================
"""
import re
import sys

PATH = "tools/ci.ps1"


def main():
    raw = open(PATH, encoding="utf-8-sig").read()

    if "$script:EXE" in raw or "$EXE" in raw:
        print("  已转换过（幂等），无需重复")
        return 0

    lines = raw.split("\n")
    out = []
    injected = False
    n_exe = 0

    for ln in lines:
        # 在 $ErrorActionPreference 之后注入平台判定
        if not injected and ln.startswith('$ErrorActionPreference'):
            out.append(ln)
            out.append('')
            out.append('# ⚠️ 平台差异：Windows 的可执行文件带 .exe，Linux 不带。')
            out.append('#    这个变量让同一份脚本能在两个平台上跑 ——')
            out.append('#    这样 GitHub Actions 可以直接调用它，')
            out.append('#    而不是维护第二套 CI 定义（两套一定会漂移）。')
            out.append('if ($IsWindows -or ($null -eq $IsWindows)) {')
            out.append('    $EXE = ".exe"          # Windows（PowerShell 5.1 没有 $IsWindows）')
            out.append('} else {')
            out.append('    $EXE = ""              # Linux / macOS')
            out.append('}')
            injected = True
            continue

        # 把 "...test_xxx.exe" 之类改成 "...test_xxx$EXE"
        # 只改字符串里的 .exe（不动注释里的说明文字）
        stripped = ln.strip()
        if not stripped.startswith("#"):
            new = ln
            # 形如 "路径/名字.exe"  ->  "路径/名字$EXE"
            new = re.sub(r'(?<=[\w/\\])\.exe(?=")', '$EXE', new)
            if new != ln:
                n_exe += 1
            ln = new
        out.append(ln)

    open(PATH, "w", encoding="utf-8-sig", newline="\r\n").write("\n".join(out))
    print("  已注入 $EXE 平台判定，替换 %d 处 .exe 引用" % n_exe)
    return 0


if __name__ == "__main__":
    sys.exit(main())
