"""Seat transplant: cut a window out of a woman's body, stitch in the donor's patch (box-mapped to her size), and give
the new vertices her own UVs / weights / material by closest point on her original surface (per UV island)."""
import os, sys, numpy as np
CUT_MODE="min"; CUT_MIN=2
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from xfer import load_node, pos_ids, tri_closest, smooth_normals, affine_box

def boundary_loops(T, pid):
    """Ordered boundary loops (lists of position ids) of a triangle set, plus the boundary edge -> triangle map."""
    cnt={}
    for t in T:
        for a,b in ((t[0],t[1]),(t[1],t[2]),(t[2],t[0])):
            k=(min(pid[a],pid[b]),max(pid[a],pid[b])); cnt[k]=cnt.get(k,0)+1
    bedges=[k for k,c in cnt.items() if c==1]
    adj={}
    for a,b in bedges: adj.setdefault(a,[]).append(b); adj.setdefault(b,[]).append(a)
    seen=set(); loops=[]
    for s in adj:
        if s in seen: continue
        loop=[s]; seen.add(s); prev=None; cur=s
        while True:
            nxt=[n for n in adj[cur] if n!=prev and n not in seen]
            if not nxt: break
            prev,cur=cur,nxt[0]; loop.append(cur); seen.add(cur)
        loops.append(loop)
    return loops

def bary(p, a, b, c):
    v0=b-a; v1=c-a; v2=p-a
    d00=v0@v0; d01=v0@v1; d11=v1@v1; d20=v2@v0; d21=v2@v1
    den=d00*d11-d01*d01 or 1e-20
    v=(d11*d20-d01*d21)/den; w=(d00*d21-d01*d20)/den
    return np.array([1-v-w,v,w])

def uv_islands(m):
    """island id per triangle: triangles connected through shared vertex records (same vb, same index)."""
    T=m['T']; parent=list(range(len(T)))
    def find(x):
        while parent[x]!=x: parent[x]=parent[parent[x]]; x=parent[x]
        return x
    # vertex records are per strip; join records with identical (vb, pos, uv)
    key={}; rid=np.zeros(len(m['P']),int)
    for i in range(len(m['P'])):
        rid[i]=key.setdefault((int(m['VB'][i]),)+tuple(np.round(m['P'][i],5))+tuple(m['UV'][i]),len(key))
    owner={}
    for ti,t in enumerate(T):
        for v in t:
            r=rid[v]
            if r in owner: parent[find(ti)]=find(owner[r])
            else: owner[r]=ti
    return np.array([find(i) for i in range(len(T))])

def transplant(m, donor, lo, hi, d_lo=None, d_hi=None):
    lo=np.array(lo,float); hi=np.array(hi,float); d_lo=lo if d_lo is None else np.array(d_lo,float); d_hi=hi if d_hi is None else np.array(d_hi,float)
    P=m['P']; T=m['T']; pid=pos_ids(P)
    inwin=((P>=lo)&(P<=hi)).all(1)
    cut=inwin[T].all(1) if CUT_MODE=='all' else (inwin[T].sum(1)>=CUT_MIN)
    isl=uv_islands(m)
    # donor patch in the same window (its own coordinates), then box-mapped onto the woman's cut extents
    gv=P[T[cut]].reshape(-1,3)
    dP=donor['P']; dT=donor['T']; dcen=dP[dT].mean(1)
    hole0=boundary_loops(T[cut],pid)
    hole0=max(hole0,key=len)
    rep0={}
    for i in range(len(P)): rep0.setdefault(pid[i],i)
    poly=np.array([P[rep0[k]][:2] for k in hole0])
    def inside(pt):
        x,y=pt; c=False; n=len(poly)
        for i in range(n):
            x1,y1=poly[i]; x2,y2=poly[(i+1)%n]
            if (y1>y)!=(y2>y) and x<(x2-x1)*(y-y1)/(y2-y1+1e-20)+x1: c=not c
        return c
    dsel=np.array([inside(c[:2]) and c[2]<0.0 for c in dcen])
    # close small holes: add donor triangles whose three positions are all used by the selection
    dpid0=pos_ids(dP)
    for it in range(3):
        used=set(dpid0[dT[dsel]].ravel())
        add=np.array([all(dpid0[v] in used for v in t) for t in dT])&~dsel
        if not add.any(): break
        dsel|=add
    dv=dP[dT[dsel]].reshape(-1,3)
    gx=np.abs(gv[:,0]).max(); dx=np.abs(dv[:,0]).max()
    fx=affine_box(np.array([-dx,dv[:,1].min(),dv[:,2].min()]),np.array([dx,dv[:,1].max(),dv[:,2].max()]),
                  np.array([-gx,gv[:,1].min(),gv[:,2].min()]),np.array([gx,gv[:,1].max(),gv[:,2].max()]))
    patchT=dT[dsel]; dpid=pos_ids(dP)
    # unique donor positions used by the patch
    used=np.unique(dpid[patchT.ravel()]); rep={}
    for i in range(len(dP)): rep.setdefault(dpid[i],i)
    Q={u:fx(dP[rep[u]]) for u in used}
    # boundary loops: the hole's (woman) and the patch's (donor)
    hole=boundary_loops(T[cut],pid); pl=boundary_loops(np.array([[dpid[a],dpid[b],dpid[c]] for a,b,c in patchT]),{i:i for i in range(dpid.max()+1)})
    return dict(cut=cut,isl=isl,patchT=patchT,dpid=dpid,Q=Q,hole=hole,patch_loops=pl,pid=pid)

def build(m, donor, lo, hi, shrink=0.92, falloff=None):
    r=transplant(m,donor,lo,hi)
    P=m['P']; T=m['T']; pid=r['pid']; cut=r['cut']
    cutT=T[cut]; A,B,C=P[cutT[:,0]],P[cutT[:,1]],P[cutT[:,2]]
    cen=P[cutT].reshape(-1,3).mean(0)
    # shrink the patch a little round the cut's centre (back view) so the zipper has width
    Q={k:np.array([cen[0]+(v[0]-cen[0])*shrink,cen[1]+(v[1]-cen[1])*shrink,v[2]]) for k,v in r['Q'].items()}
    if falloff:
        # blend the patch onto the woman's own surface near its edge: on the edge it lies on her cut surface (no step
        # for the stitch to bridge), a falloff inside it is the donor's shape
        edge=np.array([Q[k] for k in max(r['patch_loops'],key=len)])
        for k,q in list(Q.items()):
            cq=tri_closest(q,A,B,C); s0=cq[np.argmin(((cq-q)**2).sum(1))]
            d=np.sqrt(((edge-q)**2).sum(1)).min(); f=np.clip(d/falloff,0,1); f=f*f*(3-2*f)
            Q[k]=s0+(q-s0)*f
    # record lists to extend
    NP=list(P);NN=list(m['N']);NUV=list(m['UV']);NC=list(m['C']);NJ=list(m['J']);NW=list(m['W']);NVB=list(m['VB'])
    vb=int(np.bincount(m['VB'][cutT.ravel()]).argmax())
    tm=int(m['TM'][np.where(cut)[0][0]])
    newid={}
    def attrs(p):
        cq=tri_closest(p,A,B,C); k=np.argmin(((cq-p)**2).sum(1)); t=cutT[k]
        bc=np.clip(bary(cq[k],P[t[0]],P[t[1]],P[t[2]]),0,1); bc/=bc.sum()
        uv=sum(bc[i]*m['UV'][t[i]] for i in range(3)); col=sum(bc[i]*m['C'][t[i]] for i in range(3))
        w={}
        for i in range(3):
            for jj,ww in zip(m['J'][t[i]],m['W'][t[i]]):
                if ww>0: w[jj]=w.get(jj,0)+bc[i]*ww
        top=sorted(w.items(),key=lambda x:-x[1])[:3]; s=sum(x[1] for x in top) or 1
        J=[x[0] for x in top]+[0]*(3-len(top)); W=[round(100*x[1]/s) for x in top]+[0]*(3-len(top)); W[0]+=100-sum(W)
        return uv,col,J,W
    for k,p in Q.items():
        uv,col,J,W=attrs(p)
        NP.append(p);NN.append(np.zeros(3));NUV.append(uv);NC.append(col);NJ.append(J);NW.append(W);NVB.append(vb)
        newid[k]=len(NP)-1
    dpid=r['dpid']
    patch=[(newid[dpid[a]],newid[dpid[b]],newid[dpid[c]]) for a,b,c in r['patchT']]
    # zipper between the hole loop (woman's existing records in vb) and the patch loop
    rec={}  # position id -> a record of the woman's in this vb on the hole
    for t in cutT:
        for v in t:
            if m['VB'][v]==vb: rec.setdefault(pid[v],v)
    H=[rec[k] for k in max(r['hole'],key=len)]; Pl=[newid[k] for k in max(r['patch_loops'],key=len)]
    NPa=np.array(NP)
    def ang(i): d=NPa[i]-cen; return np.arctan2(d[1],d[0])
    def orient(L):
        a=np.unwrap([ang(i) for i in L]); 
        if a[-1]<a[0]: L=L[::-1]
        k=int(np.argmin([ang(i)%(2*np.pi) for i in L])); return L[k:]+L[:k]
    def area(L):
        p=NPa[L][:,:2]; return 0.5*np.sum(p[:,0]*np.roll(p[:,1],-1)-np.roll(p[:,0],-1)*p[:,1])
    if np.sign(area(H))!=np.sign(area(Pl)): Pl=Pl[::-1]
    k=int(np.argmin(((NPa[Pl]-NPa[H[0]])**2).sum(1))); Pl=Pl[k:]+Pl[:k]
    nH,nP=len(H),len(Pl); i=j=0; zip_=[]
    while i<nH or j<nP:
        if i>=nH: adv_h=False
        elif j>=nP: adv_h=True
        else:
            dh=np.linalg.norm(NPa[H[(i+1)%nH]]-NPa[Pl[j%nP]]); dp=np.linalg.norm(NPa[H[i%nH]]-NPa[Pl[(j+1)%nP]])
            adv_h=dh<=dp
        if adv_h: zip_.append((H[i%nH],Pl[j%nP],H[(i+1)%nH])); i+=1
        else: zip_.append((H[i%nH],Pl[j%nP],Pl[(j+1)%nP])); j+=1
    newT=[tuple(t) for t,c in zip(T,cut) if not c]+patch+zip_
    newTM=[tm2 for tm2,c in zip(m['TM'],cut) if not c]+[tm]*(len(patch)+len(zip_))
    out=dict(m); out.update(P=np.array(NP,float),N=np.array(NN,float),UV=np.array(NUV,float),C=np.array(NC,float),J=np.array(NJ,int),W=np.array(NW,float),VB=np.array(NVB),T=np.array(newT,int),TM=np.array(newTM))
    # winding: match the woman's cut triangles' facing (outward = away from the hip axis)
    def facing(PP,TT):
        n=np.cross(PP[TT[:,1]]-PP[TT[:,0]],PP[TT[:,2]]-PP[TT[:,0]]); c=PP[TT].mean(1); o=c-np.array([0,c[:,1].mean()*0,0])
        o=np.stack([c[:,0],np.zeros(len(c)),c[:,2]],1)
        return np.sign((n*o).sum(1))
    ref=np.sign(facing(P,cutT).sum())
    nt=out['T']; added=np.arange(len(nt)-len(patch)-len(zip_),len(nt))
    f=facing(out['P'],nt[added]); flip=added[f!=ref]
    nt[flip]=nt[flip][:,[0,2,1]]; out['T']=nt
    # normals: smooth over the patch and zipper, the hole's own records keep theirs
    sn,spid=smooth_normals(out['P'],out['T'])
    newrec=np.arange(len(P),len(NP)); out['N'][newrec]=sn[newrec]
    if falloff:
        # the hole's ring too, so the shading runs on across the stitch
        ring=set(pid[v] for v in H)
        for i in range(len(P)):
            if pid[i] in ring: out['N'][i]=sn[i]
    return out, len(patch), len(zip_)
