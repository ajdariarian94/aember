import os
import subprocess

def clean():
    current_dir = os.getcwd()
        
    command = (
        f"rm -rf {current_dir}/build/install" 
    )

    subprocess.run(command, shell=True, check=True)