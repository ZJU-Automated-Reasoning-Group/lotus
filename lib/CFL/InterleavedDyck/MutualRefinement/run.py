import subprocess
import os
import os.path
from typing import Union

def execute(cmd: list[str], t: Union[None, int] = None) -> str:
    return subprocess.run(
        cmd,
        text = True,
        stdout = subprocess.PIPE,
        stderr = subprocess.STDOUT,
        timeout = t
    ).stdout

def run_all(path: str, mode: str) -> None:
    for dirpath, dirnames, filenames in os.walk(path):
        for filename in filenames:
            if filename[-4:] == '.dot':
                name = filename[:-4]
                print(f"Running {mode} on {name}")
                try:
                    output = execute(
                        [
                            './main',
                            os.path.join(dirpath, name + '.grammar'),
                            os.path.join(dirpath, name + '.dot'),
                            mode
                        ],
                        10
                    )
                except subprocess.TimeoutExpired:
                    print('TIMEOUT')
                    output = 'TIMEOUT'
                with open(os.path.join(dirpath, name + f'.{mode}.result'), 'w') as f:
                    f.write(output)

if __name__ == '__main__':
    # Cleaning
    print(">>> Cleaning")
    execute(['make', 'clean'])
    execute(['rm', 'exp/taint/*.result'])
    execute(['rm', 'exp/valueflow/*.result'])
    execute(['rm', 'exp/simplified-taint/*.result'])
    # Compilation
    print(">>> Compiling")
    execute(['make', '-j8'])
    # Running experiments
    print(">>> Running taint")
    run_all('exp/taint/', 'naive')
    run_all('exp/taint/', 'refine')
    print(">>> Running valueflow")
    run_all('exp/valueflow/', 'naive')
    run_all('exp/valueflow/', 'refine')
    print(">>> Running simplified-taint")
    run_all('exp/simplified-taint/', 'refine')