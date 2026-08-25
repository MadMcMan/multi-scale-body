#!/usr/bin/env python3
import numpy as np
import argparse, os

def hex_element_matrices(E, nu, rho, hx, hy, hz):
    vol = hx*hy*hz
    edges = [(0,1),(0,2),(0,4),(1,3),(1,5),(2,3),(2,6),(3,7),(4,5),(4,6),(5,7),(6,7)]
    Kscalar = np.zeros((8,8))
    for a,b in edges:
        Kscalar[a,a]+=1; Kscalar[b,b]+=1
        Kscalar[a,b]-=1; Kscalar[b,a]-=1
    K0 = np.zeros((24,24))
    for d in range(3):
        for i in range(8):
            for j in range(8):
                K0[d*8+i, d*8+j] = Kscalar[i,j]
    for i in range(8):
        for d1 in range(3):
            for d2 in range(3):
                if d1!=d2:
                    K0[d1*8+i, d2*8+i] += 0.15
    h = (hx*hy*hz)**(1/3)
    Ke = K0 * (E * h * 0.08)
    Me = np.zeros((24,24))
    m_node = rho * vol / 8.0
    for i in range(8):
        for d in range(3):
            Me[d*8+i, d*8+i] = m_node / 3.0
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

if __name__=="__main__":
    ap=argparse.ArgumentParser()
    ap.add_argument("-o","--out", default="plugins/MultiScaleBody/src/ModalData.hpp")
    ap.add_argument("-g","--grid", type=int, default=4)
    args=ap.parse_args()
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
