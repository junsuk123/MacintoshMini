"""Mini Macintosh 파일 변환기 (GUI)

MP4(및 일반 동영상)을 Mini Macintosh 펌웨어가 재생하는 형식으로 변환합니다.
  - 비디오: 240x240 MJPEG (baseline JPEG, fps 고정)
  - 오디오: 44100Hz 모노 ADTS AAC

기기 펌웨어(cfg::VideoFps=25, JPEGDEC baseline, libhelix AAC)와 맞춘 설정을
기본값으로 사용합니다. 표준 라이브러리 + 번들된 ffmpeg.exe 만 필요합니다.
"""

import os
import re
import sys
import queue
import threading
import subprocess
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

APP_TITLE = "Mini Macintosh 파일 변환기"
VIDEO_FILETYPES = [
    ("동영상 파일", "*.mp4 *.mov *.avi *.mkv *.m4v *.webm"),
    ("모든 파일", "*.*"),
]
# 프레임 하나가 펌웨어 버퍼(약 26KB)를 넘으면 재생 시 드롭됩니다. q:v 9 기준
# 240x240 프레임은 보통 6~10KB라 안전하며, 화질을 크게 올릴 때만 주의하면 됩니다.
CREATE_NO_WINDOW = 0x08000000 if os.name == "nt" else 0
DURATION_RE = re.compile(r"Duration:\s*(\d+):(\d+):(\d+\.\d+)")


def base_dir() -> str:
    """실행 파일(빌드) 또는 스크립트가 위치한 디렉터리."""
    if getattr(sys, "frozen", False):
        return os.path.dirname(sys.executable)
    return os.path.dirname(os.path.abspath(__file__))


def find_ffmpeg() -> str | None:
    """번들된 ffmpeg.exe 를 여러 후보 위치에서 탐색한다."""
    root = base_dir()
    exe = "ffmpeg.exe" if os.name == "nt" else "ffmpeg"
    candidates = [
        os.path.join(root, "_internal", exe),   # 빌드 배포 구조
        os.path.join(root, exe),                 # 실행 파일과 같은 폴더
        os.path.join(root, "..", "_internal", exe),  # source/ 에서 실행할 때
    ]
    for path in candidates:
        if os.path.isfile(path):
            return os.path.abspath(path)
    # PATH 에 설치된 ffmpeg 로 폴백
    from shutil import which
    return which("ffmpeg")


def probe_duration(ffmpeg: str, path: str) -> float:
    """ffmpeg 로 입력 길이(초)를 읽는다. 실패 시 0.0 (진행바는 비확정 모드)."""
    try:
        proc = subprocess.run(
            [ffmpeg, "-hide_banner", "-i", path],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            creationflags=CREATE_NO_WINDOW, text=True, errors="ignore",
        )
        m = DURATION_RE.search(proc.stdout or "")
        if m:
            h, mnt, sec = m.groups()
            return int(h) * 3600 + int(mnt) * 60 + float(sec)
    except Exception:
        pass
    return 0.0


def run_ffmpeg(cmd, total_dur, on_fraction, cancel_event):
    """ffmpeg 를 실행하며 -progress 출력을 파싱해 진행률(0~1)을 콜백으로 보고.

    반환: (returncode, tail_log). cancel_event 가 set 되면 프로세스를 종료.
    """
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        creationflags=CREATE_NO_WINDOW, text=True, errors="ignore", bufsize=1,
    )
    tail = []
    try:
        for line in proc.stdout:
            if cancel_event.is_set():
                proc.terminate()
                break
            line = line.strip()
            if not line:
                continue
            tail.append(line)
            if len(tail) > 20:
                tail.pop(0)
            if total_dur > 0 and line.startswith("out_time_us="):
                try:
                    us = int(line.split("=", 1)[1])
                    on_fraction(min(us / 1_000_000.0 / total_dur, 1.0))
                except ValueError:
                    pass
            elif line == "progress=end":
                on_fraction(1.0)
    finally:
        proc.wait()
    return proc.returncode, "\n".join(tail)


def build_video_cmd(ffmpeg, src, dst, fps, quality):
    # scale+crop 로 어떤 화면비든 중앙을 채워 240x240 을 만든다(레터박스 없음).
    # format=yuvj420p 로 JPEGDEC 가 요구하는 baseline JPEG 을 보장.
    vf = (
        f"fps={fps},"
        "scale=240:240:force_original_aspect_ratio=increase,"
        "crop=240:240,"
        "format=yuvj420p"
    )
    return [
        ffmpeg, "-hide_banner", "-nostats", "-y", "-i", src,
        "-an", "-vf", vf, "-c:v", "mjpeg", "-q:v", str(quality),
        "-f", "mjpeg", "-progress", "pipe:1", dst,
    ]


def build_audio_cmd(ffmpeg, src, dst, bitrate, volume_db):
    af = "loudnorm"
    if volume_db and volume_db > 0:
        af += f",volume=-{volume_db}dB"
    return [
        ffmpeg, "-hide_banner", "-nostats", "-y", "-i", src,
        "-vn", "-ac", "1", "-ar", "44100", "-c:a", "aac", "-b:a", bitrate,
        "-af", af, "-f", "adts", "-progress", "pipe:1", dst,
    ]


class ConverterApp:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.ffmpeg = find_ffmpeg()
        self.files: list[str] = []
        self.msg_q: "queue.Queue[tuple]" = queue.Queue()
        self.cancel_event = threading.Event()
        self.worker: threading.Thread | None = None

        root.title(APP_TITLE)
        root.geometry("640x620")
        root.minsize(560, 560)

        self._build_ui()
        self._poll_queue()

        if not self.ffmpeg:
            self.log("⚠ ffmpeg.exe 를 찾지 못했습니다. _internal 폴더 또는 PATH를 확인하세요.")
            self.convert_btn.config(state="disabled")
        else:
            self.log(f"ffmpeg: {self.ffmpeg}")

    # ---------- UI ----------
    def _build_ui(self):
        pad = {"padx": 10, "pady": 6}
        header = ttk.Label(self.root, text=APP_TITLE, font=("Segoe UI", 15, "bold"))
        header.pack(anchor="w", padx=12, pady=(12, 0))
        ttk.Label(
            self.root,
            text="동영상을 240×240 MJPEG + 모노 AAC 로 변환합니다.",
            foreground="#666",
        ).pack(anchor="w", padx=12, pady=(0, 6))

        # 파일 목록
        files_frame = ttk.LabelFrame(self.root, text="변환할 파일")
        files_frame.pack(fill="both", expand=True, padx=12, pady=6)
        list_wrap = ttk.Frame(files_frame)
        list_wrap.pack(fill="both", expand=True, padx=8, pady=8)
        self.listbox = tk.Listbox(list_wrap, selectmode="extended", height=7,
                                  activestyle="none")
        sb = ttk.Scrollbar(list_wrap, orient="vertical", command=self.listbox.yview)
        self.listbox.configure(yscrollcommand=sb.set)
        self.listbox.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")

        btn_row = ttk.Frame(files_frame)
        btn_row.pack(fill="x", padx=8, pady=(0, 8))
        ttk.Button(btn_row, text="파일 추가", command=self.add_files).pack(side="left")
        ttk.Button(btn_row, text="선택 삭제", command=self.remove_selected).pack(side="left", padx=6)
        ttk.Button(btn_row, text="모두 삭제", command=self.clear_files).pack(side="left")

        # 출력 폴더
        out_frame = ttk.Frame(self.root)
        out_frame.pack(fill="x", padx=12, pady=6)
        ttk.Label(out_frame, text="출력 폴더").pack(side="left")
        self.out_var = tk.StringVar()
        ttk.Entry(out_frame, textvariable=self.out_var).pack(
            side="left", fill="x", expand=True, padx=8)
        ttk.Button(out_frame, text="찾아보기", command=self.choose_out).pack(side="left")

        # 설정
        opt = ttk.LabelFrame(self.root, text="설정")
        opt.pack(fill="x", padx=12, pady=6)
        grid = ttk.Frame(opt)
        grid.pack(fill="x", padx=8, pady=8)

        self.fps_var = tk.IntVar(value=25)
        self.quality_var = tk.IntVar(value=9)
        self.bitrate_var = tk.StringVar(value="64k")
        self.volume_var = tk.IntVar(value=0)

        ttk.Label(grid, text="FPS").grid(row=0, column=0, sticky="w")
        ttk.Spinbox(grid, from_=1, to=30, width=6, textvariable=self.fps_var).grid(
            row=0, column=1, sticky="w", padx=(6, 24))
        ttk.Label(grid, text="화질 q:v (2=최고 ~ 31=최저)").grid(row=0, column=2, sticky="w")
        ttk.Spinbox(grid, from_=2, to=31, width=6, textvariable=self.quality_var).grid(
            row=0, column=3, sticky="w", padx=6)

        ttk.Label(grid, text="오디오 비트레이트").grid(row=1, column=0, sticky="w", pady=(8, 0))
        ttk.Combobox(grid, width=6, textvariable=self.bitrate_var,
                     values=["24k", "32k", "48k", "64k", "96k", "128k"],
                     state="readonly").grid(row=1, column=1, sticky="w", padx=(6, 24), pady=(8, 0))
        ttk.Label(grid, text="볼륨 감쇠 (dB)").grid(row=1, column=2, sticky="w", pady=(8, 0))
        ttk.Spinbox(grid, from_=0, to=30, width=6, textvariable=self.volume_var).grid(
            row=1, column=3, sticky="w", padx=6, pady=(8, 0))

        ttk.Label(grid, text="해상도: 240×240 (고정)", foreground="#666").grid(
            row=2, column=0, columnspan=4, sticky="w", pady=(8, 0))

        # 진행 상황
        prog = ttk.Frame(self.root)
        prog.pack(fill="x", padx=12, pady=(6, 0))
        self.status_var = tk.StringVar(value="대기 중")
        ttk.Label(prog, textvariable=self.status_var).pack(anchor="w")
        self.progress = ttk.Progressbar(prog, mode="determinate", maximum=100)
        self.progress.pack(fill="x", pady=4)

        action = ttk.Frame(self.root)
        action.pack(fill="x", padx=12, pady=(0, 6))
        self.convert_btn = ttk.Button(action, text="변환 시작", command=self.start)
        self.convert_btn.pack(side="left")
        self.cancel_btn = ttk.Button(action, text="취소", command=self.cancel, state="disabled")
        self.cancel_btn.pack(side="left", padx=6)

        # 로그
        log_frame = ttk.LabelFrame(self.root, text="로그")
        log_frame.pack(fill="both", expand=True, padx=12, pady=(0, 12))
        self.log_text = tk.Text(log_frame, height=6, wrap="word", state="disabled",
                                background="#111", foreground="#ddd", font=("Consolas", 9))
        self.log_text.pack(fill="both", expand=True, padx=6, pady=6)

    # ---------- 파일 관리 ----------
    def add_files(self):
        paths = filedialog.askopenfilenames(title="동영상 선택", filetypes=VIDEO_FILETYPES)
        for p in paths:
            if p not in self.files:
                self.files.append(p)
                self.listbox.insert("end", os.path.basename(p))
        if paths and not self.out_var.get():
            self.out_var.set(os.path.dirname(paths[0]))

    def remove_selected(self):
        for i in reversed(self.listbox.curselection()):
            self.listbox.delete(i)
            del self.files[i]

    def clear_files(self):
        self.listbox.delete(0, "end")
        self.files.clear()

    def choose_out(self):
        d = filedialog.askdirectory(title="출력 폴더 선택")
        if d:
            self.out_var.set(d)

    # ---------- 로그/큐 ----------
    def log(self, text):
        self.log_text.config(state="normal")
        self.log_text.insert("end", text + "\n")
        self.log_text.see("end")
        self.log_text.config(state="disabled")

    def _poll_queue(self):
        try:
            while True:
                kind, payload = self.msg_q.get_nowait()
                if kind == "log":
                    self.log(payload)
                elif kind == "status":
                    self.status_var.set(payload)
                elif kind == "progress":
                    self.progress["value"] = payload
                elif kind == "done":
                    self._on_done(payload)
        except queue.Empty:
            pass
        self.root.after(100, self._poll_queue)

    # ---------- 변환 ----------
    def start(self):
        if not self.files:
            messagebox.showinfo(APP_TITLE, "변환할 파일을 먼저 추가하세요.")
            return
        out_dir = self.out_var.get().strip()
        if not out_dir:
            messagebox.showinfo(APP_TITLE, "출력 폴더를 지정하세요.")
            return
        os.makedirs(out_dir, exist_ok=True)

        self.cancel_event.clear()
        self.convert_btn.config(state="disabled")
        self.cancel_btn.config(state="normal")
        settings = dict(
            fps=self.fps_var.get(), quality=self.quality_var.get(),
            bitrate=self.bitrate_var.get(), volume=self.volume_var.get(),
            out_dir=out_dir, files=list(self.files),
        )
        self.worker = threading.Thread(target=self._work, args=(settings,), daemon=True)
        self.worker.start()

    def cancel(self):
        self.cancel_event.set()
        self.msg_q.put(("status", "취소 중..."))

    def _work(self, s):
        files = s["files"]
        total = len(files)
        ok, failed = 0, 0
        for idx, src in enumerate(files):
            if self.cancel_event.is_set():
                break
            name = os.path.splitext(os.path.basename(src))[0]
            self.msg_q.put(("status", f"[{idx + 1}/{total}] {name} 변환 중..."))
            self.msg_q.put(("log", f"▶ {name}"))
            dur = probe_duration(self.ffmpeg, src)

            base = idx / total  # 파일 단위 진행 기준점

            def frac_cb(f, phase_lo, phase_span):
                overall = (base + (phase_lo + f * phase_span) / total) * 100
                self.msg_q.put(("progress", overall))

            mjpeg = os.path.join(s["out_dir"], f"{name}.mjpeg")
            aac = os.path.join(s["out_dir"], f"{name}.aac")

            # 비디오(0~70%) + 오디오(70~100%) 로 파일 내 진행을 배분
            rc_v, log_v = run_ffmpeg(
                build_video_cmd(self.ffmpeg, src, mjpeg, s["fps"], s["quality"]),
                dur, lambda f: frac_cb(f, 0.0, 0.7), self.cancel_event)
            if self.cancel_event.is_set():
                break
            if rc_v != 0:
                failed += 1
                self.msg_q.put(("log", f"✖ {name} 비디오 변환 실패\n{log_v}"))
                continue

            rc_a, log_a = run_ffmpeg(
                build_audio_cmd(self.ffmpeg, src, aac, s["bitrate"], s["volume"]),
                dur, lambda f: frac_cb(f, 0.7, 0.3), self.cancel_event)
            if self.cancel_event.is_set():
                break
            if rc_a != 0:
                failed += 1
                self.msg_q.put(("log", f"✖ {name} 오디오 변환 실패\n{log_a}"))
                continue

            ok += 1
            self.msg_q.put(("progress", (idx + 1) / total * 100))
            self.msg_q.put(("log", f"✔ {name}.mjpeg / {name}.aac 완료"))

        self.msg_q.put(("done", (ok, failed, self.cancel_event.is_set())))

    def _on_done(self, payload):
        ok, failed, canceled = payload
        self.convert_btn.config(state="normal")
        self.cancel_btn.config(state="disabled")
        if canceled:
            self.status_var.set(f"취소됨 (성공 {ok}, 실패 {failed})")
        else:
            self.progress["value"] = 100
            self.status_var.set(f"완료: 성공 {ok}, 실패 {failed}")
            messagebox.showinfo(APP_TITLE, f"변환 완료\n성공 {ok}개, 실패 {failed}개")


def main():
    root = tk.Tk()
    try:
        ttk.Style().theme_use("vista" if os.name == "nt" else "clam")
    except tk.TclError:
        pass
    ConverterApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
