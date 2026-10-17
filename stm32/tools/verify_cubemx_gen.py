"""CubeMX 生成之后的"体检 + 待修清单"。

============================================================================
 为什么需要它（2026-10-11 第 44 轮，等用户执行 CubeMX 落地第 1 步期间）
============================================================================
落地第 1 步是**用户在 CubeMX 里**启用 FreeRTOS 并生成代码 —— 那一步会：

    · 新建 `Core/Src/freertos.c`（里面三个钩子与仓库**抢定义** -> multiple definition）
    · 新建/覆盖 `Core/Inc/FreeRTOSConfig.h`（用它自己的默认值 = 缺陷 R/S 回归）
    · 从 `it.c` 里删掉三个 handler（我们要确认它**真的删了**）
    · 往工程里塞内核源码（tasks/queue/list/port/heap_4）与一堆 include 路径
    · 可能顺手改动 `.cproject` / `.ioc` / 其它 `MX_*` 生成文件

人眼 diff 一个 132 文件、5MB 的工程既慢又容易漏 ✗。本工具用**生成前的哈希清单**
精确定位"加了/删了/改了哪些文件"，再对五处**关键点**给出结论与待修清单
（第 5 处是 2026-10-13 加的：IWDG 预分频的唯一真源 —— 缺陷 AK 的守卫）。

用法：
    # 生成前先记清单（已做，仓库里存的就是）
    python tools/verify_cubemx_gen.py --snapshot
    # 生成后体检
    python tools/verify_cubemx_gen.py                 # 用仓库里的清单做基线
    python tools/verify_cubemx_gen.py --manifest <别的清单>

退出码：0 = 关键点全部符合预期；1 = 有需要人工处理的项（会列出）。
"""
import argparse
import hashlib
import io
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_MANIFEST = os.path.join(REPO, 'tools', 'cubemx_manifest_before.txt')
DEFAULT_PROJECT = os.environ.get('GW_CUBE_PROJECT', r'D:\stm32cubeIDE\gateway2')
SKIP_DIRS = ('Debug', 'Release', '.settings', '.git', '.metadata')

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(errors="replace")


def walk(prj):
    out = {}
    for root, dirs, files in os.walk(prj):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for f in files:
            p = os.path.join(root, f)
            rel = os.path.relpath(p, prj).replace('\\', '/')
            try:
                h = hashlib.sha256(io.open(p, 'rb').read()).hexdigest()
            except OSError:
                continue
            out[rel] = h
    return out


def load_manifest(path):
    out = {}
    for line in io.open(path, encoding='utf-8', errors='replace'):
        line = line.strip()
        if not line or ' ' not in line:
            continue
        h, rel = line.split(None, 1)
        out[rel.strip().replace('\\', '/')] = h
    return out


def snapshot(prj, out_path):
    cur = walk(prj)
    with io.open(out_path, 'w', encoding='utf-8', newline='\n') as f:
        for rel in sorted(cur):
            f.write('%s  %s\n' % (cur[rel], rel))
    print(' 已记录 %d 个文件的哈希 -> %s' % (len(cur), out_path))
    return 0


def read(p):
    try:
        return io.open(p, encoding='utf-8', errors='replace').read()
    except OSError:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--project', default=DEFAULT_PROJECT)
    ap.add_argument('--manifest', default=DEFAULT_MANIFEST)
    ap.add_argument('--snapshot', action='store_true',
                    help='把当前工程状态记成基线清单（生成前跑）')
    a = ap.parse_args()

    if a.snapshot:
        return snapshot(a.project, a.manifest)

    if not os.path.isdir(a.project):
        print('  [FAIL] 找不到工程：%s' % a.project)
        return 2
    if not os.path.isfile(a.manifest):
        print('  [FAIL] 找不到基线清单：%s（先跑 --snapshot）' % a.manifest)
        return 2

    before = load_manifest(a.manifest)
    now = walk(a.project)
    added = sorted(set(now) - set(before))
    removed = sorted(set(before) - set(now))
    changed = sorted(r for r in set(now) & set(before) if now[r] != before[r])

    print('=' * 74)
    print(' CubeMX 生成体检（基线 %s）' % os.path.basename(a.manifest))
    print('=' * 74)
    print(' 基线 %d 个文件 -> 现在 %d 个文件' % (len(before), len(now)))
    print(' 新增 %d / 删除 %d / 修改 %d' % (len(added), len(removed), len(changed)))
    for tag, lst in (('新增', added), ('删除', removed), ('修改', changed)):
        for r in lst[:40]:
            print('   [%s] %s' % (tag, r))
        if len(lst) > 40:
            print('   [%s] …还有 %d 个' % (tag, len(lst) - 40))

    todo = []

    # ---- 关键点 0：CubeMX 里选的 FREERTOS 接口版本 ----
    #   仓库的 RTOS 层（port/gw_rtos.c）**只用原生 FreeRTOS API**，一次 CMSIS
    #   (`osThread*`/`osMutex*`) 都没有 —— 所以接口版本对业务代码零影响，
    #   它只决定 CubeMX 往工程里塞哪个包装层、以及它默认写了哪些 config*。
    ioc = None
    for f in os.listdir(a.project):
        if f.endswith('.ioc'):
            ioc = os.path.join(a.project, f)
            break
    iface = ''
    if ioc:
        txt = read(ioc) or ''
        m = re.search(r'CMSIS_(V[12])', txt)
        if m:
            iface = m.group(1)
    print('-' * 74)
    if not iface:
        print(' [0] FREERTOS 接口             : 未在 .ioc 里找到 CMSIS_Vx（还没启用 FREERTOS？）')
    elif iface == 'V1':
        print(' [0] FREERTOS 接口             : CMSIS_V1（与仓库一致 ✓）')
        print('     说明：我们用原生 API，v1 包装层只是被链接进来；未引用会被 --gc-sections 丢掉')
    else:
        print(' [0] FREERTOS 接口             : CMSIS_V2（**与仓库文档不一致**）')
        todo.append('用了 CMSIS_V2：① 确认生成的 cmsis_os2.c 能在"已恢复 ★ 项的"'
                    'FreeRTOSConfig.h 下编译通过（V2 包装层对 config* 有额外要求，'
                    '例如计数信号量/任务通知等）；② 确认我们仍只调原生 API'
                    '（gw_rtos_start() 用 vTaskStartScheduler，不需要 osKernelStart）；'
                    '③ 把迁移指南与预检里的接口版本同步改掉')

    # ---- 关键点 1：freertos.c 的三个钩子（与仓库抢定义）----
    print('-' * 74)
    fc = os.path.join(a.project, 'Core', 'Src', 'freertos.c')
    if not os.path.isfile(fc):
        print(' [1] Core/Src/freertos.c      : **不存在**（若你刚生成过，说明没启用 FREERTOS？）')
        todo.append('确认 CubeMX 里 FREERTOS 已启用并重新生成')
    else:
        txt = read(fc) or ''
        hooks = [h for h in ('vApplicationIdleHook', 'vApplicationStackOverflowHook',
                             'vApplicationMallocFailedHook') if h in txt]
        cfg = [c for c in ('MX_FREERTOS_Init',) if c in txt]
        print(' [1] Core/Src/freertos.c      : 存在；钩子命中 %s；%s'
              % (hooks or '无', ('含 ' + ','.join(cfg)) if cfg else '无 MX_FREERTOS_Init'))
        if hooks:
            todo.append('删掉 freertos.c 里这三个钩子体（保留仓库 port/gw_rtos.c 版本）：'
                        + ','.join(hooks))
        if cfg:
            todo.append('不要调用 MX_FREERTOS_Init()（本项目自己建 6 个任务）')

    # ---- 关键点 2：FreeRTOSConfig.h 的 ★ 项与三个重定向宏 ----
    print('-' * 74)
    proj_cfg = os.path.join(a.project, 'Core', 'Inc', 'FreeRTOSConfig.h')
    repo_cfg = os.path.join(REPO, 'port', 'FreeRTOSConfig.h')
    CRIT = ['configUSE_PREEMPTION', 'configUSE_TIME_SLICING', 'configUSE_IDLE_HOOK',
            'configTICK_RATE_HZ', 'configMAX_PRIORITIES', 'configMINIMAL_STACK_SIZE',
            'configUSE_MUTEXES', 'configTOTAL_HEAP_SIZE',
            'configCHECK_FOR_STACK_OVERFLOW', 'configUSE_MALLOC_FAILED_HOOK',
            'configPRIO_BITS', 'configUSE_16_BIT_TICKS', 'configUSE_TIMERS',
            'configSUPPORT_DYNAMIC_ALLOCATION', 'configUSE_TASK_NOTIFICATIONS',
            'configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY',
            'configLIBRARY_LOWEST_INTERRUPT_PRIORITY']
    REDIR = ['vPortSVCHandler', 'xPortPendSVHandler', 'xPortSysTickHandler']

    def defs(path):
        t = read(path)
        if t is None:
            return None
        d = {}
        for m in re.finditer(r'^\s*#define\s+(\w+)\s+(.+?)\s*$', t, re.M):
            d[m.group(1)] = m.group(2)
        return d

    pa = defs(proj_cfg)
    ra = defs(repo_cfg)
    if pa is None:
        print(' [2] Core/Inc/FreeRTOSConfig.h: **不存在** —— CubeMX 没生成？')
        todo.append('确认 CubeMX 生成了 Core/Inc/FreeRTOSConfig.h')
    else:
        miss = [k for k in REDIR if k not in pa]
        diff = [k for k in CRIT if k in ra and pa.get(k) != ra.get(k)]
        print(' [2] Core/Inc/FreeRTOSConfig.h: 缺重定向宏 %s；★ 项与仓库不一致 %d 项 %s'
              % (miss or '无', len(diff), diff or ''))
        if miss:
            todo.append('把三个向量重定向宏（缺陷 R）补进 FreeRTOSConfig.h：'
                        + ','.join(miss))
        if diff:
            todo.append('把 ★ 项照 port/FreeRTOSConfig.h 改回（缺陷 R/S 回归）：'
                        + ','.join(diff))

    # ---- 关键点 3：it.c 的三个 handler 是否已被删 ----
    print('-' * 74)
    itc = os.path.join(a.project, 'Core', 'Src', 'stm32f1xx_it.c')
    t = read(itc)
    if t is None:
        print(' [3] Core/Src/stm32f1xx_it.c : **不存在**')
    else:
        still = [h for h in ('SVC_Handler', 'PendSV_Handler', 'SysTick_Handler')
                 if re.search(r'void\s+' + h + r'\s*\(', t)]
        print(' [3] Core/Src/stm32f1xx_it.c : 仍在的三个 handler = %s' % (still or '无（已删，符合预期）'))
        if still:
            todo.append('删掉 it.c 里这三个 handler（否则与 port.c 重复定义）：'
                        + ','.join(still))

    # ---- 关键点 4：内核源码是否进工程 ----
    print('-' * 74)
    kern = os.path.join(a.project, 'Middlewares', 'Third_Party', 'FreeRTOS', 'Source')
    need = ['tasks.c', 'queue.c', 'list.c',
            os.path.join('portable', 'GCC', 'ARM_CM3', 'port.c'),
            os.path.join('portable', 'MemMang', 'heap_4.c')]
    missing = [n for n in need if not os.path.isfile(os.path.join(kern, n))]
    print(' [4] 内核源码                 : %s'
          % ('缺 %s' % missing if missing else 'tasks/queue/list/port/heap_4 齐全'))
    if missing:
        todo.append('把内核源码加进工程（缺 %s）' % missing)

    # ---- 关键点 5：IWDG 预分频必须走"唯一真源"（缺陷 AK 的守卫）----
    #   2026-10-13 定位的缺陷 AK：`iwdg.c` 里宏写 `IWDG_PRESCALER_DIV 64u`、
    #   赋值却写 `IWDG_PRESCALER_128`，于是 `_Static_assert(IWDG_TIMEOUT_MS
    #   == 1600u)` 检查的是**那个没人改的宏** -> 自检静默失效：真实窗口 3.2s，
    #   而文档/注释/监控期望值全停在 1.6s。
    #   修法是把赋值改成 `IWDG_PRESCALER_REG`（唯一真源），赋值与自检共用它。
    #   ⚠️ 但那一行落在 CubeMX **管理区**（不在 USER CODE 段内）—— 重新生成会把
    #      它退回字面量 `IWDG_PRESCALER_128`，唯一真源被拆散、守卫随之失效。
    #      所以这里必须盯住它，否则 AK 会静默复现（同缺陷 AA 的性质）。
    print('-' * 74)
    iwdgc = os.path.join(a.project, 'Core', 'Src', 'iwdg.c')
    ti = read(iwdgc)
    if ti is None:
        print(' [5] Core/Src/iwdg.c         : **不存在**（.ioc 里没配 IWDG？）')
        todo.append('确认 CubeMX 里 IWDG 已启用并生成了 Core/Src/iwdg.c')
    else:
        lit = re.search(r'hiwdg\.Init\.Prescaler\s*=\s*(IWDG_PRESCALER_\w+)\s*;', ti)
        has_src = re.search(
            r'hiwdg\.Init\.Prescaler\s*=\s*IWDG_PRESCALER_REG\s*;', ti) is not None
        regdef = re.search(r'#define\s+IWDG_PRESCALER_REG\s+(IWDG_PRESCALER_\w+)', ti)
        # ⚠️ 断言必须**行首锚定**（(?m)^\s*_Static_assert）：
        #    说明性注释里会**引用旧代码**（例如模板头部写着
        #    "`_Static_assert(IWDG_TIMEOUT_MS == 1600u)`"），不锚定就会先命中
        #    注释里的旧值 -> 报出"副本已过期"的假警报（已踩过）。
        to_ms = re.search(r'(?m)^\s*_Static_assert\(\s*IWDG_TIMEOUT_MS\s*==\s*(\d+)u', ti)
        print(' [5] Core/Src/iwdg.c         : Prescaler 赋值 = %s；'
              '唯一真源 IWDG_PRESCALER_REG = %s；超时自检 = %s ms'
              % (lit.group(1) if lit else '**找不到赋值**',
                 regdef.group(1) if regdef else '**缺失**',
                 to_ms.group(1) if to_ms else '**缺失**'))
        if not has_src:
            todo.append('Core/Src/iwdg.c 的 `hiwdg.Init.Prescaler` 必须赋 '
                        '`IWDG_PRESCALER_REG`（唯一真源），现在赋的是字面量 %s '
                        '—— 这正是缺陷 AK：赋值与超时自检各用一个常量，'
                        '改一处就静默分叉'
                        % (lit.group(1) if lit else '（找不到赋值）'))
        if regdef is None or to_ms is None:
            todo.append('Core/Src/iwdg.c 缺 `IWDG_PRESCALER_REG` 定义或 '
                        '`_Static_assert`（缺陷 AK 的守卫没了 —— 它们是 USER CODE 段，'
                        'CubeMX 本应保留）')
        # 与 port/gw_rtos.h 的 GW_IWDG_TIMEOUT_MS 对账
        # （"同一个值散在多处、改一处必然漂"的守卫）
        mh = re.search(r'#define\s+GW_IWDG_TIMEOUT_MS\s+(\d+)u',
                       read(os.path.join(REPO, 'port', 'gw_rtos.h')) or '')
        if mh and to_ms:
            if mh.group(1) != to_ms.group(1):
                todo.append('IWDG 超时口径不一致：Core/Src/iwdg.c 的自检算的是 %s ms，'
                            '而 port/gw_rtos.h 的 GW_IWDG_TIMEOUT_MS = %s ms'
                            % (to_ms.group(1), mh.group(1)))
            else:
                print('     与 port/gw_rtos.h 的 GW_IWDG_TIMEOUT_MS=%s 一致 ✓'
                      % mh.group(1))
        # 与 templates/iwdg_template.c（仓库里的权威副本）对账。
        # ⚠️ 工程侧的 iwdg.c **不在版本控制里**，这份副本是"工程丢了还能套回来"
        #    的唯一保障 —— 所以它必须和工程保持一致；否则真出事时套回去的是
        #    个过时版本，比没有副本更坏（会让人以为已经修好了）。
        tpl = read(os.path.join(REPO, 'templates', 'iwdg_template.c'))
        if tpl is None:
            todo.append('缺 templates/iwdg_template.c —— 那是缺陷 AK 修复的权威副本；'
                        '工程侧 Core/Src/iwdg.c 不在版本控制里，丢了就没第二份了')
        else:
            t_ms = re.search(
                r'(?m)^\s*_Static_assert\(\s*IWDG_TIMEOUT_MS\s*==\s*(\d+)u', tpl)
            t_src = re.search(
                r'hiwdg\.Init\.Prescaler\s*=\s*IWDG_PRESCALER_REG\s*;', tpl) is not None
            if (not t_src) or (t_ms is None) or (
                    to_ms is not None and t_ms.group(1) != to_ms.group(1)):
                todo.append('templates/iwdg_template.c 与工程侧不一致 —— 副本已过期，'
                            '请同步（模板：赋值用唯一真源=%s、超时=%s ms；'
                            '工程侧超时=%s ms）'
                            % (t_src, t_ms.group(1) if t_ms else '缺失',
                               to_ms.group(1) if to_ms else '缺失'))
            else:
                print('     templates/iwdg_template.c 与工程侧一致 ✓（超时 %s ms）'
                      % t_ms.group(1))

    print('=' * 74)
    if todo:
        print(' 需要处理 %d 项：' % len(todo))
        for i, x in enumerate(todo, 1):
            print('   %d) %s' % (i, x))
        print('=' * 74)
        return 1
    print(' 五处关键点全部符合预期 ✓（可以进入装配 main.c 与预检）')
    print('=' * 74)
    return 0


if __name__ == '__main__':
    sys.exit(main())
