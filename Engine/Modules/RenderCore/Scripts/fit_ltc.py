#!/usr/bin/env python3
"""
fit_ltc.py -- linearly transformed cosines for the renderer's area lights
(Heitz, Dupuy, Hill, Neubelt 2016, "Real-Time Polygonal-Light Shading with
Linearly Transformed Cosines"; GLOBAL_ILLUMINATION.md, Phase 3), fitted to the
renderer's own direct-light specular (Lighting / ShadingModels.glsl): GGX,
Smith with Schlick-GGX at k = (r + 1)^2 / 8, folded with the cosine.

For each of 64 x 64 cells -- perceptual roughness r = i / 63 down one axis,
the view's sqrt(1 - cos theta) = j / 63 along the other -- the LTC's matrix M
(isotropic: m11, m22, m13 in the frame whose Z is the lobe's average
direction and X the view's azimuth) is fitted by Nelder-Mead to minimise
Heitz's error (|f - D|^3, by multiple importance sampling of both), started
from the cell before. Writes LTCTables.inl beside RenderContext.cpp:

    LTC_MATRIX[64 * 64 * 4]  inverse(M) / inverse(M)[1][1]: its (0,0), (2,0),
                             (0,2), (2,2), so GLSL's
                             mat3(vec3(a, 0, b), vec3(0, 1, 0), vec3(c, 0, d))
    LTC_AMPLITUDE[64 * 64 * 2]  the BRDF's directional albedo split by
                             Schlick's Fresnel: x = integral of f cos (1 - s),
                             y = integral of f cos s, s = (1 - VdotH)^5 -- so
                             the specular's scale is F0 x + y

Cell (i, j) is at [(j * 64 + i) * 4]: roughness along the row. Takes a few
minutes (numpy).

    python Engine/Modules/RenderCore/Scripts/fit_ltc.py
"""

import math
import sys
from pathlib import Path

import numpy as np

N        = 64
SAMPLES  = 32          # per axis: SAMPLES^2 from each distribution per error
MIN_R    = 0.04        # the renderer clamps roughness here (ShadingModels.glsl)
OUT      = Path(__file__).resolve().parents[1] / "Source" / "Private" / "RenderCore" / "LTCTables.inl"

rng_u = (np.arange(SAMPLES) + 0.5) / SAMPLES
U1, U2 = np.meshgrid(rng_u, rng_u)
U1 = U1.ravel(); U2 = U2.ravel()


def brdf_eval(V, L, r):
    """f(V, L) * cos(L) and the pdf of sampling L by GGX's D (half vectors)."""
    r = max(r, MIN_R)
    alpha = r * r
    NdotL = L[:, 2]
    NdotV = V[2]
    H = L + V
    H /= np.linalg.norm(H, axis=1, keepdims=True)
    NdotH = np.clip(H[:, 2], 0.0, 1.0)
    VdotH = np.clip(H @ V, 0.0, 1.0)
    a2 = alpha * alpha
    d = NdotH * NdotH * (a2 - 1.0) + 1.0
    D = a2 / (math.pi * d * d)
    k = (r + 1.0) ** 2 / 8.0
    nl = np.clip(NdotL, 0.0, 1.0)
    G = (NdotV / (NdotV * (1.0 - k) + k)) * (nl / (nl * (1.0 - k) + k))
    f = np.where(NdotL > 0.0, D * G / max(4.0 * NdotV, 1e-6), 0.0)    # f * cos L: the 1 / NdotL cancels the cosine
    pdf = D * NdotH / np.maximum(4.0 * VdotH, 1e-7)
    return f, pdf, VdotH


def brdf_sample(V, r):
    r = max(r, MIN_R)
    alpha = r * r
    phi = 2.0 * math.pi * U1
    cos_t = np.sqrt((1.0 - U2) / (1.0 + (alpha * alpha - 1.0) * U2))
    sin_t = np.sqrt(np.maximum(1.0 - cos_t * cos_t, 0.0))
    H = np.stack([sin_t * np.cos(phi), sin_t * np.sin(phi), cos_t], axis=1)
    L = 2.0 * (H @ V)[:, None] * H - V
    return L


def ltc_matrices(params, frame):
    m11, m22, m13 = params
    M = np.array([[m11, 0.0, m13], [0.0, m22, 0.0], [0.0, 0.0, 1.0]])
    M = frame @ M
    return M, np.linalg.inv(M), abs(np.linalg.det(np.linalg.inv(M)))


def ltc_eval(L, invM, det_inv):
    Lo = L @ invM.T
    length = np.linalg.norm(Lo, axis=1)
    Lo = Lo / length[:, None]
    return np.maximum(Lo[:, 2], 0.0) / math.pi * det_inv / length ** 3


def ltc_sample(M):
    phi = 2.0 * math.pi * U1
    cos_t = np.sqrt(1.0 - U2)
    sin_t = np.sqrt(U2)
    Lo = np.stack([sin_t * np.cos(phi), sin_t * np.sin(phi), cos_t], axis=1)
    L = Lo @ M.T
    return L / np.linalg.norm(L, axis=1, keepdims=True)


def averages(V, r):
    """The BRDF's albedo, split by Fresnel, and its lobe's average direction."""
    L = brdf_sample(V, r)
    f, pdf, VdotH = brdf_eval(V, L, r)
    w = np.where(pdf > 0.0, f / np.maximum(pdf, 1e-12), 0.0)
    s = (1.0 - VdotH) ** 5
    norm = np.mean(w)
    x = np.mean(w * (1.0 - s))
    y = np.mean(w * s)
    avg = np.sum(L * w[:, None], axis=0)
    avg[1] = 0.0
    n = np.linalg.norm(avg)
    avg = avg / n if n > 0 else np.array([0.0, 0.0, 1.0])
    return norm, x, y, avg


def error(params, V, r, norm, frame):
    if params[0] <= 1e-6 or params[1] <= 1e-6:
        return 1e30
    M, invM, det_inv = ltc_matrices(params, frame)
    total = 0.0
    for L in (ltc_sample(M), brdf_sample(V, r)):
        f, pdf_brdf, _ = brdf_eval(V, L, r)
        f = f / norm
        pdf_brdf = pdf_brdf
        d = ltc_eval(L, invM, det_inv)
        e = np.abs(f - d) ** 3
        valid = (pdf_brdf + d) > 0.0
        total += np.sum(np.where(valid, e / np.maximum(pdf_brdf + d, 1e-12), 0.0))
    return total / (2.0 * len(U1))


def nelder_mead(fn, x0, step, iters=300, tol=1e-6):
    n = len(x0)
    pts = [np.array(x0, dtype=float)]
    for i in range(n):
        p = np.array(x0, dtype=float); p[i] += step[i]; pts.append(p)
    vals = [fn(p) for p in pts]
    for _ in range(iters):
        order = np.argsort(vals); pts = [pts[i] for i in order]; vals = [vals[i] for i in order]
        if abs(vals[-1] - vals[0]) < tol * (abs(vals[0]) + 1e-12):
            break
        centroid = np.mean(pts[:-1], axis=0)
        xr = centroid + (centroid - pts[-1]); fr = fn(xr)
        if fr < vals[0]:
            xe = centroid + 2.0 * (centroid - pts[-1]); fe = fn(xe)
            pts[-1], vals[-1] = (xe, fe) if fe < fr else (xr, fr)
        elif fr < vals[-2]:
            pts[-1], vals[-1] = xr, fr
        else:
            xc = centroid + 0.5 * (pts[-1] - centroid); fc = fn(xc)
            if fc < vals[-1]:
                pts[-1], vals[-1] = xc, fc
            else:
                for i in range(1, len(pts)):
                    pts[i] = pts[0] + 0.5 * (pts[i] - pts[0]); vals[i] = fn(pts[i])
    best = int(np.argmin(vals))
    return pts[best]


def main():
    matrix = np.zeros((N, N, 4))
    amplitude = np.zeros((N, N, 2))
    for j in range(N):                       # view angle: sqrt(1 - cos theta)
        x = j / (N - 1)
        cos_t = max(1.0 - x * x, 1e-4)
        theta = min(math.acos(cos_t), 1.5707)
        V = np.array([math.sin(theta), 0.0, math.cos(theta)])
        prev = None
        for i in reversed(range(N)):         # rough to smooth: each starts from the rougher fit
            r = max(i / (N - 1), MIN_R)
            norm, ax, ay, avg = averages(V, r)
            Z = avg
            X = np.array([Z[2], 0.0, -Z[0]])   # at right angles to Z, in the plane of V
            Y = np.array([0.0, 1.0, 0.0])
            frame = np.stack([X, Y, Z], axis=1)
            alpha = r * r
            x0 = prev if prev is not None else np.array([alpha, alpha, 0.0])
            fit = nelder_mead(lambda p: error(p, V, r, norm, frame), x0, [0.05 * max(alpha, 0.01)] * 2 + [0.05])
            prev = fit
            M, invM, _ = ltc_matrices(fit, frame)
            invM = invM / invM[1, 1]
            matrix[j, i] = [invM[0, 0], invM[2, 0], invM[0, 2], invM[2, 2]]
            amplitude[j, i] = [ax, ay]
        print(f"  view {j + 1}/{N}", file=sys.stderr, flush=True)

    def literal(v):
        text = f"{v:.7g}"
        return (text if ("." in text or "e" in text) else text + ".0") + "f"

    with open(OUT, "w", newline="\n") as f:
        f.write("// Generated by Engine/Modules/RenderCore/Scripts/fit_ltc.py. Do not edit.\n")
        f.write("// Linearly transformed cosines for the renderer's GGX (GLOBAL_ILLUMINATION.md, Phase 3):\n")
        f.write("// 64 x 64 cells, roughness along a row, sqrt(1 - NdotV) down the rows.\n")
        f.write("static const float LTC_MATRIX[64 * 64 * 4] = {\n")
        for j in range(N):
            f.write("    " + ", ".join(literal(v) for v in matrix[j].ravel()) + ",\n")
        f.write("};\n")
        f.write("static const float LTC_AMPLITUDE[64 * 64 * 2] = {\n")
        for j in range(N):
            f.write("    " + ", ".join(literal(v) for v in amplitude[j].ravel()) + ",\n")
        f.write("};\n")
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
