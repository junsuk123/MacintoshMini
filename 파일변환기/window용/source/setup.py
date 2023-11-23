from cx_Freeze import setup, Executable

executables = [Executable("converter.py")]

setup(
    name="두잉오브젝트 파일 변환기",
    version="1.0",
    description="영상파일 재생시 오디오파일과 이미지 프레임 파일로 나눠주는 파일 변환기 입니다.",
    executables=executables
)
