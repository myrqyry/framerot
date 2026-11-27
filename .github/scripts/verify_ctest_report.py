"""Require a complete, passing CTest JUnit report."""
import sys
from pathlib import Path
import re
import xml.etree.ElementTree as ET


def required_names(profile):
    cmake = (Path(__file__).resolve().parents[2] / "CMakeLists.txt").read_text()
    names = set(re.findall(r"add_test\(NAME\s+(\S+)", cmake))
    if profile == "native":
        names = {n for n in names if not n.startswith("mosh_obs_")}
    if len(names) != {"native": 12, "linux": 18}[profile]:
        raise ValueError("Required test inventory changed; review the qualification floor")
    return names


def verify(root, minimum, required=()):
    cases = root.findall(".//testcase")
    names = [c.get("name") for c in cases]
    if len(cases) < minimum or None in names or len(set(names)) != len(cases):
        raise ValueError("Missing or duplicate test results")
    if set(required) - set(names):
        raise ValueError("Missing required test names: " + str(sorted(set(required) - set(names))))
    for case in cases:
        if case.get("status") != "run" or any(
            case.find(tag) is not None for tag in ("skipped", "failure", "error")
        ):
            raise ValueError(f"Test did not pass: {case.get('name')}")
    return len(cases)


if __name__ == "__main__":
    try:
        print(f"Verified {verify(ET.parse(sys.argv[1]).getroot(), int(sys.argv[2]), required_names(sys.argv[3]) if len(sys.argv)>3 else ())} passing tests")
    except (ValueError, ET.ParseError, OSError) as exc:
        sys.exit(str(exc))
