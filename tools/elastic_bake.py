#!/usr/bin/env python3
"""Additive elastic bank: isotropic trilinear hex FEM, consistent mass.

Fixed 16^3 material field comes from the existing 4^3 voxel-shaped bodies.
Both mechanical resolutions integrate that SAME field against their own
trilinear basis (Galerkin coarsening). These are voxel approximations, not
imports of the paper's original triangle meshes. Frequencies are rad/s.
Resolution morph pairs frequency-sorted modes, not formal modal tracking.
Classic data and its bake are never modified by this tool.
"""
import argparse
from pathlib import Path
import numpy as np
from scipy import sparse, linalg
from scipy.sparse.linalg import eigsh
import modal_bake as classic

CORNERS = np.array([(x,y,z) for z in (0,1) for y in (0,1) for x in (0,1)])


def shape(p):
    factors = np.where(CORNERS, p, 1-p)
    N = factors.prod(axis=1)
    grad = np.empty((8,3))
    for d in range(3):
        grad[:,d] = (2*CORNERS[:,d]-1)*factors[:,[j for j in range(3) if j != d]].prod(axis=1)
    return N, grad


def elasticity(E, nu):
    if E <= 0 or not -1 < nu < .5:
        raise ValueError('invalid isotropic material')
    mu = E/(2*(1+nu)); lam = E*nu/((1+nu)*(1-2*nu))
    D = np.zeros((6,6)); D[:3,:3] = lam
    D[np.arange(3),np.arange(3)] += 2*mu
    D[3:,3:] = np.eye(3)*mu
    return D


def element(E, nu, rho, h, weights=None):
    """Integrate a hex against a subvoxel material distribution.

    Full 2x2x2 Gauss rule per occupied subvoxel. No underintegration,
    artificial springs, diagonal stabilizers or rigid-mode clamping.
    """
    weights = np.ones((1,1,1)) if weights is None else weights
    sub = weights.shape[0]; D = elasticity(E,nu)
    K = np.zeros((24,24)); M = np.zeros((24,24))
    q = (.5-.5/np.sqrt(3), .5+.5/np.sqrt(3))
    for cell in np.argwhere(weights > 0):
        weight = weights[tuple(cell)]*(h/sub)**3/8
        for x in q:
            for y in q:
                for z in q:
                    N, grad = shape((cell + (x,y,z))/sub); grad /= h
                    B = np.zeros((6,24))
                    B[0,0::3]=grad[:,0]; B[1,1::3]=grad[:,1]; B[2,2::3]=grad[:,2]
                    B[3,0::3]=grad[:,1]; B[3,1::3]=grad[:,0]
                    B[4,1::3]=grad[:,2]; B[4,2::3]=grad[:,1]
                    B[5,0::3]=grad[:,2]; B[5,2::3]=grad[:,0]
                    K += weight*(B.T@D@B)
                    M += weight*rho*np.kron(np.outer(N,N),np.eye(3))
    return K,M


def material_field(preset):
    # Classic shape rules evaluated ONCE at their authored resolution.
    # Refinement subdivides physical cells, never re-evaluates index-based shapes.
    occ = classic.build_grid(4,preset)[3]
    return occ.repeat(4,axis=0).repeat(4,axis=1).repeat(4,axis=2)


def assemble(preset, g, field=None):
    field = material_field(preset) if field is None else field
    if field.shape != (16,16,16) or 16 % g:
        raise ValueError('mechanical grid must divide the fixed 16^3 field')
    nn=g+1; h=preset['L']/g; sub=16//g
    rows=[]; cols=[]; kval=[]; mval=[]; cache={}
    for x in range(g):
        for y in range(g):
            for z in range(g):
                w=field[x*sub:(x+1)*sub,y*sub:(y+1)*sub,z*sub:(z+1)*sub]
                if not np.any(w): continue
                key=w.tobytes()
                if key not in cache:
                    cache[key]=element(preset['E'],preset['nu'],preset['rho'],h,w)
                K,M=cache[key]
                nodes=CORNERS+(x,y,z)
                ids=((nodes[:,2]*nn+nodes[:,1])*nn+nodes[:,0])*3
                ids=(ids[:,None]+np.arange(3)).ravel()
                rows.extend(np.repeat(ids,24)); cols.extend(np.tile(ids,24))
                kval.extend(K.ravel()); mval.extend(M.ravel())
    n=nn**3*3
    K=sparse.coo_matrix((kval,(rows,cols)),shape=(n,n)).tocsc()
    M=sparse.coo_matrix((mval,(rows,cols)),shape=(n,n)).tocsc()
    active=np.flatnonzero(M.diagonal()>0)
    return K[active,:][:,active], M[active,:][:,active], active


def modes(K,M,nmax=128):
    n=K.shape[0]
    if n <= 450:
        values,V=linalg.eigh(K.toarray(),M.toarray())
    else:
        # A negative shift makes the factorization nonsingular without changing K.
        scale=float(np.max(K.diagonal()/M.diagonal()))
        values,V=eigsh(K,k=min(nmax+6,n-1),M=M,sigma=-scale*1e-5,
                      which='LM',v0=np.random.default_rng(47).normal(size=n),tol=1e-10)
    order=np.argsort(values); values=values[order]; V=V[:,order]
    scale=float(np.max(K.diagonal()/M.diagonal()))
    tol=scale*1e-9
    if np.any(values < -tol): raise ValueError(f'negative stiffness eigenvalues: {values[:8]}')
    if np.count_nonzero(np.abs(values)<=tol)!=6:
        raise ValueError(f'expected six rigid modes: {values[:10]}, tolerance {tol}')
    values=values[6:6+nmax]; V=V[:,6:6+nmax]
    if len(values)==0 or np.any(values<=tol): raise ValueError('no elastic spectrum')
    return np.sqrt(values),V


def gains(V,active,g,field):
    full=np.zeros(((g+1)**3*3,V.shape[1])); full[active]=V
    out=np.zeros((V.shape[1],16,16)); nn=g+1
    occupied=np.argwhere(field>0)
    for iy in range(16):
        for ix in range(16):
            xy=np.array([ix,iy])/15
            col=np.minimum((xy*16).astype(int),15)
            zs=np.flatnonzero(field[col[0],col[1],:]>0)
            if not len(zs):
                # Empty disc positions project to nearest occupied surface column.
                distance=((occupied[:,:2]+.5-xy*16)**2).sum(axis=1)
                nearest=occupied[np.argmin(distance)]
                col=nearest[:2]; xy=(col+.5)/16
                zs=np.flatnonzero(field[col[0],col[1],:]>0)
            p=np.array([xy[0],xy[1],(zs[-1]+1)/16])*g
            cell=np.minimum(np.floor(p).astype(int),g-1)
            N,_=shape(p-cell); nodes=CORNERS+cell
            ids=((nodes[:,2]*nn+nodes[:,1])*nn+nodes[:,0])*3+2
            out[:,iy,ix]=N@full[ids]
    norm=np.max(np.abs(out).sum(axis=0))
    if not norm>1e-12: raise ValueError('silent excitation map')
    out/=norm
    # Fix arbitrary eigenvector signs for reproducible signed maps.
    for m in range(len(out)):
        a=out[m].ravel()
        if a[np.argmax(np.abs(a))]<0: out[m]*=-1
    return out


def bake(p):
    field=material_field(p); data=[]
    for g in (4,8):
        K,M,active=assemble(p,g,field); freq,V=modes(K,M)
        G=gains(V,active,g,field)
        decay=.5*(p['alpha1']+p['alpha2']*freq**2)
        data.append((freq,decay,G))
        translation=(active%3==0).astype(float)
        mass=float(translation@M@translation)
        expected=field.sum()*(p['L']/16)**3*p['rho']
        if not np.isclose(mass,expected,rtol=1e-10): raise ValueError('mass not conserved')
        print(f"{p['name']} g={g} DOF={len(active)} rigid=6 mass={mass:.8g} f0={freq[0]/(2*np.pi):.4f}Hz",flush=True)
    return data


def emit(results,path):
    def arr(a):
        a=np.asarray(a)
        if a.ndim==1: return '{'+','.join(f'{float(v):.9e}f' for v in a)+'}'
        return '{'+',\n'.join(arr(v) for v in a)+'}'
    def pad(a,n):
        result=np.zeros((128,)+a.shape[1:]);result[:n]=a[:n];return result
    banks=[]; fine=[]
    for p,data in zip(classic.PRESETS,results):
        (f,d,G),(ff,dd,GG)=data; n=min(len(f),len(ff),128)
        banks.append('{"'+p['name']+'",'+str(n)+','+','.join(arr(pad(a,n)) for a in (f,d,G,ff,dd))+'}')
        fine.append(arr(pad(GG,n)))
    text='// Generated by tools/elastic_bake.py; angular frequencies rad/s. Do not edit.\n#pragma once\n#include "ModalData.hpp"\nnamespace modal {\n'
    text+='inline const PresetData kElasticPresets[kNumPresets] = {'+',\n'.join(banks)+'};\n'
    text+='inline const float kElasticFineGains[kNumPresets][kMaxModes][kGainGrid][kGainGrid] = {'+',\n'.join(fine)+'};\n}\n'
    Path(path).write_text(text,encoding='utf-8')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('-o','--out',default=str(Path(__file__).resolve().parents[1]/'plugins/MultiScaleBody/src/ElasticModalData.hpp'))
    args=parser.parse_args();emit([bake(p) for p in classic.PRESETS],args.out)
    print('Wrote',args.out)

if __name__=='__main__': main()
