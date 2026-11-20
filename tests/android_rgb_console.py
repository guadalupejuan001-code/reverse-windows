"""Exercise the visible RGB console on the same Android installation as the boot smoke test."""
import json
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

def bounds(node):
    return tuple(map(int, re.findall(r"-?\d+", node.get("bounds"))))

def tap(field, value):
    left, top, right, bottom = bounds(find(field, value))
    assert right > left and bottom > top
    adb("shell", "input", "tap", str((left+right)//2), str((top+bottom)//2))

def capture(name):
    (root / name).write_bytes(adb("exec-out", "screencap", "-p"))

def settings():
    tap("resource-id", resource+"vm_console_menu")
    tap("text", "Configuración")

def control(name):
    return find("resource-id", resource + name)

def toggle(name):
    tap("resource-id", resource + "vm_toggle_" + name)

def folded_display():
    visible = {n.get("resource-id"): n for n in nodes()}
    deck = bounds(visible[resource + "vm_console"])
    display = bounds(visible[resource + "vm_display"])
    menu = bounds(visible[resource + "vm_console_menu"])
    assert (display[1], display[3]) == (deck[1], deck[3]), (display, deck)
    assert display[0] == deck[0] and display[2] <= menu[0]
    assert display[2] - display[0] >= .8 * (deck[2] - deck[0]), (display, deck)
    for name in ("vm_keyboard", "vm_touchpad", "vm_left_click", "vm_right_click"):
        assert resource + name not in visible, name
    return display

def wait_register(index, expected):
    for _ in range(40):
        result=subprocess.run(["adb","exec-out","run-as",os.environ["ANDROID_APP_ID"],"cat","files/ultimo-diagnostico.json"],capture_output=True)
        try:
            report=json.loads(result.stdout)
            if report.get("source")=="iso" and int(report["machine"]["regs_hex"][index],16)==expected:
                return report
        except (KeyError,ValueError):
            pass
        time.sleep(.5)
    raise AssertionError(f"El invitado no alcanzó el registro {index} = {expected:x}")

if sys.argv[1] == "fixture":
    from c10_android_boot_iso import BOOT, SECTOR, generate
    binary=Path(sys.argv[2]).read_bytes()
    assert 0<len(binary)<510
    img=generate()
    sector=bytearray(512);sector[:len(binary)]=binary;sector[510:]=bytes([0x55,0xaa])
    img[BOOT*SECTOR:BOOT*SECTOR+512]=sector
    Path(sys.argv[3]).write_bytes(img)
elif sys.argv[1] == "pointer":
    wait_register(5,0x52504701)
    display = folded_display()
    assert bounds(control("vm_monitor")) == display
    toggle("pointer")
    l,t,r,b = bounds(control("vm_touchpad"))
    assert (l,t,r,b) == bounds(control("vm_display")) == display
    adb("shell","input","swipe",str((l+r)//2),str((t+b)//2),str((l+3*r)//4),str((3*t+b)//4),"190")
    tap("resource-id",resource+"vm_left_click")
    tap("resource-id",resource+"vm_right_click")
    report=wait_register(0,0x52474250)
    assert int(report["machine"]["regs_hex"][6],16)==31
    (root/"rgb-pointer-guest.json").write_text(json.dumps(report))
    print("PRIME_RGB_ANDROID_POINTER_PASS guest=real16 motion=1 left_down_up=1 right_down_up=1")
elif sys.argv[1] == "key":
    # Press a visible physical key and let the caller check that the guest actually halts.
    display = folded_display()
    assert bounds(control("vm_monitor")) == display
    toggle("keyboard")
    left, top, right, bottom = bounds(control("vm_monitor"))
    assert (left, top, right) == display[:3]
    assert bottom - top >= (display[3] - top) * 2 / 3
    tap("text", "Ñ")
elif sys.argv[1] == "console":
    display = folded_display()
    capture("consola-invitado.png")
    toggle("keyboard")
    dock = bounds(control("vm_keyboard"))
    reduced = bounds(control("vm_display"))
    assert reduced == (display[0], display[1], display[2], dock[1])
    assert dock == (display[0], reduced[3], display[2], display[3])
    assert 0 < dock[3] - dock[1] <= (display[3] - display[1]) // 3
    if not any(n.get("content-desc") == "Bloq Num" for n in nodes()):
        tap("text", "Completo")
    tree = list(control("vm_keyboard").iter("node"))
    for name in ("Ñ","Esc","F12","Bloq\nNum","Alt Gr","Copiar","Pegar"):
        assert any(" ".join(n.get("text","").split())==" ".join(name.split()) for n in tree),name
    caps=[n for n in tree if n.get("class")=="android.widget.Button"]
    assert len(caps)==108, len(caps)  # 105 physical keys and 3 shortcuts inside the dock.
    capture("consola-rgb-completa.png")
    tap("text","ABC")
    find("text","Completo");capture("consola-rgb-pulgares.png")
    tap("text","Núm");find("content-desc","Numérico 7")
    tap("text","Fn");find("text","F12")
    tap("text","Completo");find("text","Alt Gr")
    toggle("pointer")
    assert bounds(control("vm_touchpad")) == reduced
    control("vm_left_click")
    control("vm_right_click")
    toggle("keyboard")
    assert bounds(control("vm_touchpad")) == display
    adb("shell", "input", "keyevent", "KEYCODE_BACK")
    assert folded_display() == display
    tap("resource-id",resource+"vm_console_menu");tap("text","Registro")
    find("text","Registro del invitado")
    tap("text","‹  Consola")
    assert folded_display() == display
    tap("resource-id",resource+"vm_console_menu");tap("text","Métricas")
    find("text","Rendimiento");capture("consola-rgb-metricas.png")
    tap("text","‹  Consola");settings()
    find("resource-id",resource+"vm_copy_diagnostic")
    print("PRIME_RGB_CONSOLE_PASS guest_full_height=1 dock_max_third=1 physical_keys=105 thumb_modes=3 pointer_buttons=2 log=1 metrics=1 persisted_diagnostic=1")
elif sys.argv[1] == "settings":
    settings()
