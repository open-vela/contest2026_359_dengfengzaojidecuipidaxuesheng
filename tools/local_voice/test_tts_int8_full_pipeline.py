import random
from pathlib import Path
from test_tts_int8_pipeline import program,execute
s=(Path(__file__).resolve().parents[2]/'app/espdl-quickapp/experimental/local_tts_int8_dot_p4.S').read_text()
p=program(s,'glv_tts_dot_batch_full_int8_pie');one=program(s,'glv_tts_dot_int8_pie')
rng=random.Random(51);cases=0
for mode in range(8):
 def data(n,bit):
  return [(-128 if mode&bit else 127) if mode<4 else rng.randint(-128,127) for _ in range(n)]
 for stride in (16,240,256,272,528,736,1152,2288):
  for rows,patches in ((1,1),(2,3),(3,8)):
   w=data(rows*stride,1);x=data(patches*stride,2)
   expected=[sum(w[r*stride+i]*x[a*stride+i] for i in range(stride))&0xffffffff for r in range(rows) for a in range(patches)]
   assert execute(p,w,x,stride,rows,patches,full=True)==expected
   assert execute(one,w[:stride],x[:stride],stride,single=True)==expected[0]
   cases+=1
print('FULL_ROW_INT8_SOURCE_ADDRESS_INTEGER_BOUNDS_REFERENCE_OK',cases)
