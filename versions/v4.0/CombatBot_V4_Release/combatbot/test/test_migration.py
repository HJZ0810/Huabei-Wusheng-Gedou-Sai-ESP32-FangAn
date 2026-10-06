"""验证真实离线转换器的单位转换与拒绝路径，不连接设备。"""
import importlib.util
import json
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("migration", Path(__file__).resolve().parents[1] /
                                           "scripts/convert_workbuddy_config.py")
migration = importlib.util.module_from_spec(spec)
spec.loader.exec_module(migration)


class MigrationTests(unittest.TestCase):
    def test_semantics_and_units(self):
        result, notes = migration.convert({"wheelDia": 72, "irAlarm": 25, "servoMin": 500,
                                           "servoMax": 2500, "servoUp": 1500,
                                           "servoDown": 500, "grayInvert": True,
                                           "e18ActiveLow": True, "gyroInvert": True,
                                           "safetyIR": True, "vKp": .01, "gzBias": .1})
        self.assertEqual(result["wheelMm"], 72)
        self.assertEqual(result["servoUpDeg"], 90)
        self.assertEqual(result["servoDownDeg"], 0)
        self.assertFalse(result["grayActiveHigh"])
        self.assertFalse(result["e18ActiveHigh"])
        self.assertEqual(result["gyroSign"], -1)
        self.assertEqual(result["gyroBias"], 0)
        self.assertTrue(result["safetyEnabled"])
        self.assertGreater(result["irSlowdownCm"], result["irThresholdCm"])
        self.assertNotIn("speedPid", result)
        self.assertTrue(any("vKp" in n for n in notes))

    def test_reject_ambiguous_or_invalid_values(self):
        for data in ({"wheelDia": True}, {"wheelDia": float("nan")}, {"hbTimeout": float("inf")},
                     {"safetyIR": 1}, {"invert": [False] * 3}, {"wheelMm": 72},
                     {"servoMin": 2000, "servoMax": 1000}, {"servoUp": 3000},
                     {"irAlarm": 10}, {"irAlarm": 150}):
            with self.subTest(data=data), self.assertRaises(ValueError):
                migration.convert(data)
        with self.assertRaises(ValueError):
            json.loads('{"wheelDia":72,"wheelDia":80}', object_pairs_hook=migration.unique_object)

    def test_slowdown_and_protection_compatibility(self):
        result, _ = migration.convert({"irAlarm": 145, "safetyIR": False, "safetyEdge": False})
        self.assertEqual(result["irSlowdownCm"], 150)
        self.assertFalse(result["safetyEnabled"])


if __name__ == "__main__":
    unittest.main()
