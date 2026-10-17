"""编译检查 tools/ 下所有 Python 脚本（语法错误当场暴露）。

============================================================================
 为什么需要它（2026-10-11 第 22 轮加）
============================================================================
CI 的"控制台编码"那一步只**静态检查**每个脚本的 print 有没有编码守卫 ——
它**不执行**脚本，所以一个**语法错误**的脚本能一路混到"有人真的去跑它"那一刻。

⚠️ 而我本人在同一天里**第 5 次**犯了同一个错：在 Python 双引号字符串里
   又写了一个 ASCII 双引号（`help="只核对"板子上跑的镜像"..."`）。
   每次都是靠"改完立刻 ast.parse"自己发现的 —— 但那依赖我记得做。
   一条 `py_compile` 全量编译的检查就能把它变成**机械**保证。

用法：python tools/check_tools_syntax.py
退出码：0 = 全部能编译；1 = 有脚本编译不过（打印文件与行列）
"""
import io
import os
import sys

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(errors="replace")

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(REPO, "tools")


def main():
    files = sorted(f for f in os.listdir(TOOLS) if f.endswith(".py"))
    bad = []
    for f in files:
        p = os.path.join(TOOLS, f)
        try:
            # ⚠️ 用 compile() 直接编源码，**不要** py_compile(cfile=os.devnull)：
            #    在 Windows 上 os.devnull == "nul"，py_compile 会抱怨
            #    "nul is a non-regular file" -> **每个脚本都被报成失败**。
            #    第一版就是这么写的，于是这条检查对**全部 37 个**脚本都报 FAIL ——
            #    一条"对什么都报错"的检查，和"对什么都通过"一样没用。
            src = io.open(p, encoding="utf-8").read()
            compile(src, p, "exec")
        except SyntaxError as e:
            bad.append((f, "第 %s 行: %s" % (e.lineno, e.msg), e.text))
        except Exception as e:                      # noqa: BLE001
            bad.append((f, str(e), None))

    print("  编译检查 %d 个工具脚本" % len(files))
    if not bad:
        print("  [ OK ] 全部能编译")
        return 0
    for f, msg, txt in bad:
        print("  [FAIL] %s" % f)
        print("         %s" % msg)
        if txt:
            print("         %s" % txt.rstrip()[:120])
    print("  ==> %d 个脚本编译不过（多半是字符串里的引号写错）" % len(bad))
    return 1


if __name__ == "__main__":
    sys.exit(main())
