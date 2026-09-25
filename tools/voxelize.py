"""Automatic surface-mesh voxelization used by the modal-body baking tools.

The implementation follows section 3.1 of the Multi-Scale Body paper: a mesh
is uniformly scaled into a padded voxel grid, every triangle is rasterized as
boundary voxels, and a flood fill from the grid border labels the solid
interior.  Only NumPy is required; the small procedural meshes make the
module useful as a self-contained smoke test without external assets.
"""

from __future__ import annotations

from collections import deque
import math
import operator

import numpy as np


_EPS = 1.0e-10


def _as_mesh(verts, faces):
    """Validate and normalize input array types for the rasterizer."""
    vertices = np.asarray(verts, dtype=np.float64)
    triangles = np.asarray(faces, dtype=np.int64)
    if vertices.ndim != 2 or vertices.shape[1] != 3 or len(vertices) == 0:
        raise ValueError("verts must have shape (N, 3) with N > 0")
    if triangles.ndim != 2 or triangles.shape[1] != 3 or len(triangles) == 0:
        raise ValueError("faces must have shape (M, 3) with M > 0")
    if not np.isfinite(vertices).all():
        raise ValueError("verts must contain only finite values")
    if triangles.min() < 0 or triangles.max() >= len(vertices):
        raise ValueError("faces contain an out-of-range vertex index")
    return vertices, triangles


def _triangle_box_overlap(triangle, centers):
    """Return a mask saying which voxel boxes overlap one triangle.

    A voxel at integer ``(x, y, z)`` is represented by the axis-aligned cube
    centered at that coordinate with side length one.  The separating-axis
    test uses the three box axes, the triangle normal, and the nine edge/box
    cross products.  Touching is included, which conservatively preserves
    very thin triangles on the grid boundary.
    """
    a, b, c = triangle
    edges = (b - a, c - b, a - c)
    normal = np.cross(edges[0], -edges[1])
    normal_length = float(np.linalg.norm(normal))
    if normal_length <= _EPS:
        return np.zeros(len(centers), dtype=bool)

    axes = [
        np.array((1.0, 0.0, 0.0)),
        np.array((0.0, 1.0, 0.0)),
        np.array((0.0, 0.0, 1.0)),
        normal / normal_length,
    ]
    for edge in edges:
        for box_axis in axes[:3]:
            axis = np.cross(edge, box_axis)
            length = float(np.linalg.norm(axis))
            if length > _EPS:
                axes.append(axis / length)

    overlap = np.ones(len(centers), dtype=bool)
    half_width = 0.5
    for axis in axes:
        tri_projection = triangle @ axis
        tri_low = float(tri_projection.min())
        tri_high = float(tri_projection.max())
        radius = half_width * float(np.abs(axis).sum())
        projected = centers @ axis
        overlap &= (projected + radius >= tri_low - _EPS) & (
            projected - radius <= tri_high + _EPS
        )
        if not overlap.any():
            break
    return overlap


def _box_mesh(lo, hi):
    """Return the eight vertices and twelve triangles of an axis-aligned box."""
    x0, y0, z0 = lo
    x1, y1, z1 = hi
    vertices = np.array(
        [
            (x0, y0, z0),
            (x1, y0, z0),
            (x1, y1, z0),
            (x0, y1, z0),
            (x0, y0, z1),
            (x1, y0, z1),
            (x1, y1, z1),
            (x0, y1, z1),
        ],
        dtype=np.float32,
    )
    faces = np.array(
        [
            (0, 2, 1),
            (0, 3, 2),  # bottom
            (4, 5, 6),
            (4, 6, 7),  # top
            (0, 1, 5),
            (0, 5, 4),  # y = y0
            (1, 2, 6),
            (1, 6, 5),  # x = x1
            (2, 3, 7),
            (2, 7, 6),  # y = y1
            (3, 0, 4),
            (3, 4, 7),  # x = x0
        ],
        dtype=np.int32,
    )
    return vertices, faces


def make_sphere(subdiv=3):
    """Create a deterministic icosphere centered at the origin.

    ``subdiv`` is the number of recursive four-way subdivisions.  The default
    (3) gives 1,280 triangles, enough to exercise robust triangle rasterizing
    while remaining inexpensive for the coarse modal grid.
    """
    try:
        levels = operator.index(subdiv)
    except TypeError as exc:
        raise TypeError("subdiv must be an integer") from exc
    if levels < 0:
        raise ValueError("subdiv must be non-negative")

    golden = (1.0 + math.sqrt(5.0)) / 2.0
    vertices = np.array(
        [
            (-1.0, golden, 0.0),
            (1.0, golden, 0.0),
            (-1.0, -golden, 0.0),
            (1.0, -golden, 0.0),
            (0.0, -1.0, golden),
            (0.0, 1.0, golden),
            (0.0, -1.0, -golden),
            (0.0, 1.0, -golden),
            (golden, 0.0, -1.0),
            (golden, 0.0, 1.0),
            (-golden, 0.0, -1.0),
            (-golden, 0.0, 1.0),
        ],
        dtype=np.float64,
    )
    vertices /= np.linalg.norm(vertices, axis=1)[:, None]
    faces = np.array(
        [
            (0, 11, 5),
            (0, 5, 1),
            (0, 1, 7),
            (0, 7, 10),
            (0, 10, 11),
            (1, 5, 9),
            (5, 11, 4),
            (11, 10, 2),
            (10, 7, 6),
            (7, 1, 8),
            (3, 9, 4),
            (3, 4, 2),
            (3, 2, 6),
            (3, 6, 8),
            (3, 8, 9),
            (4, 9, 5),
            (2, 4, 11),
            (6, 2, 10),
            (8, 6, 7),
            (9, 8, 1),
        ],
        dtype=np.int64,
    )

    for _ in range(levels):
        midpoint_cache = {}
        next_faces = []

        def midpoint(i, j):
            nonlocal vertices
            key = (min(i, j), max(i, j))
            existing = midpoint_cache.get(key)
            if existing is not None:
                return existing
            point = (vertices[i] + vertices[j]) * 0.5
            point /= np.linalg.norm(point)
            index = len(vertices)
            vertices = np.vstack((vertices, point))
            midpoint_cache[key] = index
            return index

        for i0, i1, i2 in faces:
            a = midpoint(i0, i1)
            b = midpoint(i1, i2)
            c = midpoint(i2, i0)
            next_faces.extend(((i0, a, c), (i1, b, a), (i2, c, b), (a, b, c)))
        faces = np.asarray(next_faces, dtype=np.int64)

    return (vertices * 0.5).astype(np.float32), faces.astype(np.int32)


def make_torus(R=0.35, r=0.12, nu=24, nv=12):
    """Create a closed torus (a bowl/ring-shaped body) around the z axis."""
    try:
        major_segments = operator.index(nu)
        minor_segments = operator.index(nv)
    except TypeError as exc:
        raise TypeError("nu and nv must be integers") from exc
    if major_segments < 3 or minor_segments < 3:
        raise ValueError("nu and nv must both be at least 3")
    if not (math.isfinite(R) and math.isfinite(r) and R > 0.0 and r > 0.0):
        raise ValueError("R and r must be finite and positive")

    vertices = np.empty((major_segments * minor_segments, 3), dtype=np.float64)
    for i in range(major_segments):
        u = 2.0 * math.pi * i / major_segments
        for j in range(minor_segments):
            v = 2.0 * math.pi * j / minor_segments
            radial = R + r * math.cos(v)
            vertices[i * minor_segments + j] = (
                radial * math.cos(u),
                radial * math.sin(u),
                r * math.sin(v),
            )

    faces = []
    for i in range(major_segments):
        next_i = (i + 1) % major_segments
        for j in range(minor_segments):
            next_j = (j + 1) % minor_segments
            a = i * minor_segments + j
            b = next_i * minor_segments + j
            c = next_i * minor_segments + next_j
            d = i * minor_segments + next_j
            faces.extend(((a, b, c), (a, c, d)))
    return vertices.astype(np.float32), np.asarray(faces, dtype=np.int32)


def make_plate(t=0.06):
    """Create a thin rectangular slab in the x-y plane."""
    try:
        thickness = float(t)
    except (TypeError, ValueError) as exc:
        raise ValueError("t must be a finite positive number") from exc
    if not math.isfinite(thickness) or thickness <= 0.0:
        raise ValueError("t must be a finite positive number")
    return _box_mesh((-0.5, -0.5, -thickness * 0.5), (0.5, 0.5, thickness * 0.5))


def make_blade_box():
    """Create a box carrying two thin protruding blades.

    The blades intentionally intersect the main box at its right face.  The
    resulting overlapping component surfaces exercise the same boundary and
    flood-fill path used for non-manifold/thin-part input meshes.
    """
    main_lo = (-0.4, -0.3, -0.2)
    main_hi = (0.1, 0.3, 0.2)
    pieces = [
        (main_lo, main_hi),
        ((0.1, -0.30, -0.025), (0.5, -0.08, 0.025)),
        ((0.1, 0.08, -0.025), (0.5, 0.30, 0.025)),
    ]
    vertices = []
    faces = []
    for lo, hi in pieces:
        piece_vertices, piece_faces = _box_mesh(lo, hi)
        offset = len(vertices)
        vertices.append(piece_vertices)
        faces.append(piece_faces + offset)
    return np.concatenate(vertices, axis=0).astype(np.float32), np.concatenate(faces, axis=0).astype(np.int32)


def voxelize_mesh(verts, faces, g=16, pad=1):
    """Rasterize a closed surface mesh and fill its solid interior.

    Parameters
    ----------
    verts, faces:
        Surface vertices and triangle indices.
    g:
        Number of voxels along each axis.
    pad:
        Number of grid cells kept free at each side of the normalized mesh.

    Returns
    -------
    occ:
        ``(g, g, g)`` float32 occupancy: 0 outside, 0.5 boundary, 1 inside.
    """
    try:
        grid_size = operator.index(g)
        padding = operator.index(pad)
    except TypeError as exc:
        raise TypeError("g and pad must be integers") from exc
    if grid_size < 2 * padding + 2:
        raise ValueError("grid must have at least two cells beyond the padding")
    if padding < 0:
        raise ValueError("pad must be non-negative")

    vertices, triangles = _as_mesh(verts, faces)
    minimum = vertices.min(axis=0)
    maximum = vertices.max(axis=0)
    span = maximum - minimum
    largest_span = float(span.max())
    if largest_span <= _EPS:
        raise ValueError("mesh has zero spatial extent")
    center = (minimum + maximum) * 0.5
    target_center = np.array([(grid_size - 1) * 0.5] * 3)
    target_span = grid_size - 1 - 2 * padding
    normalized = (vertices - center) * (target_span / largest_span) + target_center
    normalized = np.clip(normalized, padding, grid_size - 1 - padding)

    boundary = np.zeros((grid_size, grid_size, grid_size), dtype=bool)
    for triangle in normalized[triangles]:
        low = np.floor(triangle.min(axis=0) - 0.5).astype(np.int64)
        high = np.ceil(triangle.max(axis=0) + 0.5).astype(np.int64)
        low = np.maximum(low, 0)
        high = np.minimum(high, grid_size - 1)
        if np.any(low > high):
            continue
        axes = [np.arange(low[axis], high[axis] + 1, dtype=np.int64) for axis in range(3)]
        x, y, z = np.meshgrid(*axes, indexing="ij")
        centers = np.column_stack((x.ravel(), y.ravel(), z.ravel()))
        touched = _triangle_box_overlap(triangle, centers)
        if touched.any():
            selected = centers[touched]
            boundary[selected[:, 0], selected[:, 1], selected[:, 2]] = True

    occupancy = np.zeros((grid_size, grid_size, grid_size), dtype=np.float32)
    occupancy[boundary] = 0.5

    # Start from every unblocked cell on the six grid faces.  Propagating only
    # through non-boundary cells makes holes in a mesh correctly remain outside.
    visited = np.zeros_like(boundary)
    queue = deque()
    for index in np.ndindex(boundary.shape):
        if not boundary[index] and (
            index[0] == 0
            or index[1] == 0
            or index[2] == 0
            or index[0] == grid_size - 1
            or index[1] == grid_size - 1
            or index[2] == grid_size - 1
        ):
            visited[index] = True
            queue.append(index)
    while queue:
        x, y, z = queue.popleft()
        for neighbor in ((x - 1, y, z), (x + 1, y, z), (x, y - 1, z), (x, y + 1, z), (x, y, z - 1), (x, y, z + 1)):
            nx, ny, nz = neighbor
            if 0 <= nx < grid_size and 0 <= ny < grid_size and 0 <= nz < grid_size:
                neighbor_index = (nx, ny, nz)
                if not boundary[neighbor_index] and not visited[neighbor_index]:
                    visited[neighbor_index] = True
                    queue.append(neighbor_index)

    occupancy[~boundary & ~visited] = 1.0
    return occupancy


def boundary_count(occ):
    """Return the number of boundary (0.5) voxels in an occupancy array."""
    occupancy = np.asarray(occ)
    if occupancy.ndim != 3:
        raise ValueError("occ must be a three-dimensional array")
    return int(np.count_nonzero(occupancy == 0.5))


def _self_test():
    """Exercise representative solids and report one pass/fail summary."""
    checks = []

    def check(name, condition, detail):
        checks.append((name, bool(condition), detail))

    sphere_occ = voxelize_mesh(*make_sphere(), g=16)
    sphere_fraction = float((sphere_occ > 0.0).mean())
    sphere_boundary = boundary_count(sphere_occ)
    center = sphere_occ[8, 8, 8]
    corner_values = [sphere_occ[i, j, k] for i in (0, 15) for j in (0, 15) for k in (0, 15)]
    check("sphere center", center == 1.0, f"center={center}")
    check("sphere corners outside", all(value == 0.0 for value in corner_values), f"corners={set(corner_values)}")
    check("sphere boundary", sphere_boundary > 0, f"boundary={sphere_boundary}")
    check("sphere fraction", 0.2 <= sphere_fraction <= 0.6, f"occupied_fraction={sphere_fraction:.6f}")

    torus_occ = voxelize_mesh(*make_torus(), g=20)
    torus_fraction = float((torus_occ > 0.0).mean())
    check("torus fraction", 0.1 <= torus_fraction <= 0.5, f"occupied_fraction={torus_fraction:.6f}")

    plate_occ = voxelize_mesh(*make_plate(), g=20)
    plate_fraction = float((plate_occ > 0.0).mean())
    plate_z_layers = np.flatnonzero(np.any(plate_occ == 1.0, axis=(0, 1)))
    check("plate fraction", plate_fraction < 0.35, f"occupied_fraction={plate_fraction:.6f}")
    check("plate thinness", len(plate_z_layers) <= 4, f"inside_z_layers={plate_z_layers.tolist()}")

    blade_occ = voxelize_mesh(*make_blade_box(), g=20)
    blade_fraction = float((blade_occ > 0.0).mean())
    check("blade occupancy", np.isfinite(blade_occ).all() and blade_fraction > 0.0, f"occupied_fraction={blade_fraction:.6f}")
    check("blade non-empty vs plate", blade_fraction > plate_fraction, f"blade_fraction={blade_fraction:.6f}, plate_fraction={plate_fraction:.6f}")

    print("Automatic voxelization self-test")
    print(f"  sphere: inside_fraction={sphere_fraction:.6f}, boundary={sphere_boundary}")
    print(f"  torus:  inside_fraction={torus_fraction:.6f}, boundary={boundary_count(torus_occ)}")
    print(f"  plate:  inside_fraction={plate_fraction:.6f}, inside_z_layers={plate_z_layers.tolist()}")
    print(f"  blades: inside_fraction={blade_fraction:.6f}, boundary={boundary_count(blade_occ)}")
    for name, passed, detail in checks:
        print(f"  {'PASS' if passed else 'FAIL'}: {name} ({detail})")
    passed = all(result for _, result, _ in checks)
    print(f"{'PASS' if passed else 'FAIL'}: {sum(result for _, result, _ in checks)}/{len(checks)} checks")
    return passed


if __name__ == "__main__":
    raise SystemExit(0 if _self_test() else 1)
