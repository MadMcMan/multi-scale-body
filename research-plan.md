# Robust & Multi-Scale Modal Analysis — Mathematics Research Plan
Source: *A Robust and Multi-Scale Modal Analysis for Sound Synthesis* (2009) — Cécile Picard, François Faure, Paul G. Kry, George Drettakis (INRIA / McGill; DAFx-09, Como, paper 47)
Folder: ranked/Synthesis & Sound Generation/0004-Multi-Scale Body

## 1. Core mathematical object

A linear modal model of struck-object sound: an $n$-DOF elastic continuum with mass matrix $\mathbf{M}$ and stiffness matrix $\mathbf{K}$ assembled by finite elements, whose damped response decouples (under Rayleigh damping) into $n$ independent single-DOF oscillators. Each mode contributes one damped sinusoid: $s(t) = \sum_i a_i \sin(\omega_i t)\,e^{-d_i t}$. The central object is the generalized eigenpair set $(\lambda_i, \mathbf{x}_i)$ of $(\mathbf{K},\mathbf{M})$; from it come frequencies $\omega_i$, decay rates $d_i$, and location-dependent gains $a_i$ (the *Sound Map*). The paper's contribution is the *mesh pipeline* producing $\mathbf{K},\mathbf{M}$ robustly: automatic voxelization of a surface model into a sparse regular grid, recursive fine-to-coarse merging into hexahedral elements, each coarse cell's mass/stiffness a material-weighted average of its eight children. This handles non-manifold, thin-featured geometry where tetrahedralization fails, and gives multi-scale control of modal data volume.

## 2. Full equation inventory (from the PDF)

**Modal synthesis equation (Eq. 1).**
$$s(t) = \sum_{i=1}^{n} a_i \sin(\omega_i t)\, e^{-d_i t} \tag{1}$$
$s(t)$: emitted sound (arbitrary units); $n$: retained modes; $\omega_i$: natural angular frequency of mode $i$ (rad/s); $d_i$: decay rate (1/s); $a_i$: gain of mode $i$ at the strike point — the Sound Map. Excitation is a Dirac ("such as a regular impact"); $a_i = 0$ when the strike point is a vibration node of mode $i$.

**Continuum dynamics (standard; paper states it in words [RECONSTRUCTED form]).**
$$\mathbf{M}\ddot{\mathbf{u}} + \mathbf{C}\dot{\mathbf{u}} + \mathbf{K}\mathbf{u} = \mathbf{f}(t)$$
$\mathbf{M},\mathbf{C},\mathbf{K} \in \mathbb{R}^{n\times n}$: mass, damping, stiffness matrices, "assembled... by summing the contribution of each cell"; $\mathbf{u}(t)$: DOF displacements; $\mathbf{f}(t)$: nodal forces (Dirac impacts).

**Rayleigh damping (text, per [9] Bathe).**
$$\mathbf{C} = \alpha_1 \mathbf{M} + \alpha_2 \mathbf{K}$$
$\alpha_1$ (mass damping, 1/s), $\alpha_2$ (stiffness damping, s). This assumption decouples the damped system. Damping is uniform; contact-position-dependent damping and changing boundary constraints are excluded.

**Generalized eigenproblem (Algorithm 1: "solve the eigenproblem"; standard form [RECONSTRUCTED]).**
$$\mathbf{K}\mathbf{x}_i = \lambda_i \mathbf{M}\mathbf{x}_i, \qquad i = 1,\dots,n$$
$\lambda_i = \omega_i^2$; $\mathbf{x}_i$: mode shape. Each free body has **six zero eigenvalues** (rigid-body freedoms), dropped (bowl: 822 → 816 tetrahedral modes; 81 → 75 hexahedral). Frequencies shift with grid resolution, so $\lambda_i$ depends on the FEM grid.

**Fine-to-coarse element averaging (text; exact weights not given).** Coarse cell $c$ from children $e_1,\dots,e_8$:
$$\mathbf{K}_c = \sum_{e \in \mathrm{children}(c)} w_e \mathbf{K}_e, \qquad \mathbf{M}_c = \sum_{e \in \mathrm{children}(c)} w_e \mathbf{M}_e, \qquad \textstyle\sum w_e = 1 \quad \text{[RECONSTRUCTED form]}$$
$w_e$: material-distribution weights (fill fraction / density per child). Fine grid is one level finer than coarse in all experiments (bowl 4³→2³; squirrel 8³→4³; blades 14³→7³).

**Surface embedding (text).** Surface points do not coincide with mechanical DOFs; motion comes from **trilinear interpolation** of mode shapes at cell-local coordinates $(\xi,\eta,\zeta) \in [0,1]^3$:
$$\mathbf{u}(\xi,\eta,\zeta) = \sum_{i,j,k \in \{0,1\}} N_i(\xi)N_j(\eta)N_k(\zeta)\, \mathbf{u}_{ijk}, \quad N_0(t)=1-t,\; N_1(t)=t \quad \text{[RECONSTRUCTED standard form]}$$
This defines the Sound Map gains $a_i$.

**Rendering:** per-mode **reson filter** (van den Doel et al. [6]); no radiation properties considered. (Algorithm 1 of the paper: compute mass/stiffness at the mechanical level, assemble, solve the eigenproblem, store eigenpairs — expanded in Section 3.)

## 3. Algorithm in mathematical form

Input: triangle mesh, material $(E,\nu,\rho)$, Rayleigh damping $(\alpha_1,\alpha_2)$, coarse grid $g^3$, fine grid $f^3$ ($f = g{+}1$ in all experiments).

1. **Voxelize**: embed the surface in a sparse regular grid at fine resolution $f^3$; mark boundary voxels; fill the interior for solid objects; thin/surface parts occupy voxels as-is.
2. **Merge**: recursively merge voxels to coarse resolution $g^3$; each coarse cell's $\mathbf{K}_c,\mathbf{M}_c$ = weighted average of its 8 children's, weights from material distribution.
3. **Assemble**: $\mathbf{K} = \sum_c \mathbf{K}_c$, $\mathbf{M} = \sum_c \mathbf{M}_c$ (trilinear hexahedral elements — element matrices not given in the paper).
4. **Eigen-solve**: partial generalized eigendecomposition $\mathbf{K}\mathbf{x}_i = \lambda_i\mathbf{M}\mathbf{x}_i$; drop the 6 rigid-body modes per free body; order by $\lambda_i$.
5. **Sound Map**: for each impact point, $a_i$ = trilinearly interpolated mode-shape component; render Eq. 1 via reson filters with $\omega_i = \sqrt{\lambda_i}$ and $d_i$ from Rayleigh damping.

Convergence/stability: empirical only. Frequency content converges by 4×4×4 (squirrel, Fig. 6); higher resolution raises the frequency range (extra DOFs) and shifts frequencies; 2×2×2 is "extremely coarse" and flattens Sound Map variation.

## 4. Data & details from the paper

**Metal bowl** (274 vertices; impacts at points 30 top, 40 side, 52 bottom). Aluminum: $E = 69\times10^9$ Pa, $\nu = 0.33$, $\rho = 2700$ kg/m³; Rayleigh $\alpha_2 = 3\times10^{-7}$ s, $\alpha_1 = 10$ s⁻¹. Tetrahedral baseline: Tetgen, 2426 tetrahedra, 822 modes (816 after dropping 6 rigid-body), **5 minutes** on a 2.33 GHz Intel Core Duo, 2 GB. Hexahedral: coarse 2×2×2 (fine 4×4×4), 81 modes → 75 used; audibly similar with ~10× fewer modes; normalized power spectra at the 3 impact points compare well.

**Squirrel** (999 vertices; impacts at points 29, 287, 986). Pine: $E = 12\times10^9$ Pa, $\nu = 0.3$, $\rho = 750$ kg/m³; Rayleigh $\alpha_2 = 8\times10^{-6}$ s, $\alpha_1 = 50$ s⁻¹. Coarse grids 2³, 3³, 4³, 8³, 9³, fine one level up; convergence by 4×4×4; resolution raises frequency range and shifts frequencies.

**Squirrel with thin blades** (tetrahedralization-hostile): coarse 7³, fine 14³; impacts at points 100, 135, 146, 200, 400; Sound Map preserved — mode amplitudes vary with location; hitting the lightweight wings emphasizes higher frequencies.

**Cost/memory (Table 1, unoptimized, 2.33 GHz Core Duo):** 7³: 115.11 s / 11.309 MB; 6³: 39.14 s / 5.698 MB; 5³: 12.98 s / 2.663 MB; 4³: 5.35 s / 1.042 MB.

**Evaluation:** no numerical error metric — spectral comparison (power spectra normalized by max amplitude) and listening. Limitation: low FEM resolution reduces Sound Map expressiveness; improvement suggested via better coarse-element mass/stiffness approximation [14].

## 5. What must still be researched/derived

1. **Hexahedral element matrices.** The paper omits the trilinear hex element formulations; derive from standard FEM: $\mathbf{K}_e = \int_{V_e}\mathbf{B}^\top\mathbf{D}\mathbf{B}\,dV$ with $\mathbf{B}$ the strain–displacement matrix, $\mathbf{D}$ the isotropic Hooke tensor in $(E,\nu)$; closed forms for the 8-node hexahedron with material averaging.
2. **Weighting rule for coarse cells.** The "weighted average based on material distribution" is unspecified: derive exact $w_e$ (mass-proportional vs. stiffness-proportional vs. volume-fraction) and analyze which preserves low modes and the Sound Map; compare with Nesme et al. [14].
3. **Convergence theory.** The 4³ result is empirical: a priori FEM error in $\omega_i$, $d_i$ vs. cell size $h$; eigenvalue interlacing bounds under grid refinement.
4. **Frequency-shift law.** Quantify the systematic frequency shift vs. resolution — stiffening/softening of the coarse model; can it be calibrated against analytic plate/shell modes?
5. **Damping decoupling validity.** Rayleigh damping decouples exactly only when proportional; quantify residual modal cross-coupling after coarse-element averaging, and error from uniform damping for mixed materials.
6. **Sound Map conditioning.** Sensitivity of $a_i$ to strike location (nodes → spectral nulls), to grid resolution (why 2³ flattens variation), and ill-conditioning near element boundaries (trilinear continuity).
7. **Complexity analysis.** Cost model: voxelization $O(f^3)$, assembly $O(g^3)$, partial eigendecomposition vs. DOF count and retained modes (Lanczos/ARPACK); memory scaling matching Table 1.
8. **Rigid-body mode removal.** Deflation strategy, tolerance for $\lambda_i \approx 0$, effect of inexact rigid-body modes on low-frequency accuracy.

## 6. Literature needed

- Paper's references: O'Brien, Shen, Gatchalian, "Synthesizing sounds from rigid-body simulations," SCA'02 [1] (the modal pipeline followed); Adrien, "The missing link: modal synthesis," 1991 [2]; Cook, *Real Sound Synthesis for Interactive Applications*, 2002 [3]; van den Doel, Kry, Pai, "Foley automatic," SIGGRAPH'01 [6] (reson-filter rendering); Maxwell & Bindel, DAFx'07 [7]; Bonneel et al., "Fast modal sounds," SIGGRAPH'08 [8]; Bathe, *Finite Element Procedures in Engineering Analysis*, 1982 [9] (Rayleigh damping, FEM); Nesme, Payan, Faure, VRIPHYS'06 [10] (voxel embedding); O'Brien, Cook, Essl, SIGGRAPH'01 [11]; James, Barbic, Pai, "Precomputed acoustic transfer," TOG 25(3) [12]; Nesme, Kry, Jeřábková, Faure, SIGGRAPH'09 [14]; van den Doel & Pai, ICAD'96 [15].
- Canonical: Bathe (above); Zienkiewicz & Taylor, *The Finite Element Method* (hexahedral elements, eigenproblems); Parlett, *The Symmetric Eigenvalue Problem*; Strang & Fix, *An Analysis of the Finite Element Method* (eigenvalue error bounds); modal synthesis theory (van den Doel & Pai; O'Brien et al.).

## 7. Math verification targets

- **Mode-count invariant**: exactly 6 rigid-body modes per free body; after deflation all retained $\lambda_i > 0$ — assert the gap between the 6 smallest ($\approx 0$) and the 7th.
- **Spectrum consistency**: at fixed grid, eigen-solve frequencies $\omega_i$ coincide with spectral peaks of the synthesized impulse response; each measured decay envelope matches $e^{-d_i t}$.
- **Sound Map null test**: striking at a modal node of mode $i$ suppresses it — spectral null at $\omega_i$ in the rendered response; node located from the trilinear-interpolated mode shape.
- **Grid convergence**: squirrel frequency lists at 4³ vs 8³ vs 9³ — relative error in shared $\omega_i$ shrinks with resolution (reproduces Fig. 6).
- **Material scaling**: frequencies scale as $\sqrt{E/\rho}$ under isotropic rescaling (dimensional analysis test with two materials or two sizes).
- **Element-assembly consistency**: uniform-density cube must reproduce analytic first free-block modes within FEM discretization error; rigid-body eigenvalues machine-zero.
- **Rayleigh identity**: single-mode damped frequency $= \omega_i\sqrt{1-\zeta_i^2}$ with $\zeta_i = d_i/\omega_i$ [RECONSTRUCTED standard] — reson filter must reproduce Eq. 1's envelope and phase.
- **Positivity**: $\mathbf{M}$ symmetric positive definite, $\mathbf{K}$ positive semi-definite (6 null directions) for every voxelization, including thin/non-manifold inputs — check the assembled matrices' eigenvalues, not just the generalized problem.
