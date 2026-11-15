"""hold_check.py <body file> <fit line> <money> [g0,g1,v0,v1 | all] [-v] (Nov 15): the chest grab's hold as the runtime plays it (posed bones in float, other edited channels from the 8-bit copy) vs
intended (everything in float): his hand points in her chest frame (bone 2), at whole and half frames."""
import os, sys, subprocess, numpy as np
REPO=os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),'../../../..'))
HERE=os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0,REPO+'/tools/models/punish_fit'); sys.path.insert(0,REPO+'/tools/models')
from chest_suplex import *
from chest_grip import pair_at, half
from mot import Mot
from fk import Motion
from scipy.spatial.transform import Rotation as Rot
body, block_line, money = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
fix = sys.argv[4] if len(sys.argv) > 4 else ''   # which channels play in float too: g0 g1 v0 v1 (comma list) or 'all'
lines=open(PF).read().split('\n')
blk=[]
for l in lines[block_line:]:
    if l.startswith('fit '): break
    blk.append(l)
head=lines[block_line-1]; scale=float(re.search(r'scale=([\d.]+)',head).group(1))
def keys_of(items,n=3):
    out=[]
    for it in items:
        f,v=it.split(':'); out.append((float(f),np.array([float(x) for x in v.split(',')][:n])))
    return out
def at(keys,f):
    if f<=keys[0][0]: return keys[0][1]
    if f>=keys[-1][0]: return keys[-1][1]
    for (f0,a),(f1,b) in zip(keys,keys[1:]):
        if f0<=f<=f1: return a+(b-a)*(f-f0)/(f1-f0)
poses={}
for l in blk:
    if l.startswith('pose '):
        p=l.split(); poses.setdefault((p[1],int(p[2]),int(p[4])),[]).append((float(p[3]),keys_of(p[5:])))
def line_pre(who,mo,b,t):
    L=sorted(poses[(who,mo,b)]); ms=[m for m,_ in L]
    if len(L)==1 or money<=ms[0]: lo=hi=L[0]
    elif money>=ms[-1]: lo=hi=L[-1]
    else:
        lo=max(x for x in L if x[0]<=money); hi=min(x for x in L if x[0]>=money)
    k=lo[1]
    if t<k[0][0] or t>k[-1][0]: return None
    w=(money-lo[0])/(hi[0]-lo[0]) if hi[0]>lo[0] else 0.0
    rv=at(lo[1],t)*(1-w)+at(hi[1],t)*w
    return Rot.from_rotvec(rv,degrees=True).as_matrix()
if scale>1.01:
    pair=held_fit(EX+'/em/'+body+'.dat',scale,0.95)
else:
    pair=Pair(EX+'/em/'+body+'.dat',1.0)
half(pair)
N=pair.gm.frames
gk=pair.fit['gene'][333]; vk=pair.fit['victim'][329]
gv=[curve(gk,f,14) for f in range(N)]; vv=[curve(vk,f,2) for f in range(pair.vm.frames)]
def bake(mo,ops,tag):
    t_,blob=motion(mo); open(tag+'_src.bin','wb').write(blob)
    with open(tag+'_ops.txt','w') as fo:
        for op in ops: fo.write(op+'\n')
    subprocess.run([os.environ.get('H5',os.path.join(HERE,'bake')),tag+'_src.bin',tag+'_out.bin',tag+'_ops.txt'],check=True)
    m=Motion(mo); m.m=Mot(open(tag+'_out.bin','rb').read()); m.ch={}
    for r in m.m.records:
        if r.sentinel or r.component>5: continue
        m.ch[(r.bone,r.component)]=r
    return m
gops=[]
h=[v[3]+(v[13] if len(v)>13 else 0) for v in gv]
if any(abs(x)>0 for x in h): gops.append('curve 0 1 %d '%N+' '.join('%.7f'%x for x in h))
p1=[gene_pre(v)[1] for v in gv]
gops.append('pre 1 %d '%N+' '.join('%.7f'%x for M in p1 for x in M.flatten()))
vops=['curve 0 1 %d '%len(vv)+' '.join('%.7f'%v[0] for v in vv), 'curve 1 3 %d '%len(vv)+' '.join('%.7f'%D(v[1]) for v in vv)]
gb=bake(333,gops,'g'); vb=bake(329,vops,'v')
GP=[b for (w,mo,b) in poses if w=='gene']; VP=[b for (w,mo,b) in poses if w=='victim']
fx=set(fix.split(',')) if fix else set()
if 'all' in fx: fx={'g0','g1','v0','v1'}
def frame(t, runtime):
    q=pair_at(pair,t)   # placement, curves at t (float)
    gvt=q['gv']; vvt=curve(vk,t,2)
    gl={}; vl={}
    for b in GP:
        P=line_pre('gene',333,b,t)
        if P is not None: gl[b]=P@rot([pair.gm.get(b,3+c,t) for c in range(3)])
    for b in VP:
        P=line_pre('victim',329,b,t)
        if P is not None: vl[b]=P@rot([pair.vm.get(b,3+c,t) for c in range(3)])
    if not runtime:
        Wg,Pg,_=pose_pre(pair.gene.skel,pair.gm,t,gene_pre(gvt),hip_dy=gvt[3]+(gvt[13] if len(gvt)>13 else 0),local=gl)
        Wv,Pv,_=pose_pre(pair.vic.skel,pair.vm,t,None,hip_dy=vvt[0],bend=D(vvt[1]),local=vl)
    else:
        # the copy for everything not posed; channels named in fx in float
        class Mix:
            def __init__(s,base,orig,flt): s.base,s.orig,s.flt=base,orig,flt
            def get(s,b,c,t,d=0.0): return (s.orig if (b,c) in s.flt else s.base).get(b,c,t,d)
            def has(s,b,c): return s.base.has(b,c)
        gflt=set(); vflt=set(); gpre={}; hip=0.0; vhip=0.0; vb_=0.0
        if 'g0' in fx: gflt.add((0,1)); hip=gvt[3]+(gvt[13] if len(gvt)>13 else 0)
        if 'g1' in fx: gflt|={(1,3),(1,4),(1,5)}; gpre={1:gene_pre(gvt)[1]}
        if 'v0' in fx: vflt.add((0,1)); vhip=vvt[0]
        if 'v1' in fx: vflt.add((1,3)); vb_=D(vvt[1])
        Wg,Pg,_=pose_pre(pair.gene.skel,Mix(gb,pair.gm,gflt),t,gpre,hip_dy=hip,local=gl)
        Wv,Pv,_=pose_pre(pair.vic.skel,Mix(vb,pair.vm,vflt),t,None,hip_dy=vhip,bend=vb_,local=vl)
    G,V=q['G'],q['V']
    Cw=V['R']@Wv[2]; Cp=to_world(V,Pv[2])
    out=[]
    for hb,sx in ((10,-1),(16,1)):
        Hw=G['R']@Wg[hb]
        for loc in ((0,0,0),(0.075*sx,-0.042,0.015),(-0.075*sx,-0.042,0.015),(0.15,0,0),(-0.15,0,0)):
            w=to_world(G,Pg[hb]+Wg[hb]@np.array(loc))
            out.append(Cw.T@(w-Cp)/V['s'])
    return np.array(out)
# the hold: where the hand's line is held (weight 1): use the bust line's frames with weight 1
bl=next(l for l in blk if l.startswith('bust '))
hold=[float(k.split(':')[0]) for k in bl.split()[2:] if float(k.split(':')[1])>=0.999]
h0,h1=min(hold),max(hold)
res=[]
for t in np.arange(h0,h1+0.01,0.5):
    a=frame(t,False); b=frame(t,True)
    res.append((t,np.linalg.norm(a-b,axis=1).max()*100))
w=[r for r in res if r[0]==int(r[0])]; hf=[r for r in res if r[0]!=int(r[0])]
print('%s scale %.2f money %.2f fix %s  hold %.1f-%.1f: whole frames max %.2f cm mean %.2f | half frames max %.2f cm mean %.2f'%(body,scale,money,fix or '-',h0,h1,
      max(x for _,x in w),np.mean([x for _,x in w]),max(x for _,x in hf),np.mean([x for _,x in hf])))
if '-v' in sys.argv: print(' '.join('%.1f:%.2f'%r for r in res))
