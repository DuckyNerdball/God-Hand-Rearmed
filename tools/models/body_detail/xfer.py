"""Prototype: subdivide a woman's bust (bone-29 region) and shrinkwrap it onto Olivia's bust, scaled to her size."""
import sys, numpy as np
import os; _H=os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0,os.path.join(_H,'..','xavier')); sys.path.insert(0,os.path.join(_H,'..'))
from gamemodel import dat_read, scr_read, mdb_read, tm3_read, strip_tris
EX=os.environ.get('GODHAND_EXTRACT','extract')

def load_node(f, md, node_names):
    ents=dat_read(open(EX+'/'+f,'rb').read()); raw=ents[md][1]; k,nodes=scr_read(raw)
    P=[];N=[];UV=[];C=[];J=[];W=[];VB=[];T=[];TM=[]
    vbi=0
    for nd in nodes:
        if nd['name'] not in node_names: continue
        m=mdb_read(raw,nd['mdb_off'])
        for vb in m['vbs']:
            base=len(P)
            for k2 in range(vb['count']):
                P.append(vb['pos'][k2]); N.append(np.array(vb['nrm'][k2],float)/127 if vb['nrm'] else (0,1,0))
                UV.append(vb['uv'][k2]); C.append(vb['col'][k2] if vb['col'] else (128,128,128,128))
                jb,wb=vb['wt'][k2]; J.append([jb[1]//4,jb[2]//4,jb[3]//4]); W.append([wb[0],wb[1],wb[2]])
                VB.append(vbi)
            for a,b,c in strip_tris(vb['flags']): T.append((base+a,base+b,base+c)); TM.append(vb['mat'])
            vbi+=1
    return dict(P=np.array(P,float),N=np.array(N,float),UV=np.array(UV,float),C=np.array(C,float),J=np.array(J,int),
                W=np.array(W,float),VB=np.array(VB),T=np.array(T,int),TM=np.array(TM),ents=ents,md=md)

def w_on(m,bone):
    return np.where(m['J']==bone,m['W'],0).sum(1)

def pos_ids(P):
    key={}; ids=np.zeros(len(P),int)
    for i,p in enumerate(np.round(P,5)):
        ids[i]=key.setdefault(tuple(p),len(key))
    return ids

def subdivide(m, region, levels=2):
    """Red-green subdivision of the triangles flagged in region (bool per triangle). Midpoints are per vertex buffer
    (attributes lerped), positions shared by position id so seams stay closed."""
    P=list(m['P']);N=list(m['N']);UV=list(m['UV']);C=list(m['C']);J=list(m['J']);W=list(m['W']);VB=list(m['VB'])
    T=[tuple(t) for t in m['T']]; TM=list(m['TM']); reg=list(region)
    for lev in range(levels):
        pid=pos_ids(np.array(P))
        split=set()
        for t,r in zip(T,reg):
            if r:
                for a,b in ((t[0],t[1]),(t[1],t[2]),(t[2],t[0])): split.add(frozenset((pid[a],pid[b])))
        mids={}
        def mid(a,b):
            key=(min(a,b),max(a,b))
            if key in mids: return mids[key]
            w={}
            for jj,ww in zip(list(J[a])+list(J[b]),list(W[a])+list(W[b])):
                if ww>0: w[jj]=w.get(jj,0)+ww
            top=sorted(w.items(),key=lambda x:-x[1])[:3]; s=sum(x[1] for x in top) or 1
            jj=[x[0] for x in top]+[0]*(3-len(top)); ww=[round(100*x[1]/s) for x in top]+[0]*(3-len(top))
            ww[0]+=100-sum(ww)
            P.append((np.array(P[a])+np.array(P[b]))/2); N.append((np.array(N[a])+np.array(N[b]))/2)
            UV.append((np.array(UV[a])+np.array(UV[b]))/2); C.append((np.array(C[a])+np.array(C[b]))/2)
            J.append(jj); W.append(ww); VB.append(VB[a])
            mids[key]=len(P)-1; return mids[key]
        nT=[];nTM=[];nreg=[]
        for t,tm,r in zip(T,TM,reg):
            a,b,c=t
            sab=frozenset((pid[a],pid[b])) in split; sbc=frozenset((pid[b],pid[c])) in split; sca=frozenset((pid[c],pid[a])) in split
            n=sab+sbc+sca
            if n==0: nT.append(t);nTM.append(tm);nreg.append(r); continue
            if n==3:
                ab,bc,ca=mid(a,b),mid(b,c),mid(c,a)
                for q in ((a,ab,ca),(ab,b,bc),(ca,bc,c),(ab,bc,ca)): nT.append(q);nTM.append(tm);nreg.append(r)
                continue
            # rotate so the pattern starts at a split edge
            if n==1:
                while not (frozenset((pid[a],pid[b])) in split): a,b,c=b,c,a
                ab=mid(a,b)
                for q in ((a,ab,c),(ab,b,c)): nT.append(q);nTM.append(tm);nreg.append(r)
            else:  # two split edges: rotate so ab and bc are split
                while not ((frozenset((pid[a],pid[b])) in split) and (frozenset((pid[b],pid[c])) in split)): a,b,c=b,c,a
                ab,bc=mid(a,b),mid(b,c)
                for q in ((ab,b,bc),(a,ab,bc),(a,bc,c)): nT.append(q);nTM.append(tm);nreg.append(r)
        T,TM,reg=nT,nTM,nreg
    out=dict(m); out.update(P=np.array(P,float),N=np.array(N,float),UV=np.array(UV,float),C=np.array(C,float),
                            J=np.array(J,int),W=np.array(W,float),VB=np.array(VB),T=np.array(T,int),TM=np.array(TM))
    return out, np.array(reg)

def tri_closest(p, A, B, C):
    """closest points on many triangles to point p (vectorized over triangles)."""
    ab=B-A; ac=C-A; ap=p-A
    d1=(ab*ap).sum(1); d2=(ac*ap).sum(1)
    bp=p-B; d3=(ab*bp).sum(1); d4=(ac*bp).sum(1)
    cp=p-C; d5=(ab*cp).sum(1); d6=(ac*cp).sum(1)
    va=d3*d6-d5*d4; vb=d5*d2-d1*d6; vc=d1*d4-d3*d2
    den=va+vb+vc; den=np.where(np.abs(den)<1e-20,1e-20,den)
    v=vb/den; w=vc/den
    res=A+ab*v[:,None]+ac*w[:,None]
    # regions
    m=(d1<=0)&(d2<=0); res[m]=A[m]
    m=(d3>=0)&(d4<=d3); res[m]=B[m]
    m=(d6>=0)&(d5<=d6); res[m]=C[m]
    vcm=(vc<=0)&(d1>=0)&(d3<=0); t=d1/np.where(d1-d3==0,1e-20,d1-d3); res[vcm]=(A+ab*t[:,None])[vcm]
    vbm=(vb<=0)&(d2>=0)&(d6<=0); t=d2/np.where(d2-d6==0,1e-20,d2-d6); res[vbm]=(A+ac*t[:,None])[vbm]
    vam=(va<=0)&((d4-d3)>=0)&((d5-d6)>=0); t=(d4-d3)/np.where((d4-d3)+(d5-d6)==0,1e-20,(d4-d3)+(d5-d6)); res[vam]=(B+(C-B)*t[:,None])[vam]
    return res

def ray_hit(o, d, A, B, C):
    e1=B-A; e2=C-A; pv=np.cross(d,e2); det=(e1*pv).sum(1)
    ok=np.abs(det)>1e-12; inv=np.where(ok,1/np.where(ok,det,1),0)
    tv=o-A; u=(tv*pv).sum(1)*inv; qv=np.cross(tv,e1); v=(qv*d).sum(1)*inv; t=(e2*qv).sum(1)*inv
    hit=ok&(u>=-1e-6)&(v>=-1e-6)&(u+v<=1+1e-6)
    return np.where(hit,t,np.nan)

def smooth_normals(P, T):
    pid=pos_ids(P); n=np.zeros((pid.max()+1,3))
    fn=np.cross(P[T[:,1]]-P[T[:,0]],P[T[:,2]]-P[T[:,0]])
    for k in range(3): np.add.at(n,pid[T[:,k]],fn)
    n/=np.linalg.norm(n,axis=1)[:,None]+1e-12
    return n[pid], pid

def affine_box(src_lo, src_hi, dst_lo, dst_hi):
    s=(dst_hi-dst_lo)/(src_hi-src_lo)
    return lambda X: (X-src_lo)*s+dst_lo

def transfer(m, donor, bone=29, levels=2, falloff=0.02, max_t=0.05, region_fn=None):
    w=w_on(m,bone)
    region=(w[m['T']]>0).all(1) if region_fn is None else region_fn(m)
    sm,reg=subdivide(m,region,levels)
    # donor surface: its bone region (any vertex on the bone), mapped onto the woman's region box
    dw=w_on(donor,bone); dT=donor['T'][(dw[donor['T']]>0).any(1)]
    gsel=m['P'][w>0]; dsel=donor['P'][dw>0]
    fx=affine_box(np.array([-dsel[:,0].max(),dsel[:,1].min(),dsel[:,2].min()]),np.array([dsel[:,0].max(),dsel[:,1].max(),dsel[:,2].max()]),
                  np.array([-gsel[:,0].max(),gsel[:,1].min(),gsel[:,2].min()]),np.array([gsel[:,0].max(),gsel[:,1].max(),gsel[:,2].max()]))
    DP=fx(donor['P']); A,B,C=DP[dT[:,0]],DP[dT[:,1]],DP[dT[:,2]]
    P=sm['P'].copy(); nrm,pid=smooth_normals(P,sm['T'])
    inreg=np.zeros(len(P),bool); inreg[sm['T'][reg].ravel()]=True
    outv=np.zeros(len(P),bool); outv[sm['T'][~reg].ravel()]=True
    border=inreg&outv
    bpos=P[border]
    moved={}
    newP=P.copy()
    for i in np.where(inreg&~border)[0]:
        key=pid[i]
        if key in moved: newP[i]=moved[key]; continue
        o=P[i]; d=nrm[i]
        t=ray_hit(o,d,A,B,C); t=t[np.isfinite(t)&(np.abs(t)<max_t)]
        if len(t): q=o+d*t[np.argmin(np.abs(t))]
        else:
            cq=tri_closest(o,A,B,C); q=cq[np.argmin(((cq-o)**2).sum(1))]
        dist=np.sqrt(((bpos-o)**2).sum(1)).min() if len(bpos) else 1
        f=np.clip(dist/falloff,0,1); f=f*f*(3-2*f)
        moved[key]=o+(q-o)*f; newP[i]=moved[key]
    sm['P']=newP
    nrm2,_=smooth_normals(newP,sm['T'])
    N=sm['N'].copy(); N[inreg&~border]=nrm2[inreg&~border]
    sm['N']=N
    return sm, reg

def transfer2(m, donor, bone=29, levels=2, max_t=0.05, smooth_iters=12, region_mode='any'):
    w=w_on(m,bone)
    region=(w[m['T']]>0).any(1) if region_mode=='any' else (w[m['T']]>0).all(1)
    sm,reg=subdivide(m,region,levels)
    dw=w_on(donor,bone); dT=donor['T'][(dw[donor['T']]>0).any(1)]
    gsel=m['P'][w>0]; dsel=donor['P'][dw>0]
    fx=affine_box(np.array([-dsel[:,0].max(),dsel[:,1].min(),dsel[:,2].min()]),np.array([dsel[:,0].max(),dsel[:,1].max(),dsel[:,2].max()]),
                  np.array([-gsel[:,0].max(),gsel[:,1].min(),gsel[:,2].min()]),np.array([gsel[:,0].max(),gsel[:,1].max(),gsel[:,2].max()]))
    DP=fx(donor['P']); A,B,C=DP[dT[:,0]],DP[dT[:,1]],DP[dT[:,2]]
    P=sm['P']; nrm,pid=smooth_normals(P,sm['T'])
    npos=pid.max()+1
    # per position id: original position, normal, region / border flags
    Ppos=np.zeros((npos,3)); Npos=np.zeros((npos,3)); Ppos[pid]=P; Npos[pid]=nrm
    inreg=np.zeros(npos,bool); inreg[pid[sm['T'][reg].ravel()]]=True
    outv=np.zeros(npos,bool); outv[pid[sm['T'][~reg].ravel()]]=True
    fixed=~inreg|outv
    D=np.zeros((npos,3))
    for k in np.where(~fixed)[0]:
        o=Ppos[k]; d=Npos[k]
        t=ray_hit(o,d,A,B,C); t=t[np.isfinite(t)&(np.abs(t)<max_t)]
        if len(t): q=o+d*t[np.argmin(np.abs(t))]
        else:
            cq=tri_closest(o,A,B,C); q=cq[np.argmin(((cq-o)**2).sum(1))]
        D[k]=q-o
    # smooth the displacement over the region graph, border held at zero (a few Jacobi steps), keeping the interior's
    # projected targets as soft anchors
    nb=[set() for _ in range(npos)]
    for t in pid[sm['T'][reg]]:
        for a,b in ((t[0],t[1]),(t[1],t[2]),(t[2],t[0])): nb[a].add(b); nb[b].add(a)
    D0=D.copy()
    for it in range(smooth_iters):
        Dn=D.copy()
        for k in np.where(~fixed)[0]:
            if nb[k]: Dn[k]=0.5*D0[k]+0.5*np.mean([D[j] for j in nb[k]],0)
        D=Dn
    newP=P+D[pid]
    sm['P']=newP
    nrm2,_=smooth_normals(newP,sm['T'])
    N=sm['N'].copy(); mv=~fixed[pid]; N[mv]=nrm2[mv]
    sm['N']=N
    # folded triangles (normal flipped against the original)
    fn0=np.cross(P[sm['T'][:,1]]-P[sm['T'][:,0]],P[sm['T'][:,2]]-P[sm['T'][:,0]])
    fn1=np.cross(newP[sm['T'][:,1]]-newP[sm['T'][:,0]],newP[sm['T'][:,2]]-newP[sm['T'][:,0]])
    flips=((fn0*fn1).sum(1)<0).sum()
    return sm, reg, flips

def seat_mask(P, lo=(-0.17,0.80,-1.0), hi=(0.17,1.13,-0.015)):
    lo=np.array(lo); hi=np.array(hi)
    return ((P>=lo)&(P<=hi)).all(1)

def transfer_mask(m, donor, gmask, dmask, levels=1, max_t=0.05, smooth_iters=12, box_x_sym=True):
    """Like transfer2, with the woman's region and the donor's surface picked by vertex masks."""
    region=gmask[m['T']].any(1)
    sm,reg=subdivide(m,region,levels)
    dT=donor['T'][dmask[donor['T']].any(1)]
    gsel=m['P'][gmask]; dsel=donor['P'][dmask]
    gx=np.abs(gsel[:,0]).max(); dx=np.abs(dsel[:,0]).max()
    fx=affine_box(np.array([-dx,dsel[:,1].min(),dsel[:,2].min()]),np.array([dx,dsel[:,1].max(),dsel[:,2].max()]),
                  np.array([-gx,gsel[:,1].min(),gsel[:,2].min()]),np.array([gx,gsel[:,1].max(),gsel[:,2].max()]))
    DP=fx(donor['P']); A,B,C=DP[dT[:,0]],DP[dT[:,1]],DP[dT[:,2]]
    P=sm['P']; nrm,pid=smooth_normals(P,sm['T'])
    npos=pid.max()+1
    Ppos=np.zeros((npos,3)); Npos=np.zeros((npos,3)); Ppos[pid]=P; Npos[pid]=nrm
    inreg=np.zeros(npos,bool); inreg[pid[sm['T'][reg].ravel()]]=True
    outv=np.zeros(npos,bool); outv[pid[sm['T'][~reg].ravel()]]=True
    fixed=~inreg|outv
    D=np.zeros((npos,3))
    for k in np.where(~fixed)[0]:
        o=Ppos[k]; d=Npos[k]
        t=ray_hit(o,d,A,B,C); t=t[np.isfinite(t)&(np.abs(t)<max_t)]
        if len(t): q=o+d*t[np.argmin(np.abs(t))]
        else:
            cq=tri_closest(o,A,B,C); q=cq[np.argmin(((cq-o)**2).sum(1))]
        D[k]=q-o
    nb=[set() for _ in range(npos)]
    for t in pid[sm['T'][reg]]:
        for a,b in ((t[0],t[1]),(t[1],t[2]),(t[2],t[0])): nb[a].add(b); nb[b].add(a)
    D0=D.copy()
    for it in range(smooth_iters):
        Dn=D.copy()
        for k in np.where(~fixed)[0]:
            if nb[k]: Dn[k]=0.5*D0[k]+0.5*np.mean([D[j] for j in nb[k]],0)
        D=Dn
    newP=P+D[pid]; sm['P']=newP
    nrm2,_=smooth_normals(newP,sm['T'])
    N=sm['N'].copy(); mv=~fixed[pid]; N[mv]=nrm2[mv]; sm['N']=N
    fn0=np.cross(P[sm['T'][:,1]]-P[sm['T'][:,0]],P[sm['T'][:,2]]-P[sm['T'][:,0]])
    fn1=np.cross(newP[sm['T'][:,1]]-newP[sm['T'][:,0]],newP[sm['T'][:,2]]-newP[sm['T'][:,0]])
    return sm, reg, int(((fn0*fn1).sum(1)<0).sum())

def transfer_falloff(m, donor, gmask, dmask, levels=2, max_t=0.05, falloff=0.03, smooth_iters=2):
    region=gmask[m['T']].any(1)
    sm,reg=subdivide(m,region,levels)
    dT=donor['T'][dmask[donor['T']].any(1)]
    gsel=m['P'][gmask]; dsel=donor['P'][dmask]
    gx=np.abs(gsel[:,0]).max(); dx=np.abs(dsel[:,0]).max()
    fx=affine_box(np.array([-dx,dsel[:,1].min(),dsel[:,2].min()]),np.array([dx,dsel[:,1].max(),dsel[:,2].max()]),
                  np.array([-gx,gsel[:,1].min(),gsel[:,2].min()]),np.array([gx,gsel[:,1].max(),gsel[:,2].max()]))
    DP=fx(donor['P']); A,B,C=DP[dT[:,0]],DP[dT[:,1]],DP[dT[:,2]]
    P=sm['P']; nrm,pid=smooth_normals(P,sm['T'])
    npos=pid.max()+1
    Ppos=np.zeros((npos,3)); Npos=np.zeros((npos,3)); Ppos[pid]=P; Npos[pid]=nrm
    inreg=np.zeros(npos,bool); inreg[pid[sm['T'][reg].ravel()]]=True
    outv=np.zeros(npos,bool); outv[pid[sm['T'][~reg].ravel()]]=True
    fixed=~inreg|outv
    bpos=Ppos[inreg&outv]
    D=np.zeros((npos,3))
    for k in np.where(~fixed)[0]:
        o=Ppos[k]; d=Npos[k]
        t=ray_hit(o,d,A,B,C); t=t[np.isfinite(t)&(np.abs(t)<max_t)]
        if len(t): q=o+d*t[np.argmin(np.abs(t))]
        else:
            cq=tri_closest(o,A,B,C); q=cq[np.argmin(((cq-o)**2).sum(1))]
        dist=np.sqrt(((bpos-o)**2).sum(1)).min() if len(bpos) else 1.0
        f=np.clip(dist/falloff,0,1); f=f*f*(3-2*f)
        D[k]=(q-o)*f
    nb=[set() for _ in range(npos)]
    for t in pid[sm['T'][reg]]:
        for a,b in ((t[0],t[1]),(t[1],t[2]),(t[2],t[0])): nb[a].add(b); nb[b].add(a)
    for it in range(smooth_iters):
        Dn=D.copy()
        for k in np.where(~fixed)[0]:
            if nb[k]: Dn[k]=0.5*D[k]+0.5*np.mean([D[j] for j in nb[k]],0)
        D=Dn
    newP=P+D[pid]; sm['P']=newP
    nrm2,_=smooth_normals(newP,sm['T'])
    N=sm['N'].copy(); mv=~fixed[pid]; N[mv]=nrm2[mv]; sm['N']=N
    fn0=np.cross(P[sm['T'][:,1]]-P[sm['T'][:,0]],P[sm['T'][:,2]]-P[sm['T'][:,0]])
    fn1=np.cross(newP[sm['T'][:,1]]-newP[sm['T'][:,0]],newP[sm['T'][:,2]]-newP[sm['T'][:,0]])
    return sm, reg, int(((fn0*fn1).sum(1)<0).sum())

def transfer_radial(m, donor, gmask, dmask, levels=2, z0=0.0, falloff=0.03, smooth_iters=1, min_r=0.04):
    region=gmask[m['T']].any(1)
    sm,reg=subdivide(m,region,levels)
    dT=donor['T'][dmask[donor['T']].any(1)]
    gsel=m['P'][gmask]; dsel=donor['P'][dmask]
    gx=np.abs(gsel[:,0]).max(); dx=np.abs(dsel[:,0]).max()
    fx=affine_box(np.array([-dx,dsel[:,1].min(),dsel[:,2].min()]),np.array([dx,dsel[:,1].max(),dsel[:,2].max()]),
                  np.array([-gx,gsel[:,1].min(),gsel[:,2].min()]),np.array([gx,gsel[:,1].max(),gsel[:,2].max()]))
    DP=fx(donor['P']); A,B,C=DP[dT[:,0]],DP[dT[:,1]],DP[dT[:,2]]
    P=sm['P']; nrm,pid=smooth_normals(P,sm['T'])
    npos=pid.max()+1
    Ppos=np.zeros((npos,3)); Ppos[pid]=P
    inreg=np.zeros(npos,bool); inreg[pid[sm['T'][reg].ravel()]]=True
    outv=np.zeros(npos,bool); outv[pid[sm['T'][~reg].ravel()]]=True
    fixed=~inreg|outv
    bpos=Ppos[inreg&outv]
    D=np.zeros((npos,3))
    for k in np.where(~fixed)[0]:
        o=Ppos[k]; a=np.array([0.0,o[1],z0]); v=np.array([o[0],0.0,o[2]-z0]); r=np.linalg.norm(v)
        if r<min_r: continue
        d=v/r
        t=ray_hit(a,d,A,B,C); t=t[np.isfinite(t)&(t>0)]
        if not len(t): continue
        q=a+d*t.max()  # the outer surface
        dist=np.sqrt(((bpos-o)**2).sum(1)).min() if len(bpos) else 1.0
        f=np.clip(dist/falloff,0,1); f=f*f*(3-2*f)
        D[k]=(q-o)*f
    nb=[set() for _ in range(npos)]
    for t in pid[sm['T'][reg]]:
        for a2,b2 in ((t[0],t[1]),(t[1],t[2]),(t[2],t[0])): nb[a2].add(b2); nb[b2].add(a2)
    for it in range(smooth_iters):
        Dn=D.copy()
        for k in np.where(~fixed)[0]:
            if nb[k]: Dn[k]=0.5*D[k]+0.5*np.mean([D[j] for j in nb[k]],0)
        D=Dn
    newP=P+D[pid]; sm['P']=newP
    nrm2,_=smooth_normals(newP,sm['T'])
    N=sm['N'].copy(); mv=~fixed[pid]; N[mv]=nrm2[mv]; sm['N']=N
    fn0=np.cross(P[sm['T'][:,1]]-P[sm['T'][:,0]],P[sm['T'][:,2]]-P[sm['T'][:,0]])
    fn1=np.cross(newP[sm['T'][:,1]]-newP[sm['T'][:,0]],newP[sm['T'][:,2]]-newP[sm['T'][:,0]])
    return sm, reg, int(((fn0*fn1).sum(1)<0).sum())
