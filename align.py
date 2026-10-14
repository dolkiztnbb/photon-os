import sys
import math
a = open(sys.argv[1],mode='rb')
b = open(sys.argv[2],mode='rb')
sa = a.read()
sb = b.read()
a.close();b.close()
al = 0x1000
da = math.ceil(len(sa)/al)*al-len(sa)
c = open(sys.argv[3],mode='wb')
c.write(sa+b'\x00'*da+sb)
c.close()
