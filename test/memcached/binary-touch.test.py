from llib.MemcachedBinaryConnection import MemcachedBinaryConnection as MBC
from llib.MemcachedBinaryConnection import STATUS, COMMANDS

mc = MBC("localhost", 11211)

def __check(res, flags, val):
    print res.get("flags", -1), flags
    print '(res.get("flags", -1) == flags)', (res.get("flags", -1) == flags)
    print res.get('val', None) + ' ::: ' + val
    print '(res.get("val", None) == val)', (res.get("val", None) == val)

def check(key, flags, val):
    res = mc.get(key)
    __check(res[0], flags, val)

def set(key, expire, flags, value):
    res = mc.set(key, value, expire, flags)
    check(key, flags, value)

def empty(key):
    res = mc.get(key)
    print(res[0]["status"] == STATUS["KEY_ENOENT"])

print("""
#-----------------------------------------------------------------------------#
############################## Diagnostics Touch ##############################
#-----------------------------------------------------------------------------#
""")

import time

mc.flush()
mc.set("totouch", "toast", 1, 0)
res = mc.touch("totouch", 10)
time.sleep(2)
check("totouch", 0, "toast")

mc.set("totouch", "toast2", 1, 0)
res = mc.gat("totouch", 10)
print 'res[0]["val"] == "toast2"', res[0]["val"] == "toast2"
time.sleep(2)
check("totouch", 0, "toast2")

mc.set("totouch", "toast3", 1, 0)
mc.touch("totouch", 1)
time.sleep(3)
empty("totouch")
