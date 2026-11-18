import json, numpy as np, sys
W,H = int(sys.argv[1]), int(sys.argv[2])
import os
J=json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../../resources/saloon/saloon_layers.json')))
FW,FH=J['size']
s = H/FH if W/H >= 4/3 else W/(FH*4/3)
dx = sum(p[0] for p in J['door'])/4*FW
ox, oy = W/2 - dx*s, (H-FH*s)/2
def scr(nx, ny): return (ox+nx*FW*s, oy+ny*FH*s)
q=[scr(*p) for p in J['bill']]
# homography paper(700x1000)->quad
src=[(0,0),(700,0),(700,1000),(0,1000)]
A=[];b=[]
for (x,y),(u,v) in zip(src,q):
    A.append([x,y,1,0,0,0,-u*x,-u*y]); b.append(u)
    A.append([0,0,0,x,y,1,-v*x,-v*y]); b.append(v)
h=np.linalg.solve(np.array(A),np.array(b)); Hm=np.append(h,1).reshape(3,3)
def m(x,y):
    p=Hm@[x,y,1]; return (int(p[0]/p[2]), int(p[1]/p[2]))
out={'play':m(350,385),'options':m(350,513),'quit':m(350,641),'dontshow':m(350,908)}
br=J['home']['book']['rect']; out['book']=tuple(int(v) for v in scr((br[0]+br[2])/2/FW,(br[1]+br[3])/2/FH))
p=[x for x in J['home']['snacks']['pieces'] if x['name']=='bananas'][0]['rect']; out['bananas']=tuple(int(v) for v in scr((p[0]+p[2])/2/FW,(p[1]+p[3])/2/FH))
sm=J['home']['snacks']['smoothie']['rect']; out['smoothie']=tuple(int(v) for v in scr((sm[0]+sm[2])/2/FW,(sm[1]*0.45+sm[3]*0.55)/FH))
for k,v in out.items(): print(k, v[0], v[1])
