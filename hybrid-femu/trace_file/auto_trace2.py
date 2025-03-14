import os
import numpy as np
import time
import subprocess

file_name = "HM_0.txt"

if __name__ == "__main__":
    fp = open(file_name, 'r')
    undo_req_num = 0
    i = 0
    typ = 0

    start_time = time.time()
    while True:
        line = fp.readline()
        if len(line) == 0:
            break
        #if i >= 200000:
        #    break
        t = line.split(" ")[0]
        start_address = np.uint64(line.split(" ")[2])
        size = int(line.split(" ")[3])
        typ = int(line.split(" ")[4])
        print("{}\n".format(i+1))
        req = "nvme write /dev/nvme0n1 -s {} -c {} -z {} -d './a' -t > x.txt".format(np.uint64(0), int(0), 512)
        subprocess.run(req, shell=True)
        i += 1

    end_time = time.time()
    total_time = end_time - start_time
    print("total_time: {}".format(total_time))
    print("undo req number is {}".format(undo_req_num))
