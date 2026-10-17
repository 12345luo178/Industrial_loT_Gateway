"""落地预演：在**工程副本**上把"落地"整套做一遍，并真的构建 + 校验产物。

============================================================================
 为什么需要它（2026-10-11 第 32 轮）
============================================================================
落地（⑤）当天要动的东西分布在四处：CubeMX 生成物、main.c、it.c 的三个 handler、
`.cproject` 的宏、以及**构建路径**（`-Rtos`）。每一样都有过"只有真按流程走才暴露"的坑：

    · 生成的 freertos.c 与我们抢三个钩子      -> multiple definition（第 26 轮）
    · CubeMX 重写 main.c（main() 里没有 USER CODE 标记）-> 骨架，编译能过但什么都不做（第 25 轮）
    · FreeRTOSConfig.h 被换成默认值            -> 缺陷 R/S + 16 位 tick/定时器（第 17/30 轮）
    · 构建脚本产不出带内核的镜像               -> 落地后没有可烧镜像（第 31 轮）

这些之前都是**手敲命令**预演出来的（第 19/24/31 轮各来一遍），命令没沉淀。
本工具把整套预演固化成一条命令：**复制工程 -> 改造成"已落地"状态 -> 跑预检 ->
用 `-Rtos` 真构建 -> 校验产物符号**，全程**不碰真工程、不碰 build/ota**。

用法：
    python tools/rehearse_landing.py            # 全流程（约 2~4 分钟）
    python tools/rehearse_landing.py --keep     # 保留副本目录，便于人工查看
退出码：0 = 预演通过；1 = 某一步失败（打印原因与产物位置）；2 = 环境不全（缺工程/工具链）
"""
import argparse
import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile

# ⚠️ 编码守卫（GBK 控制台下中文 print 会 UnicodeEncodeError）
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(errors="replace")

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(REPO, 'tools')
NM = None
for _root, _dirs, _files in os.walk(r'D:\stm32cubeIDE'):
    if 'arm-none-eabi-nm.exe' in _files:
        NM = os.path.join(_root, 'arm-none-eabi-nm.exe')
        break

KERNEL_SUB = os.path.join('Middlewares', 'Third_Party', 'FreeRTOS', 'Source')
HANDLERS = ('SVC_Handler', 'PendSV_Handler', 'SysTick_Handler')


def find_kernel():
    roots = []
    if os.environ.get('GW_FW_ROOT'):
        roots.append(os.environ['GW_FW_ROOT'])
    roots.append(os.path.join(os.environ.get('USERPROFILE', ''), 'STM32Cube',
                              'Repository'))
    for r in roots:
        if not os.path.isdir(r):
            continue
        for d in sorted(os.listdir(r), reverse=True):
            if d.startswith('STM32Cube_FW_F1'):
                cand = os.path.join(r, d, KERNEL_SUB)
                if os.path.isdir(cand):
                    return cand
    return None


def sha256(p):
    h = hashlib.sha256()
    with io.open(p, 'rb') as f:
        for c in iter(lambda: f.read(65536), b''):
            h.update(c)
    return h.hexdigest()


def baseline_hashes():
    d = os.path.join(REPO, 'build', 'ota')
    out = {}
    if os.path.isdir(d):
        for n in sorted(os.listdir(d)):
            if n.endswith('.bin'):
                out[n] = sha256(os.path.join(d, n))
    return out


def run(cmd, quiet=True):
    p = subprocess.run(cmd, capture_output=True, text=True,
                       encoding='utf-8', errors='replace')
    if not quiet:
        print(p.stdout)
    return p.returncode, p.stdout + p.stderr


def copy_project(src, dst):
    """只拷构建与预检需要的：Core（含 Startup）、Drivers、.cproject、.ioc。"""
    for sub in ('Core', 'Drivers'):
        s = os.path.join(src, sub)
        if os.path.isdir(s):
            shutil.copytree(s, os.path.join(dst, sub))
    for f in ('.cproject', '.project'):
        s = os.path.join(src, f)
        if os.path.isfile(s):
            shutil.copyfile(s, os.path.join(dst, f))
    for f in os.listdir(src):
        if f.endswith('.ioc'):
            shutil.copyfile(os.path.join(src, f), os.path.join(dst, f))


def land_it(prj, kernel, workdir):
    """把副本改造成"已落地"状态。返回 (ok, 说明列表)"""
    notes = []

    # 1) 内核源码进工程（模拟 CubeMX 添加）
    kdst = os.path.join(prj, KERNEL_SUB)
    if not os.path.isdir(kdst):
        shutil.copytree(kernel, kdst)
    notes.append('内核源码已放入工程 Middlewares/...')

    # 2) 仓库的 FreeRTOSConfig.h（预检会与它逐项比对；这一步等价于"CubeMX 生成后改回 ★ 项"）
    shutil.copyfile(os.path.join(REPO, 'port', 'FreeRTOSConfig.h'),
                    os.path.join(prj, 'Core', 'Inc', 'FreeRTOSConfig.h'))
    notes.append('port/FreeRTOSConfig.h 已放到 Core/Inc/')

    # 3) 删掉 it.c 里那三个与内核重名的 handler（只删这三个）
    itc = os.path.join(prj, 'Core', 'Src', 'stm32f1xx_it.c')
    if os.path.isfile(itc):
        t = io.open(itc, encoding='utf-8', errors='replace').read()
        n = 0
        for fn in HANDLERS:
            pat = re.compile(r'(?:^|\n)void\s+' + fn + r'\s*\(void\)\s*\{.*?\n\}', re.S)
            t, k = pat.subn('', t)
            n += k
        io.open(itc, 'w', encoding='utf-8', newline='\n').write(t)
        notes.append('it.c 删掉 %d/3 个 handler' % n)

    # 4) .cproject 加宏
    cp = os.path.join(prj, '.cproject')
    if os.path.isfile(cp):
        t = io.open(cp, encoding='utf-8', errors='replace').read()
        if 'GW_USE_FREERTOS' not in t:
            t = t.replace('<listOptionValue builtIn="false" value="USE_HAL_DRIVER"/>',
                          '<listOptionValue builtIn="false" value="USE_HAL_DRIVER"/>'
                          '<listOptionValue builtIn="false" value="GW_USE_FREERTOS=1"/>')
            io.open(cp, 'w', encoding='utf-8', newline='').write(t)
        notes.append('.cproject 已定义 GW_USE_FREERTOS=1')

    # 5) 装配生产版 main.c 并覆盖（这一步用的就是落地当天要用的那个工具）
    out_main = os.path.join(workdir, 'landing_main.c')
    rc, o = run([sys.executable, os.path.join(TOOLS, 'make_landing_main.py'),
                 '--project', prj, '--out', out_main, '--verify-compile'])
    if rc != 0:
        return False, notes + ['装配 main.c 失败：\n' + o]
    shutil.copyfile(out_main, os.path.join(prj, 'Core', 'Src', 'main.c'))
    notes.append('生产版 main.c 已装配并覆盖（含编译验证）')
    return True, notes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--project',
                    default=os.environ.get('GW_CUBE_PROJECT',
                                           r'D:\stm32cubeIDE\gateway2'))
    ap.add_argument('--keep', action='store_true', help='保留副本目录')
    a = ap.parse_args()

    if not os.path.isfile(os.path.join(a.project, 'Core', 'Src', 'main.c')):
        print('  [FAIL] 找不到工程 main.c：%s' % a.project)
        return 2
    if NM is None:
        print('  [FAIL] 找不到 arm-none-eabi-nm（需要 STM32CubeIDE 的 GNU 工具）')
        return 2

    base_before = baseline_hashes()
    work = tempfile.mkdtemp(prefix='rehearse_landing_')
    prj = os.path.join(work, 'prj')
    os.makedirs(prj)
    print('=' * 70)
    print(' 落地预演（工程副本，不碰真工程与 build/ota）')
    print('   源工程 : %s' % a.project)
    print('   副本   : %s' % prj)
    print('=' * 70)

    kernel = find_kernel()
    if kernel is None:
        print('  [FAIL] 找不到 FreeRTOS 内核源码（设 GW_FW_ROOT 或检查 STM32Cube 仓库）')
        return 2

    print(' 1) 复制工程 ...')
    copy_project(a.project, prj)
    print(' 2) 改造成"已落地"状态 ...')
    ok, notes = land_it(prj, kernel, work)
    for n in notes:
        print('    · %s' % n)
    if not ok:
        return 1

    print(' 3) 落地预检（要求 0 矛盾）...')
    rc, out = run([sys.executable, os.path.join(TOOLS, 'check_landing.py'),
                   '--project', prj])
    for line in out.splitlines():
        if ('小结' in line) or line.strip().startswith('==>'):
            print('    ' + line.strip()[:150])
    # ⚠️ 把"待办"也打出来：预演的通过门槛是 0 矛盾，但待办项正是
        #    落地当天还需要人工做/确认的事（例如 CubeMX 的 freertos.c）。
    for line in out.splitlines():
        if '[待办]' in line:
            print('    (落地当天仍需处理) ' + line.strip()[:140])
    if rc != 0:
        print('  [FAIL] 预检有矛盾（退出码 %d）—— 见上面 [矛盾] 行' % rc)
        for line in out.splitlines():
            if '[矛盾]' in line:
                print('    ' + line.strip()[:170])
        return 1

    print(' 4) 用 -Rtos 真构建槽位镜像 ...')
    outdir = os.path.join(work, 'out')
    rc, out = run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass',
                   '-File', os.path.join(TOOLS, 'build_ota_images.ps1'),
                   '-Project', prj, '-OutDir', outdir, '-Optimize', 's',
                   '-SlotB', '-Golden', '-Rtos'])
    for line in out.splitlines():
        if '[OK]' in line or 'multiple definition' in line:
            print('    ' + line.strip()[:130])
    if rc != 0:
        print('  [FAIL] 构建失败（退出码 %d）' % rc)
        return 1

    print(' 5) 校验产物符号（证明它真是 RTOS 镜像、且保留 it.c 的中断）...')
    elf = os.path.join(outdir, 'app_a.elf')
    nmout = subprocess.run([NM, elf], capture_output=True, text=True,
                           encoding='utf-8', errors='replace').stdout
    need = ['gw_rtos_create_tasks', 'vTaskStartScheduler', 'xTaskCreate',
            'vApplicationIdleHook', 'TIM4_IRQHandler']
    miss = [s for s in need if (' ' + s) not in nmout and ('\t' + s) not in nmout]
    if miss:
        print('  [FAIL] 镜像里缺符号：%s' % ', '.join(miss))
        return 1
    print('    · %s 全部存在 ✓' % ' / '.join(need))

    print(' 6) 确认没碰到真工程与冻结基线 ...')
    base_after = baseline_hashes()
    if base_before != base_after:
        print('  [FAIL] **build/ota 的哈希变了** —— 预演不该碰它！')
        return 1
    print('    · build/ota 哈希未变 ✓（%d 个 bin）' % len(base_after))

    print('=' * 70)
    print(' 预演通过：预检 0 矛盾 + 镜像构建成功 + 符号齐全 + 冻结基线未动')
    print('=' * 70)
    if a.keep:
        print(' 副本保留在：%s' % work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
