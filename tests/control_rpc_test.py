"""Real stdio endpoint, isolated profile, no account, runtime or VR hardware."""
import json
import os
from pathlib import Path
import queue
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zipfile

binary = Path(sys.argv[1]).resolve()
probe = Path(sys.argv[2]).resolve()


def apk(path, package="com.example.control"):
    strings = ["label", "versionCode", "versionName", "package", "manifest", "application", package, "1.0", "Control fixture"]
    data, offsets = b"", []
    for text in strings:
        offsets.append(len(data))
        data += struct.pack("<H", len(text)) + text.encode("utf-16-le") + b"\0\0"
    data += b"\0" * (-len(data) % 4)
    start = 28 + len(offsets) * 4
    pool = struct.pack("<HH6I", 1, 28, start + len(data), len(strings), 0, 0, start, 0)
    pool += struct.pack("<" + "I" * len(offsets), *offsets) + data
    resource_map = struct.pack("<HH4I", 0x180, 8, 20, 0x01010001, 0x0101021b, 0x0101021c)

    def element(name, attributes):
        node = struct.pack("<HH5I6H", 0x102, 16, 36 + 20 * len(attributes), 1, 0xffffffff, 0xffffffff, name, 20, 20, len(attributes), 0, 0, 0)
        # Node header has five uint32 fields after its two uint16 fields.
        return node + b"".join(struct.pack("<IIIHBBI", 0xffffffff, key, raw, 8, 0, kind, value)
                               for key, raw, kind, value in attributes)

    def end(name):
        return struct.pack("<HH5I", 0x103, 16, 24, 1, 0xffffffff, 0xffffffff, name)

    body = pool + resource_map
    body += element(4, [(3, 6, 3, 6), (1, 0xffffffff, 0x10, 1), (2, 7, 3, 7)])
    body += element(5, [(0, 8, 3, 8)]) + end(5) + end(4)
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("AndroidManifest.xml", struct.pack("<HHI", 3, 8, len(body) + 8) + body)


class Client:
    def __init__(self, profile):
        self.process = subprocess.Popen([str(binary)], env=dict(os.environ, VENTURE_DATA_DIR=str(profile)),
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        encoding="utf-8", bufsize=1)
        self.messages = queue.Queue()
        self.state = {}
        self.sequence = 0
        self.errors = []
        threading.Thread(target=lambda: [self.messages.put(json.loads(line)) for line in self.process.stdout], daemon=True).start()
        threading.Thread(target=lambda: self.errors.extend(self.process.stderr), daemon=True).start()

    def wait(self, predicate):
        until = time.monotonic() + 30
        while time.monotonic() < until:
            message = self.messages.get(timeout=max(.1, until - time.monotonic()))
            if message.get("event") == "state":
                self.state = message
            if predicate(message):
                return message
        raise AssertionError("Protocol timeout: " + "".join(self.errors))

    def call(self, method, params=None, error=False):
        self.sequence += 1
        self.process.stdin.write(json.dumps(dict(id=self.sequence, method=method, params=params or {})) + "\n")
        self.process.stdin.flush()
        message = self.wait(lambda m: m.get("id") == self.sequence)
        if error:
            assert message["error"]["code"] and message["error"]["message"], message
            return message
        assert "error" not in message, message
        return message["result"]

    def task(self, method, params):
        task_id = self.call(method, params)["taskId"]
        def completed(message):
            return next((t for t in message.get("tasks", []) if t["taskId"] == task_id and t["state"] != "running"), None)
        message = self.wait(completed)
        task = completed(message)
        assert task["state"] == "complete", task
        return task["result"]

    def library(self):
        self.call("library")
        self.wait(lambda m: m.get("event") == "state" and not m["scanning"])
        return self.state["library"]

    def close(self):
        self.process.stdin.close()
        assert self.process.wait(timeout=15) == 0, "".join(self.errors)


with tempfile.TemporaryDirectory(prefix="venture-control-rpc-") as directory:
    root = Path(directory)
    profile = root / "profile"
    profile.mkdir()
    (profile / "settings.json").write_text(json.dumps({"checkForUpdates": False, "addGamesToSteam": False}))
    source = root / "source"
    source.mkdir()
    apk(source / "game.apk")
    obb = source / "sdcard/Android/obb/com.example.control/main.1.com.example.control.obb"
    obb.parent.mkdir(parents=True)
    obb.write_bytes(b"modified expansion")
    c = Client(profile)
    try:
        assert c.call("hello")["protocol"] == 1
        c.library()
        found = c.task("scan", {"path": str(source)})
        assert len(found) == 1 and found[0]["package"] == "com.example.control", found
        installed = c.task("install", {"path": str(source / "game.apk"), "sourceRoot": str(source / "game.apk"), "package": "com.example.control"})
        games = c.library()
        assert len(games) == 1 and games[0]["complete"], games
        params = {"installId": installed["installId"]}
        target = Path(installed["installId"])
        assert (target / obb.name).read_bytes() == b"modified expansion"
        # Modify lengths after installation; scanning and launching still work.
        (target / obb.name).write_bytes(b"different bytes after install")
        assert c.library()[0]["complete"]
        profile_id = "/interaction_profiles/oculus/touch_controller"
        global_controls = {profile_id: {"left": {"primary": "/input/y/click"}}}
        game_controls = {profile_id: {"right": {"primary": ["/input/a/click", "/input/b/click"]},
                                     "menu": {"mode": "chord", "first": "/user/hand/left/input/thumbstick/click",
                                              "second": "/user/hand/right/input/thumbstick/click", "holdMs": 0}}}
        c.call("settings.set", {"changes": {"runtimeExecutable": str(probe), "controls": global_controls,
                                           "display": {"pcAudio": "on", "resolution": "quest3", "frameRate": "90",
                                                       "advanced": {"gpu-bounds-checks": "off", "density-maps": "off"}}}})
        c.call("settings.set", {"changes": {"display": {"customFrameRate": -5}}}, error=True)
        assert c.call("settings.get")["display"]["pcAudio"] == "on"
        c.call("settings.set", dict(params, changes={"controls": game_controls,
                        "display": {"pcAudio": "off", "mirrorEye": "right", "advanced": {"gpu-bounds-checks": "on"}}}))
        c.call("settings.set", dict(params, changes={"display": {"mirror": "full"}}))
        assert c.call("settings.get", params)["display"]["pcAudio"] == "off"
        (target / "probe-readiness").touch()
        c.call("play", params)
        deadline = time.monotonic() + 20
        while not (target / "launch-options.json").exists() and time.monotonic() < deadline:
            c.call("task.get", {"taskId": "nonexistent"}, error=True)
            time.sleep(.05)
        options = json.loads((target / "launch-options.json").read_text())
        assert options["pcAudio"] == "off" and options["mirrorEye"] == "right"
        assert options["resolution"] == "1680x1760" and options["frameRate"] == "90"
        assert options["gpuBounds"] == "1" and options["densityMaps"] == "0"
        assert json.loads(options["controls"]) == game_controls
        # Spawning the process alone must not remove the frontend's loading mask.
        running = c.library()[0]
        assert running["running"] and running["loading"], running
        c.call("play", params, error=True)
        (target / "probe-render").touch()
        c.wait(lambda m: m.get("event") == "state" and
               m["library"][0]["running"] and not m["library"][0]["loading"])
        (target / "probe-exit").touch()
        # Wait until the child's package lock is released before patching.
        c.library()
        while c.state["library"][0]["running"]:
            c.wait(lambda m: m.get("event") == "state")
        assert not c.state["library"][0]["loading"]
        for name in ("probe-readiness", "probe-render", "probe-exit"):
            (target / name).unlink()
        patch = root / "patch/sdcard/Android/data/com.example.control/files"
        patch.mkdir(parents=True)
        (patch / "save").write_text("save one")
        c.task("patch", dict(params, path=str(root / "patch")))
        dirs = c.call("directories", params)
        assert (Path(dirs["saves"]) / "external-data/files/save").read_text() == "save one"
        saves = root / "saved-copy/files"
        saves.mkdir(parents=True)
        (saves / "save").write_text("save two")
        result = c.task("saves.import", dict(params, path=str(saves)))
        assert result["backup"] and (Path(result["backup"]) / "Android-data/files/save").read_text() == "save one"
        c.call("settings.reset", params)
        assert c.call("settings.get", params)["display"]["pcAudio"] == ""
        assert c.call("settings.get", params)["controls"] is None
        assert c.call("settings.get")["controls"]["global"] == global_controls
        c.call("unknown", error=True)
        second = subprocess.run([str(binary)], input='', env=dict(os.environ, VENTURE_DATA_DIR=str(profile)), capture_output=True, encoding="utf-8", timeout=15)
        assert second.returncode != 0 and "Another Venture instance" in second.stderr
        (target / obb.name).unlink()
        assert not c.library()[0]["complete"]
        c.call("play", params, error=True)
        # Physical copies in separate libraries cannot be selected by package alone.
        library2 = root / "second-library"
        duplicate = library2 / target.parent.name / target.name
        shutil.copytree(target, duplicate)
        c.call("settings.set", {"changes": {"libraryFolders": [str(library2)]}})
        assert len(c.library()) == 2
        c.call("patch", {"package": "com.example.control", "path": str(root / "patch")}, error=True)
        other = {"installId": str(duplicate)}
        c.task("patch", dict(other, path=str(source)))
        assert (duplicate / obb.name).exists() and not (target / obb.name).exists()
        # The pinned version's settings and saves have their own Venture identity.
        plan = json.loads((duplicate / "plan.json").read_text())
        plan.pop("source", None)
        plan["appId"], plan["binaryId"] = "123456", "456789"
        plan["pinned"] = True
        (duplicate / "plan.json").write_text(json.dumps(plan))
        pinned = duplicate.with_name("1-456789")
        duplicate.rename(pinned)
        duplicate = pinned
        other = {"installId": str(duplicate)}
        c.library()
        c.call("settings.set", dict(other, changes={"display": {"pcAudio": "on"}}))
        assert c.call("settings.get", params)["display"]["pcAudio"] == ""
        pinned_dirs = c.call("directories", other)
        assert pinned_dirs["saves"] != dirs["saves"]
        c.task("saves.import", dict(other, path=str(saves)))
        assert (Path(pinned_dirs["saves"]) / "external-data/files/save").read_text() == "save two"
        # A real disk task can be cancelled through the protocol without publication.
        large = root / "cancel-patch/Android/obb/com.example.control" / obb.name
        large.parent.mkdir(parents=True)
        with large.open("wb") as stream:
            stream.truncate(1 << 30)
        task = c.call("patch", dict(other, path=str(root / "cancel-patch")))["taskId"]
        c.call("task.cancel", {"taskId": task})
        until = time.monotonic() + 15
        state = c.call("task.get", {"taskId": task})
        while state["state"] == "running" and time.monotonic() < until:
            time.sleep(.05)
            state = c.call("task.get", {"taskId": task})
        assert state["state"] == "cancelled", state
        assert (duplicate / obb.name).read_bytes() == b"modified expansion"
        c.task("uninstall", params)
        assert duplicate.exists(), "Uninstall must preserve the explicitly unselected copy"
        assert (Path(dirs["saves"]) / "external-data/files/save").read_text() == "save two"
    finally:
        c.close()
print("PASS control install, modified files, launch environment, patch, save backup, reset, occupancy and uninstall")
