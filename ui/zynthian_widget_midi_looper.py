#!/usr/bin/python3
# -*- coding: utf-8 -*-
import bisect
import glob
import logging
import mmap
import os
import struct
import time
import tkinter

from zyngui import zynthian_gui_config
from zyngui import zynthian_widget_base

MULTI_INSTANCE = True

STATES = ["EMPTY", "ARM REC", "REC", "STOPPING", "PLAY", "OVERDUB", "STOP", "ARM PLAY"]
COLORS = ["#808080", "#ff3030", "#ff3030", "#ff3030", "#30d030", "#ffb000", "#c0c0c0", "#30d030"]
LAYER_COLORS = ["#ff6b6b", "#ffa94d", "#ffe066", "#69db7c", "#38d9a9", "#4dabf7", "#9775fa", "#f783ac"]
MSGS = {1: "Saved", 2: "Loaded", 3: "Slot deleted", 4: "Slot empty", 5: "Save failed",
        6: "No loop", 7: "Quantized", 8: "Loop /2", 9: "Loop x2", 10: "Not possible",
        11: "Edit undone", 12: "Select a slot (1-16)", 13: "Load pending...",
        14: "Grid is OFF", 15: "Busy", 16: "Layer transposed",
        17: "No layers selected", 18: "Transpose is 0", 19: "Exporting..."}
CONFIRMS = {1: "Press again: CLEAR", 2: "Press again: DELETE LAYER", 3: "Press again: DELETE SLOT"}

HDR = struct.Struct("<8I8I8i2I")    # magic,seq,ver,loop_len,nlayers,bpb,state,mute ; n[8] ; transp[8] ; ncc,pad
EV = struct.Struct("<IIBBBB")
MAGIC = 0x4D4C5A31
PPQN = 960
try:
    from zynlibs.zynseq import zynseq as zynseq_mod
except Exception:
    zynseq_mod = None
EXPORT_STEPS_PER_BEAT = 4          # шагов на долю в экспортируемом паттерне
EXPORT_USE_OFFSET = True           # сохранять точное время нот через смещение внутри шага
CC_LANE = 0.74                      # доля высоты, которая остаётся нотам (остальное под дорожку CC)


def dim(col, f):
    r, g, b = int(col[1:3], 16), int(col[3:5], 16), int(col[5:7], 16)
    return "#%02x%02x%02x" % (int(r * f), int(g * f), int(b * f))


def cc_value(st, d1, d2):
    typ = st & 0xF0
    if typ == 0xE0:
        return ((d2 << 7) | d1) / 16383.0
    if typ == 0xD0:
        return d1 / 127.0
    return d2 / 127.0


class zynthian_widget_midi_looper(zynthian_widget_base.zynthian_widget_base):

    def __init__(self, parent):
        super().__init__(parent)
        base = self.rows
        self.rows_normal = max(2, base // 2)
        self.rows_edit = min(max(base - 1, 2), self.rows_normal + 1)
        self.rows = self.rows_normal
        self.widget_canvas = tkinter.Canvas(
            self, highlightthickness=0, relief='flat',
            bg=zynthian_gui_config.color_bg)
        self.widget_canvas.grid(sticky='news')
        self.widget_canvas.bind("<ButtonPress-1>", self._on_click)
        self.last_msg = None
        self.toast = ""
        self.toast_until = 0.0
        self.mm = None
        self.mm_path = None
        self.cache_ver = -1
        self.data = None
        self.roll_key = None
        self.hits = []
        self.last_click = 0.0

    def on_size(self, event):
        if not super().on_size(event):
            return
        self.widget_canvas.grid(row=0, column=0, sticky='news')
        self.roll_key = None

    # ---------- вспомогательное ----------
    def _font(self, h, div, mn):
        return (zynthian_gui_config.font_family, max(mn, h // div))

    def _page_symbols(self):
        try:
            return {z.symbol for z in self.zyngui_control.zcontrollers if z is not None}
        except Exception:
            return set()

    def _zval(self, sym, default):
        try:
            return float(self.processor.controllers_dict[sym].value)
        except Exception:
            return default

    def _zset(self, sym, value):
        try:
            self.processor.controllers_dict[sym].set_value(value)
            return True
        except Exception:
            return False

    def _set_rows(self, n):
        if n == self.rows:
            return
        self.rows = n
        try:
            name = "midi_looper#%s" % self.processor.id
            self.zyngui.after_idle(self.zyngui_control.display_widget, name)
        except Exception:
            pass

    def _on_click(self, event):
        now = time.monotonic()
        if now - self.last_click < 0.25:
            return
        for (x0, y0, x1, y1, i) in self.hits:
            if x0 <= event.x <= x1 and y0 <= event.y <= y1:
                self.last_click = now
                mask = int(self._zval("qmask", 255))
                self._zset("qmask", mask ^ (1 << i))
                return

    def _open_mm(self):
        path = None
        try:
            pid = self.processor.engine.proc.pid
            path = "/dev/shm/midi_looper_%d" % pid
        except Exception:
            files = glob.glob("/dev/shm/midi_looper_*")
            if len(files) == 1:
                path = files[0]
        if not path or not os.path.exists(path):
            return False
        if path == self.mm_path and self.mm:
            return True
        try:
            if self.mm:
                self.mm.close()
            with open(path, "rb") as f:
                self.mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            self.mm_path = path
            self.cache_ver = -1
            return True
        except Exception:
            self.mm = None
            return False

    def _read(self):
        if not self._open_mm():
            return False
        mm = self.mm
        for _ in range(3):
            h = HDR.unpack_from(mm, 0)
            if h[0] != MAGIC or (h[1] & 1):
                continue
            seq, ver = h[1], h[2]
            if ver == self.cache_ver and self.data is not None:
                return True
            nl = min(h[4], 8)
            ns = list(h[8:16])
            ncc = min(h[24], 16384)
            total = sum(ns[:nl])
            raw = mm[HDR.size:HDR.size + (total + ncc) * EV.size]
            if HDR.unpack_from(mm, 0)[1] != seq:
                continue
            layers, ccs, fnotes, off = [], [], [], 0
            for i in range(nl):
                chunk = raw[off * EV.size:(off + ns[i]) * EV.size]
                notes, fl = [], []
                for (t, d, st, p, v, _) in EV.iter_unpack(chunk):
                    typ = st & 0xF0
                    if typ == 0x90 and d > 0:
                        notes.append((t, d, p))
                        fl.append((t, d, st, p, v))
                    elif typ in (0xB0, 0xA0, 0xD0, 0xE0):          # старые слои могли содержать CC
                        ccs.append((t, st, p, v))
                layers.append(notes)
                fnotes.append(fl)
                off += ns[i]
            chunk = raw[total * EV.size:(total + ncc) * EV.size]
            for (t, d, st, p, v, _) in EV.iter_unpack(chunk):
                ccs.append((t, st, p, v))
            self.data = {"loop_len": h[3], "bpb": h[5] or 4, "mute": h[7],
                         "transp": list(h[16:24]), "layers": layers, "ccs": ccs, "fnotes": fnotes}
            self.cache_ver = ver
            return True
        return False

    # ---------- piano roll + CC ----------
    def _draw_roll(self, x0, y0, x1, y1, sel, pend, h):
        c = self.widget_canvas
        c.delete("roll")
        d = self.data
        fam = zynthian_gui_config.font_family
        if not d or d["loop_len"] <= 0 or not d["layers"]:
            c.create_text((x0 + x1) // 2, (y0 + y1) // 2, text="no loop", fill="#808080",
                          tags="roll", font=(fam, 9))
            return
        L = d["loop_len"]
        bar = d["bpb"] * PPQN
        w = x1 - x0
        has_cc = bool(d["ccs"])
        ny1 = y0 + int((y1 - y0) * CC_LANE) if has_cc else y1       # низ области нот
        cy0, cy1 = ny1 + 4, y1                                        # дорожка CC

        pitches = []
        for i, notes in enumerate(d["layers"]):
            cur = d["transp"][i]
            for (t, du, p) in notes:
                pitches.append(p + cur)
                if i == sel and pend:
                    pitches.append(p + cur + pend)
        if pitches:
            lo, hi = max(0, min(pitches) - 1), min(127, max(pitches) + 1)
        else:
            lo, hi = 55, 67
        if hi - lo < 11:
            mid = (hi + lo) // 2
            lo, hi = mid - 6, mid + 6
        span = hi - lo + 1
        rh = (ny1 - y0) / float(span)

        tick = 0
        bar_no = 1
        while tick < L:
            x = x0 + w * tick / L
            major = (tick % bar == 0)
            c.create_line(x, y0, x, y1, fill=("#6a6a6a" if major else "#2a2a2a"), tags="roll")
            if major and w / max(1, L / bar) > 18:
                c.create_text(x + 2, y0, anchor="nw", text=str(bar_no), fill="#9a9a9a", tags="roll",
                              font=(fam, 7))
                bar_no += 1
            tick += PPQN
        c.create_rectangle(x0, y0, x1, ny1, outline="#555555", tags="roll")
        if has_cc:
            c.create_rectangle(x0, cy0, x1, cy1, outline="#555555", tags="roll")
            c.create_text(x0 + 3, cy0 + 1, anchor="nw", text="CC", fill="#808080", tags="roll", font=(fam, 7))

        def rect(t, du, p, color, outline=False):
            y = ny1 - (p - lo + 1) * rh
            hgt = max(2.0, rh - 1)
            parts = [(t, min(du, L - t))]
            if t + du > L:
                parts.append((0, t + du - L))
            for (a, ln) in parts:
                xa = x0 + w * a / L
                xb = x0 + w * (a + ln) / L
                if outline:
                    c.create_rectangle(xa, y, max(xb, xa + 2), y + hgt, outline=color, tags="roll")
                else:
                    c.create_rectangle(xa, y, max(xb, xa + 2), y + hgt, fill=color, width=0, tags="roll")

        # CC: каждый контроллер своим цветом (ступенчатая линия + полупрозрачная штриховка)
        if has_cc:
            curves = {}
            for (t, st, p, v) in d["ccs"]:
                typ = st & 0xF0
                key = (st, p if typ in (0xB0, 0xA0) else 0)
                curves.setdefault(key, []).append((min(t, L), cc_value(st, p, v)))
            for idx, key in enumerate(sorted(curves)):
                ev = sorted(curves[key])
                if len(ev) > 600:
                    ev = ev[::len(ev) // 600 + 1]
                col = LAYER_COLORS[idx % 8]
                pts = [x0, cy1 - ev[-1][1] * (cy1 - cy0)]
                for (t, v) in ev:
                    x = x0 + w * t / L
                    yv = cy1 - v * (cy1 - cy0)
                    pts += [x, pts[-1], x, yv]
                pts += [x1, pts[-1]]
                c.create_polygon(*(pts + [x1, cy1, x0, cy1]), fill=col, stipple="gray25",
                                 outline="", tags="roll")
                c.create_line(*pts, fill=col, width=1, tags="roll")

        # ноты
        total = sum(len(n) for n in d["layers"])
        step = max(1, total // 1500)
        order = [i for i in range(len(d["layers"])) if i != sel]
        if 0 <= sel < len(d["layers"]):
            order.append(sel)
        for i in order:
            muted = (d["mute"] >> i) & 1
            base = LAYER_COLORS[i % 8]
            if muted:
                col = "#505050"
            elif i == sel:
                col = base
            else:
                col = dim(base, 0.55)
            cur = d["transp"][i]
            notes = d["layers"][i]
            for k in range(0, len(notes), step):
                t, du, p = notes[k]
                q = p + cur
                if lo <= q <= hi:
                    rect(t, du, q, col)
                if i == sel and pend:
                    g = q + pend
                    if lo <= g <= hi:
                        rect(t, du, g, "#ffffff", outline=True)

    # ---------- экспорт: .mid + паттерн секвенсора ----------
    def _toast(self, text, secs=3.0):
        self.toast = text
        self.toast_until = time.monotonic() + secs

    @staticmethod
    def _vlq(n):
        out = [n & 0x7F]
        n >>= 7
        while n:
            out.append((n & 0x7F) | 0x80)
            n >>= 7
        return bytes(reversed(out))

    def _write_smf(self, notes, ccs, L, bpm, bpb):
        ev = []
        for (t, du, ch, p, v) in notes:
            end = min(t + du, L)
            if end <= t:
                end = t + 1
            ev.append((t, 1, bytes([0x90 | ch, p, v])))
            ev.append((end, 0, bytes([0x80 | ch, p, 0])))
        for (t, st, d1, d2) in ccs:
            if (st & 0xF0) in (0xC0, 0xD0):
                ev.append((t, 1, bytes([st, d1])))
            else:
                ev.append((t, 1, bytes([st, d1, d2])))
        ev.sort(key=lambda e: (e[0], e[1]))
        tr = bytearray()
        tempo = int(60000000.0 / max(20.0, bpm))
        tr += b"\x00\xff\x51\x03" + tempo.to_bytes(3, "big")
        tr += b"\x00\xff\x58\x04" + bytes([max(1, min(bpb, 16)), 2, 24, 8])
        name = b"MIDI Looper"
        tr += b"\x00\xff\x03" + bytes([len(name)]) + name
        last = 0
        for (t, _, data) in ev:
            tr += self._vlq(t - last) + data
            last = t
        tr += self._vlq(max(0, L - last)) + b"\xff\x2f\x00"
        base = os.environ.get("ZYNTHIAN_MY_DATA_DIR", "/zynthian/zynthian-my-data")
        d = os.path.join(base, "capture")
        os.makedirs(d, exist_ok=True)
        path = os.path.join(d, time.strftime("midi_looper_%Y%m%d_%H%M%S.mid"))
        with open(path, "wb") as f:
            f.write(b"MThd" + struct.pack(">IHHH", 6, 0, 1, PPQN))
            f.write(b"MTrk" + struct.pack(">I", len(tr)) + bytes(tr))
        return path

    @staticmethod
    def _is_playing(state):
        names = []
        if zynseq_mod:
            for n in ("SEQ_PLAYING", "SEQ_STARTING", "SEQ_STOPPING", "SEQ_RESTARTING"):
                v = getattr(zynseq_mod, n, None)
                if v is not None:
                    names.append(v)
        if names:
            return state in names
        stopped = getattr(zynseq_mod, "SEQ_STOPPED", 0) if zynseq_mod else 0
        return state != stopped

    def _write_pattern(self, notes, ccs, L, bpb, chan):
        zs = self.zyngui.state_manager.zynseq
        lib = zs.libseq
        scene, phrase = zs.scene, zs.phrase
        # 1. что сейчас в ячейке этой цепочки (фраза x канал)
        try:
            cell_pat = int(lib.getPattern(scene, phrase, chan, 0, 0))
        except Exception:
            cell_pat = -1
        try:
            playing = self._is_playing(lib.getPlayState(scene, phrase, chan))
        except Exception:
            playing = False
        assign = False
        if playing and cell_pat >= 1:
            target = cell_pat                       # играющий паттерн перезаписываем
        else:
            target = None
            for n in range(1, 1000):                # первый пустой паттерн
                try:
                    empty = zs.is_pattern_empty(n)
                except Exception:
                    empty = lib.isPatternEmpty(n)
                if empty:
                    target = n
                    break
            if target is None:
                raise RuntimeError("no free pattern")
            assign = not (cell_pat >= 1)           # свободную ячейку займём новым паттерном
        # 2. заполняем паттерн
        lib.selectPattern(target)
        lib.clearPattern(target)
        spb = EXPORT_STEPS_PER_BEAT
        lib.setStepsPerBeat(spb)
        beats = max(1, int(round(L / float(PPQN))))
        lib.setBeatsInPattern(target, beats)
        step_ticks = PPQN / float(spb)
        nsteps = beats * spb
        for (t, du, ch, p, v) in notes:
            pos = t / step_ticks
            if EXPORT_USE_OFFSET:
                step = int(pos)
                off = pos - step
                if off < 0.03:
                    off = 0.0
                elif off > 0.97:
                    step += 1
                    off = 0.0
            else:
                step = int(round(pos))
                off = 0.0
            lib.addNote(step % nsteps, p, max(1, min(127, v)), max(0.1, du / step_ticks), off)
        # CC: движение ручки превращаем в плавные отрезки по шагам
        streams = {}
        for (t, st, d1, d2) in ccs:
            if (st & 0xF0) == 0xB0:
                streams.setdefault(d1, []).append((t, d2))
        for cc, evs in streams.items():
            evs.sort()
            segs, seg = [], [evs[0]]
            for e in evs[1:]:
                if e[0] - seg[-1][0] > step_ticks * 2.5:      # пауза: новое движение ручки
                    segs.append(seg)
                    seg = [e]
                else:
                    seg.append(e)
            segs.append(seg)
            for seg in segs:
                times = [e[0] for e in seg]

                def val_at(tk, seg=seg, times=times):
                    if tk <= times[0]:
                        return seg[0][1]
                    if tk >= times[-1]:
                        return seg[-1][1]
                    i = bisect.bisect_left(times, tk)
                    t0, v0 = seg[i - 1]
                    t1, v1 = seg[i]
                    return v0 if t1 == t0 else v0 + (v1 - v0) * (tk - t0) / float(t1 - t0)

                s0 = int(times[0] // step_ticks)
                s1 = int(times[-1] // step_ticks)
                for sidx in range(s0, s1 + 1):
                    va = int(round(val_at(sidx * step_ticks)))
                    vb = int(round(val_at((sidx + 1) * step_ticks)))
                    lib.addControl(sidx % nsteps, cc, max(0, min(127, va)), max(0, min(127, vb)), 1, 0)
        # 3. назначаем в ячейку, если она была свободна
        if assign:
            try:
                lib.setChannel(scene, phrase, chan, 0, chan)
            except Exception:
                pass
            lib.addPattern(scene, phrase, chan, 0, 0, target)
        return target

    def _export(self, m):
        self.cache_ver = -1
        ok = self._read()
        d = self.data
        if not ok or not d or d["loop_len"] <= 0:
            self._toast("Export: no loop data")
            return
        L, bpb = d["loop_len"], d["bpb"]
        notes = []
        for i, lay in enumerate(d["fnotes"]):
            if (d["mute"] >> i) & 1:
                continue
            tr = d["transp"][i]
            for (t, du, st, p, v) in lay:
                notes.append((t, du, st & 0x0F, max(0, min(127, p + tr)), v))
        ccs = sorted(d["ccs"])
        try:
            chan = int(self.processor.midi_chan)
        except Exception:
            chan = notes[0][2] if notes else 0
        mid_ok, pat = False, None
        try:
            path = self._write_smf(notes, ccs, L, float(m.get("bpm", 120.0)), bpb)
            logging.info("MIDI Looper: exported %s", path)
            mid_ok = True
        except Exception as e:
            logging.error("MIDI Looper: MID export failed => %s", e)
        try:
            pat = self._write_pattern(notes, ccs, L, bpb, chan)
        except Exception as e:
            logging.error("MIDI Looper: pattern export failed => %s", e)
        if mid_ok and pat is not None:
            self._toast("Exported: MID + pattern %s" % pat)
        elif mid_ok:
            self._toast("MID saved, pattern failed (see log)")
        elif pat is not None:
            self._toast("Pattern %s written, MID failed" % pat)
        else:
            self._toast("Export failed (see log)")

    # ---------- основной цикл ----------
    def refresh_gui(self):
        m = self.monitors or {}
        c = self.widget_canvas
        c.delete("dyn")
        w, h = max(self.width, 40), max(self.height, 40)
        fam = zynthian_gui_config.font_family
        now = time.monotonic()

        st = int(m.get("state", 0))
        if st < 0 or st > 7:
            st = 0
        run = int(m.get("run", 0))
        bars = m.get("bars", 0.0)
        pos = min(max(m.get("pos", 0.0), 0.0), 1.0)
        nl = int(m.get("layers", 0))
        mask = int(m.get("mute", 0))
        conf = int(m.get("confirm", 0))
        bpm = m.get("bpm", 0.0)
        beat = m.get("beat", 0.0)
        slots = int(m.get("slots", 0))
        msg = int(m.get("msg", 0))

        if self.last_msg is None:
            self.last_msg = msg
        elif msg != self.last_msg:
            self.last_msg = msg
            code = msg % 100
            if code in MSGS:
                self.toast = MSGS[code]
                self.toast_until = now + 2.0
            if code == 16:
                self._zset("transp", 0)
            if code == 19:
                self._export(m)

        blink = int(now * 4) % 2 == 0
        col = COLORS[st]
        if st in (1, 3, 7) and not blink:
            col = "#4a4a4a"
        if not run and st != 0:
            col = "#707070"

        syms = self._page_symbols()
        if "eapply" in syms:
            mode = "roll"
        elif "qapply" in syms:
            mode = "quant"
        elif "slot" in syms:
            mode = "slots"
        else:
            mode = "main"

        self._set_rows(self.rows_edit if mode == "roll" else self.rows_normal)

        info = f"{bars:.1f} bars" if st in (2, 3) else (f"{bars:g} bars" if bars > 0 else "")
        self.hits = []

        if mode == "roll":
            sel = int(self._zval("elayer", 1)) - 1
            pend = int(round(self._zval("transp", 0)))
            ok = self._read()
            cur = 0
            if self.data and 0 <= sel < len(self.data["transp"]):
                cur = self.data["transp"][sel]
            lcol = LAYER_COLORS[max(sel, 0) % 8]
            c.create_text(8, 1, anchor="nw", tags="dyn", fill=lcol,
                          text=f"Layer {sel + 1}   {cur:+d} > {cur + pend:+d}",
                          font=self._font(h, 13, 8))
            c.create_text(w - 8, 2, anchor="ne", tags="dyn", fill=zynthian_gui_config.color_tx,
                          text=info, font=self._font(h, 15, 8))
            x0, y0, x1, y1 = 8, int(h * 0.15), w - 8, int(h * 0.95)
            key = (self.cache_ver, sel, pend, w, h, ok)
            if key != self.roll_key:
                self.roll_key = key
                self._draw_roll(x0, y0, x1, y1, sel, pend, h)
            c.create_line(x0 + (x1 - x0) * pos, y0, x0 + (x1 - x0) * pos, y1,
                          fill="#ffffff", width=2, tags="dyn")
        else:
            if self.roll_key is not None:
                c.delete("roll")
                self.roll_key = None
            c.create_text(8, 2, anchor="nw", text=STATES[st], fill=col, tags="dyn",
                          font=self._font(h, 11, 9))
            c.create_text(w - 8, 3, anchor="ne", tags="dyn", fill=zynthian_gui_config.color_tx,
                          text=f"{info}   {bpm:.0f} BPM", font=self._font(h, 15, 8))
            y0, y1 = int(h * 0.28), int(h * 0.38)
            c.create_rectangle(8, y0, w - 8, y1, outline=zynthian_gui_config.color_tx_off, tags="dyn")
            c.create_rectangle(8, y0, 8 + int((w - 16) * pos), y1, fill=col, width=0, tags="dyn")

            if mode == "main":
                bw = (w - 16 - 18) // 4
                for i in range(4):
                    x = 8 + i * (bw + 6)
                    on = run and (int(beat) == i)
                    c.create_rectangle(x, int(h * 0.45), x + bw, int(h * 0.55), tags="dyn",
                                       outline=zynthian_gui_config.color_tx_off,
                                       fill=("#e0e0e0" if on and i == 0 else ("#808080" if on else "")))
                lw = (w - 16 - 7 * 4) // 8
                for i in range(8):
                    x = 8 + i * (lw + 4)
                    ya, yb = int(h * 0.63), int(h * 0.83)
                    lc = LAYER_COLORS[i]
                    if i < nl:
                        muted = (mask >> i) & 1
                        c.create_rectangle(x, ya, x + lw, yb, outline=lc, tags="dyn", width=2,
                                           fill=("" if muted else lc))
                        c.create_text(x + lw // 2, (ya + yb) // 2, text=str(i + 1), tags="dyn",
                                      fill=(lc if muted else "#000000"), font=self._font(h, 18, 7))
                    else:
                        c.create_rectangle(x, ya, x + lw, yb, outline=zynthian_gui_config.color_tx_off, tags="dyn")
            elif mode == "quant":
                qmask = int(self._zval("qmask", 255))
                c.create_text(8, int(h * 0.44), anchor="nw", text="Quantize layers (tap to toggle):",
                              fill=zynthian_gui_config.color_tx, tags="dyn", font=self._font(h, 15, 8))
                lw = (w - 16 - 7 * 6) // 8
                for i in range(8):
                    x = 8 + i * (lw + 6)
                    ya, yb = int(h * 0.58), int(h * 0.88)
                    exists = i < nl
                    on = exists and ((qmask >> i) & 1)
                    lc = LAYER_COLORS[i]
                    c.create_rectangle(x, ya, x + lw, yb, tags="dyn",
                                       outline=(lc if exists else zynthian_gui_config.color_tx_off),
                                       width=2 if exists else 1,
                                       fill=(lc if on else ""))
                    c.create_text(x + lw // 2, (ya + yb) // 2, text=str(i + 1), tags="dyn",
                                  fill=("#000000" if on else (lc if exists else "#505050")),
                                  font=self._font(h, 14, 8))
                    if exists:
                        self.hits.append((x, ya, x + lw, yb, i))
            else:  # slots
                cur = int(self._zval("slot", 0))
                sw = (w - 16 - 7 * 4) // 8
                for i in range(16):
                    r, k = divmod(i, 8)
                    x = 8 + k * (sw + 4)
                    ya = int(h * (0.48 + 0.21 * r))
                    yb = ya + int(h * 0.17)
                    used = (slots >> i) & 1
                    selected = (cur == i + 1)
                    c.create_rectangle(x, ya, x + sw, yb, tags="dyn",
                                       outline=("#ffffff" if selected else zynthian_gui_config.color_tx_off),
                                       width=(3 if selected else 1),
                                       fill=(zynthian_gui_config.color_hl if used else ""))
                    c.create_text(x + sw // 2, (ya + yb) // 2, text=str(i + 1), tags="dyn",
                                  fill=("#000000" if used else "#c0c0c0"), font=self._font(h, 18, 7))

        text, tcol = None, "#ff9000"
        if conf in CONFIRMS:
            text = CONFIRMS[conf]
        elif now < self.toast_until:
            text, tcol = self.toast, "#ffffff"
        elif not run:
            text = "waiting for zynseq (press Play)"
        if text:
            c.create_text(w // 2, int(h * 0.99), anchor="s", text=text, fill=tcol, tags="dyn",
                          font=self._font(h, 15, 8))
