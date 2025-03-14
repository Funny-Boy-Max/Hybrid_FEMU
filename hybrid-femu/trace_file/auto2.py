import os
import numpy as np
import time
import subprocess

if __name__ == "__main__":
    req = "python3 auto_trace2.py"
    subprocess.run(req, shell=True)

    #time.sleep(100)

    req = "python3 auto_trace_read2.py"
    subprocess.run(req, shell=True)
