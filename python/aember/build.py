import os
import subprocess

def build():
    command = (
        f"cmake --build build/x86_64-gnu --parallel" 
    )

    subprocess.run(command, shell=True, check=True)