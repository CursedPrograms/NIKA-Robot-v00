"""nika_telemetry.py - her session as a CSV, for scripts/fortran (telemetry_stats, make_dataset).

The same columns as MILA's exports, so the same tools read them, plus what's only hers: the thought
she's having and its pitch. Saved in Documents\\NIKA-telemetry.
"""
import os
import time

HEADER = ("timestamp,dist_cm,left_cm,right_cm,temp_c,hum_pct,speed_pct,mode,last_cmd,guard,motor_l,motor_r,"
          "thought,hz\n")
FIELDS = ("dist", "left", "right", "temp", "hum", "speed")
MAX_ROWS = 20000


def folder():
    return os.path.join(os.path.expanduser("~"), "Documents", "NIKA-telemetry")


def number(v):
    """Her numbers as text, blank if she has none ("---" is no reading)."""
    try:
        return f"{float(v):g}"
    except (TypeError, ValueError):
        return ""


class Recorder:
    def __init__(self):
        self.rows = []
        self.seen = None

    def add(self, status, now=None):
        """One /status. The controller draws faster than she answers, so a repeat isn't a new reading."""
        if not status or status is self.seen:
            return
        self.seen = status
        if len(self.rows) >= MAX_ROWS:
            del self.rows[0]
        at = time.localtime(now) if now else time.localtime()
        ms = int(((now or time.time()) % 1) * 1000)
        cells = [time.strftime("%Y-%m-%d %H:%M:%S", at) + f".{ms:03d}"]
        cells += [number(status.get(k)) for k in FIELDS]
        cells += [str(status.get("mode", "")), str(status.get("cmd", "")), number(status.get("guard")),
                  number(status.get("ml")), number(status.get("mr")), str(status.get("thought", "")),
                  number(status.get("hz"))]
        self.rows.append(",".join(c.replace(",", " ") for c in cells))

    def export(self):
        """Writes the session; returns its path, or None if there's nothing yet."""
        if not self.rows:
            return None
        os.makedirs(folder(), exist_ok=True)
        path = os.path.join(folder(), time.strftime("telemetry_%Y%m%d_%H%M%S.csv"))
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(HEADER + "\n".join(self.rows) + "\n")
        return path
