"""Shared SDK native caller. Run real codec tests before staging the OBS module."""
import os
import shutil
import subprocess
import sys
from native_caller import layout, verify_payload
from native_sdk import dependency_dirs
MODULE="framerot"
DOCS=("README.md","docs/guide.md","docs/technical.md","docs/development.md")

def configure(root, work, target, version):
    deps, _ = dependency_dirs(work/f"obs-studio-{version}",target,version)
    return ["-DFRAMEROT_CODEC_ONLY=OFF","-DFRAMEROT_OBS_TESTS=OFF",
            "-DFRAMEROT_PACKAGE_LAYOUT="+("legacy" if version=="32.2.0" else "modern"),
            "-DRETRO_FFMPEG_ROOT="+deps.as_posix(),
            "-DPython3_EXECUTABLE="+sys.executable.replace("\\","/")]

def stage_payload(root, installed, payload, target, version):
    binary,data,notice=layout(payload,MODULE,target,version)
    if target.startswith("macos-"):
        shutil.copytree(installed/"framerot.plugin",payload/"framerot.plugin")
    else:
        original=installed/("bin/64bit" if version=="32.2.0" else "")/"framerot.dll"
        binary.parent.mkdir(parents=True)
        shutil.copy2(original,binary)
    data.mkdir(parents=True,exist_ok=True)
    assets={name:(root/name).read_bytes() for name in DOCS}
    assets["OBS_SDK_VERSION"]=(version+"\n").encode()
    for name,content in assets.items():
        dest=data/name;dest.parent.mkdir(parents=True,exist_ok=True)
        dest.write_bytes(content)
    license_bytes=(root/"COPYING").read_bytes()
    notice.with_name("COPYING").write_bytes(license_bytes)
    return verify_payload(payload,MODULE,target,version,assets,license_bytes,license_name="COPYING")

def stage(root, installed, payload, target, version):
    work=installed.parent
    deps,_=dependency_dirs(work/f"obs-studio-{version}",target,version)
    env=os.environ.copy()
    if target=="windows-x64":
        env["PATH"]=os.pathsep.join([str(deps/"bin"),str(deps/"lib"),env.get("PATH","")])
    else:
        env["DYLD_LIBRARY_PATH"]=str(deps/"lib")
    report=work/"native-ctest.xml"
    subprocess.run(["ctest","--test-dir",str(work/"build"),"-C","RelWithDebInfo",
                    "--no-tests=error","--output-on-failure","--output-junit",str(report)],
                   check=True,env=env)
    subprocess.run([sys.executable,str(root/".github/scripts/verify_ctest_report.py"),
                    str(report),"12","native"],check=True)
    return stage_payload(root,installed,payload,target,version)
