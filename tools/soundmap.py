"""Trilinear modal Sound Map sampling for the coarse modal grid.

The grid uses ``nn = g + 1`` nodes per axis and row-major node numbering::

    node = (iz * nn * nn + iy * nn + ix)
    dof  = node * 3 + component

The helpers in this module deliberately return unnormalized modal gains so
callers can apply the Sound Map normalization appropriate to their pipeline.
"""

from __future__ import annotations

import numpy as np


def _validate_grid(g: int, nn: int, h: float) -> None:
    """Validate the common grid geometry without copying modal data."""
    if g < 1 or nn != g + 1:
        raise ValueError("grid must have g >= 1 and nn == g + 1")
    if not np.isfinite(h) or h <= 0.0:
        raise ValueError("h must be a positive finite grid spacing")


def build_surface_grid(
    active_nodes: set[int],
    g: int,
    nn: int,
    h: float,
    sz: int = 16,
) -> np.ndarray:
    """Return ``(sz, sz, 3)`` world-space points on the top active surface.

    The regular sample plane spans ``[0, g*h]`` on x and y.  Each sample is
    assigned to the node column containing its x/y coordinate.  A missing
    column falls back to the z plane of the globally topmost active node.
    """
    _validate_grid(g, nn, h)
    if sz < 2:
        raise ValueError("surface grid size must be at least 2")

    active = {int(node_id) for node_id in active_nodes}
    global_top = 0
    for node_id in active:
        global_top = max(global_top, node_id // (nn * nn))

    xy = np.linspace(0.0, float(g * h), int(sz), dtype=np.float64)
    points = np.empty((sz, sz, 3), dtype=np.float64)
    # Index the returned array as [y, x, 3] to match the engine's
    # gain[m][y][x] layout: x varies along the column (2nd) axis, y along the
    # row (1st) axis. A caller flattening to (n_points, 3) and reshaping the
    # gains back to (n_modes, sz, sz) then aligns with the engine.
    points[:, :, 0] = xy[None, :]
    points[:, :, 1] = xy[:, None]

    # Floor maps a world coordinate to its containing node column.  Clipping
    # only the final x=y=g samples keeps the boundary column on the grid.
    ix_columns = np.minimum(np.floor(xy / h).astype(np.int64), g)
    iy_columns = np.minimum(np.floor(xy / h).astype(np.int64), g)

    for gy, iy in enumerate(iy_columns):
        for gx, ix in enumerate(ix_columns):
            top = global_top
            for iz in range(global_top, -1, -1):
                node_id = (iz * nn * nn + int(iy) * nn + int(ix))
                if node_id in active:
                    top = iz
                    break
            points[gy, gx, 2] = top * h

    return points


def sample_gain_surface(
    vecs: np.ndarray,
    K_idx: np.ndarray,
    nn: int,
    h: float,
    g: int,
    points: np.ndarray,
    direction: tuple[float, float, float] = (0.0, 0.0, 1.0),
) -> np.ndarray:
    """Sample and project modal mode shapes at 3D points.

    Parameters
    ----------
    vecs:
        Eigenvectors with shape ``(n_active, n_modes)``.
    K_idx:
        Active full-grid DOF indices, in the row order used by ``vecs``.
    points:
        World-space strike points with shape ``(n_points, 3)``.

    Returns
    -------
    numpy.ndarray
        Unnormalized gains with shape ``(n_modes, n_points)``.  Inactive
        corner DOFs contribute zero rather than being synthesized from data.
    """
    _validate_grid(g, nn, h)

    eigenvectors = np.asarray(vecs, dtype=np.float64)
    if eigenvectors.ndim != 2:
        raise ValueError("vecs must have shape (n_active, n_modes)")
    active_dofs = np.asarray(K_idx, dtype=np.int64)
    if active_dofs.ndim != 1:
        raise ValueError("K_idx must be one-dimensional")
    if eigenvectors.shape[0] != len(active_dofs):
        raise ValueError("vecs rows and K_idx length must match")

    strike_direction = np.asarray(direction, dtype=np.float64)
    if strike_direction.shape != (3,):
        raise ValueError("direction must contain exactly three components")
    if not np.all(np.isfinite(strike_direction)):
        raise ValueError("direction components must be finite")

    strike_points = np.asarray(points, dtype=np.float64)
    if strike_points.size == 0:
        strike_points = strike_points.reshape(0, 3)
    if strike_points.ndim != 2 or strike_points.shape[1] != 3:
        raise ValueError("points must have shape (n_points, 3)")
    if not np.all(np.isfinite(strike_points)):
        raise ValueError("points must contain only finite coordinates")

    n_modes = eigenvectors.shape[1]
    gains = np.zeros((n_modes, len(strike_points)), dtype=np.float64)

    full_dof_count = 3 * nn * nn * nn
    if active_dofs.size and (
        np.min(active_dofs) < 0 or np.max(active_dofs) >= full_dof_count
    ):
        raise ValueError("K_idx contains a DOF outside the nn^3 grid")

    # Dense row lookup keeps the eight-corner interpolation deterministic and
    # makes the full-DOF-to-eigenvector-row mapping explicit.
    dof_to_row = np.full(full_dof_count, -1, dtype=np.int64)
    dof_to_row[active_dofs] = np.arange(len(active_dofs), dtype=np.int64)

    grid_coordinates = strike_points / h
    origins = np.floor(grid_coordinates).astype(np.int64)
    origins = np.clip(origins, 0, nn - 2)
    fractions = grid_coordinates - origins

    for dz in (0, 1):
        wz = fractions[:, 2] if dz else 1.0 - fractions[:, 2]
        for dy in (0, 1):
            wy = fractions[:, 1] if dy else 1.0 - fractions[:, 1]
            for dx in (0, 1):
                wx = fractions[:, 0] if dx else 1.0 - fractions[:, 0]
                corner_weight = wx * wy * wz

                node_ids = (
                    (origins[:, 2] + dz) * nn * nn
                    + (origins[:, 1] + dy) * nn
                    + (origins[:, 0] + dx)
                )

                # Interpolate the complete displacement vector, then project
                # it.  The default direction therefore uses component 2, while
                # oblique strike directions remain a proper dot product.
                projected_corner = np.zeros(
                    (len(strike_points), n_modes), dtype=np.float64
                )
                for component, direction_weight in enumerate(strike_direction):
                    if direction_weight == 0.0:
                        continue
                    rows = dof_to_row[node_ids * 3 + component]
                    active = rows >= 0
                    if np.any(active):
                        projected_corner[active] += (
                            direction_weight * eigenvectors[rows[active], :]
                        )

                gains += corner_weight[None, :] * projected_corner.T

    return gains


def _linear_field(
    coordinates: np.ndarray, coefficients: np.ndarray
) -> np.ndarray:
    """Evaluate one or more linear scalar fields on world coordinates."""
    a, b, c, d = coefficients
    return a + b * coordinates[..., 0] + c * coordinates[..., 1] + d * coordinates[..., 2]


def _run_self_test() -> int:
    """Exercise interpolation, active-DOF handling, and surface construction."""
    checks: list[tuple[str, bool, str]] = []

    g = 3
    nn = g + 1
    h = 0.25
    rng = np.random.default_rng(47)
    n_modes = 5
    full_dofs = np.arange(3 * nn * nn * nn, dtype=np.int64)
    random_vecs = rng.normal(size=(len(full_dofs), n_modes))

    # A linear mode shape is reproduced exactly by trilinear interpolation.
    coefficients = rng.normal(size=(n_modes, 4))
    vecs = random_vecs.copy()
    for iz in range(nn):
        for iy in range(nn):
            for ix in range(nn):
                node = (iz * nn * nn + iy * nn + ix) * 3 + 2
                world = np.array([ix * h, iy * h, iz * h])
                vecs[node, :] = [
                    _linear_field(world, row) for row in coefficients
                ]

    fractions = np.array([0.125, 0.375, 0.625, 0.875])
    fraction_x, fraction_y, fraction_z = np.meshgrid(
        fractions, fractions, fractions, indexing="ij"
    )
    interior_points = h * np.column_stack(
        (fraction_x.ravel(), fraction_y.ravel(), fraction_z.ravel())
    )
    linear_gains = sample_gain_surface(
        vecs, full_dofs, nn, h, g, interior_points
    )
    expected = np.column_stack(
        [_linear_field(interior_points, row) for row in coefficients]
    ).T
    linear_error = float(np.max(np.abs(linear_gains - expected)))
    checks.append(
        (
            "trilinear linear-field exactness",
            linear_gains.shape == (n_modes, len(interior_points))
            and linear_error < 1.0e-9,
            f"max error={linear_error:.3e}",
        )
    )

    # Exact-node sampling must select only that node's active z eigenvector.
    node_indices = np.array([0, 25, 54], dtype=np.int64)
    node_points = np.column_stack(
        (node_indices % nn, (node_indices // nn) % nn, node_indices // (nn * nn))
    ) * h
    node_gains = sample_gain_surface(
        random_vecs, full_dofs, nn, h, g, node_points
    )
    expected_nodes = np.vstack(
        [random_vecs[int(node) * 3 + 2, :] for node in node_indices]
    ).T
    node_error = float(np.max(np.abs(node_gains - expected_nodes)))
    checks.append(
        (
            "exact grid-node sampling",
            node_gains.shape == (n_modes, len(node_points))
            and node_error < 1.0e-12,
            f"max error={node_error:.3e}",
        )
    )

    # Inactive corner DOFs are skipped instead of treated as zero-valued data.
    inactive_dof = ((1 * nn + 1) * nn + 1) * 3 + 2
    sparse_dofs = full_dofs[full_dofs != inactive_dof]
    sparse_vecs = np.ones((len(sparse_dofs), 1), dtype=np.float64)
    missing_node_gain = sample_gain_surface(
        sparse_vecs, sparse_dofs, nn, h, g, np.array([[h, h, h]])
    )
    checks.append(
        (
            "inactive corner DOF skipping",
            missing_node_gain.shape == (1, 1) and missing_node_gain[0, 0] == 0.0,
            f"gain={missing_node_gain[0, 0]:.1f}",
        )
    )

    # An oblique strike direction projects the interpolated displacement vector.
    oblique_vecs = np.zeros((len(full_dofs), 1), dtype=np.float64)
    oblique_vecs[:, 0] = np.arange(len(full_dofs), dtype=np.float64)
    oblique_direction = (0.25, 0.5, -0.75)
    oblique_point = np.array([[2.0 * h, h, 3.0 * h]])
    oblique_gain = sample_gain_surface(
        oblique_vecs, full_dofs, nn, h, g, oblique_point, oblique_direction
    )[0, 0]
    oblique_node = 3 * nn * nn + nn + 2
    expected_oblique = sum(
        direction_component * oblique_vecs[oblique_node * 3 + component, 0]
        for component, direction_component in enumerate(oblique_direction)
    )
    oblique_error = abs(float(oblique_gain - expected_oblique))
    checks.append(
        (
            "oblique strike projection",
            oblique_error < 1.0e-12,
            f"max error={oblique_error:.3e}",
        )
    )

    full_active_nodes = set(range(nn * nn * nn))
    surface_points = build_surface_grid(full_active_nodes, g, nn, h)
    full_top = g * h
    checks.append(
        (
            "full-grid surface construction",
            surface_points.shape == (16, 16, 3)
            and np.all(surface_points[:, :, 2] == full_top),
            f"shape={surface_points.shape}, top z={full_top:.2f}",
        )
    )

    passed = 0
    for name, ok, detail in checks:
        status = "PASS" if ok else "FAIL"
        print(f"{status}: {name} ({detail})")
        passed += int(ok)

    if passed == len(checks):
        print(f"PASS: soundmap self-test ({passed}/{len(checks)} checks passed)")
        return 0
    print(f"FAIL: soundmap self-test ({passed}/{len(checks)} checks passed)")
    return 1


if __name__ == "__main__":
    raise SystemExit(_run_self_test())
