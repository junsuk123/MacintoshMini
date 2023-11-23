import subprocess
import os
from tkinter import Tk, filedialog, simpledialog

def check_homebrew():
    try:
        subprocess.run(['brew', '--version'], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except subprocess.CalledProcessError as e:
        print("Homebrew가 설치되어 있지 않습니다. Homebrew를 설치합니다.")
        subprocess.run(['/bin/bash', '-c', '"$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"'])

def check_ffmpeg():
    try:
        subprocess.run(['brew', 'list', 'ffmpeg'], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except subprocess.CalledProcessError as e:
        print("ffmpeg이 설치되어 있지 않습니다. ffmpeg을 Homebrew를 이용해 설치합니다.")
        subprocess.run(['brew', 'install', 'ffmpeg'])

def convert_video(input_path, output_path, volume):
    subprocess.run(['ffmpeg', '-i', input_path, '-vf', 'scale=240:240', '-r', '25', '-c:v', 'mjpeg', '-q:v', '2', f'{output_path}.mjpeg'])
    subprocess.run(['ffmpeg', '-i', input_path, '-ar', '44100', '-ac', '1', '-ab', '24k', '-filter:a', f'loudnorm, volume=-{volume}dB', f'{output_path}.aac'])

def main():
    check_homebrew()
    check_ffmpeg()
    
    root = Tk()
    root.withdraw()

    # 입력 파일 선택 - 여러 파일 선택 가능
    input_files = filedialog.askopenfilenames(title="변환할 비디오 파일 선택", filetypes=[("Video Files", "*.mp4")])

    if not input_files:
        print("선택한 파일이 존재하지 않습니다.")
        return

    # 출력 폴더 선택
    output_folder = filedialog.askdirectory(title="저장할 폴더 선택")

    if not output_folder:
        print("폴더를 선택하지 않았습니다.")
        return

    volume = simpledialog.askinteger("볼륨 설정", "원하는 볼륨 값을 입력하세요 (0 ~ 15): ", minvalue=0, maxvalue=15)

    for input_file in input_files:
        if not os.path.isfile(input_file):
            print(f"{input_file} 파일이 존재하지 않습니다. 스킵합니다.")
            continue

        # 출력 파일명 설정
        file_name, _ = os.path.splitext(os.path.basename(input_file))
        output_file = os.path.join(output_folder, file_name)

        convert_video(input_file, output_file, volume)
        print(f"{input_file} 변환 완료")

    print("모든 동작이 완료되었습니다.")

if __name__ == "__main__":
    main()
