import importlib.util
import os
from pathlib import Path
import plistlib
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,os.environ["OBS_CI_SCRIPTS"])
spec=importlib.util.spec_from_file_location("hook",Path(__file__).with_name("ci-native.py"))
hook=importlib.util.module_from_spec(spec);spec.loader.exec_module(hook)
class NativePackage(unittest.TestCase):
    def test_all_layouts_and_notices(self):
        for target in ("windows-x64","macos-arm64","macos-x86_64"):
            for version in ("32.2.0","33.0.3"):
                with self.subTest(target=target,version=version),tempfile.TemporaryDirectory() as tmp:
                    work=Path(tmp);installed=work/"installed";stage=work/"payload"
                    if target.startswith("macos"):
                        binary=installed/"framerot.plugin/Contents/MacOS/framerot"
                        binary.parent.mkdir(parents=True)
                        (binary.parent.parent/"Info.plist").write_bytes(plistlib.dumps({"CFBundleExecutable":"framerot","CFBundlePackageType":"BNDL"}))
                    else:
                        binary=installed/("bin/64bit" if version=="32.2.0" else "")/"framerot.dll"
                        binary.parent.mkdir(parents=True)
                    binary.write_bytes(b"module")
                    result=hook.stage_payload(ROOT,installed,stage,target,version)
                    self.assertEqual(result.read_bytes(),b"module")
                    _,data,notice=hook.layout(stage,"framerot",target,version)
                    self.assertEqual(notice.with_name("COPYING").read_bytes(),(ROOT/"COPYING").read_bytes())
                    self.assertEqual((data/"OBS_SDK_VERSION").read_text().strip(),version)
                    for name in ("README.md","docs/guide.md","docs/technical.md","docs/development.md"):
                        self.assertEqual((data/name).read_bytes(),(ROOT/name).read_bytes())
if __name__=="__main__":unittest.main()
