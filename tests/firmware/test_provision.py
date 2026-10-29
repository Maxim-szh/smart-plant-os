import csv
import json
import importlib.util
import stat
import tempfile
import unittest
from argparse import Namespace
from pathlib import Path

spec = importlib.util.spec_from_file_location("provision", Path(__file__).resolve().parents[2] / "tools/provision.py")
provision = importlib.util.module_from_spec(spec)
spec.loader.exec_module(provision)


class ProvisionTests(unittest.TestCase):
    def test_idf_country_codes_are_supported(self):
        self.assertIn("01", provision.IDF_COUNTRIES)
        self.assertNotIn("RU", provision.IDF_COUNTRIES)

    def test_web_credentials_are_individual_and_certificate_is_local(self):
        with tempfile.TemporaryDirectory() as directory:
            args = Namespace(hub_mac=provision.mac("02:00:00:00:00:01"), node_mac=[provision.mac("02:00:00:00:00:02")],
                country="RU", channel=6, pins=None, web=True, output=Path(directory) / "web")
            provision.generate(args)
            access = json.loads((args.output / "access.json").read_text())
            self.assertEqual(len(access["admin_password"]), 64)
            self.assertNotEqual(access["admin_password"], access["wifi_password"])
            self.assertEqual(stat.S_IMODE((args.output / "access.json").stat().st_mode), 0o600)
            with (args.output / "hub.csv").open() as source:
                rows = {row["key"]: row["value"] for row in csv.DictReader(source)}
            self.assertEqual(rows["admin"], access["admin_password"])
            self.assertIn("BEGIN CERTIFICATE", rows["cert"])
            self.assertNotIn("web,namespace", (args.output / "node-0.csv").read_text())

    def test_pairwise_keys_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            args = Namespace(hub_mac=provision.mac("02:00:00:00:00:01"), node_mac=[provision.mac("02:00:00:00:00:02"), provision.mac("02:00:00:00:00:03")],
                country="RU", channel=6, pins=None, output=Path(directory) / "installation")
            provision.generate(args)

            def read(name):
                file = args.output / (name + ".csv")
                self.assertEqual(stat.S_IMODE(file.stat().st_mode), 0o600)
                with file.open() as source:
                    return {row["key"]: row["value"] for row in csv.DictReader(source)}

            hub, node0, node1 = read("hub"), read("node-0"), read("node-1")
            self.assertEqual(hub["p0lmk"], node0["p0lmk"])
            self.assertEqual(hub["p1app"], node1["p0app"])
            self.assertNotEqual(hub["p0app"], hub["p1app"])
            self.assertNotEqual(hub["p0app"], hub["p0lmk"])
            self.assertEqual(node0["p0id"], hub["device"])
            self.assertNotIn("pump_pin", node0)
            with self.assertRaises(FileExistsError):
                provision.generate(args)

    def test_reject_unsafe_pin_assignments(self):
        for value in ("2,3,4", "0,0,4", "0,18,4", "0,3,8", "5,3,4"):
            with self.assertRaises(Exception):
                provision.pins(value)


if __name__ == "__main__":
    unittest.main()
