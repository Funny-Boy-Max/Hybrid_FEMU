import os
import subprocess
import numpy as np
import time

file_name = "HM_0.txt"

def execute(start_address, size, undo_req_num, typ):
    if typ == 1 :
        req = "nvme read /dev/nvme0n1 -s {} -c {} -z {} -t > x.txt".format(np.uint64(0), int(2), 3*512)
        subprocess.run(req, shell=True)
        return undo_req_num
    else :
        req = "nvme write /dev/nvme0n1 -s {} -c {} -z {} -d './a' -t > x.txt".format(np.uint64(0), int(2), 3*512)
        subprocess.run(req, shell=True)
        return undo_req_num


if __name__ == "__main__":
    fp = open(file_name, 'r')
    undo_req_num = 0
    i = 0
    typ = 0
    subprocess.run("nvme write /dev/nvme0n1 -s 0 -c 1 -z 1024 -d './a'", shell=True)

    start_time = time.time()
    while True:
        line = fp.readline()
        if len(line) == 0:
            break
        #if i >= 200000:
        #    break
        t = float(line.split(" ")[0])
        
        start_address = np.uint64(line.split(" ")[2])
        size = int(line.split(" ")[3])
        typ = int(line.split(" ")[4])
        print("{} : {}\n".format(i+1, start_address))
        undo_req_num = execute(start_address, size, undo_req_num, typ)
        i += 1

    end_time = time.time()
    total_time = end_time - start_time
    print("total_time: {}".format(total_time))

    subprocess.run("nvme write /dev/nvme0n1 -s 0 -c 1 -z 1024 -d './a'", shell=True)
    print("undo req number is {}".format(undo_req_num))
