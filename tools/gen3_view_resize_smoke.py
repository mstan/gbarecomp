#!/usr/bin/env python3
"""Windows test: resize this experiment's own hidden SDL window and check scanout.

Accepts the same --exe/--bios/--rom/--state/--output/--toolchain paths as
widescreen_smoke.py. Does not send input to or resize other applications.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import time


def main():
    if os.name != "nt":
        raise SystemExit("This window-system test requires Windows")
    p=argparse.ArgumentParser(description=__doc__)
    for name in ("exe", "bios", "rom", "state", "output", "catalog"):
        p.add_argument("--"+name, type=lambda s:Path(s).resolve(), required=True)
    p.add_argument("--toolchain", type=Path)
    p.add_argument("--portrait-only", action="store_true", help="Run the three narrow-window cases")
    p.add_argument("--ui-center",type=int,help="Native Start-window center Y; verify relocated live text and cursor")
    args=p.parse_args()
    root=args.output; root.mkdir(parents=True,exist_ok=True)
    exe=root/args.exe.name
    shutil.copy2(args.exe,exe)
    for dll in args.exe.parent.glob("*.dll"):shutil.copy2(dll,root/dll.name)
    shutil.copytree(args.catalog,root/"mods",dirs_exist_ok=True)
    env=os.environ.copy()
    for key in list(env):
        if key.startswith("GBARECOMP_"):del env[key]
    env.update(GBARECOMP_STRICT_STATIC="1", RECOMP_RTC_EPOCH="1789261200", SDL_RENDER_DRIVER="software", SDL_AUDIODRIVER="dummy", GBARECOMP_DIAGNOSTICS="0", TEMP=str(root), TMP=str(root))
    if args.toolchain:env["PATH"]=str(args.toolchain)+os.pathsep+env["PATH"]
    user=C.WinDLL("user32",use_last_error=True)
    user.SetProcessDPIAware()
    callback_type=C.WINFUNCTYPE(W.BOOL,W.HWND,W.LPARAM)
    user.EnumWindows.argtypes=[callback_type,W.LPARAM]
    user.GetWindowThreadProcessId.argtypes=[W.HWND,C.POINTER(W.DWORD)]
    user.GetWindowLongW.argtypes=[W.HWND,C.c_int]
    user.AdjustWindowRectEx.argtypes=[C.POINTER(W.RECT),W.DWORD,W.BOOL,W.DWORD]
    user.SetWindowPos.argtypes=[W.HWND,W.HWND,C.c_int,C.c_int,C.c_int,C.c_int,W.UINT]
    user.ShowWindow.argtypes=[W.HWND,C.c_int]
    user.GetClientRect.argtypes=[W.HWND,C.POINTER(W.RECT)]
    user.GetClassNameW.argtypes=[W.HWND,W.LPWSTR,C.c_int]
    results=[]
    native_ui=None
    if args.ui_center is not None:
        (root/"mods"/"state.toml").write_text('''format_version = 1
[[package]]
id = "pokemon-gen3.enhancement.widescreen"
version = "1.0.0"
[[feature]]
package_id = "pokemon-gen3.enhancement.widescreen"
id = "widescreen"
enabled = false
''')
        with (root/"native-ui.log").open("w") as log:
            subprocess.run([str(exe),"--bios",str(args.bios),"--rom",str(args.rom),
                "--volume","0","--save-path",str(root/"test.sav"),"--load-state",str(args.state),
                "--no-window","--frames","180","--dump-png",str(root/"native-ui.png")],
                cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW,
                check=True,timeout=45)
        from PIL import Image
        native_ui=Image.open(root/"native-ui.png").convert("RGB").crop((192,16,224,64)).tobytes()
    cases=[("fit",960,540,284,160),("fit",1260,540,373,160),("fit",1920,540,569,160),
           ("fit",640,800,240,300),("fit",903,480,301,160),("21:9",960,540,373,160),
           ("fit",540,960,240,427),("fit",450,1000,240,533)]
    if args.portrait_only: cases=[case for case in cases if case[4]>160]
    for index,(aspect,width,height,expected,expected_height) in enumerate(cases):
        (root/"mods"/"state.toml").write_text(f'''format_version = 1
[[package]]
id = "pokemon-gen3.enhancement.widescreen"
version = "1.0.0"
[[feature]]
package_id = "pokemon-gen3.enhancement.widescreen"
id = "widescreen"
enabled = true
[feature.values]
aspect = "{aspect}"
''')
        shot=root/f"resize-{index}.png"
        with (root/f"resize-{index}.log").open("w") as log:
            process=subprocess.Popen([str(exe),"--bios",str(args.bios),"--rom",str(args.rom),
                "--volume","0","--save-path",str(root/"test.sav"),"--load-state",str(args.state),
                "--window","--frames","180","--dump-png",str(shot)],cwd=root,env=env,
                stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                handles=[]
                @callback_type
                def enum(hwnd,_):
                    pid=W.DWORD();user.GetWindowThreadProcessId(hwnd,C.byref(pid))
                    name=C.create_unicode_buffer(128);user.GetClassNameW(hwnd,name,len(name))
                    if pid.value==process.pid and name.value=="SDL_app":handles.append(hwnd)
                    return True
                deadline=time.monotonic()+15
                while not handles:
                    if process.poll() is not None:raise RuntimeError("game exited before window creation")
                    user.EnumWindows(enum,0)
                    if time.monotonic()>deadline:raise RuntimeError("SDL window not found")
                    time.sleep(.02)
                hwnd=handles[0];user.ShowWindow(hwnd,0)
                # SDL creates its renderer after its HWND. Resizing before
                # that initialization can precede the first size event.
                time.sleep(.3)
                rect=W.RECT(0,0,width,height)
                user.AdjustWindowRectEx(C.byref(rect),user.GetWindowLongW(hwnd,-16),False,user.GetWindowLongW(hwnd,-20))
                if not user.SetWindowPos(hwnd,None,0,0,rect.right-rect.left,rect.bottom-rect.top,0x16):
                    raise C.WinError(C.get_last_error())
                client=W.RECT();user.GetClientRect(hwnd,C.byref(client))
                process.wait(timeout=30)
                if process.returncode:raise RuntimeError(f"game exited {process.returncode}")
            finally:
                if process.poll() is None:process.terminate();process.wait(timeout=10)
        actual=struct.unpack(">II",shot.read_bytes()[16:24])
        result=dict(aspect=aspect,requested=[width,height],client=[client.right,client.bottom],scanout=list(actual),expected=expected)
        results.append(result)
        if actual!=(expected,expected_height):raise AssertionError(result)
        from PIL import Image
        image=Image.open(shot).convert("RGB")
        left=(expected-240)//2;top=(expected_height-160)//2
        margin_pixels=sum(any(image.getpixel((x,y))) for y in range(expected_height) for x in range(expected)
                          if x<left or x>=left+240 or y<top or y>=top+160)
        result["nonblack_margin_pixels"]=margin_pixels
        if native_ui is not None:
            right=expected-240-left;bottom=expected_height-160-top
            dy=80+(bottom-top)//2-args.ui_center if top>0 else 0
            dest=image.crop((192+left+right,16+top+dy,224+left+right,64+top+dy)).tobytes()
            if dest!=native_ui:raise AssertionError(f"relocated menu/cursor pixels changed: {result}")
            result["ui_matching_channels"]=len(dest)
        if not margin_pixels:raise AssertionError("expanded raster contains no world scenery")
        text=(root/f"resize-{index}.log").read_text()
        if "[gen3:view] verified-field" not in text:raise AssertionError("field renderer never verified")
        if "self_heal_coverage=FULLY_STATIC" not in text:raise AssertionError("coverage report missing")
    (root/"report.json").write_text(json.dumps(results,indent=2)+"\n")
    print(json.dumps(results))


if __name__=="__main__":main()
