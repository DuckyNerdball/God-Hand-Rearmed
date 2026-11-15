import sys, numpy as np
import os; _H=os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0,os.path.join(_H,'..','xavier')); sys.path.insert(0,os.path.join(_H,'..')); 
from gamemodel import dat_read, scr_read, mdb_read, tm3_read, strip_tris
from gametex import tim3_decode
EX=os.environ.get('GODHAND_EXTRACT','extract')
def load(f, md=None, tm=None, skip=lambda n: 'mip' in n or n.endswith('_mi')):
    ents=dat_read(open(EX+'/'+f,'rb').read())
    if md is None: md=next(i for i,(t,b,o) in enumerate(ents) if t=='MD' and b)
    if tm is None: tm=md-1 if md>0 and ents[md-1][0]=='TM3' else None
    raw=ents[md][1]; kind,nodes=scr_read(raw)
    texs=tm3_read(ents[tm][1]) if tm is not None else []
    dec={}
    tris=[]  # (V3x3, UV3x2, mat)
    V=[];UV=[];T=[];MAT=[];J=[];W=[];NODE=[]
    for nd in nodes:
        if skip(nd['name']): continue
        m=mdb_read(raw,nd['mdb_off'])
        for vb in m['vbs']:
            base=len(V)
            for k in range(vb['count']):
                V.append(vb['pos'][k]); u,v=vb['uv'][k]; UV.append((u/4096.0,1.0+v/4096.0))
                if vb['wt']:
                    jb,wb=vb['wt'][k]; J.append([jb[1]//4,jb[2]//4,jb[3]//4]); W.append(list(wb[:3]))
                else: J.append([0,0,0]); W.append([0,0,0])
                NODE.append(nd['name'])
            for a,b,c in strip_tris(vb['flags']): T.append((base+a,base+b,base+c)); MAT.append(vb['mat'])
    imgs={}
    for mt in set(MAT):
        if mt<len(texs):
            try: imgs[mt]=tim3_decode(texs[mt][1])[0]
            except Exception as e: imgs[mt]=None
    bones=mdb_read(raw,nodes[0]['mdb_off'])['bones']
    return dict(V=np.array(V,float),UV=np.array(UV),T=np.array(T),MAT=np.array(MAT),imgs=imgs,J=np.array(J),W=np.array(W,float),NODE=np.array(NODE),bones=bones,texnames=[t[0] for t in texs])
def render(objs, view, size, extent, bg=(255,255,255), shade_on=True):
    H,Wd=size; c,span=extent; scale=min(H,Wd)/span
    img=np.zeros((H,Wd,3),float); img[:]=bg; zb=np.full((H,Wd),-1e9)
    light=np.array([0.3,0.6,0.75]); light/=np.linalg.norm(light)
    for o in objs:
        V,T,UV,MAT,imgs=o['V'],o['T'],o['UV'],o['MAT'],o['imgs']
        cv=V@view.T
        px=(cv[:,0]-c[0])*scale+Wd/2; py=H/2-(cv[:,1]-c[1])*scale; pz=cv[:,2]
        a=cv[T[:,0]];b=cv[T[:,1]];cc=cv[T[:,2]]
        nrm=np.cross(b-a,cc-a); nrm/=np.linalg.norm(nrm,axis=1)[:,None]+1e-12
        shade=np.abs(nrm@light)*0.6+0.4 if shade_on else np.ones(len(T))
        vsh=None
        if 'N' in o and shade_on:
            nv=o['N']@view.T; nv=nv/(np.linalg.norm(nv,axis=1)[:,None]+1e-12); vsh=np.abs(nv@light)*0.6+0.4
        for t in range(len(T)):
            i0,i1,i2=T[t]
            xs_=np.array([px[i0],px[i1],px[i2]]); ys_=np.array([py[i0],py[i1],py[i2]])
            x0=int(max(0,np.floor(xs_.min()))); x1=int(min(Wd-1,np.ceil(xs_.max())))
            y0=int(max(0,np.floor(ys_.min()))); y1=int(min(H-1,np.ceil(ys_.max())))
            if x1<x0 or y1<y0: continue
            xs,ys=np.meshgrid(np.arange(x0,x1+1)+0.5,np.arange(y0,y1+1)+0.5)
            ax,bx,cx=xs_; ay,by,cy=ys_
            den=(by-cy)*(ax-cx)+(cx-bx)*(ay-cy)
            if abs(den)<1e-9: continue
            w1=((by-cy)*(xs-cx)+(cx-bx)*(ys-cy))/den; w2=((cy-ay)*(xs-cx)+(ax-cx)*(ys-cy))/den; w3=1-w1-w2
            m=(w1>=-1e-4)&(w2>=-1e-4)&(w3>=-1e-4)
            if not m.any(): continue
            z=w1*pz[i0]+w2*pz[i1]+w3*pz[i2]
            sub=zb[y0:y1+1,x0:x1+1]
            tex=imgs.get(MAT[t])
            if tex is not None:
                u=w1*UV[i0,0]+w2*UV[i1,0]+w3*UV[i2,0]; v=w1*UV[i0,1]+w2*UV[i1,1]+w3*UV[i2,1]
                th,tw=tex.shape[:2]
                tx=np.clip((u%1.0)*tw,0,tw-1).astype(int); ty=np.clip((1-(v%1.0))*th,0,th-1).astype(int)
                col=tex[ty,tx]
                m&=col[...,3]>64
                m&=z>sub
                if not m.any(): continue
                sub[m]=z[m]
                sh=shade[t] if vsh is None else (w1*vsh[i0]+w2*vsh[i1]+w3*vsh[i2])[m][:,None]
                img[y0:y1+1,x0:x1+1][m]=col[...,:3][m]*sh
            else:
                m&=z>sub; sub[m]=z[m]; img[y0:y1+1,x0:x1+1][m]=np.array((200,180,170))*shade[t]
    return np.clip(img,0,255).astype(np.uint8)
def rotY(a):
    c,s=np.cos(a),np.sin(a); return np.array([[c,0,s],[0,1,0],[-s,0,c]])
