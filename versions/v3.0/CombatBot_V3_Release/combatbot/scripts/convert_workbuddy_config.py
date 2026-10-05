"""
CombatBot Fusion · WorkBuddy 配置显式转换
============================================================================
职责：生成可供融合版导入的候选 JSON 与转换说明；不连接设备、不写 NVS。
边界：PID、旧标定零偏和无法对应的硬件策略不照搬，避免单位相同而行为不同。
使用：python scripts/convert_workbuddy_config.py old.json fusion.json
============================================================================
"""
import argparse
import json
import math
from pathlib import Path

ALIASES = {
    "apSsid": "apSsid", "apPass": "apPass", "staSsid": "staSsid", "staPass": "staPass",
    "staEnable": "staEnabled", "mdns": "hostname", "wheelDia": "wheelMm",
    "wheelBase": "wheelbaseMm", "track": "trackMm", "ppr": "ppr",
    "kOdo": "pulsesPerCm", "kTurn": "turnFactor", "maxSpeed": "maxSpeed",
    "defSpeed": "defaultSpeed", "accel": "acceleration", "decel": "deceleration",
    "maxOmega": "maxYawRate", "turnAccel": "turnAcceleration", "turnDecel": "turnDeceleration",
    "alpha": "fusionAlpha", "invert": "invert", "pwmDead": "pwmDeadzone",
    "scaleL": "leftTrim", "scaleR": "rightTrim", "irAlarm": "irThresholdCm",
    "safetyEdge": "edgeProtection", "safetyIR": "irProtection", "irScale": "irScale",
    "servoMin": "servoMinUs", "servoMax": "servoMaxUs", "servoCenter": "servoCenterUs",
    "hbTimeout": "heartbeatMs", "batLow": "batteryLowV", "stallTime": "stallTimeoutMs",
    "impactThresh": "impactThresholdG", "telemetryHz": "telemetryHz",
}
OMITTED = {
    "vKp": "速度环输出单位与控制组合不同，保留融合版 PID，重新调参",
    "vKi": "速度环输出单位与控制组合不同，保留融合版 PID，重新调参",
    "vKd": "速度环输出单位与控制组合不同，保留融合版 PID，重新调参",
    "pKp": "旧位置环字段未进入实际控制，不迁入新闭环",
    "pKi": "旧位置环字段未进入实际控制，不迁入新闭环",
    "pKd": "旧位置环字段未进入实际控制，不迁入新闭环",
    "hKp": "航向环控制组合不同，保留融合版 PID，重新调参",
    "hKi": "航向环控制组合不同，保留融合版 PID，重新调参",
    "hKd": "航向环控制组合不同，保留融合版 PID，重新调参",
    "gzBias": "旧零偏为 rad/s 且测量链不同，重置为 0 后执行融合版 IMU 标定",
    "pwmMax": "融合版通过速度上限控制目标，未提供旧 PWM 上限字段",
    "balLR": "旧左右平衡叠加语义不同，仅迁移 scaleL/scaleR",
    "batCrit": "电池化学体系与严重低压停车阈值未核实，不自动迁移",
    "lostBrake": "当前硬件只有撤 PWM，未实现独立主动制动",
    "captive": "融合版 AP 常驻门户，不迁移此旧开关",
    "calDist": "融合版标定流程自行定义动作长度，不作为运行配置迁入",
    "calTurnDeg": "融合版独立轮行程标定流程使用固定目标，不迁入",
}
SPECIAL = {"grayInvert", "e18ActiveLow", "gyroInvert", "servoUp", "servoDown"}
BOOL_SOURCE = {"staEnable", "safetyEdge", "safetyIR", "grayInvert", "e18ActiveLow", "gyroInvert",
               "lostBrake", "captive"}
STR_SOURCE = {"apSsid", "apPass", "staSsid", "staPass", "mdns"}


def unique_object(items):
    """拒绝重复键，避免同一文件在不同解析器中产生不同参数。"""
    result = {}
    for key, value in items:
        if key in result:
            raise ValueError(f"重复 JSON 键：{key}")
        result[key] = value
    return result


def convert(source):
    """返回部分配置候选及说明；设备仍须对合并后的完整配置做最终校验。"""
    if not isinstance(source, dict):
        raise ValueError("输入必须是 WorkBuddy 导出的 JSON 对象")
    unknown = set(source) - set(ALIASES) - set(OMITTED) - SPECIAL
    if unknown:
        raise ValueError("未识别的源字段：" + ", ".join(sorted(unknown)))
    for key, value in source.items():
        if key in BOOL_SOURCE:
            if not isinstance(value, bool):
                raise ValueError(f"{key} 必须为布尔值")
        elif key in STR_SOURCE:
            if not isinstance(value, str):
                raise ValueError(f"{key} 必须为字符串")
        elif key == "invert":
            if not isinstance(value, list) or len(value) != 4 or any(type(v) is not bool for v in value):
                raise ValueError("invert 必须为四个布尔值")
        elif type(value) not in (int, float) or not math.isfinite(value):
            raise ValueError(f"{key} 必须为有限数值")
    result = {new: source[old] for old, new in ALIASES.items() if old in source}
    notes = [f"{key}：{OMITTED[key]}" for key in source if key in OMITTED]
    for old, new in (("grayInvert", "grayActiveHigh"), ("e18ActiveLow", "e18ActiveHigh")):
        if old in source:
            result[new] = not source[old]
    if "gyroInvert" in source:
        result["gyroSign"] = -1 if source["gyroInvert"] else 1
    result["gyroBias"] = 0
    notes.append("导入后必须核对陀螺轴/方向并重新执行 IMU 零偏标定。")
    if "safetyEdge" in source or "safetyIR" in source:
        result["safetyEnabled"] = source.get("safetyEdge", False) or source.get("safetyIR", False)
        notes.append("已由旧边缘/IR开关推导安全总开关；倾倒与IR无效保护沿用融合版默认，需核对传感器。")
    if "irAlarm" in source:
        stop = source["irAlarm"]
        if not 20 <= stop < 150:
            raise ValueError("irAlarm 必须在 [20,150) cm，给渐进限速预留有效测距区间")
        result["irSlowdownCm"] = min(150, max(60, stop + 35))
    low, high = source.get("servoMin", 500), source.get("servoMax", 2500)
    if not 500 <= low < high <= 2500:
        raise ValueError("舵机脉宽要求 500 <= servoMin < servoMax <= 2500 µs")
    for old, new in (("servoUp", "servoUpDeg"), ("servoDown", "servoDownDeg")):
        if old in source:
            if not low <= source[old] <= high:
                raise ValueError(f"{old} 超出舵机脉宽范围")
            # 两版舵机均对端点线性插值；这里只转换目标位置，不改变端点单位。
            result[new] = (source[old] - low) * 180 / (high - low)
    notes.append("候选文件为部分更新：请先恢复融合版默认，再导入；未迁字段保持新默认。设备后端会再次校验范围。")
    return result, notes


def main():
    parser = argparse.ArgumentParser(description="离线转换 WorkBuddy 参数，不连接设备。")
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.input.resolve() == args.output.resolve():
        parser.error("输出必须与输入文件不同，保留原始导出")
    if args.output.exists() or args.output.with_suffix(".说明.md").exists():
        parser.error("输出文件已存在，请换一个新文件名")
    source = json.loads(args.input.read_text(encoding="utf-8-sig"), object_pairs_hook=unique_object)
    converted, notes = convert(source)
    args.output.write_text(json.dumps(converted, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    args.output.with_suffix(".说明.md").write_text("# WorkBuddy 配置转换说明\n\n" +
                                                 "\n".join("- " + item for item in notes) + "\n", encoding="utf-8")
    print("已生成候选 JSON 与说明；没有连接设备或修改 NVS。")


if __name__ == "__main__":
    main()
