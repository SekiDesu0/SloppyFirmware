import socket
import struct
import json
import math
import os
import time
import tkinter as tk
from tkinter import ttk

# Optional SteamVR fusion (right controller only - thumb + index)
try:
    import openvr
    VR_AVAILABLE = True
except Exception:
    VR_AVAILABLE = False


# ---------------------------------------------------------------------------
# Protocol (mirrors firmware lib/PacketIO)  - v3: HELLO now carries `hand`
# ---------------------------------------------------------------------------
MAGIC      = 0x534C5031  # "SLP1"
PORT       = 4242

TYPE_HELLO     = 1
TYPE_WELCOME   = 2
TYPE_DATA      = 3
TYPE_KEEPALIVE = 4
TYPE_BYE      = 5

HAND_UNKNOWN = 0
HAND_LEFT    = 1
HAND_RIGHT   = 2
HAND_LABELS  = {HAND_UNKNOWN: "auto", HAND_LEFT: "left", HAND_RIGHT: "right"}

KEEPALIVE_INTERVAL_S = 1.0
DEVICE_TIMEOUT_S     = 5.0

# Header: magic(I) type(B) fw(B) reserved(H)
HEADER_FMT = "<IBBH"
# Hello: header + mac(6s) + deviceType(B) + channelCount(B) + hand(B) + reserved(B) = 18
HELLO_FMT = HEADER_FMT + "6sBBBB"
# Welcome: header + dataPort(H) + keepaliveMs(H) = 12
WELCOME_FMT = HEADER_FMT + "HH"
# Data: header + packetId(I) uptime(I) filtered(12H) touch(H) i2c(H) loop(H) rssi(b) reserved(B) = 48
DATA_FMT = HEADER_FMT + "II12HHHHbB"
DATA_LEN = struct.calcsize(DATA_FMT)
# Keepalive: header + lastSeenPacketId(I) = 12
KEEP_FMT = HEADER_FMT + "I"

HELLO_LEN = struct.calcsize(HELLO_FMT)
WELCOME_LEN = struct.calcsize(WELCOME_FMT)
KEEP_LEN = struct.calcsize(KEEP_FMT)


# ---------------------------------------------------------------------------
# Config persistence
# ---------------------------------------------------------------------------
CONFIG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tracker_config.json")

# Default electrode -> joint map. Per-hand copies live in cfg["hands"][...].
JOINT_KEYS = [
    ("mid_p",   "Middle Prox"),
    ("mid_d",   "Middle Dist"),
    ("ring_p",  "Ring Prox"),
    ("ring_d",  "Ring Dist"),
    ("pinky_p", "Pinky Prox"),
    ("pinky_d", "Pinky Dist"),
    # Left hand has no SteamVR fusion, so we also map thumb + index from electrodes:
    ("thumb_p", "Thumb Prox"),
    ("thumb_d", "Thumb Dist"),
    ("index_p", "Index Prox"),
    ("index_d", "Index Dist"),
]
DEFAULT_MAP_RIGHT = {k: i for i, (k, _) in enumerate(JOINT_KEYS[:6])}  # mid/ring/pinky only
DEFAULT_MAP_LEFT  = {k: (i if i < 12 else None) for i, (k, _) in enumerate(JOINT_KEYS)}

DEFAULTS = {
    "baseline":   200,
    "max_delta":  185,
    "coupling":   0.4,
    "smoothing": {
        "enabled":      True,
        "ema_alpha":     0.35,   # 0..1, higher = more responsive, less smooth
        "median_window": 3,      # odd
        "deadband":      0.015,  # ignore changes smaller than this (0..1)
    },
    "mac_hand": {},  # { "mac_str": "left"/"right" }
    "hands": {
        "left":  {"electrode_map": DEFAULT_MAP_LEFT},
        "right": {"electrode_map": DEFAULT_MAP_RIGHT},
    },
}


def load_config():
    cfg = json.loads(json.dumps(DEFAULTS))  # deep copy
    if os.path.exists(CONFIG_PATH):
        try:
            with open(CONFIG_PATH, "r") as f:
                user = json.load(f)
            for k in DEFAULTS:
                if k in user:
                    cfg[k] = user[k]
            for h in ("left", "right"):
                if h not in cfg["hands"]:
                    cfg["hands"][h] = {"electrode_map": DEFAULTS["hands"][h]["electrode_map"]}
                cfg["hands"][h].setdefault("electrode_map", {})
                for jk, _ in JOINT_KEYS:
                    cfg["hands"][h]["electrode_map"].setdefault(jk, None)
        except Exception as e:
            print(f"[cfg] Failed to load {CONFIG_PATH}: {e}")
    # Coerce None / int electrode values
    for h in ("left", "right"):
        for jk in cfg["hands"][h]["electrode_map"]:
            v = cfg["hands"][h]["electrode_map"][jk]
            cfg["hands"][h]["electrode_map"][jk] = None if v in (None, "None") else int(v)
    return cfg


def save_config(cfg):
    try:
        with open(CONFIG_PATH, "w") as f:
            json.dump(cfg, f, indent=2)
    except Exception as e:
        print(f"[cfg] Failed to save {CONFIG_PATH}: {e}")


# ---------------------------------------------------------------------------
# UDP
# ---------------------------------------------------------------------------
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("0.0.0.0", PORT))
sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
sock.setblocking(False)
print(f"Server listening for HELLO broadcasts on UDP {PORT}")


def mac_bytes_to_str(b):
    return ":".join("%02X" % x for x in b)


# ---------------------------------------------------------------------------
# Smoothing (per-joint). Median filter -> EMA -> deadband.
# ---------------------------------------------------------------------------
class Smoother:
    def __init__(self):
        self.history = []
        self.smoothed = 0.0
        self.last_out = 0.0

    def reset(self):
        self.history.clear()
        self.smoothed = 0.0
        self.last_out = 0.0

    def update(self, raw, alpha, window, deadband, enabled):
        if not enabled:
            return raw
        # median
        self.history.append(raw)
        if len(self.history) > window:
            self.history.pop(0)
        ordered = sorted(self.history)
        median = ordered[len(ordered) // 2]
        # EMA
        self.smoothed = alpha * median + (1.0 - alpha) * self.smoothed
        # deadband
        if abs(self.smoothed - self.last_out) < deadband:
            return self.last_out
        self.last_out = self.smoothed
        return self.last_out


# ---------------------------------------------------------------------------
# Per-hand runtime state
# ---------------------------------------------------------------------------
class HandState:
    def __init__(self, name):
        self.name = name
        self.mac = None        # str, once assigned
        self.ip = None
        self.port = PORT
        self.fw = 0
        self.last_seen = 0.0
        self.last_keepalive_sent = 0.0
        self.last_packet_id = 0
        self.filtered = [0] * 12
        self.touch = 0
        self.meta = {"i2c_ms": 0, "loop_ms": 0, "rssi": 0, "uptime": 0}
        joint_names = [k for k, _ in JOINT_KEYS]
        self.smoothers = {k: Smoother() for k in joint_names}

    def reset_stream_state(self):
        self.last_packet_id = 0
        for s in self.smoothers.values():
            s.reset()

    def is_alive(self):
        return self.ip is not None and (time.time() - self.last_seen) < DEVICE_TIMEOUT_S

    def send_welcome(self):
        if self.ip is None:
            return
        pkt = struct.pack(WELCOME_FMT, MAGIC, TYPE_WELCOME, 3, 0, PORT, int(KEEPALIVE_INTERVAL_S * 1000))
        sock.sendto(pkt, (self.ip, self.port))
        self.last_keepalive_sent = time.time()
        print(f"[server] Sent WELCOME to {self.ip}:{self.port} (hand={self.name})")

    def send_keepalive(self):
        if self.ip is None:
            return
        pkt = struct.pack(KEEP_FMT, MAGIC, TYPE_KEEPALIVE, 3, 0, self.last_packet_id)
        sock.sendto(pkt, (self.ip, self.port))
        self.last_keepalive_sent = time.time()


hands = {
    "left":  HandState("left"),
    "right": HandState("right"),
}

# Pending devices = discovered by HELLO but not yet assigned to a hand slot.
# { mac_str: {"ip":.., "fw":.., "channel_count":.., "hand_hint":.., "last_seen":..} }
pending = {}

cfg = load_config()


# ---------------------------------------------------------------------------
# SteamVR fusion (right controller only)
# ---------------------------------------------------------------------------
vr_sys = None
vr_enabled = False

try:
    vr_sys = openvr.init(openvr.VRApplication_Background)
    vr_enabled = True
    print("SteamVR hooked. Right-controller thumb + index fusion active.")
except Exception as e:
    print(f"[WARNING] SteamVR not running. Thumb/Index fusion disabled. ({e})")


def get_vr_flex():
    if not vr_enabled or vr_sys is None:
        return 0.0, ("None", 0.0, 0.0, 0.0), 0.0
    right_id = None
    for i in range(openvr.k_unMaxTrackedDeviceCount):
        if vr_sys.getTrackedDeviceClass(i) == openvr.TrackedDeviceClass_Controller:
            if vr_sys.getControllerRoleForTrackedDeviceIndex(i) == openvr.TrackedControllerRole_RightHand:
                right_id = i
                break
    if right_id is not None:
        result, state = vr_sys.getControllerState(right_id)
        if result:
            trig_val = state.rAxis[1].x
            trig_touched = bool(state.ulButtonTouched & (1 << openvr.k_EButton_Axis1))
            index_flex = trig_val if trig_val > 0.05 else (0.2 if trig_touched else 0.0)
            thumb_btn = "None"
            thumb_flex = 0.0
            stick_x = state.rAxis[0].x
            stick_y = state.rAxis[0].y
            if bool(state.ulButtonPressed & (1 << openvr.k_EButton_A)):
                thumb_btn, thumb_flex = "A", 1.0
            elif bool(state.ulButtonTouched & (1 << openvr.k_EButton_A)):
                thumb_btn, thumb_flex = "A", 0.5
            elif bool(state.ulButtonPressed & (1 << openvr.k_EButton_ApplicationMenu)):
                thumb_btn, thumb_flex = "B", 1.0
            elif bool(state.ulButtonTouched & (1 << openvr.k_EButton_ApplicationMenu)):
                thumb_btn, thumb_flex = "B", 0.5
            elif bool(state.ulButtonPressed & (1 << openvr.k_EButton_SteamVR_Touchpad)):
                thumb_btn, thumb_flex = "Stick", 1.0
            elif bool(state.ulButtonTouched & (1 << openvr.k_EButton_SteamVR_Touchpad)):
                thumb_btn, thumb_flex = "Stick", 0.5
            grip_pressed = bool(state.ulButtonPressed & (1 << openvr.k_EButton_Grip))
            grip_touched = bool(state.ulButtonTouched & (1 << openvr.k_EButton_Grip))
            grip_flex = 1.0 if grip_pressed else (0.5 if grip_touched else 0.0)
            return index_flex, (thumb_btn, thumb_flex, stick_x, stick_y), grip_flex
    return 0.0, ("None", 0.0, 0.0, 0.0), 0.0


# ---------------------------------------------------------------------------
# GUI
# ---------------------------------------------------------------------------
root = tk.Tk()
root.title("SloppyHands Tracker (v3 - dual hand + smoothing)")
root.geometry("980x820")
root.configure(bg="#222")

diag_text = tk.StringVar()
diag_text.set("Waiting for device HELLO broadcasts on UDP 4242...")
diag_label = tk.Label(root, textvariable=diag_text, bg="#222", fg="#00FF00", font=("Consolas", 10))
diag_label.pack(fill=tk.X, pady=3)

# --- Device list ---
dev_frame = tk.LabelFrame(root, text="Discovered devices", bg="#222", fg="#FFAA00",
                          font=("Consolas", 10, "bold"), padx=6, pady=4)
dev_frame.pack(fill=tk.X, padx=10, pady=4)

tk.Label(dev_frame, text="MAC                IP            FW  Ch  Hand     RSSI  Last",
        bg="#222", fg="#888", font=("Consolas", 9)).grid(row=0, column=0, sticky="w")
dev_rows = {}  # mac -> {"row": int, "widgets": {...}}
dev_row_var = {}  # mac -> StringVar for hand combobox


def assign_hand(mac_str, new_hand):
    """Assign a discovered device to a hand slot (or 'auto' = put back to pending)."""
    # capture the device IP / fw before any eviction so we don't lose it
    captured_ip = None
    captured_fw = 0
    for h in ("left", "right"):
        if hands[h].mac == mac_str:
            captured_ip = hands[h].ip
            captured_fw = hands[h].fw
    if captured_ip is None and mac_str in pending:
        captured_ip = pending[mac_str].get("ip")
        captured_fw = pending[mac_str].get("fw", 0)

    # remove from current slot
    for h in ("left", "right"):
        if hands[h].mac == mac_str:
            hands[h].mac = None
            hands[h].ip = None
            hands[h].reset_stream_state()
    # clear persisted mapping
    cfg["mac_hand"].pop(mac_str, None)

    if new_hand in ("left", "right"):
        # evict whatever was in that slot first, back to pending
        old_mac = hands[new_hand].mac
        if old_mac and old_mac != mac_str:
            pending[old_mac] = {
                "ip": hands[new_hand].ip,
                "fw": hands[new_hand].fw,
                "channel_count": 12,
                "hand_hint": HAND_UNKNOWN,
                "last_seen": time.time(),
            }
        # promote
        hands[new_hand].mac = mac_str
        hands[new_hand].ip = captured_ip
        hands[new_hand].fw = captured_fw
        hands[new_hand].port = PORT
        hands[new_hand].last_seen = time.time()
        hands[new_hand].last_keepalive_sent = 0.0  # force fresh WELCOME
        if captured_ip is not None:
            hands[new_hand].send_welcome()
        cfg["mac_hand"][mac_str] = new_hand
        pending.pop(mac_str, None)
    else:
        # auto: keep in pending
        if captured_ip is not None:
            pending[mac_str] = {
                "ip": captured_ip,
                "fw": captured_fw,
                "channel_count": 12,
                "hand_hint": HAND_UNKNOWN,
                "last_seen": time.time(),
            }

    save_config(cfg)
    global _last_known_macs
    _last_known_macs = set()  # force rebuild
    rebuild_device_list()


_last_known_macs = set()


def rebuild_device_list():
    """Rebuild widget rows ONLY when the set of MACs changes. Cheap to call
    every frame: it diffs and exits early when nothing structural changed."""
    global _last_known_macs
    all_macs = set(pending.keys()) | {hands[h].mac for h in hands if hands[h].mac}
    all_macs.discard(None)
    if all_macs == _last_known_macs:
        return  # nothing to rebuild
    _last_known_macs = set(all_macs)

    # destroy rows no longer needed
    for mac in list(dev_rows.keys()):
        if mac not in all_macs:
            dev_rows[mac]["widgets"]["frame"].destroy()
            del dev_rows[mac]
            dev_row_var.pop(mac, None)
    # create missing rows
    r = 1
    for mac in sorted(all_macs):
        if mac not in dev_rows:
            row_frame = tk.Frame(dev_frame, bg="#222")
            row_frame.grid(row=r, column=0, sticky="w")
            var = tk.StringVar(value="auto")
            cb = ttk.Combobox(row_frame, textvariable=var, values=["auto", "left", "right"],
                              width=6, state="readonly")
            dev_row_var[mac] = var
            widgets = {
                "frame": row_frame,
                "hand": cb,
            }
            for col, key in enumerate(["mac", "ip", "fw", "ch", "hand", "rssi", "last"]):
                if key == "hand":
                    cb.grid(row=0, column=col, padx=4)
                else:
                    widgets[key] = tk.Label(row_frame, text="", bg="#222", fg="#00FF00",
                                             font=("Consolas", 9))
                    widgets[key].grid(row=0, column=col, padx=4, sticky="w")
            widgets["mac"].configure(text=mac)
            # Hook up assignment AFTER widgets exist so the trace doesn't fire
            # for the initial "auto".
            var.trace_add("write", lambda *a, _m=mac, _v=var: assign_hand(_m, _v.get()))
            dev_rows[mac] = {"row": r, "widgets": widgets}
        else:
            dev_rows[mac]["row"] = r
            dev_rows[mac]["widgets"]["frame"].grid(row=r, column=0, sticky="w")
        r += 1


def refresh_device_values():
    """Per-frame cheap update of label text + hand combobox. Does NOT touch
    widget structure and does NOT clobber an in-progress hand edit."""
    now = time.time()
    for mac, row in dev_rows.items():
        w = row["widgets"]
        # find current assignment
        cur = "auto"
        for h in ("left", "right"):
            if hands[h].mac == mac:
                cur = h
                break
        info = pending.get(mac)
        hand_state = hands[cur] if cur in ("left", "right") else None
        # ip
        if info is not None:
            w["ip"].configure(text=info["ip"])
            w["fw"].configure(text=str(info["fw"]))
            w["ch"].configure(text=str(info["channel_count"]))
            elapsed = now - info["last_seen"]
        elif hand_state is not None and hand_state.is_alive():
            w["ip"].configure(text=hand_state.ip)
            w["fw"].configure(text=str(hand_state.fw))
            w["ch"].configure(text="12")
            elapsed = now - hand_state.last_seen
        else:
            w["ip"].configure(text="?")
            w["fw"].configure(text="?")
            w["ch"].configure(text="?")
            elapsed = 0
        # rssi/last from the hand_state if alive
        if hand_state is not None and hand_state.is_alive():
            w["rssi"].configure(text=f"{hand_state.meta['rssi']}dBm")
        else:
            w["rssi"].configure(text="-")
        w["last"].configure(text=f"{elapsed:0.1f}s")
        # Only update combobox if different AND only when not actively being
        # edited (combobox has focus). This stops us fighting the user.
        cb_var = dev_row_var.get(mac)
        if cb_var is not None and cur != cb_var.get():
            try:
                focused = root.focus_get()
                w["hand"]  # may be None briefly
                if focused is not w["hand"]:
                    cb_var.set(cur)
            except Exception:
                pass


# --- Canvases for the two hands side by side ---
canvas_frame = tk.Frame(root, bg="#222")
canvas_frame.pack(fill=tk.BOTH, expand=True, padx=10, pady=4)

left_canvas = tk.Canvas(canvas_frame, width=460, height=420, bg="#1a1a1a",
                        highlightthickness=1, highlightbackground="#444")
left_canvas.pack(side=tk.LEFT, padx=4)
right_canvas = tk.Canvas(canvas_frame, width=460, height=420, bg="#1a1a1a",
                         highlightthickness=1, highlightbackground="#444")
right_canvas.pack(side=tk.LEFT, padx=4)

# --- Bottom controls: tabs for per-hand mapping + global settings ---
bottom = ttk.Notebook(root)
bottom.pack(fill=tk.BOTH, expand=False, padx=10, pady=6)

map_tabs = {}
map_vars = {"left": {}, "right": {}}
ELECTRODE_CHOICES = ["None"] + [str(i) for i in range(12)]


def on_map_changed(*_):
    for h in ("left", "right"):
        for name, var in map_vars[h].items():
            v = var.get()
            cfg["hands"][h]["electrode_map"][name] = None if v == "None" else int(v)
    cfg["baseline"]  = baseline_var.get()
    cfg["max_delta"] = max_delta_var.get()
    cfg["coupling"]  = coupling_var.get()
    cfg["smoothing"]["enabled"]      = bool(smooth_enable_var.get())
    cfg["smoothing"]["ema_alpha"]    = smooth_alpha_var.get()
    cfg["smoothing"]["median_window"] = int(smooth_median_var.get())
    if cfg["smoothing"]["median_window"] % 2 == 0:
        cfg["smoothing"]["median_window"] += 1  # force odd
    cfg["smoothing"]["deadband"]     = smooth_deadband_var.get()
    save_config(cfg)


def build_map_tab(hand):
    tab = tk.Frame(bottom, bg="#222")
    for i, (key, label) in enumerate(JOINT_KEYS):
        row, col = divmod(i, 2)
        tk.Label(tab, text=label, bg="#222", fg="#00FF00", font=("Consolas", 9)).grid(
            row=row, column=col * 2, sticky="e", padx=2, pady=2)
        cur = cfg["hands"][hand]["electrode_map"].get(key, "None")
        cur = "None" if cur is None else str(cur)
        var = tk.StringVar(value=cur)
        om = ttk.Combobox(tab, textvariable=var, values=ELECTRODE_CHOICES, width=6, state="readonly")
        om.grid(row=row, column=col * 2 + 1, sticky="w", padx=2, pady=2)
        map_vars[hand][key] = var
        var.trace_add("write", on_map_changed)
    return tab


map_tabs["left"] = build_map_tab("left")
map_tabs["right"] = build_map_tab("right")
bottom.add(map_tabs["left"], text="Left hand map")
bottom.add(map_tabs["right"], text="Right hand map")

# Settings tab
settings_tab = tk.Frame(bottom, bg="#222")
settings_tab.pack(fill=tk.BOTH, expand=True)
bottom.add(settings_tab, text="Settings")

baseline_var  = tk.DoubleVar(value=cfg["baseline"])
max_delta_var = tk.DoubleVar(value=cfg["max_delta"])
coupling_var  = tk.DoubleVar(value=cfg["coupling"])
smooth_enable_var   = tk.IntVar(value=1 if cfg["smoothing"]["enabled"] else 0)
smooth_alpha_var    = tk.DoubleVar(value=cfg["smoothing"]["ema_alpha"])
smooth_median_var   = tk.IntVar(value=cfg["smoothing"]["median_window"])
smooth_deadband_var = tk.DoubleVar(value=cfg["smoothing"]["deadband"])


def slider(parent, var, lo, hi, label, res=0.05, row=0):
    s = tk.Scale(parent, from_=lo, to=hi, resolution=res, orient=tk.HORIZONTAL,
                 variable=var, label=label, bg="#222", fg="#00FF00",
                 highlightthickness=0, length=280)
    s.grid(row=row, column=0, columnspan=2, sticky="w", padx=10, pady=2)


row = 0
tk.Checkbutton(settings_tab, text="Enable smoothing", variable=smooth_enable_var,
               bg="#222", fg="#00FF00", selectcolor="#000",
               command=on_map_changed).grid(row=row, column=0, sticky="w", padx=10); row += 1
slider(settings_tab, baseline_var,       0, 400, "Baseline (raw)",   res=1, row=row); row += 1
slider(settings_tab, max_delta_var,     1, 400, "Max Delta (raw)",  res=1, row=row); row += 1
slider(settings_tab, coupling_var,       0,   1, "Distal Coupling",       row=row); row += 1
slider(settings_tab, smooth_alpha_var,  0.01, 1, "Smoothing EMA alpha", res=0.01, row=row); row += 1
slider(settings_tab, smooth_median_var, 1,   9, "Median window (odd)", res=2, row=row); row += 1
slider(settings_tab, smooth_deadband_var, 0, 0.2, "Deadband",        res=0.005, row=row); row += 1

for v in (baseline_var, max_delta_var, coupling_var,
          smooth_alpha_var, smooth_median_var, smooth_deadband_var):
    v.trace_add("write", on_map_changed)
on_map_changed()


# ---------------------------------------------------------------------------
# Channel bars + skeleton drawing -- PRE-ALLOCATED canvas items.
# We create every line/oval/rectangle/text ONCE at startup; per-frame we just
# move them with canvas.coords() instead of delete+create. This is the single
# biggest performance fix for tkinter canvases (item creation is slow).
# ---------------------------------------------------------------------------
def normalize_joint(hand_state, joint_key):
    idx = cfg["hands"][hand_state.name]["electrode_map"].get(joint_key)
    if idx is None or not (0 <= idx < 12):
        return 0.0
    raw = hand_state.filtered[idx]
    delta = max(0, baseline_var.get() - raw)
    norm = min(1.0, delta / max(1, max_delta_var.get()))
    sm = hand_state.smoothers[joint_key]
    return sm.update(norm,
                     smooth_alpha_var.get(),
                     int(smooth_median_var.get()),
                     smooth_deadband_var.get(),
                     bool(smooth_enable_var.get()))


class HandView:
    """Owns all canvas items for one hand. Pre-allocates everything; update()
    just repositions existing items. No delete/create in the hot path."""

    FINGERS = ("thumb", "index", "mid", "ring", "pinky")

    def __init__(self, canvas, wrist, mcp, default_angles, lengths,
                 palm_color, finger_color, title, joint_fill="red"):
        self.canvas = canvas
        self.wrist = wrist
        self.mcp = mcp
        self.default_angles = default_angles
        self.lengths = lengths
        self.finger_color = finger_color
        self.joint_fill = joint_fill
        self.title = title

        # title text (static)
        canvas.create_text(230, 20, text=title, fill="#FFAA00",
                            font=("Consolas", 12, "bold"))

        # palm lines (6 segments) -- static geometry, set once
        palm_pts = [wrist, mcp["thumb"], mcp["index"], mcp["mid"],
                    mcp["ring"], mcp["pinky"], wrist]
        self.palm_items = []
        for i in range(len(palm_pts) - 1):
            self.palm_items.append(canvas.create_line(
                palm_pts[i][0], palm_pts[i][1],
                palm_pts[i + 1][0], palm_pts[i + 1][1],
                fill=palm_color, width=2))

        # per finger: 3 line segments + 4 joint ovals (pre-created at 0,0)
        self.finger_lines = {}   # name -> [line_id, line_id, line_id]
        self.finger_joints = {}  # name -> [oval_id x4]
        for f in self.FINGERS:
            lines = [canvas.create_line(0, 0, 0, 0, fill=finger_color, width=3)
                     for _ in range(3)]
            ovals = [canvas.create_oval(0, 0, 0, 0, fill=joint_fill,
                                        outline=joint_fill) for _ in range(4)]
            self.finger_lines[f] = lines
            self.finger_joints[f] = ovals

        # channel bars: 12 rectangles (height moves) + 12 labels (static)
        self.bar_items = []
        self.bar_labels = []
        for i in range(12):
            x = 10 + i * 22
            self.bar_items.append(canvas.create_rectangle(
                x, 410, x + 16, 410, fill="#0a0", outline="#0f0"))
            self.bar_labels.append(canvas.create_text(
                x + 8, 415, text=str(i), fill="#888", font=("Consolas", 7)))

        # keep a list of "skeleton-like" items so we can show/hide as a group
        self._all_items = []
        for lid in self.palm_items:
            self._all_items.append(lid)
        for f in self.FINGERS:
            self._all_items.extend(self.finger_lines[f])
            self._all_items.extend(self.finger_joints[f])
        # bars are always visible (even when hand offline) so not in _all_items

    def _update_finger(self, name, prox_flex, distal_flex, angle_override=None):
        base_angle = self.default_angles[name] if angle_override is None else angle_override
        lengths = self.lengths[name]
        max_bend = math.radians(80)
        coupling = coupling_var.get()
        effective_distal = min(1.0, max(distal_flex, prox_flex * coupling))
        a1 = math.radians(base_angle) + (prox_flex * max_bend)
        a2 = a1 + (effective_distal * max_bend)
        a3 = a2 + (effective_distal * max_bend)
        sx, sy = self.mcp[name]
        j1x = sx + lengths[0] * math.cos(a1); j1y = sy + lengths[0] * math.sin(a1)
        j2x = j1x + lengths[1] * math.cos(a2); j2y = j1y + lengths[1] * math.sin(a2)
        j3x = j2x + lengths[2] * math.cos(a3); j3y = j2y + lengths[2] * math.sin(a3)
        pts = [(sx, sy), (j1x, j1y), (j2x, j2y), (j3x, j3y)]
        c = self.canvas
        lines = self.finger_lines[name]
        ovals = self.finger_joints[name]
        for i in range(3):
            c.coords(lines[i], pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1])
        for i, (px, py) in enumerate(pts):
            c.coords(ovals[i], px - 4, py - 4, px + 4, py + 4)

    def _update_bars(self, filtered):
        c = self.canvas
        for i in range(12):
            x = 10 + i * 22
            raw = filtered[i] if i < len(filtered) else 0
            h = min(120, max(0, raw // 4))
            c.coords(self.bar_items[i], x, 410 - h, x + 16, 410)

    def set_skeleton_visible(self, visible):
        state = tk.NORMAL if visible else tk.HIDDEN
        for it in self._all_items:
            self.canvas.itemconfig(it, state=state)

    def update_bars(self, filtered):
        self._update_bars(filtered)

    def hide_finger(self, name):
        for it in self.finger_lines[name]:
            self.canvas.itemconfig(it, state=tk.HIDDEN)
        for it in self.finger_joints[name]:
            self.canvas.itemconfig(it, state=tk.HIDDEN)

    def show_finger(self, name):
        for it in self.finger_lines[name]:
            self.canvas.itemconfig(it, state=tk.NORMAL)
        for it in self.finger_joints[name]:
            self.canvas.itemconfig(it, state=tk.NORMAL)


# per-hand layout tables
RIGHT_LAYOUT = {
    "wrist": (240, 380),
    "mcp":   {"thumb": (120, 300), "index": (150, 230), "mid": (190, 210),
              "ring": (240, 220), "pinky": (290, 250)},
    "angles": {"index": -105, "mid": -90, "ring": -75, "pinky": -60},
    "lengths": {"thumb": [40, 30, 25], "index": [55, 35, 25], "mid": [60, 40, 25],
                "ring": [55, 35, 25], "pinky": [40, 25, 20]},
}
LEFT_LAYOUT = {
    "wrist": (220, 380),
    "mcp":   {"thumb": (340, 300), "index": (310, 230), "mid": (270, 210),
              "ring": (220, 220), "pinky": (170, 250)},
    "angles": {"thumb": -80, "index": -75, "mid": -90, "ring": -105, "pinky": -120},
    "lengths": {"thumb": [40, 30, 25], "index": [55, 35, 25], "mid": [60, 40, 25],
                "ring": [55, 35, 25], "pinky": [40, 25, 20]},
}


def render_right(hs, vr_data):
    """Right hand: thumb + index from SteamVR (overridable), other joints ESP32."""
    if not hs.is_alive():
        right_view.set_skeleton_visible(False)
        right_view.update_bars(hs.filtered)
        return
    right_view.set_skeleton_visible(True)

    index_flex, (thumb_btn, thumb_raw, stick_x, stick_y), grip_flex = vr_data
    if thumb_btn == "A":
        thumb_angle, thumb_f = -100, thumb_raw * 0.8
    elif thumb_btn == "B":
        thumb_angle, thumb_f = -120, thumb_raw * 0.6
    elif thumb_btn == "Stick":
        thumb_angle = -140 + (stick_x * 30)
        thumb_f = min(1.0, max(0.0, 0.4 - (stick_y * 0.4)))
        if thumb_raw == 1.0:
            thumb_f = 1.0
    else:
        thumb_angle, thumb_f = -150, 0.0

    idx_t = cfg["hands"]["right"]["electrode_map"].get("thumb_p")
    idx_d = cfg["hands"]["right"]["electrode_map"].get("thumb_d")
    if idx_t is not None or idx_d is not None:
        thumb_p = normalize_joint(hs, "thumb_p") if idx_t is not None else thumb_f
        thumb_d = normalize_joint(hs, "thumb_d") if idx_d is not None else thumb_p
    else:
        thumb_p, thumb_d = thumb_f, thumb_f

    idx_ip = cfg["hands"]["right"]["electrode_map"].get("index_p")
    idx_id = cfg["hands"]["right"]["electrode_map"].get("index_d")
    if idx_ip is not None or idx_id is not None:
        i_p = normalize_joint(hs, "index_p") if idx_ip is not None else index_flex
        i_d = normalize_joint(hs, "index_d") if idx_id is not None else i_p
    else:
        i_p, i_d = index_flex, index_flex

    mid_p = normalize_joint(hs, "mid_p")
    mid_d = normalize_joint(hs, "mid_d")
    if mid_d == 0.0 and mid_p == 0.0:
        mid_d = grip_flex  # fallback to SteamVR grip when electrodes unassigned

    right_view._update_finger("thumb",  thumb_p, thumb_d, angle_override=thumb_angle)
    right_view._update_finger("index", i_p, i_d)
    right_view._update_finger("mid",    mid_p, mid_d)
    right_view._update_finger("ring",   normalize_joint(hs, "ring_p"),  normalize_joint(hs, "ring_d"))
    right_view._update_finger("pinky",  normalize_joint(hs, "pinky_p"), normalize_joint(hs, "pinky_d"))
    right_view.update_bars(hs.filtered)


def render_left(hs):
    """Left hand: all joints from ESP32 electrodes."""
    if not hs.is_alive():
        left_view.set_skeleton_visible(False)
        left_view.update_bars(hs.filtered)
        return
    left_view.set_skeleton_visible(True)
    left_view._update_finger("thumb", normalize_joint(hs, "thumb_p"), normalize_joint(hs, "thumb_d"))
    left_view._update_finger("index", normalize_joint(hs, "index_p"), normalize_joint(hs, "index_d"))
    left_view._update_finger("mid",   normalize_joint(hs, "mid_p"),   normalize_joint(hs, "mid_d"))
    left_view._update_finger("ring",  normalize_joint(hs, "ring_p"),  normalize_joint(hs, "ring_d"))
    left_view._update_finger("pinky", normalize_joint(hs, "pinky_p"), normalize_joint(hs, "pinky_d"))
    left_view.update_bars(hs.filtered)


# Pre-allocate all canvas items once (no per-frame create/delete).
right_view = HandView(
    right_canvas, RIGHT_LAYOUT["wrist"], RIGHT_LAYOUT["mcp"],
    RIGHT_LAYOUT["angles"], RIGHT_LAYOUT["lengths"],
    palm_color="#00FF00", finger_color="#00FF00", title="RIGHT HAND")
left_view = HandView(
    left_canvas, LEFT_LAYOUT["wrist"], LEFT_LAYOUT["mcp"],
    LEFT_LAYOUT["angles"], LEFT_LAYOUT["lengths"],
    palm_color="#00FFCC", finger_color="#00FFCC", title="LEFT HAND")
right_view.set_skeleton_visible(False)
left_view.set_skeleton_visible(False)


# ---------------------------------------------------------------------------
# Packet handling
# ---------------------------------------------------------------------------
def handle_packet(data, addr):
    if len(data) < struct.calcsize(HEADER_FMT):
        return
    magic, ptype, fw, _r = struct.unpack(HEADER_FMT, data[:struct.calcsize(HEADER_FMT)])
    if magic != MAGIC:
        return

    if ptype == TYPE_HELLO and len(data) >= HELLO_LEN:
        fields = struct.unpack(HELLO_FMT, data[:HELLO_LEN])
        mac_bytes = fields[4]
        device_type = fields[5]
        channel_count = fields[6]
        hand_hint = fields[7]
        mac_str = mac_bytes_to_str(mac_bytes)
        # refresh pending
        pending[mac_str] = {
            "ip": addr[0],
            "fw": fw,
            "channel_count": channel_count,
            "hand_hint": hand_hint,
            "last_seen": time.time(),
        }
        # auto-assigned via persisted config?
        pref = cfg["mac_hand"].get(mac_str)
        already_in_slot = None
        for h in ("left", "right"):
            if hands[h].mac == mac_str:
                already_in_slot = h
        if already_in_slot:
            # refresh that slot's ip / welcome if needed
            hands[already_in_slot].ip = addr[0]
            hands[already_in_slot].last_seen = time.time()
            if hands[already_in_slot].last_keepalive_sent == 0:
                hands[already_in_slot].send_welcome()
        elif pref in ("left", "right"):
            # auto-promote
            assign_hand(mac_str, pref)
        else:
            # if hand_hint from firmware is explicit, auto-promote to free slot
            if hand_hint in (HAND_LEFT, HAND_RIGHT):
                h = "left" if hand_hint == HAND_LEFT else "right"
                # only if slot free
                if hands[h].mac is None:
                    assign_hand(mac_str, h)
        rebuild_device_list()

    elif ptype == TYPE_DATA and len(data) >= DATA_LEN:
        fields = struct.unpack(DATA_FMT, data[:DATA_LEN])
        packet_id = fields[4]
        uptime    = fields[5]
        filtered  = list(fields[6:18])
        touch     = fields[18]
        i2c_ms    = fields[19]
        loop_ms   = fields[20]
        rssi      = fields[21]
        # find which hand slot owns the sender by ip
        target = None
        for h in ("left", "right"):
            if hands[h].ip == addr[0]:
                target = hands[h]
                break
        if target is None and addr[0] in [p["ip"] for p in pending.values()]:
            # find mac for this ip and auto-assign (just refresh) - rebuild welcome
            for mac, info in pending.items():
                if info["ip"] == addr[0]:
                    # device is streaming despite our session being lost; just bind by ip
                    target = hands["left"] if hands["left"].mac == mac else hands["right"]
                    if target.mac != mac:
                        # find empty slot
                        for h in ("left", "right"):
                            if hands[h].mac is None:
                                hands[h].mac = mac
                                hands[h].ip = addr[0]
                                target = hands[h]
                                cfg["mac_hand"][mac] = h
                                save_config(cfg)
                                break
                    if target.mac != mac:
                        # give up; just bind to whichever hand has matching ip
                        target = None
                    break
        if target is None:
            # unknown sender; ignore (will be picked up by its next HELLO)
            return
        target.filtered = filtered
        target.touch = touch
        target.meta = {"i2c_ms": i2c_ms, "loop_ms": loop_ms, "rssi": rssi, "uptime": uptime}
        target.last_seen = time.time()
        if packet_id > target.last_packet_id:
            target.last_packet_id = packet_id


def update_gui():
    # 1) drain UDP
    while True:
        try:
            raw_bytes, addr = sock.recvfrom(1024)
        except (BlockingIOError, OSError):
            break
        handle_packet(raw_bytes, addr)

    # 2) sweep dead sessions: a hand that went silent gets unbound so HELLO can re-handshake
    for h in ("left", "right"):
        hs = hands[h]
        if hs.mac is not None and not hs.is_alive():
            print(f"[server] {h} hand ({hs.mac} @ {hs.ip}) went silent; unbinding.")
            pending[hs.mac] = {"ip": hs.ip, "fw": hs.fw, "last_seen": time.time(),
                               "channel_count": 12, "hand_hint": HAND_UNKNOWN}
            cfg["mac_hand"].pop(hs.mac, None)
            save_config(cfg)
            hs.mac = None
            hs.ip = None
            hs.reset_stream_state()
            global _last_known_macs
            _last_known_macs = set()  # force rebuild next pass
    # also drop stale pending entries
    now = time.time()
    for mac in list(pending.keys()):
        if now - pending[mac].get("last_seen", 0) > DEVICE_TIMEOUT_S * 2:
            pending.pop(mac, None)
    rebuild_device_list()       # only does work when MAC set changes
    refresh_device_values()     # cheap per-frame label text update

    # 3) keepalives for alive hands
    for h in ("left", "right"):
        hs = hands[h]
        if hs.ip and hs.is_alive() and (now - hs.last_keepalive_sent) >= KEEPALIVE_INTERVAL_S:
            hs.send_keepalive()

    # 4) diagnostics
    parts = []
    for h in ("left", "right"):
        hs = hands[h]
        if hs.is_alive():
            parts.append(f"{h.upper()}:{hs.mac} id={hs.last_packet_id} rssi={hs.meta['rssi']}dBm "
                         f"i2c={hs.meta['i2c_ms']}ms loop={hs.meta['loop_ms']}ms")
        else:
            parts.append(f"{h.upper()}:--")
    diag_text.set("  |  ".join(parts))

    # 5) render -- pre-allocated items, just coords() updates
    render_right(hands["right"], get_vr_flex())
    render_left(hands["left"])

    root.after(33, update_gui)   # 30 FPS - plenty for a skeleton preview


def on_closing():
    print("Shutting down...")
    for h in ("left", "right"):
        hs = hands[h]
        if hs.ip:
            try:
                sock.sendto(struct.pack(KEEP_FMT, MAGIC, TYPE_BYE, 3, 0, hs.last_packet_id),
                            (hs.ip, hs.port))
            except Exception:
                pass
    try:
        save_config(cfg)
    except Exception:
        pass
    try:
        if vr_enabled:
            openvr.shutdown()
    except Exception:
        pass
    sock.close()
    root.destroy()


root.protocol("WM_DELETE_WINDOW", on_closing)
# Initial render using pre-allocated view objects.
render_right(hands["right"], (0.0, ("None", 0.0, 0.0, 0.0), 0.0))
render_left(hands["left"])
root.after(33, update_gui)   # 30 FPS - plenty for a skeleton preview
root.mainloop()