#!/usr/bin/env python3
"""Test server for the Web App Test tile: serves the page and logs every request and page report."""
import io, json, math, os, struct, sys, threading, time, wave

import lameenc
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "www")
LOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "server.log")


def beep_wav():
    rate, buf = 48000, io.BytesIO()
    frames = bytearray()
    for i in range(rate * 2):  # 2 s: 150 ms 880 Hz beep, then silence
        v = int(8000 * math.sin(2 * math.pi * 880 * i / rate)) if i < rate * 0.15 else 0
        frames += struct.pack("<h", v)
    with wave.open(buf, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate); w.writeframes(bytes(frames))
    return buf.getvalue()


BEEP = beep_wav()
RATE = 44100


def tone(n, start):
    """n stereo s16 frames of a 150 ms 880 Hz beep every 2 s, from sample index start."""
    out = bytearray()
    for i in range(start, start + n):
        t = i % (RATE * 2)
        v = int(8000 * math.sin(2 * math.pi * 880 * i / RATE)) if t < RATE * 0.15 else 0
        out += struct.pack("<hh", v, v)
    return bytes(out)


def encoder():
    e = lameenc.Encoder()
    e.set_bit_rate(128); e.set_in_sample_rate(RATE); e.set_channels(2); e.set_quality(5)
    return e


def beep_mp3():
    e = encoder()
    return e.encode(tone(RATE * 2, 0)) + e.flush()


BEEP_MP3 = beep_mp3()


def long_mp3(seconds):
    e = encoder()
    out = bytearray()
    for start in range(0, RATE * seconds, RATE):
        out += e.encode(tone(RATE, start))
    return bytes(out + e.flush())


LONG_MP3 = long_mp3(300)  # 5 minutes, a normal file with a length


class LiveFile:
    """One hour of MP3 with a fixed length, produced in real time (about 1 s ahead)."""
    LENGTH = 3600 * 16000  # 128 kbit/s

    def __init__(self):
        self.buf = bytearray()
        self.cond = threading.Condition()
        self.began = None

    def start(self):
        with self.cond:
            if self.began is None:
                self.began = time.time()
                threading.Thread(target=self.produce, daemon=True).start()

    def produce(self):
        e, pos, chunk = encoder(), 0, RATE // 10
        while len(self.buf) < self.LENGTH:
            data = e.encode(tone(chunk, pos))
            pos += chunk
            with self.cond:
                self.buf += data[: self.LENGTH - len(self.buf)]
                self.cond.notify_all()
            ahead = pos / RATE - (time.time() - self.began)
            if ahead > 1.0:
                time.sleep(ahead - 1.0)

    def read(self, start, n, timeout=30):
        """Up to n bytes from start; waits for bytes that are not produced yet."""
        with self.cond:
            if start > len(self.buf) + 64000:  # far ahead of the live point: filler
                return b"\0" * min(n, 65536)
            self.cond.wait_for(lambda: len(self.buf) > start, timeout)
            return bytes(self.buf[start:start + n])


LIVE = LiveFile()

# The newest live WAV stream: when it began and how many seconds of audio it has sent,
# so page reports (audio.currentTime) can be turned into a measured delay.
WAV_STATE = {"began": None, "sent_seconds": 0.0}


def tiny_png(w, h):
    """A plain green w x h PNG, built by hand (no imaging library)."""
    import zlib
    raw = b"".join(b"\x00" + b"\x20\xc0\x60" * w for _ in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


CARD_PNG = tiny_png(320, 40)


def log(line):
    line = time.strftime("%H:%M:%S ") + line
    print(line, flush=True)
    with open(LOG, "a") as f:
        f.write(line + "\n")


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        log(f"GET {self.path} from {self.client_address[0]} UA={self.headers.get('User-Agent')!r}")
        path = self.path.split("?")[0]
        if path.endswith(".mp3") or path.endswith(".wav"):
            hdrs = {k: v for k, v in self.headers.items() if k.lower() not in ("user-agent", "host")}
            log(f"  headers {hdrs}")
        if path == "/stream.mp3":
            return self.stream_mp3()
        if path == "/streamlen.mp3":
            return self.stream_mp3(fake_length=1 << 30)
        if path == "/card.png":
            return self.send_file(CARD_PNG, "image/png")
        if path == "/live.wav":
            return self.send_live_wav()
        if path == "/live.mp3":
            return self.send_live()
        if path == "/long.mp3":
            return self.send_file(LONG_MP3, "audio/mpeg")
        if path == "/beep.wav":
            body, ctype = BEEP, "audio/wav"
        elif path == "/beep.mp3":
            return self.send_file(BEEP_MP3, "audio/mpeg")
        elif path in ("/", "/index.html", "/test.html"):
            name = "test.html" if path == "/test.html" else "index.html"
            with open(os.path.join(ROOT, name), "rb") as f:
                body, ctype = f.read(), "text/html; charset=utf-8"
        else:
            self.send_response(404); self.end_headers(); return
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def send_file(self, body, ctype):
        """A file with Accept-Ranges and single-range support."""
        rng = self.headers.get("Range")
        start, end = 0, len(body) - 1
        if rng and rng.startswith("bytes="):
            a, _, b = rng[6:].split(",")[0].partition("-")
            start = int(a) if a else max(0, len(body) - int(b))
            end = min(int(b), len(body) - 1) if (a and b) else end
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{end}/{len(body)}")
        else:
            self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(end - start + 1))
        self.end_headers()
        try:
            self.wfile.write(body[start:end + 1])
        except (BrokenPipeError, ConnectionResetError):
            pass
        log(f"  sent bytes {start}-{end} of {len(body)}")

    def send_live_wav(self):
        """Live PCM WAV beeps in real time, 0.25 s ahead. ?rate=&bits= pick the format."""
        from urllib.parse import parse_qs, urlparse
        q = parse_qs(urlparse(self.path).query)
        rate = int(q.get("rate", [RATE])[0])
        bits = int(q.get("bits", [16])[0])
        # prefill: ms of silence sent at once (meets the player's start buffer);
        # hold: ms to then send nothing, so the player drains that buffer.
        prefill = int(q.get("prefill", [0])[0])
        hold = int(q.get("hold", [0])[0])
        frame = 2 * bits // 8
        byte_rate = rate * frame
        data_len = (0xFFFFFFF0 - 64) // byte_rate * byte_rate
        header = (b"RIFF" + struct.pack("<I", 36 + data_len) + b"WAVEfmt " +
                  struct.pack("<IHHIIHH", 16, 1, 2, rate, byte_rate, frame, bits) +
                  b"data" + struct.pack("<I", data_len))
        # One 2 s beep period in this format, looped.
        period = bytearray()
        for i in range(rate * 2):
            v = math.sin(2 * math.pi * 880 * i / rate) * 0.25 if i < rate * 0.15 else 0.0
            sample = int(v * (2 ** (bits - 1) - 1)).to_bytes(bits // 8, "little", signed=True)
            period += sample + sample
        period = bytes(period)
        log(f"  wav format {rate} Hz {bits} bit, {byte_rate} B/s")
        rng = self.headers.get("Range")
        total = len(header) + data_len
        start = 0
        if rng and rng.startswith("bytes=") and rng[6:].split("-")[0]:
            start = int(rng[6:].split("-")[0])
        if start > 0:
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{total - 1}/{total}")
        else:
            self.send_response(200)
        self.send_header("Content-Type", "audio/wav")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(total - start))
        self.end_headers()
        began, pos = time.time(), 0
        chunk = byte_rate // 20 // frame * frame
        try:
            if start == 0:
                self.wfile.write(header)
            sent_audio = 0.0
            if prefill:
                silence = bytes(byte_rate * prefill // 1000 // frame * frame)
                self.wfile.write(silence)
                self.wfile.flush()
                sent_audio = len(silence) / byte_rate
                time.sleep(hold / 1000)
            # From here on, live audio: real time measured from now.
            began = time.time() - sent_audio + hold / 1000 if prefill else time.time()
            WAV_STATE["began"] = began
            WAV_STATE["base"] = sent_audio
            WAV_STATE["live_start"] = time.time()
            WAV_STATE["byte_rate"] = byte_rate
            WAV_STATE["pos0"] = 0
            live_began = time.time()
            while True:
                at = pos % len(period)
                part = period[at:at + chunk]
                if len(part) < chunk:
                    part += period[:chunk - len(part)]
                self.wfile.write(part)
                pos += chunk
                WAV_STATE["sent_seconds"] = sent_audio + pos / byte_rate
                ahead = pos / byte_rate - (time.time() - live_began)
                if ahead > (0.05 if prefill else 0.25):
                    time.sleep(ahead - (0.05 if prefill else 0.25))
        except (BrokenPipeError, ConnectionResetError, OSError):
            log(f"  live wav closed after {time.time() - began:.1f}s (start {start})")

    def send_live(self):
        LIVE.start()
        total = LIVE.LENGTH
        rng = self.headers.get("Range")
        start, end = 0, total - 1
        if rng and rng.startswith("bytes="):
            a, _, b = rng[6:].split(",")[0].partition("-")
            start = int(a) if a else max(0, total - int(b))
            end = min(int(b), total - 1) if (a and b) else end
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{end}/{total}")
        else:
            self.send_response(200)
        self.send_header("Content-Type", "audio/mpeg")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(end - start + 1))
        self.end_headers()
        pos, began = start, time.time()
        try:
            while pos <= end:
                data = LIVE.read(pos, min(65536, end - pos + 1))
                if not data:
                    break
                self.wfile.write(data)
                pos += len(data)
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        log(f"  live range {start}-{end}: sent {pos - start} bytes in {time.time() - began:.1f}s (produced {len(LIVE.buf)})")

    def stream_mp3(self, fake_length=None):
        """Endless live MP3, paced in real time like internet radio."""
        self.send_response(200)
        self.send_header("Content-Type", "audio/mpeg")
        if fake_length:
            self.send_header("Content-Length", str(fake_length))
        self.send_header("Cache-Control", "no-store")
        self.send_header("icy-name", "PS5 Speaker test")
        self.end_headers()
        e, pos, began = encoder(), 0, time.time()
        chunk = RATE // 10
        try:
            while True:
                self.wfile.write(e.encode(tone(chunk, pos)))
                self.wfile.flush()
                pos += chunk
                ahead = pos / RATE - (time.time() - began)
                if ahead > 1.0:  # keep at most ~1 s buffered ahead of real time
                    time.sleep(ahead - 1.0)
        except (BrokenPipeError, ConnectionResetError, OSError):
            log(f"STREAM closed after {time.time() - began:.1f}s ({pos / RATE:.1f}s of audio sent) to {self.client_address[0]}")

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        data = self.rfile.read(n).decode("utf-8", "replace")
        extra = ""
        try:
            at = json.loads(data).get("at")
            if WAV_STATE.get("live_start") and at:
                extra = f"  [delay {WAV_STATE['sent_seconds'] - at:.2f} s]"
        except ValueError:
            pass
        log(f"PAGE {self.client_address[0]} {data}{extra}")
        self.send_response(204); self.end_headers()


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    log(f"listening on 0.0.0.0:{port}")
    ThreadingHTTPServer(("0.0.0.0", port), Handler).serve_forever()
