import subprocess

def build():
    command = (
        f"cmake --build build/configure/x86_64-gnu --parallel" 
    )

    subprocess.run(command, shell=True, check=True)