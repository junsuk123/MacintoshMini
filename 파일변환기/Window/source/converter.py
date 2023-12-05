import os
import subprocess
import tkinter as tk
from tkinter import filedialog
from tkinter import messagebox
from tkinter import simpledialog

def get_current_path():
    if getattr(sys, 'frozen', False):  # 실행 파일로 빌드된 경우
        return os.path.dirname(sys.executable)
    elif __file__:
        return os.path.dirname(os.path.abspath(__file__))
    return os.getcwd()

def convert_video(video_file_path,volume):
    # 비디오 파일 이름 추출
    video_file_name = os.path.splitext(os.path.basename(video_file_path))[0]

    # 현재 스크립트 파일의 디렉토리 가져오기
    script_directory = os.path.join(get_current_path(), "_internal")
    ffmpeg_path = os.path.join(script_directory, "ffmpeg.exe")

    # 파일 이름을 팝업 창으로 표시
    # messagebox.showinfo("안내", f"선택한 비디오 파일 이름: {video_file_name}")

    

    # ffmpeg 명령어 실행
    mjpeg_command = (
        f'{os.path.join(script_directory, "ffmpeg.exe")} -i "{video_file_path}" -vf "fps=25,scale=-1:240:flags=lanczos,'
        f'crop=240:in_h:(in_w-240)/2:0" -q:v 9 "{video_file_name}.mjpeg"'
    )
    mp3_command = (
        f'{os.path.join(script_directory, "ffmpeg.exe")} -i "{video_file_path}" -ar 44100 -ac 1 -ab 24k '
        f'-filter:a loudnorm -filter:a "volume=-{volume}dB" "{video_file_name}.aac"'
    )

    try:
	# mp3 변환 시작
        subprocess.run(mjpeg_command, shell=True, check=True)

        # mp3 변환 시작
        subprocess.run(mp3_command, shell=True, check=True)

        # 변환 완료 안내 팝업 창 표시
        #messagebox.showinfo("안내", f"{video_file_name}의 오디오 변환이 완료되었습니다.\n경로: {audio_file_path}")

    except subprocess.CalledProcessError as e:
        messagebox.showinfo("안내", f"{video_file_name} 파일 변환 중 오류가 발생했습니다.")
        print(f"{video_file_name} 파일 변환 중 오류 발생: {e}")

def main():
    # 파일 대화 상자 열기
    root = tk.Tk()
    root.withdraw()
    video_file_paths = filedialog.askopenfilenames(filetypes=[("Video files", "*.mp4")])

    if not video_file_paths:
        return
# 사용자로부터 볼륨 값을 입력 받음
    volume = simpledialog.askinteger("볼륨 설정", "원하는 볼륨 값을 입력하세요 (0 ~ 15): ", minvalue=0, maxvalue=15)
    for video_file_path in video_file_paths:
        convert_video(video_file_path,volume)

    messagebox.showinfo("안내", f"변환이 모두 완료 되었습니다.")

if __name__ == "__main__":
    main()
