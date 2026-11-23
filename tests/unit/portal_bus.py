"""The launcher's file chooser portal (src/kamite/portal_file_chooser.cpp) against a real session bus: a private
dbus-daemon with a fake org.freedesktop.portal.Desktop, and portal_pick_test, which goes through libdbus-1 as GHR does.

    python3 -I portal_bus.py path/to/portal_pick_test

Checks what OpenFile is sent (parent window, title, handle_token, modal, filters, directory, current_folder); that the
chosen file's URI comes back as its path (the document portal's, spaces escaped); that cancelling, a dialog ended
another way, an empty choice and a URI that is no local file all come back as cancelled (no second picker);
that a portal from before handle_token (answering on a path of its own) still gets through; that a Response from
anyone but the portal is ignored; and that no portal, a portal that errors, one that never replies, one that goes
away with the dialog open, and no bus at all come back as unavailable without hanging, and without libdbus starting a
bus of its own. Exit 77 (skipped) without dbus-daemon or jeepney, 1 instead when KAMITE_REQUIRE_ALL_TESTS=1.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

SKIP = 1 if os.environ.get("KAMITE_REQUIRE_ALL_TESTS") == "1" else 77

try:
    from jeepney import DBusAddress, HeaderFields, MessageType, new_error, new_method_return, new_signal
    from jeepney.bus_messages import message_bus
    from jeepney.io.blocking import open_dbus_connection
except ImportError:
    print("skipped: python3's jeepney is missing", file=sys.stderr)
    sys.exit(SKIP)

if not shutil.which("dbus-daemon"):
    print("skipped: dbus-daemon is missing", file=sys.stderr)
    sys.exit(SKIP)

TEST = sys.argv[1]
failures = 0
PORTAL = "org.freedesktop.portal.Desktop"
REQUEST = "org.freedesktop.portal.Request"


def check(ok, what):
    global failures
    if not ok:
        failures += 1
        print(f"FAIL: {what}", file=sys.stderr)


def response(path, code, results, destination=None):
    msg = new_signal(DBusAddress(path, interface=REQUEST), "Response", "ua{sv}", (code, results))
    if destination:
        msg.header.fields[HeaderFields.destination] = destination
    return msg


class Portal:
    """A desktop portal: owns org.freedesktop.portal.Desktop, answers FileChooser.OpenFile as `mode` says."""

    def __init__(self, address, mode="choose", uri="file:///run/user/1000/doc/3f2a/God%20Hand.iso", code=0,
                 spoofer=None):
        self.address, self.mode, self.uri, self.code, self.spoofer = address, mode, uri, code, spoofer
        self.calls = []
        self.conn = open_dbus_connection(bus=address)
        reply = self.conn.send_and_get_reply(message_bus.RequestName(PORTAL))
        assert reply.body[0] == 1, f"could not own {PORTAL}"
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        while True:
            try:
                msg = self.conn.receive()
            except Exception:
                return
            if msg.header.message_type != MessageType.method_call:
                continue
            fields = msg.header.fields
            sender = fields.get(HeaderFields.sender)
            self.calls.append((fields.get(HeaderFields.path), fields.get(HeaderFields.interface),
                               fields.get(HeaderFields.member), msg.body, sender))
            if self.mode == "stall":
                continue  # never answers: past the caller's reply timeout
            if self.mode == "error":
                self.conn.send(new_error(msg, "org.freedesktop.DBus.Error.UnknownMethod", "s", ("no FileChooser",)))
                continue
            token = msg.body[2].get("handle_token", ("s", ""))[1]
            expected = "/org/freedesktop/portal/desktop/request/" + sender[1:].replace(".", "_") + "/" + token
            path = "/org/freedesktop/portal/desktop/request/old_portal/7" if self.mode == "oldpath" else expected
            self.conn.send(new_method_return(msg, "o", (path,)))
            if self.mode == "stall_response":
                continue  # the dialog stays open
            time.sleep(0.2)  # the player choosing
            if self.mode == "vanish":
                self.conn.close()  # the portal dies with the dialog open
                return
            if self.spoofer:
                # Someone else on the bus answers first, broadcast and straight to GHR: both must be ignored.
                evil = {"uris": ("as", ["file:///home/deck/.ssh/id_rsa"])}
                self.spoofer.send(response(path, 0, evil))
                self.spoofer.send(response(path, 0, evil, destination=sender))
                time.sleep(0.2)
            results = {"uris": ("as", [self.uri] if self.uri is not None else [])}
            self.conn.send(response(path, self.code, results))


BUS_DIR = tempfile.mkdtemp(prefix="ghr-portal-bus-")
BUS_CONFIG = os.path.join(BUS_DIR, "session.conf")
BUS_TEMPLATE = f"""<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>{{listen}}</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
"""


def bus(listen=None):
    with open(BUS_CONFIG, "w") as handle:
        handle.write(BUS_TEMPLATE.replace("{listen}", listen or f"unix:dir={BUS_DIR}"))
    daemon = subprocess.Popen(["dbus-daemon", f"--config-file={BUS_CONFIG}", "--nofork", "--print-address=1"],
                              stdout=subprocess.PIPE, text=True)
    address = daemon.stdout.readline().strip()
    if not address:
        print(f"FAIL: dbus-daemon did not start (exit {daemon.wait()})", file=sys.stderr)
        sys.exit(1)
    return daemon, address


def pick(address, kind="disc", parent="x11:1a00004", folder="/home/deck/", timeout_ms=None, env_extra=None,
         limit=20):
    env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS=address)
    if env_extra is not None:
        env.update(env_extra)
        env = {k: v for k, v in env.items() if v is not None}
    args = [TEST, kind, parent, folder] + ([str(timeout_ms)] if timeout_ms else [])
    started = time.monotonic()
    try:
        run = subprocess.run(args, capture_output=True, text=True, env=env, timeout=limit)
    except subprocess.TimeoutExpired:
        return "hung", limit
    return run.stdout.strip(), time.monotonic() - started


DISC_FILTERS = [("PS2 disc image (*.iso, *.chd)", [(0, "*.iso"), (0, "*.ISO"), (0, "*.chd"), (0, "*.CHD")]),
                ("All files", [(0, "*")])]

# 1. A disc image chosen: what OpenFile is sent, and the document portal's path back, spaces unescaped.
daemon, address = bus()
p = Portal(address)
out, _ = pick(address)
check(out == "chosen /run/user/1000/doc/3f2a/God Hand.iso", f"chosen: {out!r}")
check(len(p.calls) == 1, f"OpenFile calls: {len(p.calls)}")
if p.calls:
    path, interface, member, body, _ = p.calls[0]
    check((path, interface, member) == ("/org/freedesktop/portal/desktop", "org.freedesktop.portal.FileChooser",
                                        "OpenFile"), f"the call: {p.calls[0][:3]}")
    check(body[0] == "x11:1a00004" and body[1] == "Choose your disc image (USA, SLUS-21503)", f"args: {body[:2]}")
    options = body[2]
    check(options.get("modal") == ("b", True), f"modal: {options.get('modal')}")
    check("directory" not in options, f"a file picker sent directory: {options.get('directory')}")
    token = options.get("handle_token", ("", ""))
    check(token[0] == "s" and token[1].startswith("ghr") and all(c.isalnum() or c == "_" for c in token[1]),
          f"handle_token: {token}")
    check(options.get("filters") == ("a(sa(us))", DISC_FILTERS), f"filters: {options.get('filters')}")
    check(options.get("current_folder") == ("ay", b"/home/deck/\0"), f"current_folder: {options.get('current_folder')}")
daemon.kill(); daemon.wait()

# 2. A folder (the texture pack): directory, no filters, no parent window.
daemon, address = bus()
p = Portal(address, uri="file:///home/deck/Textures/SLUS-21503")
out, _ = pick(address, kind="folder", parent="")
check(out == "chosen /home/deck/Textures/SLUS-21503", f"folder: {out!r}")
if p.calls:
    body = p.calls[0][3]
    check(body[0] == "" and body[2].get("directory") == ("b", True) and "filters" not in body[2],
          f"folder args: {body[0]!r} {body[2]}")
daemon.kill(); daemon.wait()

# 3. The zip filter.
daemon, address = bus()
p = Portal(address, uri="file:///home/deck/Downloads/mod.zip")
out, _ = pick(address, kind="zip")
check(out == "chosen /home/deck/Downloads/mod.zip", f"zip: {out!r}")
if p.calls:
    check(p.calls[0][3][2].get("filters") == ("a(sa(us))", [("Mod (*.zip)", [(0, "*.zip"), (0, "*.ZIP")]),
                                                            ("All files", [(0, "*")])]), "zip filters")
daemon.kill(); daemon.wait()

# 4. Cancelled (1), ended another way (2), chosen nothing, a URI that is no local file: all cancelled.
for label, kwargs, expect in [
        ("cancelled", dict(code=1), "cancelled"),
        ("ended another way", dict(code=2), "cancelled the dialog ended without a choice (response 2)"),
        ("no file", dict(uri=None), "cancelled the portal answered with no file"),
        ("no local file", dict(uri="https://example.com/a.iso"),
         "cancelled the chosen file is no local path: https://example.com/a.iso")]:
    daemon, address = bus()
    Portal(address, **kwargs)
    out, _ = pick(address)
    check(out == expect, f"{label}: {out!r}")
    daemon.kill(); daemon.wait()

# 5. A portal from before handle_token: it answers on a path of its own.
daemon, address = bus()
Portal(address, mode="oldpath", uri="file:///srv/God%20Hand.iso")
out, _ = pick(address)
check(out == "chosen /srv/God Hand.iso", f"old portal: {out!r}")
daemon.kill(); daemon.wait()

# 6. Someone else answers first (broadcast and straight to GHR) with another file: ignored, the portal's file wins.
daemon, address = bus()
spoofer = open_dbus_connection(bus=address)
Portal(address, spoofer=spoofer)
out, _ = pick(address)
check(out == "chosen /run/user/1000/doc/3f2a/God Hand.iso", f"spoofed answer taken: {out!r}")
spoofer.close()
daemon.kill(); daemon.wait()

# 7. No portal on the bus: unavailable at once (the launcher then tries zenity / kdialog).
daemon, address = bus()
out, took = pick(address)
check(out.startswith("unavailable OpenFile: org.freedesktop.DBus.Error.ServiceUnknown"), f"no portal: {out!r}")
check(took < 5, f"no portal took {took:.1f} s")
daemon.kill(); daemon.wait()

# 8. A portal without a FileChooser (an error reply): unavailable.
daemon, address = bus()
Portal(address, mode="error")
out, _ = pick(address)
check(out.startswith("unavailable OpenFile: org.freedesktop.DBus.Error.UnknownMethod"), f"error reply: {out!r}")
daemon.kill(); daemon.wait()

# 9. A portal that never replies to OpenFile: unavailable after the reply timeout, no hang.
daemon, address = bus()
Portal(address, mode="stall")
out, took = pick(address, timeout_ms=1500)
check(out.startswith("unavailable OpenFile: org.freedesktop.DBus.Error.NoReply"), f"stalled portal: {out!r}")
check(took < 10, f"stalled portal took {took:.1f} s")
daemon.kill(); daemon.wait()

# 10. The portal goes away with the dialog open: unavailable, no hang.
daemon, address = bus()
Portal(address, mode="vanish")
out, took = pick(address, limit=15)
check(out == "unavailable the portal went away while the dialog was open", f"vanished portal: {out!r}")
daemon.kill(); daemon.wait()

# 11. The bus itself goes away with the dialog open: unavailable, no hang.
daemon, address = bus()
Portal(address, mode="stall_response")  # replies, then never answers the dialog
threading.Timer(1.0, lambda: daemon.kill()).start()
out, took = pick(address, limit=15)
check(out == "unavailable the session bus went away while the dialog was open", f"dead bus: {out!r}")
daemon.wait()

# 12. No bus at the address.
out, _ = pick("unix:path=/nonexistent/ghr-portal-test")
check(out.startswith("unavailable no session bus ("), f"no bus: {out!r}")

# 13. No address and no $XDG_RUNTIME_DIR/bus: unavailable, and no dbus-launch started (X11 autolaunch).
empty = tempfile.mkdtemp()


def autolaunches():
    if not shutil.which("pgrep"):
        return None
    found = subprocess.run(["pgrep", "-f", "dbus-launch --autolaunch"], capture_output=True, text=True).stdout
    return set(found.split())


before = autolaunches()
out, _ = pick("", env_extra={"DBUS_SESSION_BUS_ADDRESS": None, "XDG_RUNTIME_DIR": empty, "DISPLAY": ":65000"})
check(out.startswith("unavailable no session bus ($DBUS_SESSION_BUS_ADDRESS unset"), f"no address: {out!r}")
after = autolaunches()
check(before is None or after <= before, "a dbus-launch was started")

# 13b. No address, and $XDG_RUNTIME_DIR/bus is no socket (a plain file): libdbus would skip it and autolaunch, so GHR
#      must refuse before libdbus is asked (its own message, not a dbus-launch error).
not_socket = tempfile.mkdtemp()
open(os.path.join(not_socket, "bus"), "w").close()
out, _ = pick("", env_extra={"DBUS_SESSION_BUS_ADDRESS": None, "XDG_RUNTIME_DIR": not_socket, "DISPLAY": ":65000"})
check(out.startswith("unavailable no session bus ($DBUS_SESSION_BUS_ADDRESS unset"), f"bus that is no socket: {out!r}")

# 14. No address, but $XDG_RUNTIME_DIR/bus (systemd's user bus): that bus is the one used.
runtime_dir = tempfile.mkdtemp()
daemon, address = bus(listen=f"unix:path={runtime_dir}/bus")
Portal(address, uri="file:///srv/a.iso")
out, _ = pick("", env_extra={"DBUS_SESSION_BUS_ADDRESS": None, "XDG_RUNTIME_DIR": runtime_dir})
check(out == "chosen /srv/a.iso", f"the bus at $XDG_RUNTIME_DIR/bus: {out!r}")
daemon.kill(); daemon.wait()

print(f"portal_bus: {'OK' if failures == 0 else f'{failures} failed'}", file=sys.stderr)
sys.exit(1 if failures else 0)
