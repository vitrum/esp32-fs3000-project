#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
骑行气动测试后处理：由 ESP32 记录的 CSV 计算 CdA

输入列（固件 csv_logger 输出）：
    t_ms,powerW,cadenceRpm,speedMps,vAirMps,yawDeg,postureDeg,rhoKgM3,powerSrc
    （postureDeg 为可选列：姿态传感器未接入时旧格式数据仍可处理；
      powerSrc（0=无 1=BLE 2=ANT+）为可选列，脚本按列名读取，多出的列自动忽略，
      ANT+/BLE 功率计混用数据可直接处理）

物理模型（逐窗口）：
    P_total = P_aero + P_rr + P_grade + P_accel
    P_aero  = 0.5 * rho * CdA * v_air^2 * v_g * cos(yaw)
    CdA     = P_aero / (0.5 * rho * v_air^2 * v_g * cos(yaw))

用法：
    python3 cda_postprocess.py data.csv \
        --mass 75 --crr 0.004 --grade 0 --wheel-circ 2.105

输出：
    1) 窗口级 CdA 明细（cda_windowed.csv）
    2) 按偏航角分箱的中位数 CdA（终端打印 + cda_yaw_bins.csv）
    3) 按姿态分箱的 CdA 对比（postureDeg < 阈值=气动姿势 vs ≥ 阈值=直立姿势，
       终端打印 + cda_posture_bins.csv；无 postureDeg 列时自动跳过）
    4) 总体中位数/均值
"""
import argparse
import csv
import math
import statistics


def parse_args():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("csv", help="ESP32 记录的 CSV 路径")
    p.add_argument("--mass", type=float, default=75.0,
                   help="车手+车总质量 kg（默认 75）")
    p.add_argument("--crr", type=float, default=0.004,
                   help="滚动阻力系数 Crr（默认 0.004，胎压/路面不同需标定）")
    p.add_argument("--grade", type=float, default=0.0,
                   help="坡度（小数，默认 0；平路填 0，有 GPS 高程可逐窗口传入）")
    p.add_argument("--min-v", type=float, default=3.0,
                   help="有效窗口最低车速 m/s（默认 3）")
    p.add_argument("--min-power", type=float, default=30.0,
                   help="有效窗口最低功率 W（默认 30）")
    p.add_argument("--max-yaw", type=float, default=45.0,
                   help="最大有效偏航角 °（探针测角范围约 ±45°，默认 45）")
    p.add_argument("--posture-threshold", type=float, default=15.0,
                   help="姿态分箱阈值 °：postureDeg < 阈值 视为气动姿势，≥ 阈值 视为直立（默认 15）")
    p.add_argument("--out-prefix", default="cda",
                   help="输出文件前缀（默认 cda）")
    return p.parse_args()


def load_rows(path):
    rows = []
    with open(path, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            try:
                rows.append({k: float(v) for k, v in r.items()})
            except (ValueError, KeyError):
                continue
    if not rows:
        raise SystemExit("错误：CSV 为空或列名不匹配")
    return rows


def compute_windows(rows, args):
    g = 9.80665
    grade = math.tan(math.atan(args.grade))  # 小数坡度直接使用
    out = []
    prev = None
    for r in rows:
        vg = r["speedMps"]
        a = 0.0
        if prev is not None:
            dt = max((r["t_ms"] - prev["t_ms"]) / 1000.0, 0.01)
            a = (vg - prev["speedMps"]) / dt
        prev = r

        vair = r["vAirMps"]
        yaw = math.radians(r["yawDeg"])
        power = r["powerW"]
        rho = r.get("rhoKgM3", 1.225)

        # 有效性筛选：低速/低功率/视风过小/大偏航角的数据不可信
        if vg < args.min_v or power < args.min_power or vair < 1.0:
            continue
        if abs(r["yawDeg"]) > args.max_yaw:
            continue
        cos_yaw = math.cos(yaw)
        if cos_yaw < 0.3:
            continue

        p_rr = args.crr * args.mass * g * vg
        p_g  = args.mass * g * grade * vg
        p_a  = args.mass * a * vg
        p_aero = power - p_rr - p_g - p_a

        denom = 0.5 * rho * vair * vair * vg * cos_yaw
        if denom <= 0:
            continue
        cda = p_aero / denom
        if cda <= 0:               # 物理上 CdA 必须为正，负值多为异常窗口
            continue

        out.append({
            "t_ms": r["t_ms"],
            "powerW": power,
            "speedMps": vg,
            "vAirMps": vair,
            "yawDeg": r["yawDeg"],
            "rhoKgM3": rho,
            "cda_m2": cda,
        })
        if "postureDeg" in r:
            out[-1]["postureDeg"] = r["postureDeg"]
    return out


YAW_BINS = [(0, 2.5), (2.5, 5), (5, 10), (10, 15), (15, 20), (20, 90)]


def bin_stats(windows):
    stats = []
    for lo, hi in YAW_BINS:
        vals = [w["cda_m2"] for w in windows if lo <= abs(w["yawDeg"]) < hi]
        if len(vals) < 3:
            continue
        stats.append({
            "yaw_lo": lo, "yaw_hi": hi, "n": len(vals),
            "median": statistics.median(vals),
            "mean": statistics.mean(vals),
            "stdev": statistics.pstdev(vals),
        })
    return stats


def posture_bin_stats(windows, threshold):
    """按姿态阈值分成两组：< 阈值 = 气动姿势，≥ 阈值 = 直立姿势"""
    aero = [w["cda_m2"] for w in windows if w.get("postureDeg", float("inf")) < threshold]
    upright = [w["cda_m2"] for w in windows if w.get("postureDeg", float("inf")) >= threshold]
    return aero, upright


def main():
    args = parse_args()
    rows = load_rows(args.csv)
    wins = compute_windows(rows, args)
    if not wins:
        raise SystemExit("错误：没有通过有效性筛选的窗口，请检查数据/参数")

    prefix = args.out_prefix
    with open(prefix + "_windowed.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(wins[0].keys()))
        w.writeheader()
        w.writerows(wins)

    stats = bin_stats(wins)
    with open(prefix + "_yaw_bins.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["yaw_lo", "yaw_hi", "n", "median", "mean", "stdev"])
        w.writeheader()
        w.writerows(stats)

    all_vals = [x["cda_m2"] for x in wins]
    print(f"\n有效窗口数: {len(wins)}")
    print(f"CdA 总体中位数: {statistics.median(all_vals):.4f} m²")
    print(f"CdA 总体均值  : {statistics.mean(all_vals):.4f} m² (±{statistics.pstdev(all_vals):.4f})")
    print(f"\n{'偏航角(°)':<12}{'样本数':<8}{'CdA中位数(m²)':<16}{'均值':<10}{'标准差'}")
    for s in stats:
        print(f"{s['yaw_lo']:>3.1f}~{s['yaw_hi']:<5.1f}   {s['n']:<8}"
              f"{s['median']:<16.4f}{s['mean']:<10.4f}{s['stdev']:.4f}")

    # 姿态分箱（仅当数据含 postureDeg 列）
    if "postureDeg" in wins[0]:
        aero, upright = posture_bin_stats(wins, args.posture_threshold)
        print(f"\n姿态分箱（阈值 {args.posture_threshold:g}°，<阈值=气动姿势）")
        if len(aero) < 3 or len(upright) < 3:
            print("  样本不足（每组需 ≥3 个窗口），跳过姿态对比")
        else:
            med_a, med_u = statistics.median(aero), statistics.median(upright)
            diff = (med_u - med_a) / med_a * 100.0 if med_a > 0 else float("nan")
            print(f"  气动姿势: n={len(aero):<5} 中位数 {med_a:.4f} m²  均值 {statistics.mean(aero):.4f}")
            print(f"  直立姿势: n={len(upright):<5} 中位数 {med_u:.4f} m²  均值 {statistics.mean(upright):.4f}")
            print(f"  CdA 差异: 直立比气动高 {diff:.1f}%")
            with open(prefix + "_posture_bins.csv", "w", newline="") as f:
                w = csv.writer(f)
                w.writerow(["group", "n", "median_cda_m2", "mean_cda_m2", "stdev"])
                w.writerow(["aero", len(aero), f"{med_a:.4f}",
                            f"{statistics.mean(aero):.4f}", f"{statistics.pstdev(aero):.4f}"])
                w.writerow(["upright", len(upright), f"{med_u:.4f}",
                            f"{statistics.mean(upright):.4f}", f"{statistics.pstdev(upright):.4f}"])
            print(f"  已写: {prefix}_posture_bins.csv")
    else:
        print("\n数据不含 postureDeg 列，跳过姿态分箱（旧格式 CSV 兼容）")

    print(f"\n明细已写: {prefix}_windowed.csv / {prefix}_yaw_bins.csv")
    print("提示: 对比两种姿势时，同路段/同条件各骑 2~3 圈，按偏航角分箱对比中位数。")


if __name__ == "__main__":
    main()
