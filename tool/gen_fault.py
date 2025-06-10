import json
import random

N = 100
BASE = 0x80000000
BLKSIZE = 64
faults = [BASE + i * BLKSIZE for i in random.sample(range(1 << 12), N)]
json.dump(faults, open("fault_1x.json", "w"))
