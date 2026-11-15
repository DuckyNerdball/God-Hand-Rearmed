"""chest_check.py <body hash> <scale> <money> [victim motion]: 60 FPS check of the chest grab's edits: per bone the edited motion against the intended rotation (pre . own) at every
frame and half frame (the half frames against the slerp of the intended rotations either side). Mirrors rewrite()."""
import os, sys, subprocess, numpy as np
ROOT=os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),'../../../..'))
HERE=os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0,ROOT+'/tools/models'); sys.path.insert(0,ROOT+'/tools/models/punish_fit')
from mot import Mot
from common import motion
from scipy.spatial.transform import Rotation as Rot, Slerp
D=np.radians
def Rx(a): c,s=np.cos(a),np.sin(a); return np.array([[1,0,0],[0,c,-s],[0,s,c]])
def Ry(a): c,s=np.cos(a),np.sin(a); return np.array([[c,0,s],[0,1,0],[-s,0,c]])
def Rz(a): c,s=np.cos(a),np.sin(a); return np.array([[c,-s,0],[s,c,0],[0,0,1]])
R=lambda e: Rz(e[2])@Ry(e[1])@Rx(e[0])
key, scale, money = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
vm = int(sys.argv[4]) if len(sys.argv) > 4 else 333
gene = vm == 333
lines=open(ROOT+'/resources/punish_fit.txt').read().split('\n')
i=next(k for k,l in enumerate(lines) if l.startswith('fit Suplex ') and key in l.split()[2].split(',') and 'scale=%.2f'%scale in l)
block=[]
for l in lines[i+1:]:
    if l.startswith('fit '): break
    block.append(l)
def keys_of(items, n):
    out=[]
    for it in items:
        f,v=it.split(':'); vals=[float(x) for x in v.split(',')][:n]; out.append((float(f),vals+[0.0]*(n-len(vals))))
    return out
def at(keys,f):
    if f<=keys[0][0]: return np.array(keys[0][1])
    if f>=keys[-1][0]: return np.array(keys[-1][1])
    for (f0,a),(f1,b) in zip(keys,keys[1:]):
        if f0<=f<=f1: t=(f-f0)/(f1-f0); return np.array(a)+(np.array(b)-np.array(a))*t
t_,blob=motion(vm); a=Mot(blob); N=a.frame_count
pre={}
if gene:
    gk=keys_of(next(l for l in block if l.startswith('gene %d '%vm)).split()[2:],14)
    for f in range(N):
        v=list(at(gk,f))+[0]*14; x=D(np.array(v[4:12]))
        for b,M in ((1,Rz(x[1])@Rx(x[0])),(13,Rz(x[2])@Ry(x[3])),(12,Rz(x[2]/2)@Ry(x[3]/2)),(7,Rz(-x[5])@Ry(-x[6])),(6,Rz(-x[5]/2)@Ry(-x[6]/2)),(14,Ry(x[4])),(8,Ry(-x[7]))):
            pre.setdefault(b,[]).append(M)
poses={}
for l in block:
    if l.startswith('pose %s %d '%('gene' if gene else 'victim',vm)):
        p=l.split(); poses.setdefault(int(p[4]),[]).append((float(p[3]),keys_of(p[5:],3)))
extra=[]
for b in sorted(poses):
    L=sorted(poses[b]); ms=[m for m,_ in L]
    lo=max([x for x in L if x[0]<=money] or [L[0]]); hi=min([x for x in L if x[0]>=money] or [L[-1]])
    if money<=ms[0]: lo=hi=L[0]
    if money>=ms[-1]: lo=hi=L[-1]
    t=(money-lo[0])/(hi[0]-lo[0]) if hi[0]>lo[0] else 0.0
    first=int(np.ceil(lo[1][0][0])); last=int(lo[1][-1][0])
    arr=list(pre[b]) if b in pre else [np.eye(3)]*N
    for f in range(first,last+1):
        rv=at(lo[1],f)*(1-t)+at(hi[1],f)*t
        arr[f]=Rot.from_rotvec(rv,degrees=True).as_matrix()
    if b in pre: pre[b]=arr
    else: extra.append((b,arr))
order=[(b,pre[b]) for b in (1,13,12,7,6,14,8) if b in pre]+extra
with open('pre.txt','w') as fo:
    for b,arr in order:
        fo.write('%d %d\n'%(b,N)); fo.write(' '.join('%.7f'%x for M in arr for x in M.flatten())+'\n')
open('src.bin','wb').write(blob)
subprocess.run([os.environ.get('H4',os.path.join(HERE,'apply_pose')),'src.bin','out.bin','pre.txt'],check=True)
b_=Mot(open('out.bin','rb').read())
ang=lambda A,B: np.degrees(np.arccos(np.clip((np.trace(A.T@B)-1)/2,-1,1)))
allpre=dict(order)
for b,arr in order:
    if b not in (5,7,8,10,11,13,14,16,6,12): continue
    want=[arr[f]@R(a.pose(f,30)[b][1]) for f in range(N)]
    e_int=max(ang(R(b_.pose(f,30)[b][1]),want[f]) for f in range(N))
    e_half=[]; own_half=[]
    for f in range(N-1):
        mid=Slerp([0,1],Rot.from_matrix([want[f],want[f+1]]))([0.5]).as_matrix()[0]
        e_half.append(ang(R(b_.pose(f+0.5,30)[b][1]),mid))
        omid=Slerp([0,1],Rot.from_matrix([R(a.pose(f,30)[b][1]),R(a.pose(f+1,30)[b][1])]))([0.5]).as_matrix()[0]
        own_half.append(ang(R(a.pose(f+0.5,30)[b][1]),omid))
    k=int(np.argmax(e_half))
    print('bone %2d  frames %.1f  half %.1f@%.1f  (the original own half-frame drift there %.1f, worst anywhere %.1f)'%(b,e_int,e_half[k],k+0.5,own_half[k],max(own_half)))
