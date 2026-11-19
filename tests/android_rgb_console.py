"""Exercise the visible RGB console on the same Android installation as the boot smoke test."""
import os
from pathlib import Path
import re
import subprocess
import sys
import time
from xml.etree import ElementTree

root = Path(os.environ["RUNNER_TEMP"]) / "reverse-windows-x86_64"
resource = os.environ["ANDROID_APP_ID"] + ":id/"

def adb(*args):
    return subprocess.check_output(["adb", *args])

def nodes():
    adb("shell", "uiautomator", "dump", "/sdcard/prime-rgb.xml")
    return list(ElementTree.fromstring(adb("exec-out", "cat", "/sdcard/prime-rgb.xml")).iter("node"))

def find(field, value):
    for _ in range(4):
        for node in nodes():
            if node.get(field) == value:
                return node
        time.sleep(.25)
    raise AssertionError(f"Control ausente: {field}={value}")

def tap(field, value):
    left, top, right, bottom = map(int, re.findall(r"-?\d+", find(field, value).get("bounds")))
    assert right > left and bottom > top
    adb("shell", "input", "tap", str((left+right)//2), str((top+bottom)//2))

def capture(name):
    (root / name).write_bytes(adb("exec-out", "screencap", "-p"))

def settings():
    tap("resource-id", resource+"vm_console_menu")
    tap("text", "Configuración")

if sys.argv[1] == "key":
    # Press a visible physical key and let the caller check that the guest actually halts.
    tap("text", "Ñ")
elif sys.argv[1] == "console":
    tree=nodes()
    for id_ in ("vm_console_menu", "vm_touchpad", "vm_left_click", "vm_right_click"):
        assert any(n.get("resource-id")==resource+id_ for n in tree),id_
    for name in ("Ñ","Esc","F12","Bloq\nNum","Alt Gr","Copiar","Pegar"):
        assert any(n.get("text")==name for n in tree),name
    caps=[n for n in tree if n.get("class")=="android.widget.Button"]
    assert len(caps)==111, len(caps)  # 105 physical keys, 3 shortcuts, menu, 2 clicks.
    capture("consola-rgb-completa.png")
    tap("text","ABC")
    find("text","Completo");capture("consola-rgb-pulgares.png")
    tap("text","Núm");find("text","Núm")
    tap("text","Fn");find("text","F12")
    tap("text","Completo");find("text","Alt Gr")
    tap("resource-id",resource+"vm_console_menu");tap("text","Métricas")
    find("text","Métricas");capture("consola-rgb-metricas.png")
    tap("text","‹  Consola");settings()
    find("resource-id",resource+"vm_copy_diagnostic")
    print("PRIME_RGB_CONSOLE_PASS physical_keys=105 thumb_modes=3 pointer_buttons=2 metrics=1 persisted_diagnostic=1")
elif sys.argv[1] == "settings":
    settings()
