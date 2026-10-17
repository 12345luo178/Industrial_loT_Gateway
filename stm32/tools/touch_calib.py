#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""触摸标定计算器：把 DIAG 页抄来的 TOUCH RAW 数值换算成 touch_set_calib(...) 一行。

为什么需要：XPT2046 的原始值（0..4095）与屏幕像素之间是线性映射，但每个面板、
每种接线都不一样；更坑的是**轴反了**的时候，正确做法是改 invert 标志而不是
把 min/max 对调——后者会把线性度调坏，症状是"四角准、中间偏"。
本工具复刻 port/touch_xpt2046.c 里 touch_get() 的**同一套公式**（含钳位），
所以算出来的常数拿去用不会有偏差。

用法：
  1) 上板跑 UI，进 DIAG 页，按住**显示区左上角**抄下 TOUCH RAW 的 x,y；
  2) 按住**右下角**再抄一组；
  3) python tools/touch_calib.py --tl 380,420 --br 3820,3760
     （顺序反了/上下反了它会在"诊断"里直接告诉你该加 --invert-x / --invert-y）

自检：python tools/touch_calib.py --self-test
"""
import argparse
import sys

try:
    sys.stdout.reconfigure(errors="replace")
except Exception:
    pass

LCD_W, LCD_H = 320, 240


def scale(v, lo, hi, out_max):
    """与 port/touch_xpt2046.c 的 scale() 逐字一致（含钳位、hi<=lo 返回 0）。"""
    if hi <= lo:
        return 0
    r = ((v - lo) * out_max) // (hi - lo)
    if r < 0:
        r = 0
    if r > out_max:
        r = out_max
    return r


def map_xy(raw_x, raw_y, x_min, x_max, y_min, y_max,
           swap_xy=False, invert_x=False, invert_y=False):
    rx, ry = (raw_y, raw_x) if swap_xy else (raw_x, raw_y)
    x = scale(rx, x_min, x_max, LCD_W - 1)
    y = scale(ry, y_min, y_max, LCD_H - 1)
    if invert_x:
        x = (LCD_W - 1) - x
    if invert_y:
        y = (LCD_H - 1) - y
    return x, y


def self_test():
    checks = []

    def ck(cond, what):
        checks.append((bool(cond), what))

    # 合成一个"面板"：左上角原始 (380,420)，右下角 (3820,3760)
    tl = (380, 420)
    br = (3820, 3760)
    x_min, x_max = tl[0], br[0]
    y_min, y_max = tl[1], br[1]

    x, y = map_xy(*tl, x_min, x_max, y_min, y_max)
    ck((x, y) == (0, 0), "左上角原始值应映射到 (0,0)，实得 %s" % ((x, y),))
    x, y = map_xy(*br, x_min, x_max, y_min, y_max)
    ck((x, y) == (319, 239), "右下角原始值应映射到 (319,239)，实得 %s" % ((x, y),))
    # 中点线性
    x, y = map_xy((tl[0] + br[0]) // 2, (tl[1] + br[1]) // 2,
                  x_min, x_max, y_min, y_max)
    ck(abs(x - 159) <= 1 and abs(y - 119) <= 1,
       "中点应接近 (159,119)，实得 %s" % ((x, y),))
    # 钳位：超出范围不越界
    x, y = map_xy(0, 0, x_min, x_max, y_min, y_max)
    ck((x, y) == (0, 0), "低于下限应钳到 (0,0)，实得 %s" % ((x, y),))
    x, y = map_xy(4095, 4095, x_min, x_max, y_min, y_max)
    ck((x, y) == (319, 239), "高于上限应钳到 (319,239)，实得 %s" % ((x, y),))
    # invert 的语义：左右互换
    x1, _ = map_xy(tl[0], tl[1], x_min, x_max, y_min, y_max)
    x2, _ = map_xy(tl[0], tl[1], x_min, x_max, y_min, y_max, invert_x=True)
    ck(x1 == 0 and x2 == 319, "invert_x 应把 0 变成 319，实得 %d/%d" % (x1, x2))
    # swap：驱动里是"先交换原始值，再用 x_min/x_max 约束交换后的 X"
    #（见 touch_xpt2046.c 的 touch_get()：s_swap 交换 rx/ry 之后才 scale(rx, s_x_min...))
    # 所以边界**不跟着换**，只有输入的两个原始值互换。
    xs, ys = map_xy(tl[1], tl[0], x_min, x_max, y_min, y_max, swap_xy=True)
    ck((xs, ys) == (0, 0), "swap_xy 后左上仍应是 (0,0)，实得 %s" % ((xs, ys),))

    bad = [w for ok, w in checks if not ok]
    for ok, w in checks:
        print("  [%s] %s" % ("OK" if ok else "FAIL", w))
    print("通过 %d 项, 失败 %d 项" % (len(checks) - len(bad), len(bad)))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description="触摸标定计算器（复刻驱动里的映射公式）")
    ap.add_argument("--tl", help="显示区左上角的原始值，形如 380,420")
    ap.add_argument("--br", help="显示区右下角的原始值，形如 3820,3760")
    ap.add_argument("--swap", action="store_true", help="交换 X/Y 轴")
    ap.add_argument("--invert-x", action="store_true")
    ap.add_argument("--invert-y", action="store_true")
    ap.add_argument("--self-test", action="store_true", help="离线自检（不连板子）")
    a = ap.parse_args()

    if a.self_test:
        return self_test()
    if not a.tl or not a.br:
        print("  需要 --tl 与 --br（或 --self-test）")
        return 2

    tl = tuple(int(v) for v in a.tl.split(","))
    br = tuple(int(v) for v in a.br.split(","))

    # 轴方向诊断：**这才是最容易调坏的地方**
    print("  抄到的两组数：左上 %s  右下 %s" % (tl, br))
    hint = []
    if tl[0] > br[0]:
        hint.append("X 轴原始值随屏幕向右而**减小** => 应加 --invert-x（不要对调 min/max）")
    if tl[1] > br[1]:
        hint.append("Y 轴原始值随屏幕向下而**减小** => 应加 --invert-y（不要对调 min/max）")
    if hint:
        for h in hint:
            print("  ⚠️ " + h)
    else:
        print("  ✓ 两个轴的方向都正常（不需要 invert）")

    x_min, x_max = min(tl[0], br[0]), max(tl[0], br[0])
    y_min, y_max = min(tl[1], br[1]), max(tl[1], br[1])

    print()
    print("  写进 port/touch_xpt2046.c 的默认常数：")
    print("    x_min = %u, x_max = %u, y_min = %u, y_max = %u" % (x_min, x_max, y_min, y_max))
    print()
    print("  或运行时调用（把 0/1 按上面的诊断结果填）：")
    print("    touch_set_calib(%uu, %uu, %uu, %uu, %d, %d, %d);"
          % (x_min, x_max, y_min, y_max,
             1 if a.swap else 0, 1 if a.invert_x else 0, 1 if a.invert_y else 0))
    print()
    print("  用这套常数复核（应与上面两组数对应）：")
    for name, r in (("左上", tl), ("右下", br)):
        sx, sy = map_xy(r[0], r[1], x_min, x_max, y_min, y_max, a.swap,
                        a.invert_x, a.invert_y)
        print("    %s 原始 %s -> 屏幕 (%d,%d)" % (name, r, sx, sy))
    return 0


if __name__ == "__main__":
    sys.exit(main())
