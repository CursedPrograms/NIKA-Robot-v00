"""nika_sound.py - hear NIKA on this PC: she has no speaker, so her sound comes over WiFi.

Her ESP8266's GET /sound?since=<seq> gives her thought (the synth brain's square wave, in Hz)
and every beep her UNO played since then (chirps, Brainfuck talk, wall ticks), each stamped
with her clock in ms. This plays both with pygame, as square waves:

    sound = NikaSound("http://192.168.4.1:5013")
    sound.toggle()          # L in nika_controller.py: listen / mute
    sound.on                # listening?

The thought hums quietly underneath; beeps are scheduled by her timestamps (SND_LEAD_S
behind), so a chirp keeps its rhythm whatever the polling does. No numpy: the waves are
made with the array module.
"""
import array
import threading
import time

import pygame
import requests

RATE = 22050
THOUGHT_VOL = 0.06
BEEP_VOL = 0.25
POLL_S = 0.15
SND_LEAD_S = 0.3


def _square(hz, seconds, volume):
    """A mono 16-bit square wave as a pygame Sound (whole cycles, so a loop doesn't click)."""
    hz = max(20, int(hz))
    period = max(2, int(RATE / hz))
    cycles = max(1, int(seconds * RATE / period))
    amp = int(32767 * volume)
    half = period // 2
    one = [amp] * half + [-amp] * (period - half)
    samples = array.array("h", one * cycles)
    return pygame.mixer.Sound(buffer=samples.tobytes())


class NikaSound:
    def __init__(self, base_url):
        self.base = base_url.rstrip("/")
        self.on = False
        self.error = ""
        self._hz = None
        self._thought_ch = None
        self._beep_ch = None
        self._cache = {}

    def toggle(self):
        if self.on:
            self.on = False
            return
        try:
            if not pygame.mixer.get_init():
                pygame.mixer.init(frequency=RATE, size=-16, channels=1)
            pygame.mixer.set_num_channels(max(8, pygame.mixer.get_num_channels()))
            self._thought_ch = pygame.mixer.Channel(6)
            self._beep_ch = pygame.mixer.Channel(7)
        except pygame.error as e:
            self.error = f"no sound device: {e}"
            return
        self.on, self.error, self._hz = True, "", None
        threading.Thread(target=self._run, daemon=True, name="nika-sound").start()

    def _beep(self, hz, ms):
        key = (int(hz), int(ms))
        if key not in self._cache:
            if len(self._cache) > 64:
                self._cache.clear()
            self._cache[key] = _square(hz, ms / 1000, BEEP_VOL)
        return self._cache[key]

    def _run(self):
        since, base = 0, None     # base: her clock (s) -> ours (time.monotonic)
        queue = []                # (when, hz, ms), by when
        while self.on:
            try:
                j = requests.get(f"{self.base}/sound", params={"since": since}, timeout=1.5).json()
                self.error = ""
                hz = int(j.get("hz") or 0)
                if hz and hz != self._hz:   # her thought changed pitch
                    self._hz = hz
                    self._thought_ch.play(_square(hz, 1.0, THOUGHT_VOL), loops=-1)
                if since == 0:
                    since = j.get("seq", 0)          # only what comes after Listen
                for seq, t, bhz, ms in j.get("beeps", []):
                    if seq <= since:
                        continue
                    now = time.monotonic()
                    when = None if base is None else base + t / 1000
                    if when is None or when < now or when > now + 2:
                        base = now + SND_LEAD_S - t / 1000
                        when = base + t / 1000
                    queue.append((when, bhz, ms))
                since = max(since, j.get("seq", since))
            except (requests.RequestException, ValueError, TypeError) as e:
                self.error = f"can't hear her ({type(e).__name__})"
            # play what's due, until the next poll
            until = time.monotonic() + POLL_S
            while self.on and time.monotonic() < until:
                queue.sort()
                while queue and queue[0][0] <= time.monotonic():
                    _, bhz, ms = queue.pop(0)
                    self._beep_ch.play(self._beep(bhz, ms))
                time.sleep(0.005)
        if self._thought_ch:
            self._thought_ch.stop()
        if self._beep_ch:
            self._beep_ch.stop()
