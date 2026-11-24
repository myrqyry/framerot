"""Require a complete, passing CTest JUnit report."""
import sys
import xml.etree.ElementTree as ET


def verify(root, minimum):
    cases = root.findall(".//testcase")
    names = [c.get("name") for c in cases]
    if len(cases) < minimum or None in names or len(set(names)) != len(cases):
        raise ValueError("Missing or duplicate test results")
    for case in cases:
        if case.get("status") != "run" or any(
            case.find(tag) is not None for tag in ("skipped", "failure", "error")
        ):
            raise ValueError(f"Test did not pass: {case.get('name')}")
    return len(cases)


if __name__ == "__main__":
    try:
        print(f"Verified {verify(ET.parse(sys.argv[1]).getroot(), int(sys.argv[2]))} passing tests")
    except (ValueError, ET.ParseError, OSError) as exc:
        sys.exit(str(exc))
