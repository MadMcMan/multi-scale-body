#!/usr/bin/env python3
import numpy as np
import argparse, os, sys

# Global excitation trim. Replacing the fake element with real B^T*D*B changed
# the mass-normalized eigenvector magnitudes (and hence the modal excitation),
# which dropped the rendered default ~5 dB (golden peak 0.363 -> 0.198). This
# constant restores the pre-rebake reference loudness; it scales every shipped
# gain uniformly, so relative body balance and the Sound Map are unchanged.
kGainTrim = 1.70

def hex_element_matrices(E, nu, rho, hx, hy, hz):
    """Standard 8-node trilinear hexahedral element (B^T*D*B, 2x2x2 Gauss).

    DOF layout is component-major, [d*8+i] with d in {0,1,2} = (x,y,z) and
    i the node, matching the assembly in build_grid(). A real element is
    positive-semidefinite with EXACTLY 6 zero modes (the rigid-body motions);
    the old graph-Laplacian heuristic was indefinite with only 2 null dims.
    """
    nat = np.array([[-1,-1,-1],[ 1,-1,-1],[-1, 1,-1],[ 1, 1,-1],
                    [-1,-1, 1],[ 1,-1, 1],[-1, 1, 1],[ 1, 1, 1]], dtype=float)
    lam = E*nu/((1.0+nu)*(1.0-2.0*nu))
    mu  = E/(2.0*(1.0+nu))
    D = np.zeros((6,6))
    D[0:3,0:3] = lam
    D[0,0]=D[1,1]=D[2,2] = lam+2.0*mu
    D[3,3]=D[4,4]=D[5,5] = mu
    a,b,c = hx/2.0, hy/2.0, hz/2.0
    detJ = a*b*c
    inv = (2.0/hx, 2.0/hy, 2.0/hz)
    Ke = np.zeros((24,24))
    Me = np.zeros((24,24))
    g = 1.0/np.sqrt(3.0)
    for s1 in (-g,g):
      for s2 in (-g,g):
        for s3 in (-g,g):
          xi,eta,zeta = s1,s2,s3
          dN = np.empty((8,3))
          for i in range(8):
              xi_i, eta_i, zeta_i = nat[i]
              t = 0.125
              dN[i,0] = t*xi_i*(1.0+eta*eta_i)*(1.0+zeta*zeta_i)
              dN[i,1] = t*eta_i*(1.0+xi*xi_i)*(1.0+zeta*zeta_i)
              dN[i,2] = t*zeta_i*(1.0+xi*xi_i)*(1.0+eta*eta_i)
          B = np.zeros((6,24))
          for i in range(8):
              dx,dy,dz = dN[i,0]*inv[0], dN[i,1]*inv[1], dN[i,2]*inv[2]
              B[0,0*8+i]=dx; B[1,1*8+i]=dy; B[2,2*8+i]=dz
              B[3,1*8+i]=dz; B[3,2*8+i]=dy
              B[4,0*8+i]=dz; B[4,2*8+i]=dx
              B[5,0*8+i]=dy; B[5,1*8+i]=dx
          Ke += (B.T @ D @ B) * detJ
          Nv = np.array([0.125*(1.0+xi*nat[i,0])*(1.0+eta*nat[i,1])*(1.0+zeta*nat[i,2]) for i in range(8)])
          NN = np.outer(Nv,Nv) * (rho*detJ)
          for d in range(3):
              Me[d*8:(d+1)*8, d*8:(d+1)*8] += NN
    return Ke, Me

def build_grid(g, preset):
    E, nu, rho = preset['E'], preset['nu'], preset['rho']
    L = preset.get('L', 0.4)
    h = L / g
    occ = np.ones((g,g,g), dtype=float)
    name = preset['name']
    if name in ('Bowl','LogDrum'):
        inner = np.zeros((g,g,g), dtype=bool)
        if g>=3:
            inner[1:-1,1:-1,1:-1]=True
            inner[1:-1,1:-1,0]=False
        occ[inner]=0.15
    elif name=='Plate':
        occ[:,:,:]=0.0
        occ[:,:,g-1]=1.0
        if g>=4:
            occ[:,:,g-2]=0.5
    elif name=='Squirrel':
        occ[:,:,:]=0.0
        occ[1:3,1:3,1:3]=1.0
        if g>=4:
            occ[3,1:3,1:3]=0.7
            occ[0,2,2]=0.5
    elif name=='Blade':
        occ[:,:,:]=0.0
        occ[1:3,1:3,1:3]=1.0
        occ[0,1:3,2]=0.9
        occ[3,1:3,2]=0.9
        if g>=4:
            occ[1,0,2]=0.6
            occ[2,3,2]=0.6
    elif name=='Shell':
        occ[:,:,:]=0.0
        for x in range(g):
            for y in range(g):
                for z in range(g):
                    cx, cy, cz = g/2-0.5, g/2-0.5, 0
                    dx, dy, dz = x-cx, y-cy, z-cz
                    r = np.sqrt(dx*dx+dy*dy+dz*dz)
                    if 1.2 < r < 2.0 and z < g*0.6:
                        occ[x,y,z]=1.0
                    elif 1.0 < r < 2.2 and z < g*0.7:
                        occ[x,y,z]=0.5
    elif name in ('Bar','Marimba','Kalimba','Celesta'):
        occ[:,:,:]=0.0
        occ[:,1:3,1]=1.0
        if g>=4:
            occ[:,1,1]=0.8
            occ[:,2,1]=0.8
    elif name=='Membrane':
        occ[:,:,:]=0.0
        for x in range(g):
            for y in range(g):
                dx, dy = x-g/2+0.5, y-g/2+0.5
                if dx*dx+dy*dy < (g*0.45)**2:
                    occ[x,y,g-1]=1.0
                    if g>=4:
                        occ[x,y,g-2]=0.3
    elif name=='Bell':
        occ[:,:,:]=0.0
        for x in range(g):
            for y in range(g):
                for z in range(g):
                    cx, cy, cz = g/2-0.5, g/2-0.5, 0
                    dx, dy, dz = x-cx, y-cy, z-cz
                    r = np.sqrt(dx*dx+dy*dy+dz*dz)
                    flare = 1.0 + 0.25*max(0, z-1)
                    if (1.0*flare) < r < (1.9*flare) and z < g*0.65:
                        occ[x,y,z]=1.0
                    elif (0.9*flare) < r < (2.05*flare) and z < g*0.72:
                        occ[x,y,z]=0.45
        if g>=4:
            occ[:,:,0]=np.maximum(occ[:,:,0], 0.6)
    elif name=='Glass':
        inner = np.zeros((g,g,g), dtype=bool)
        if g>=3:
            inner[1:-1,1:-1,1:-1]=True
            inner[1:-1,1:-1,0]=False
            occ[:,:,:]=1.0
            occ[inner]=0.08
            if g>=4:
                occ[1,1,1]=0.12
                occ[g-2,g-2,1]=0.12
        else:
            occ[:,:,:]=1.0
    elif name=='Chime':
        occ[:,:,:]=0.0
        for x in range(g):
            for y in range(g):
                for z in range(g):
                    dy, dz = y-g/2+0.5, z-g/2+0.5
                    r = np.sqrt(dy*dy+dz*dz)
                    if 0.7 < r < 1.15:
                        occ[x,y,z]=1.0
                    elif 0.6 < r < 1.25:
                        occ[x,y,z]=0.4
    elif name=='Gong':
        occ[:,:,:]=0.0
        for x in range(g):
            for y in range(g):
                dx, dy = x-g/2+0.5, y-g/2+0.5
                rad = np.sqrt(dx*dx+dy*dy)
                if rad < g*0.48:
                    occ[x,y,g-1]=1.0
                    if g>=4:
                        occ[x,y,g-2]=0.55
                    if rad < g*0.22:
                        occ[x,y,g-1]=1.0
                        if g>=4:
                            occ[x,y,g-2]=0.9
                            occ[x,y,g-3]=0.25
                elif rad < g*0.52:
                    occ[x,y,g-1]=0.35
    elif name=='Handpan':
        occ[:,:,:]=0.0
        for x in range(g):
            for y in range(g):
                dx, dy = x-g/2+0.5, y-g/2+0.5
                rad = np.sqrt(dx*dx+dy*dy)
                if rad < 1.75:
                    occ[x,y,g-1]=1.0
                    if g>=4: occ[x,y,g-2]=0.55
                elif rad < 1.95:
                    occ[x,y,g-1]=0.5
                if rad < 0.85 and g>=4:
                    occ[x,y,g-3]=0.35
    elif name=='Cowbell':
        occ[:,:,:]=0.0
        for x in range(g):
            for y in range(g):
                for z in range(g):
                    m = max(abs(y-g/2+0.5), abs(z-g/2+0.5))
                    if m <= 1.15:
                        occ[x,y,z] = 1.0 if m >= 0.45 else 0.0
                    else:
                        occ[x,y,z] = 0.4
    nn = g+1
    ndof = nn*nn*nn*3
    K = np.zeros((ndof, ndof))
    M = np.zeros((ndof, ndof))
    def node_id(ix,iy,iz):
        return (iz*nn*nn + iy*nn + ix)
    for cx in range(g):
        for cy in range(g):
            for cz in range(g):
                w = occ[cx,cy,cz]
                if w<=1e-6:
                    continue
                Ke, Me = hex_element_matrices(E, nu, rho, h, h, h)
                Ke*=w; Me*=w
                nodes = [(cx,cy,cz),(cx+1,cy,cz),(cx,cy+1,cz),(cx+1,cy+1,cz),
                         (cx,cy,cz+1),(cx+1,cy,cz+1),(cx,cy+1,cz+1),(cx+1,cy+1,cz+1)]
                for a in range(8):
                    for b in range(8):
                        for da in range(3):
                            for db in range(3):
                                Ke_ab = Ke[da*8+a, db*8+b]
                                Me_ab = Me[da*8+a, db*8+b]
                                if abs(Ke_ab)<1e-12 and abs(Me_ab)<1e-12:
                                    continue
                                na = node_id(*nodes[a])
                                nb = node_id(*nodes[b])
                                ga = na*3+da
                                gb = nb*3+db
                                K[ga,gb]+=Ke_ab
                                M[ga,gb]+=Me_ab
    active = np.diag(M) > 1e-12
    idx = np.where(active)[0]
    K = K[np.ix_(idx,idx)]
    M = M[np.ix_(idx,idx)]
    return K, M, idx, occ, h, nn

def compute_modes(K, M, nmax=128):
    try:
        from scipy import linalg
        vals, vecs = linalg.eigh(K, M)
    except Exception as e:
        print("scipy eigh failed",e)
        vals, vecs = np.linalg.eig(np.linalg.solve(M, K))
        vals=np.real(vals); vecs=np.real(vecs)
        order=np.argsort(vals)
        vals=vals[order]; vecs=vecs[:,order]
    vals=np.maximum(vals,0)
    eps = 1e6
    nrigid = int(np.sum(vals < eps))
    if nrigid==0:
        nrigid=6
    if nrigid>len(vals)//2:
        nrigid=6
    print(f" dropping {nrigid} zero modes (vals < {eps:g}), total {len(vals)}")
    vals=vals[nrigid:]; vecs=vecs[:,nrigid:]
    take = min(nmax, len(vals))
    vals=vals[:take]; vecs=vecs[:,:take]
    freq = np.sqrt(vals)
    return freq, vecs, vals, nrigid

def compute_gains_direct(vecs, K_idx, g, h, nn, n_modes):
    sz=16
    gains=np.zeros((n_modes, sz, sz))
    f2a = {int(f):a for a,f in enumerate(K_idx)}
    def z_dof(ix,iy,iz):
        nid = (iz*nn*nn + iy*nn + ix)
        return nid*3 + 2
    # precompute active node ids for fast lookup (node id = dof//3)
    active_nodes = set(int(d)//3 for d in K_idx)
    def node_id2(ix,iy,iz):
        return (iz*nn*nn + iy*nn + ix)
    # global fallback: topmost active z over all nodes
    global_top = 0
    for nid in active_nodes:
        iz = nid // (nn*nn)
        if iz > global_top:
            global_top = iz
    for gx in range(sz):
        for gy in range(sz):
            fx = gx/(sz-1)
            fy = gy/(sz-1)
            x = fx * g * h
            y = fy * g * h
            ix_f = x/h
            iy_f = y/h
            ix0 = int(np.floor(ix_f)); iy0 = int(np.floor(iy_f))
            dx = ix_f - ix0; dy = iy_f - iy0
            ix0 = int(np.clip(ix0,0,nn-2)); iy0=int(np.clip(iy0,0,nn-2))
            ix1=ix0+1; iy1=iy0+1
            # find topmost ACTIVE node layer per column (search from top down)
            iz = global_top
            for cand in range(nn-1, -1, -1):
                if (node_id2(ix0,iy0,cand) in active_nodes or
                    node_id2(ix1,iy0,cand) in active_nodes or
                    node_id2(ix0,iy1,cand) in active_nodes or
                    node_id2(ix1,iy1,cand) in active_nodes):
                    iz = cand
                    break
            nodes = [(ix0,iy0,iz),(ix1,iy0,iz),(ix0,iy1,iz),(ix1,iy1,iz)]
            w = [(1-dx)*(1-dy), dx*(1-dy), (1-dx)*dy, dx*dy]
            for mode in range(n_modes):
                v=0.0
                for (ix,iy,iz2), wi in zip(nodes,w):
                    full = z_dof(ix,iy,iz2)
                    a = f2a.get(full, None)
                    if a is not None:
                        v += vecs[a, mode] * wi
                gains[mode, gy, gx] = v
    max_sum = 0
    for gy in range(sz):
        for gx in range(sz):
            max_sum = max(max_sum, np.sum(np.abs(gains[:,gy,gx])))
    if max_sum>1e-12:
        gains /= max_sum
    return gains

PRESETS = [
    {'name':'Bowl','E':69e9,'nu':0.33,'rho':2700,'alpha1':8,'alpha2':3e-7,'L':0.45},
    {'name':'WoodBlock','E':9e9,'nu':0.30,'rho':600,'alpha1':18,'alpha2':1.2e-6,'L':0.28},
    {'name':'Plate','E':200e9,'nu':0.30,'rho':7850,'alpha1':4,'alpha2':1.5e-7,'L':0.65},
    {'name':'Squirrel','E':12e9,'nu':0.30,'rho':400,'alpha1':22,'alpha2':2e-6,'L':0.32},
    {'name':'Blade','E':69e9,'nu':0.33,'rho':2700,'alpha1':6,'alpha2':2.5e-7,'L':0.50},
    {'name':'Shell','E':110e9,'nu':0.34,'rho':8500,'alpha1':5,'alpha2':2e-7,'L':0.48},
    {'name':'Bar','E':200e9,'nu':0.30,'rho':7850,'alpha1':3,'alpha2':1e-7,'L':0.70},
    {'name':'Membrane','E':2e9,'nu':0.40,'rho':1100,'alpha1':30,'alpha2':5e-6,'L':0.60},
    {'name':'Bell','E':105e9,'nu':0.34,'rho':8800,'alpha1':2.2,'alpha2':9e-8,'L':0.52},
    {'name':'Glass','E':72e9,'nu':0.23,'rho':2500,'alpha1':2.8,'alpha2':7e-8,'L':0.36},
    {'name':'Chime','E':200e9,'nu':0.30,'rho':7850,'alpha1':1.8,'alpha2':6e-8,'L':0.78},
    {'name':'Gong','E':110e9,'nu':0.33,'rho':8600,'alpha1':4.5,'alpha2':2e-7,'L':0.68},
    {'name':'Handpan','E':200e9,'nu':0.30,'rho':7850,'alpha1':3.0,'alpha2':1.0e-7,'L':2.30},
    {'name':'LogDrum','E':9e9,'nu':0.32,'rho':650,'alpha1':9,'alpha2':1.4e-6,'L':0.95},
    {'name':'Marimba','E':14e9,'nu':0.30,'rho':850,'alpha1':9,'alpha2':9e-7,'L':0.93},
    {'name':'Cowbell','E':105e9,'nu':0.34,'rho':8700,'alpha1':4.5,'alpha2':5e-7,'L':0.62},
    {'name':'Kalimba','E':200e9,'nu':0.30,'rho':7850,'alpha1':5,'alpha2':3e-6,'L':0.43},
    {'name':'Celesta','E':200e9,'nu':0.30,'rho':7850,'alpha1':1.4,'alpha2':5e-8,'L':0.29},
]

def bake_one(preset, g=4, nmax=128):
    K, M, K_idx, occ, h, nn = build_grid(g, preset)
    freq, vecs, vals, nrigid = compute_modes(K, M, nmax)
    freq = freq * 0.18
    inharm = {'Bowl':1.02,'WoodBlock':1.08,'Plate':1.18,'Squirrel':1.10,'Blade':1.22,'Shell':1.06,'Bar':1.01,'Membrane':1.15,'Bell':1.04,'Glass':1.12,'Chime':1.015,'Gong':1.09,
              'Handpan':1.03,'LogDrum':1.07,'Marimba':1.05,'Cowbell':1.13,'Kalimba':1.16,'Celesta':1.008}.get(preset['name'],1.0)
    for i in range(len(freq)):
        stretch = 1.0 + (inharm-1.0) * (i/max(1,len(freq)-1)) * 1.5
        freq[i] *= stretch
    n = len(freq)
    gains = compute_gains_direct(vecs, K_idx, g, h, nn, n)
    gains = gains * kGainTrim
    max_abs = float(np.max(np.abs(gains))) if gains.size else 0.0
    max_sum_debug = float(np.max([np.sum(np.abs(gains[:,y,x])) for y in range(16) for x in range(16)])) if gains.size else 0.0
    print(f"  gains max|g|={max_abs:.5f} max_sum={max_sum_debug:.5f}")
    if max_abs < 1e-6:
        raise RuntimeError(f"Silent body {preset['name']}: max|gains|={max_abs:g} — top layer sampling bug (no active nodes at iz=g)")
    alpha1, alpha2 = preset['alpha1'], preset['alpha2']
    decays = 0.5*(alpha1 + alpha2 * (freq**2))
    decays = np.maximum(decays, 0.6)
    if preset['name']=='Plate':
        decays *= (0.7 + 0.3 * (1.0 - np.linspace(0,1,len(decays))))
    elif preset['name']=='WoodBlock':
        decays *= (1.0 + 0.6 * np.linspace(0,1,len(decays)))
    elif preset['name']=='Membrane':
        decays *= (1.0 + 0.4 * np.linspace(0,1,len(decays)))
    elif preset['name']=='Blade':
        decays *= (0.8 + 0.2 * (1.0 - np.linspace(0,1,len(decays))))
    elif preset['name']=='Bell':
        decays *= (0.65 + 0.35 * (1.0 - np.linspace(0,1,len(decays))**0.7))
    elif preset['name']=='Glass':
        decays *= (0.55 + 0.45 * (1.0 - np.linspace(0,1,len(decays))**0.8))
    elif preset['name']=='Chime':
        decays *= (0.5 + 0.5 * (1.0 - np.linspace(0,1,len(decays))))
    elif preset['name']=='Gong':
        decays *= (0.72 + 0.28 * (1.0 - np.linspace(0,1,len(decays))))
    elif preset['name']=='Handpan':
        decays *= (0.72 + 0.28 * (1.0 - np.linspace(0,1,len(decays))))
    elif preset['name']=='LogDrum':
        decays *= (1.08 - 0.38 * np.linspace(0,1,len(decays)))
    elif preset['name']=='Marimba':
        decays *= (1.15 - 0.65 * np.linspace(0,1,len(decays))**0.8)
    elif preset['name']=='Kalimba':
        decays *= (1.0 - 0.3 * np.linspace(0,1,len(decays)))
    elif preset['name']=='Celesta':
        decays *= (0.6 + 0.4 * (1.0 - np.linspace(0,1,len(decays))))
    return {'freq':freq, 'decays':decays, 'gains':gains, 'n':n, 'nrigid':nrigid}

def emit_header(results, out_path):
    with open(out_path,'w') as f:
        f.write("#pragma once\n#include <array>\nnamespace modal {\n")
        f.write(f"inline constexpr int kNumPresets = {len(results)};\ninline constexpr int kMaxModes = 128;\ninline constexpr int kGainGrid = 16;\n")
        f.write("struct PresetData {\n const char* name;\n int n;\n float freq[128];\n float decay[128];\n float gain[128][16][16];\n};\n")
        f.write("inline constexpr PresetData kPresets[kNumPresets] = {\n")
        for r, preset in zip(results, PRESETS):
            f.write(f"  {{\n   \"{preset['name']}\",\n   {r['n']},\n")
            f.write("   {")
            for i in range(128):
                v = r['freq'][i] if i<r['n'] else 0.0
                f.write(f"{float(v):.6f}f")
                if i!=127: f.write(",")
                if i%8==7: f.write("\n    ")
            f.write("},\n")
            f.write("   {")
            for i in range(128):
                v = r['decays'][i] if i<r['n'] else 0.5
                f.write(f"{float(v):.6f}f")
                if i!=127: f.write(",")
                if i%8==7: f.write("\n    ")
            f.write("},\n")
            f.write("   {\n")
            for m in range(128):
                f.write("    {")
                for y in range(16):
                    f.write("{")
                    for x in range(16):
                        v = r['gains'][m,y,x] if m<r['n'] else 0.0
                        f.write(f"{float(v):.6f}f")
                        if x!=15: f.write(",")
                    f.write("}")
                    if y!=15: f.write(",")
                f.write("}")
                if m!=127: f.write(",")
                f.write("\n")
            f.write("   },\n")
            f.write("  },\n")
        f.write("};\n}\n")
        print(f"Wrote {out_path}")

# ================= paper-faithful multi-resolution reduction =================
# Paper-47 (DAFx-09) cites Nesme, Payan & Faure, "Animating shapes at
# arbitrary resolution with non-uniform stiffness" (VRIPHYS 2006) but omits
# the coarse-cell weighting rule. That paper's sec. 5.3 supplies it exactly:
#
#     K_parent = sum_c L_c^T K_c L_c        (and likewise for M)
#
# where u_child = L_c u_parent is the trilinear constraint that pins each child
# node to the parent's midpoints, and forces pop up through the transpose. This
# is a Galerkin projection of the child energy onto the parent basis -- NOT a
# scalar fill-fraction average. The eight 8x8 L_c are reconstructed below from
# the trilinear hat basis (the paper's Appendix A, in closed form).

def build_L_matrices():
    mats = []
    for c in range(8):                    # c = 4*cz + 2*cy + cx (node order)
        cz = (c >> 2) & 1
        cy = (c >> 1) & 1
        cx = c & 1
        L = np.zeros((8, 8))
        for j in range(8):                # child corner (local bits)
            bx = j & 1; by = (j >> 1) & 1; bz = (j >> 2) & 1
            px = -1.0 + (bx + cx)         # corner position in parent natural coords
            py = -1.0 + (by + cy)
            pz = -1.0 + (bz + cz)
            for i in range(8):            # parent corner (global order)
                nx = 2.0 * (i & 1) - 1.0
                ny = 2.0 * ((i >> 1) & 1) - 1.0
                nz = 2.0 * ((i >> 2) & 1) - 1.0
                L[j, i] = ((1.0 + nx * px) * 0.5) * ((1.0 + ny * py) * 0.5) * ((1.0 + nz * pz) * 0.5)
        mats.append(L)
    return mats

def _dof_L(L):
    Lam = np.zeros((24, 24))
    for d in range(3):
        Lam[d * 8:(d + 1) * 8, d * 8:(d + 1) * 8] = L
    return Lam

def assemble_reduced(fine_g, coarse_g, preset):
    """Galerkin-reduce a fine_g^3 grid to coarse_g^3 (fine_g == 2*coarse_g)."""
    if fine_g != 2 * coarse_g:
        raise ValueError("fine_g must be 2*coarse_g for one reduction level")
    E, nu, rho = preset['E'], preset['nu'], preset['rho']
    Lphys = preset.get('L', 0.4)
    hf = Lphys / fine_g
    # fine occupancy is the shape source (the hand-authored grids in build_occ
    # stand in for the paper's voxelized surface mesh)
    occf = build_grid(fine_g, preset)[3]
    Ld = [_dof_L(L) for L in build_L_matrices()]
    nn = coarse_g + 1
    ndof = nn * nn * nn * 3
    K = np.zeros((ndof, ndof)); M = np.zeros((ndof, ndof))
    def node_id(ix, iy, iz):
        return iz * nn * nn + iy * nn + ix
    for cx in range(coarse_g):
        for cy in range(coarse_g):
            for cz in range(coarse_g):
                Kp = np.zeros((24, 24)); Mp = np.zeros((24, 24))
                used = False
                for c in range(8):
                    fx = 2 * cx + (c & 1)
                    fy = 2 * cy + ((c >> 1) & 1)
                    fz = 2 * cz + ((c >> 2) & 1)
                    w = occf[fx, fy, fz]
                    if w <= 1e-6:
                        continue
                    Ke, Me = hex_element_matrices(E, nu, rho, hf, hf, hf)
                    Ke *= w; Me *= w
                    Lam = Ld[c]
                    Kp += Lam.T @ Ke @ Lam
                    Mp += Lam.T @ Me @ Lam
                    used = True
                if not used:
                    continue
                nodes = [(cx, cy, cz), (cx + 1, cy, cz), (cx, cy + 1, cz), (cx + 1, cy + 1, cz),
                         (cx, cy, cz + 1), (cx + 1, cy, cz + 1), (cx, cy + 1, cz + 1), (cx + 1, cy + 1, cz + 1)]
                for a in range(8):
                    for b in range(8):
                        for da in range(3):
                            for db in range(3):
                                ga = node_id(*nodes[a]) * 3 + da
                                gb = node_id(*nodes[b]) * 3 + db
                                K[ga, gb] += Kp[da * 8 + a, db * 8 + b]
                                M[ga, gb] += Mp[da * 8 + a, db * 8 + b]
    active = np.diag(M) > 1e-12
    idx = np.where(active)[0]
    return K[np.ix_(idx, idx)], M[np.ix_(idx, idx)], idx

def _raw_freqs(K, M, nmax=128):
    freq, _, _, _ = compute_modes(K, M, nmax)
    return np.asarray(freq, dtype=float)

def count_rigid(K, M, tol_rel=1e-10):
    """Count free-free rigid-body (near-zero) eigenvalues. A correct free-free
    hex FEM system has EXACTLY 6. Scale-relative so the split is independent of
    absolute units and robust to solver noise on the zero cluster."""
    from scipy import linalg
    vals = np.maximum(linalg.eigvalsh(K, M), 0.0)
    if not len(vals):
        return 0, vals
    return int(np.sum(vals < tol_rel * vals[-1])), vals

VERIFY_NAMES = ['Bowl', 'Plate', 'Bell', 'Chime']  # solid / shell / bell / thin

def _relerr(a, b):
    n = min(len(a), len(b))
    return float(np.mean(np.abs(a[:n] - b[:n]) / np.maximum(b[:n], 1e-12)))

def verify_paper_method():
    """Paper-fidelity checks for the real-hex-FEM pipeline.

    GATED (must all pass):
      - L_c operator: partition of unity (Nesme 2006 sec 5.3, App. A)
      - single element: PSD and EXACTLY 6 rigid-body zero modes
      - reduced K symmetric
      - every sampled body: exactly 6 rigid modes, both on the direct 8^3 grid
        and on the L_c-reduced 4^3 grid
      - direct-bake convergence (paper Fig. 6): the 4^3-vs-8^3 pair agrees more
        closely than the 2^3-vs-4^3 pair, i.e. frequencies stabilize as the
        mesh refines
    """
    ok = True
    # (1) L_c operator: a uniform parent displacement (translation) must pass
    # through every L_c unchanged: L_c @ ones == ones, i.e. every ROW of L_c
    # sums to 1. This is the orientation that makes K_parent = L_c^T K_c L_c
    # preserve the rigid-body null space.
    for c, L in enumerate(build_L_matrices()):
        err = float(np.abs(L.sum(axis=1) - 1.0).max())
        if err > 1e-12:
            print(f"L operator FAIL: L_{c} row-sum err {err:.3e} (translation not preserved)")
            ok = False
    if ok:
        print("L_c operator: 8 matrices, translation preserved OK (L_c @ ones == ones)")
    # (2) single element: PSD + exactly 6 rigid-body zero modes.
    Ke, _ = hex_element_matrices(1.0, 0.3, 1000.0, 1.0, 1.0, 1.0)
    ew = np.linalg.eigvalsh(Ke)
    ke_zero = int(np.sum(ew < 1e-10 * ew[-1]))
    ke_psd = bool(ew[0] >= -1e-9 * ew[-1])
    if ke_zero != 6 or not ke_psd:
        print(f"element FAIL: zero modes={ke_zero} (expect 6), PSD={ke_psd}")
        ok = False
    else:
        print(f"element: PSD, exactly {ke_zero} rigid modes (was 2 + indefinite)")
    # (3) per-body rigid modes + direct-bake convergence.
    print(f"{'body':<8} {'rigid8':>7} {'rigid4r':>8} {'2v4':>7} {'4v8':>7} {'conv':>5}")
    c24, c48 = [], []
    for p in PRESETS:
        if p['name'] not in VERIFY_NAMES:
            continue
        K8, M8, _, _, _, _ = build_grid(8, p)
        n8, _ = count_rigid(K8, M8)
        K4r, M4r, _ = assemble_reduced(8, 4, p)
        n4r, _ = count_rigid(K4r, M4r)
        if n8 != 6 or n4r != 6:
            print(f"  {p['name']}: rigid modes 8^3={n8} reduced-4^3={n4r} (expect 6)")
            ok = False
        sym = float(np.abs(K4r - K4r.T).max() / max(1.0, float(np.abs(K4r).max())))
        if sym > 1e-9:
            print(f"  {p['name']}: reduced K not symmetric ({sym:.2e})")
            ok = False
        f2 = _raw_freqs(*build_grid(2, p)[:2])
        f4 = _raw_freqs(*build_grid(4, p)[:2])
        f8 = _raw_freqs(K8, M8)
        d24, d48 = _relerr(f2, f4), _relerr(f4, f8)
        c24.append(d24); c48.append(d48)
        print(f"{p['name']:<8} {n8:>7} {n4r:>8} {d24:>7.3f} {d48:>7.3f} {'YES' if d48 < d24 else 'no':>5}")
    if c24:
        m24, m48 = float(np.median(c24)), float(np.median(c48))
        conv = m48 < m24
        print(f"median direct-bake convergence: 2^3-vs-4^3 {m24:.3f}  4^3-vs-8^3 {m48:.3f}  -> {'CONVERGING' if conv else 'NOT converging'}")
        if not conv:
            print("convergence FAIL: refined pair does not agree more closely")
            ok = False
    return ok

if __name__=="__main__":
    ap=argparse.ArgumentParser()
    ap.add_argument("-o","--out", default="plugins/MultiScaleBody/src/ModalData.hpp")
    ap.add_argument("-g","--grid", type=int, default=4)
    ap.add_argument("--verify", action="store_true", help="run paper-method checks and exit")
    args=ap.parse_args()
    if args.verify:
        sys.exit(0 if verify_paper_method() else 1)
    results=[]
    for p in PRESETS:
        print(f"Baking {p['name']} g={args.grid} ...")
        r=bake_one(p, g=args.grid)
        print(f"  modes={r['n']} f0={r['freq'][0]/(2*np.pi):.1f}Hz fmax={r['freq'][-1]/(2*np.pi):.1f}Hz rigid={r['nrigid']}")
        results.append(r)
    out = args.out
    if not os.path.isabs(out):
        base = os.path.join(os.path.dirname(__file__), "..")
        out = os.path.normpath(os.path.join(base, out))
    emit_header(results, out)
