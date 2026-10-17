"""自证 tools/check_landing.py 的四项检查（缺陷 W/R/S 的工程侧翻版）。

⚠️ 为什么要把自证放进仓库（而不是只在本地跑一次）：
   本项目的纪律是"**证据可复现**" —— 一条检查如果只在某天本地跑过一次，
   下一个人无法确认它到底能不能报错。这个脚本搭四种**合成工程**：

     A. 宏已定义 + 内核在 + handler 删了 + main() 落地了，但**没合并内务**
        -> 必须报 [矛盾]（这就是"工程 main.c 还是 W 修复前版本"的真实状态）
     B. 内务也合并好了 -> 必须 0 矛盾
     C. 在 B 的基础上，往裸机主循环里偷加一句 gw_log_tick
        -> 必须报 [矛盾]（主循环结构）
     D. 在 B 的基础上，把工程的 FreeRTOSConfig.h 换成"CubeMX 默认那种"
        （删掉三个向量重定向宏 + 堆改成 8KB）
        -> 必须报 [矛盾]（**缺陷 R 与 S 的回归**：
           没有重定向 -> 第一次任务切换就 HardFault；堆不够 -> 任务创建失败）

   用法：python tools/selfproof_landing.py
   退出码：0 = 四种情形都符合预期；1 = 有不符合（打印是哪一种）
"""
import io
import os
import re
import shutil
import sys
import tempfile

# ⚠️ 编码守卫（GBK 控制台下中文 print 会 UnicodeEncodeError）——
#    与其它工具脚本同一约定，tools/check_console_encoding.py 会检查这一句。
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(errors="replace")

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

MAIN_OK = """static void app_housekeeping(uint32_t dt)
{
    gw_fault_port_tick(dt);
    gw_log_tick(&g_log, dt);
    gw_ota_bridge_tick(&g_ota);
}

#if defined(GW_USE_FREERTOS) && (GW_USE_FREERTOS == 1)
void gw_rtos_housekeeping(uint32_t dt_ms) { app_housekeeping(dt_ms); }
#endif

int main(void)
{
    HAL_Init();
    gw_rtos_create_tasks(&g_app);
    gw_rtos_start();

#if defined(GW_USE_FREERTOS) && (GW_USE_FREERTOS == 1)
    /* RTOS 分支：不返回 */
    for (;;) { vTaskDelay(1); }
#else  /* ---------------- 14) 裸机主循环 ---------------- */
    for (;;) {
        gw_app_poll(&g_app, dt);
        app_housekeeping(dt);
        HAL_Delay(1);
    }
#endif
}
"""

# A：只落地 main()、没合并内务 —— 把内务又摊回主循环里（= 旧版本的样子）
MAIN_NO_HK = ("""int main(void)
{
    HAL_Init();
    gw_rtos_create_tasks(&g_app);
    gw_rtos_start();
#else  /* ---------------- 14) 裸机主循环 ---------------- */
    for (;;) {
        gw_app_poll(&g_app, dt);
        gw_fault_port_tick(dt);
        gw_log_tick(&g_log, dt);
        gw_ota_bridge_tick(&g_ota);
        HAL_Delay(1);
    }
#endif
}
""")

# C：合并好了，但主循环里偷偷多一句有副作用的调用
MAIN_STRAY = MAIN_OK.replace("        app_housekeeping(dt);",
                             "        app_housekeeping(dt);\n        gw_log_tick(&g_log, dt);")


def make(main_c, config_mode='repo', extra=None):
    root = tempfile.mkdtemp(prefix='selfproof_land_')
    for d in (('Core', 'Src'), ('Core', 'Inc'),
              ('Middlewares', 'Third_Party', 'FreeRTOS', 'Source')):
        os.makedirs(os.path.join(root, *d), exist_ok=True)

    for f in ('tasks.c', 'queue.c', 'list.c'):
        io.open(os.path.join(root, 'Middlewares', 'Third_Party', 'FreeRTOS',
                             'Source', f), 'w').write('/* kernel */\n')

    # it.c：三个 handler 已删（落地后的状态）
    io.open(os.path.join(root, 'Core', 'Src', 'stm32f1xx_it.c'), 'w',
            encoding='utf-8').write('void TIM4_IRQHandler(void) { }\n')

    io.open(os.path.join(root, 'Core', 'Src', 'main.c'), 'w',
            encoding='utf-8').write(main_c)

    io.open(os.path.join(root, '.cproject'), 'w', encoding='utf-8').write(
        '<option><listOptionValue value="GW_USE_FREERTOS=1"/></option>\n')
    io.open(os.path.join(root, 'gateway2.ioc'), 'w', encoding='utf-8').write(
        'NVIC.TimeBaseIP=TIM4\n')

    for src, dst in ((os.path.join(REPO, 'inc', 'gw_config.h'),
                      os.path.join(root, 'Core', 'Inc', 'gw_config.h')),
                     (os.path.join(REPO, 'port', 'FreeRTOSConfig.h'),
                      os.path.join(root, 'Core', 'Inc', 'FreeRTOSConfig.h'))):
        if os.path.isfile(src):
            shutil.copyfile(src, dst)

    # 可选：额外塞进去的文件（例如 CubeMX 生成的 freertos.c）
    for rel, content in (extra or {}).items():
        p = os.path.join(root, *rel.split('/'))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        io.open(p, 'w', encoding='utf-8', newline='\n').write(content)

    if config_mode == 'cube_default':
        # 模拟"CubeMX 用它自己的默认值新建/覆盖了 FreeRTOSConfig.h"：
        #   去掉三个向量重定向宏 + 堆改成 8KB（缺陷 R 与 S 的回归）
        p = os.path.join(root, 'Core', 'Inc', 'FreeRTOSConfig.h')
        txt = io.open(p, encoding='utf-8').read()
        for line in ('#define vPortSVCHandler     SVC_Handler\n',
                     '#define xPortPendSVHandler  PendSV_Handler\n',
                     '#define xPortSysTickHandler SysTick_Handler\n'):
            txt = txt.replace(line, '/* CubeMX 默认没有这一行 */\n')
        txt = re.sub(r'#define configTOTAL_HEAP_SIZE\s+.*',
                     '#define configTOTAL_HEAP_SIZE ((size_t)(8 * 1024))', txt)
        # 再加两项"CubeMX 默认值"：16 位 tick（65.5 秒回绕 -> 心跳/确认判据全乱）
        # 与开定时器（多出定时器任务+队列 -> 挤爆 12KB 堆）
        txt = re.sub(r'#define configUSE_16_BIT_TICKS\s+0',
                     '#define configUSE_16_BIT_TICKS                  1', txt)
        txt = re.sub(r'#define configUSE_TIMERS\s+0',
                     '#define configUSE_TIMERS                        1', txt)
        io.open(p, 'w', encoding='utf-8').write(txt)
    return root


# F：CubeMX「生成代码」把 main() 整段重写成了骨架（本项目 main() 里没有任何
#    USER CODE 标记，所以重新生成 = 整段被换掉；编译能过、烧进去什么都不做）
MAIN_SKELETON = """#include "main.h"

int main(void)
{
  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART1_UART_Init();
  MX_CAN_Init();
  MX_IWDG_Init();
  while (1)
  {
  }
}
"""

CASES = [
    ("A 只落地 main()、内务没合并（工程 main.c 的真实旧状态）", MAIN_NO_HK, True, 'repo'),
    ("B 内务也合并好", MAIN_OK, False, 'repo'),
    ("C 主循环里偷加一句 gw_log_tick", MAIN_STRAY, True, 'repo'),
    ("D 工程 FreeRTOSConfig.h 被换成 CubeMX 默认（无重定向 + 堆 8KB + 16 位 tick + 开定时器）",
     MAIN_OK, True, 'cube_default'),
]

# E：工程 main.c 还是 L0 上板验证版，却已经定义了宏
#    （用**真实的** L0 模板当合成 main.c —— 这样自证与现场一致）
_L0 = os.path.join(REPO, 'templates', 'main_L0_bringup.c')
if os.path.isfile(_L0):
    CASES.append(("E 工程 main.c 还是 L0 版却已定义 GW_USE_FREERTOS=1",
                  io.open(_L0, encoding='utf-8').read(), True, 'repo'))
CASES.append(("F 工程 main.c 被 CubeMX 生成代码冲成骨架", MAIN_SKELETON, True, 'repo'))

# G：CubeMX 生成 FreeRTOS 代码时会新建 Core/Src/freertos.c，并按配置生成钩子；
#    而仓库在 port/gw_rtos.c 里已经定义了同样三个 -> multiple definition（缺陷 V 一类）
CUBE_FREERTOS_C = '''#include "FreeRTOS.h"
#include "task.h"

void vApplicationIdleHook(void) { }
'''
CASES.append(("G CubeMX 的 freertos.c 与仓库抢钩子", MAIN_OK, True, 'repo',
              {'Core/Src/freertos.c': CUBE_FREERTOS_C}))


def main():
    import check_landing as L

    bad = 0
    for case in CASES:
        title, main_c, want_bad, mode = case[:4]
        extra = case[4] if len(case) > 4 else None
        root = make(main_c, mode, extra)
        out_path = os.path.join(root, 'out.txt')
        rc = os.system('python "%s" --project "%s" > "%s" 2>&1'
                       % (os.path.join(HERE, 'check_landing.py'), root, out_path))
        out = io.open(out_path, encoding='utf-8', errors='replace').read()
        m = re.search(r'小结：(\d+) 项已就绪 / (\d+) 项待办 / \*\*(\d+) 项矛盾\*\*', out)
        n_bad = int(m.group(3)) if m else -1

        print('=' * 70)
        print('自证：%s' % title)
        for line in out.split('\n'):
            if (('内务落地' in line) or ('主循环结构' in line)
                    or ('FreeRTOSConfig.h' in line) or ('小结：' in line)):
                print('   ' + line.strip())

        okc = ((n_bad > 0) == want_bad)
        print('  -> %s（期望 %s）%s'
              % ('矛盾 %d 项' % n_bad if n_bad > 0 else '无矛盾',
                 '矛盾' if want_bad else '无矛盾', '✅' if okc else '❌ 不符合预期'))
        if not okc:
            bad += 1
        shutil.rmtree(root, ignore_errors=True)

    print('=' * 70)
    print('自证结果：%d/%d 通过' % (len(CASES) - bad, len(CASES)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
