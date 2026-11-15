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

def transplant(m, donor, lo, hi, d_lo=None, d_hi=None, mapping='box', one_vb=False):
    lo=np.array(lo,float); hi=np.array(hi,float); d_lo=lo if d_lo is None else np.array(d_lo,float); d_hi=hi if d_hi is None else np.array(d_hi,float)
    P=m['P']; T=m['T']; pid=pos_ids(P)
    inwin=((P>=lo)&(P<=hi)).all(1)
    cut=inwin[T].all(1) if CUT_MODE=='all' else (inwin[T].sum(1)>=CUT_MIN)
    if one_vb:
        # stay inside the costume piece that holds the seat: the patch then ends on the piece's own seam (a waistband
        # stays a clean line instead of sawtooth where patch triangles pick one material or the other)
        tvb=m['VB'][T[:,0]]; main=np.bincount(tvb[cut]).argmax(); cut&=tvb==main
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
    if mapping=='box':
        fx=affine_box(np.array([-dx,dv[:,1].min(),dv[:,2].min()]),np.array([dx,dv[:,1].max(),dv[:,2].max()]),
                      np.array([-gx,gv[:,1].min(),gv[:,2].min()]),np.array([gx,gv[:,1].max(),gv[:,2].max()]))
    else:
        # the donor at her own size (same skeleton, same space): only her hip width matched to the woman's at seat
        # height, so the new seat touches the old one's volume instead of being squeezed into its boxy outline
        def hipw(PP):
            b=PP[(PP[:,1]>0.95)&(PP[:,1]<1.05)&(PP[:,2]<0.0)]
            return np.abs(b[:,0]).max()
        sx=hipw(P)/hipw(dP)
        fx=lambda X: X*np.array([sx,1.0,1.0])
    patchT=dT[dsel]; dpid=pos_ids(dP)
    # unique donor positions used by the patch
    used=np.unique(dpid[patchT.ravel()]); rep={}
    for i in range(len(dP)): rep.setdefault(dpid[i],i)
    Q={u:fx(dP[rep[u]]) for u in used}
    # boundary loops: the hole's (woman) and the patch's (donor)
    hole=boundary_loops(T[cut],pid); pl=boundary_loops(np.array([[dpid[a],dpid[b],dpid[c]] for a,b,c in patchT]),{i:i for i in range(dpid.max()+1)})
    return dict(cut=cut,isl=isl,patchT=patchT,dpid=dpid,Q=Q,hole=hole,patch_loops=pl,pid=pid)

def build(m, donor, lo, hi, shrink=0.92, falloff=None, mapping='box'):
    r=transplant(m,donor,lo,hi,mapping=mapping)
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

def seg_closest(p, S0, S1):
    d=S1-S0; t=np.clip(((p-S0)*d).sum(1)/np.maximum((d*d).sum(1),1e-20),0,1); q=S0+d*t[:,None]
    return q[np.argmin(((q-p)**2).sum(1))]

def rear_profile(PP, TT, ys, xs=np.arange(0.03,0.105,0.01)):
    """The rearmost surface (most negative z) per height, over x 0.03-0.10 (nan where nothing is hit)."""
    from xfer import ray_hit
    A,B,C=PP[TT[:,0]],PP[TT[:,1]],PP[TT[:,2]]; out=np.full(len(ys),np.nan)
    for i,y in enumerate(ys):
        for x in xs:
            t=ray_hit(np.array([x,y,-1.0]),np.array([0,0,1.0]),A,B,C); t=t[np.isfinite(t)]
            if len(t): out[i]=np.nanmin([out[i],-1.0+t.min()])
    return out

def depth_match(Q, patchT, P, cutT, ys=np.arange(0.88,1.165,0.01), most=1.5, margin=0.003):
    """Shannon's patch deepened per height (z scaled about the hip axis) wherever the woman's own seat reaches further
    back (plus margin: the blend onto her surface near the edge gives back a little), never made shallower: Cash at 101%
    is never smaller than her seat (em0d's women: theirs is 1-2 cm deeper than
    Shannon's, K Nov 15). The scale is smoothed over height."""
    keys=sorted(Q); idx={k:i for i,k in enumerate(keys)}
    QP=np.array([Q[k] for k in keys]); QT=np.array([[idx[a],idx[b],idx[c]] for a,b,c in patchT])
    her=rear_profile(P,cutT,ys); his=rear_profile(QP,QT,ys)
    s=np.where(np.isfinite(her)&np.isfinite(his)&(his<0),np.clip((her-margin)/np.where(his<0,his,-1),1.0,most),1.0)
    for it in range(2): s=np.maximum(1.0,np.convolve(np.pad(s,1,mode='edge'),[0.25,0.5,0.25],'valid'))
    return {k:np.array([q[0],q[1],q[2]*np.interp(q[1],ys,s)]) for k,q in Q.items()}, s

def build2(m, donor, lo, hi, falloff=0.04, mapping='own', pin=0.85, one_vb=True, match_depth=True):
    """build() for cuts across several vertex buffers: every new triangle takes the vertex buffer (material, UVs) of
    the woman's cut triangle under it; a position gets one record per vertex buffer that uses it."""
    r=transplant(m,donor,lo,hi,mapping=mapping,one_vb=one_vb)
    P=m['P']; T=m['T']; pid=r['pid']; cut=r['cut']
    cutT=T[cut]; cutI=np.where(cut)[0]
    A,B,C=P[cutT[:,0]],P[cutT[:,1]],P[cutT[:,2]]
    Q=dict(r['Q'])
    if match_depth: Q,_=depth_match(Q,[(r['dpid'][a],r['dpid'][b],r['dpid'][c]) for a,b,c in r['patchT']],P,cutT)
    fall={}
    if falloff:
        edge=np.array([Q[k] for k in max(r['patch_loops'],key=len)])
        for k,q in list(Q.items()):
            cq=tri_closest(q,A,B,C); s0=cq[np.argmin(((cq-q)**2).sum(1))]
            d=np.sqrt(((edge-q)**2).sum(1)).min(); f=np.clip(d/falloff,0,1); f=f*f*(3-2*f)
            Q[k]=s0+(q-s0)*f; fall[k]=f
        # the patch's edge pinned onto the hole's own boundary (most of the way), so the stitch lies flat on the
        # surface instead of standing edge-on where the two edges sit at different depths
        rep0={}
        for i in range(len(P)): rep0.setdefault(int(pid[i]),i)
        hl=max(r['hole'],key=len); HP=np.array([P[rep0[k]] for k in hl])
        S0=HP; S1=np.roll(HP,-1,0)
        for k in max(r['patch_loops'],key=len):
            h=seg_closest(Q[k],S0,S1); Q[k]=Q[k]+(h-Q[k])*pin
    NP=list(P);NN=list(m['N']);NUV=list(m['UV']);NC=list(m['C']);NJ=list(m['J']);NW=list(m['W']);NVB=list(m['VB'])
    cut_vb=m['VB'][cutT[:,0]]
    vb_tm={int(m['VB'][T[i][0]]):int(m['TM'][i]) for i in cutI}
    def closest_in(p, vb):
        sel=np.where(cut_vb==vb)[0]
        cq=tri_closest(p,A[sel],B[sel],C[sel]); k=sel[np.argmin(((cq-p)**2).sum(1))]
        return k, cq[np.argmin(((cq-p)**2).sum(1))]
    def vb_at(p):
        cq=tri_closest(p,A,B,C); return int(cut_vb[np.argmin(((cq-p)**2).sum(1))])
    recs={}  # (position key, vb) -> record
    for i in range(len(P)): recs.setdefault((('o',int(pid[i])),int(m['VB'][i])),i)
    def record(key, pos, vb):
        if (key,vb) in recs: return recs[(key,vb)]
        k,cq=closest_in(pos,vb); t=cutT[k]
        bc=np.clip(bary(cq,P[t[0]],P[t[1]],P[t[2]]),0,1); bc/=bc.sum()
        uv=sum(bc[i]*m['UV'][t[i]] for i in range(3)); col=sum(bc[i]*m['C'][t[i]] for i in range(3))
        w={}
        for i in range(3):
            for jj,ww in zip(m['J'][t[i]],m['W'][t[i]]):
                if ww>0: w[jj]=w.get(jj,0)+bc[i]*ww
        top=sorted(w.items(),key=lambda x:-x[1])[:3]; s=sum(x[1] for x in top) or 1
        J=[x[0] for x in top]+[0]*(3-len(top)); W=[round(100*x[1]/s) for x in top]+[0]*(3-len(top)); W[0]+=100-sum(W)
        NP.append(np.array(pos,float));NN.append(np.zeros(3));NUV.append(uv);NC.append(col);NJ.append(J);NW.append(W);NVB.append(vb)
        recs[(key,vb)]=len(NP)-1; return recs[(key,vb)]
    rep={}
    for i in range(len(P)): rep.setdefault(int(pid[i]),i)
    def opos(k): return P[rep[k]]
    newT=[];newTM=[]
    dpid=r['dpid']
    for a,b,c in r['patchT']:
        ks=[('d',int(dpid[a])),('d',int(dpid[b])),('d',int(dpid[c]))]; ps=[Q[k[1]] for k in ks]
        vb=vb_at(np.mean(ps,0))
        newT.append(tuple(record(k,p,vb) for k,p in zip(ks,ps))); newTM.append(vb_tm[vb])
    # stitch: shortest diagonals between the hole loop and the patch loop (positions), each triangle in the buffer
    # under it
    H=list(max(r['hole'],key=len)); Pl=list(max(r['patch_loops'],key=len))
    def hp(k): return opos(k)
    def pp(k): return Q[k]
    def area(pts):
        p=np.array(pts)[:,:2]; return 0.5*np.sum(p[:,0]*np.roll(p[:,1],-1)-np.roll(p[:,0],-1)*p[:,1])
    if np.sign(area([hp(k) for k in H]))!=np.sign(area([pp(k) for k in Pl])): Pl=Pl[::-1]
    k0=int(np.argmin([((pp(k)-hp(H[0]))**2).sum() for k in Pl])); Pl=Pl[k0:]+Pl[:k0]
    nH,nP=len(H),len(Pl); i=j=0; zips=[]
    while i<nH or j<nP:
        if i>=nH: adv=False
        elif j>=nP: adv=True
        else: adv=np.linalg.norm(hp(H[(i+1)%nH])-pp(Pl[j%nP]))<=np.linalg.norm(hp(H[i%nH])-pp(Pl[(j+1)%nP]))
        if adv: zips.append((('o',H[i%nH]),('d',Pl[j%nP]),('o',H[(i+1)%nH]))); i+=1
        else: zips.append((('o',H[i%nH]),('d',Pl[j%nP]),('d',Pl[(j+1)%nP]))); j+=1
    def kpos(k): return opos(k[1]) if k[0]=='o' else Q[k[1]]
    for z in zips:
        ps=[kpos(k) for k in z]; vb=vb_at(np.mean(ps,0))
        newT.append(tuple(record(k,p,vb) for k,p in zip(z,ps))); newTM.append(vb_tm[vb])
    keepT=[tuple(t) for t,c in zip(T,cut) if not c]; keepTM=[tm for tm,c in zip(m['TM'],cut) if not c]
    out=dict(m); out.update(P=np.array(NP,float),N=np.array(NN,float),UV=np.array(NUV,float),C=np.array(NC,float),J=np.array(NJ,int),W=np.array(NW,float),VB=np.array(NVB),
                            T=np.array(keepT+newT,int),TM=np.array(keepTM+newTM))
    # winding: each new triangle faces the way her surface under it faces (her normals at the closest point of the
    # cut); outward from the hip axis turned the inner thighs' and the crotch's triangles away (K, Nov 15: a gap)
    nt=out['T']; added=np.arange(len(keepT),len(nt))
    for t in added:
        a,b,c=nt[t]; ctr=(out['P'][a]+out['P'][b]+out['P'][c])/3
        cq=tri_closest(ctr,A,B,C); k=int(np.argmin(((cq-ctr)**2).sum(1))); ct=cutT[k]
        bc=np.clip(bary(cq[k],P[ct[0]],P[ct[1]],P[ct[2]]),0,1)
        ref=sum(bc[i]*m['N'][ct[i]] for i in range(3))
        if np.cross(out['P'][b]-out['P'][a],out['P'][c]-out['P'][a])@ref<0: nt[t]=[a,c,b]
    out['T']=nt
    # normals from her mesh + the patch only: the stitch's slivers would bend the edge's shading
    sn,_=smooth_normals(out['P'],out['T'][:len(keepT)+len(r['patchT'])])
    # shading follows the geometry's blend: her own normal (at the closest point of the cut) at the patch's edge, the
    # patch's over the same falloff; the hole's ring keeps hers. (Normals recomputed on both sides of the stitch left a
    # hard line down the side of the hip, K Nov 15.)
    key_of={v:kk for (kk,vb),v in recs.items()}
    for i in range(len(P),len(out['P'])):
        q=out['P'][i]; cq=tri_closest(q,A,B,C); kk=int(np.argmin(((cq-q)**2).sum(1))); ct=cutT[kk]
        bc=np.clip(bary(cq[kk],P[ct[0]],P[ct[1]],P[ct[2]]),0,1); bc/=bc.sum()
        her=sum(bc[j2]*m['N'][ct[j2]] for j2 in range(3)); her/=np.linalg.norm(her)+1e-12
        key=key_of.get(i); f=fall.get(key[1],0.0) if key and key[0]=='d' else 0.0
        n=her*(1-f)+sn[i]*f; out['N'][i]=n/(np.linalg.norm(n)+1e-12)
    return out, len(r['patchT']), len(zips)
