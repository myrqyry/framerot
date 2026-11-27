import unittest
import xml.etree.ElementTree as ET
from verify_ctest_report import verify, required_names

def report():
    root = ET.Element("testsuite")
    for i in range(18):
        ET.SubElement(root, "testcase", name=f"test_{i}", status="run")
    return root

class ReportTests(unittest.TestCase):
    def test_complete(self):
        self.assertEqual(verify(report(), 18), 18)

    def test_rejects_wrong_named_case_at_same_count(self):
        for profile in ("linux", "native"):
            names = sorted(required_names(profile))
            root = ET.Element("testsuite")
            for name in names:
                ET.SubElement(root, "testcase", name=name, status="run")
            self.assertEqual(verify(root, len(names), names), len(names))
            root[0].set("name", "unrelated_test")
            with self.assertRaises(ValueError):
                verify(root, len(names), names)

    def test_rejects_skip(self):
        root = report()
        ET.SubElement(root[0], "skipped")
        with self.assertRaises(ValueError):
            verify(root, 18)

    def test_rejects_missing_duplicate_and_failure(self):
        for problem in ("missing", "duplicate", "failure", "notrun"):
            with self.subTest(problem=problem):
                root = report()
                if problem == "missing":
                    root.remove(root[0])
                elif problem == "duplicate":
                    root[0].set("name", root[1].get("name"))
                elif problem == "failure":
                    ET.SubElement(root[0], "failure")
                else:
                    root[0].set("status", "notrun")
                with self.assertRaises(ValueError):
                    verify(root, 18)

if __name__ == "__main__":
    unittest.main()
