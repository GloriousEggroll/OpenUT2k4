"""
OpenUT2k4 content generator (Unreal Editor Python).

Creates the small set of generated assets the game expects (see Source/OpenUT2k4/ArenaAssets.h):

  /Game/Materials/M_ArenaSurface   opaque, default lit, world-aligned grid
  /Game/Materials/M_ArenaGlow      unlit, additive, two-sided, soft fresnel falloff
  /Game/Materials/M_ArenaSprite    unlit, additive, two-sided, textured (flipbook / tiling / panning) for effects
  /Game/Materials/M_ArenaSpriteAlpha  same, translucent
  /Game/Materials/M_ArenaParticle{Add,Alpha,Mod}  UE2 emitter particles (texture x vertex colour)
  /Game/Materials/M_ArenaSkyFace   unlit face of the 3D sky box cube
  /Game/Materials/UT2004/M_Imported*  masters for UT2004 content built at runtime (Source/OpenUT2k4/ArenaRuntimeContent.h)
  /Game/Audio/S_*                  procedurally synthesised sound effects (original)
  /Game/Maps/MainMenu, /Game/Maps/Arena   blank levels with game mode overrides

Run headless:
  UnrealEditor-Cmd <Project>.uproject -run=pythonscript -script=<this file> \
      -unattended -nop4 -nosplash -nullrhi

The script is idempotent: existing assets are rebuilt/overwritten in place.
All audio is synthesised here with plain Python (math/random/struct/wave).
"""

import math
import os
import random
import shutil
import struct
import tempfile
import wave

import unreal

MATERIAL_DIR = "/Game/Materials"
UT2004_MATERIAL_DIR = MATERIAL_DIR + "/UT2004"
AUDIO_DIR = "/Game/Audio"
MAPS_DIR = "/Game/Maps"

SAMPLE_RATE = 44100

MEL = unreal.MaterialEditingLibrary
EAL = unreal.EditorAssetLibrary

_errors = []


def log(msg):
    unreal.log("[GenerateContent] " + msg)


def warn(msg):
    unreal.log_warning("[GenerateContent] " + msg)


def error(msg):
    _errors.append(msg)
    unreal.log_error("[GenerateContent] " + msg)


# =============================================================================
# Materials
# =============================================================================

def get_or_create_material(name, folder=None):
    """Return an empty material asset at <folder or MATERIAL_DIR>/name (existing graph is cleared)."""
    folder = folder or MATERIAL_DIR
    path = "%s/%s" % (folder, name)
    mat = None
    if EAL.does_asset_exist(path):
        mat = EAL.load_asset(path)
        if not isinstance(mat, unreal.Material):
            warn("%s exists but is not a Material; replacing it" % path)
            EAL.delete_asset(path)
            mat = None
    if mat is None:
        tools = unreal.AssetToolsHelpers.get_asset_tools()
        mat = tools.create_asset(name, folder, unreal.Material, unreal.MaterialFactoryNew())
        if mat is None:
            raise RuntimeError("could not create material " + path)
        log("created " + path)
    else:
        MEL.delete_all_material_expressions(mat)
        log("rebuilding " + path)
    return mat


def expr(mat, cls, x, y, **props):
    e = MEL.create_material_expression(mat, cls, x, y)
    if e is None:
        raise RuntimeError("could not create expression %s" % cls.__name__)
    for k, v in props.items():
        e.set_editor_property(k, v)
    return e


def link(src, dst, dst_input="", src_output=""):
    if not MEL.connect_material_expressions(src, src_output, dst, dst_input):
        raise RuntimeError("failed to connect %s -> %s.%s" % (src.get_name(), dst.get_name(), dst_input))


def to_prop(src, prop, src_output=""):
    if not MEL.connect_material_property(src, src_output, prop):
        raise RuntimeError("failed to connect %s -> %s" % (src.get_name(), prop))


def vec_param(mat, name, default, x, y):
    return expr(mat, unreal.MaterialExpressionVectorParameter, x, y,
                parameter_name=name, default_value=unreal.LinearColor(*default))


def scalar_param(mat, name, default, x, y):
    return expr(mat, unreal.MaterialExpressionScalarParameter, x, y,
                parameter_name=name, default_value=float(default))


def set_usages(mat):
    """Usage flags must be baked in the editor, otherwise cooked builds fall back to the default material."""
    names = [
        "MATUSAGE_SKELETAL_MESH",
        "MATUSAGE_INSTANCED_STATIC_MESHES",
        "MATUSAGE_SPLINE_MESH",
        "MATUSAGE_NIAGARA_SPRITES",
        "MATUSAGE_NIAGARA_MESH_PARTICLES",
        "MATUSAGE_NIAGARA_RIBBONS",
        "MATUSAGE_MORPH_TARGETS",
    ]
    for n in names:
        usage = getattr(unreal.MaterialUsage, n, None)
        if usage is None:
            continue
        try:
            # 5.8: set_material_usage is deprecated in favour of set_base_material_usage.
            if hasattr(MEL, "set_base_material_usage"):
                MEL.set_base_material_usage(mat, usage, True)
            else:
                MEL.set_material_usage(mat, usage)
        except Exception as ex:
            warn("could not set usage %s: %s" % (n, ex))


def finish_material(mat):
    MEL.layout_material_expressions(mat)
    errs = MEL.recompile_material(mat)
    if errs:
        for e in errs:
            error("%s compile error: %s" % (mat.get_name(), e))
    else:
        log("%s compiled OK" % mat.get_name())
    EAL.save_loaded_asset(mat, only_if_is_dirty=False)


def build_surface_material():
    mat = get_or_create_material("M_ArenaSurface")
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    mat.set_editor_property("two_sided", False)
    set_usages(mat)

    color = vec_param(mat, "Color", (0.5, 0.5, 0.5, 1.0), -1400, -400)
    emissive = vec_param(mat, "Emissive", (0.0, 0.0, 0.0, 1.0), -1400, 400)
    roughness = scalar_param(mat, "Roughness", 0.6, -1400, 200)
    metallic = scalar_param(mat, "Metallic", 0.0, -1400, 300)
    grid_scale = scalar_param(mat, "GridScale", 1.0, -2200, 0)

    # p = WorldPosition / (400 * GridScale)   -> one grid cell every 4 m at GridScale 1
    cell = expr(mat, unreal.MaterialExpressionMultiply, -2000, 0, const_b=400.0)
    link(grid_scale, cell, "A")
    world_pos = expr(mat, unreal.MaterialExpressionWorldPosition, -2000, -150)
    p = expr(mat, unreal.MaterialExpressionDivide, -1850, -100)
    link(world_pos, p, "A")
    link(cell, p, "B")

    # d = 0.5 - |frac(p) - 0.5|   distance (in cells) to the nearest cell edge, per axis
    fr = expr(mat, unreal.MaterialExpressionFrac, -1700, -100)
    link(p, fr)
    centred = expr(mat, unreal.MaterialExpressionSubtract, -1600, -100, const_b=0.5)
    link(fr, centred, "A")
    ab = expr(mat, unreal.MaterialExpressionAbs, -1500, -100)
    link(centred, ab)
    dist = expr(mat, unreal.MaterialExpressionSubtract, -1400, -100, const_a=0.5)
    link(ab, dist, "B")

    # line = 1 - saturate(d / width)   thin lines (~5 cm at GridScale 1)
    scaled = expr(mat, unreal.MaterialExpressionDivide, -1300, -100, const_b=0.012)
    link(dist, scaled, "A")
    sat = expr(mat, unreal.MaterialExpressionSaturate, -1200, -100)
    link(scaled, sat)
    lines = expr(mat, unreal.MaterialExpressionOneMinus, -1100, -100)
    link(sat, lines)

    # Ignore the axis along the surface normal (that axis is constant across the face).
    normal = expr(mat, unreal.MaterialExpressionVertexNormalWS, -1300, 50)
    nabs = expr(mat, unreal.MaterialExpressionAbs, -1200, 50)
    link(normal, nabs)
    nweight = expr(mat, unreal.MaterialExpressionOneMinus, -1100, 50)
    link(nabs, nweight)
    weighted = expr(mat, unreal.MaterialExpressionMultiply, -1000, -50)
    link(lines, weighted, "A")
    link(nweight, weighted, "B")

    mr = expr(mat, unreal.MaterialExpressionComponentMask, -900, -150, r=True, g=False, b=False, a=False)
    mg = expr(mat, unreal.MaterialExpressionComponentMask, -900, -50, r=False, g=True, b=False, a=False)
    mb = expr(mat, unreal.MaterialExpressionComponentMask, -900, 50, r=False, g=False, b=True, a=False)
    for m in (mr, mg, mb):
        link(weighted, m)
    max1 = expr(mat, unreal.MaterialExpressionMax, -800, -100)
    link(mr, max1, "A")
    link(mg, max1, "B")
    max2 = expr(mat, unreal.MaterialExpressionMax, -700, -50)
    link(max1, max2, "A")
    link(mb, max2, "B")

    # shade = lerp(1, 0.75, line): lines darken the base colour by 25%
    shade = expr(mat, unreal.MaterialExpressionLinearInterpolate, -600, -50, const_a=1.0, const_b=0.75)
    link(max2, shade, "Alpha")
    base = expr(mat, unreal.MaterialExpressionMultiply, -400, -300)
    link(color, base, "A")
    link(shade, base, "B")

    to_prop(base, unreal.MaterialProperty.MP_BASE_COLOR)
    to_prop(roughness, unreal.MaterialProperty.MP_ROUGHNESS)
    to_prop(metallic, unreal.MaterialProperty.MP_METALLIC)
    to_prop(emissive, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    finish_material(mat)


def build_glow_material():
    mat = get_or_create_material("M_ArenaGlow")
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_ADDITIVE)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property("two_sided", True)
    # Keep the normal depth test (glow is occluded by walls); additive translucency never writes depth,
    # so overlapping glows do not sort-pop against each other.
    mat.set_editor_property("disable_depth_test", False)
    set_usages(mat)

    color = vec_param(mat, "Color", (1.0, 1.0, 1.0, 1.0), -900, -200)
    intensity = scalar_param(mat, "Intensity", 8.0, -900, 0)
    opacity = scalar_param(mat, "Opacity", 1.0, -900, 100)

    # Soft falloff towards silhouette edges: facing = 1 - fresnel(exp 2.5); glow = lerp(0.08, 1, facing)
    fres = expr(mat, unreal.MaterialExpressionFresnel, -900, 250, exponent=2.5, base_reflect_fraction=0.0)
    facing = expr(mat, unreal.MaterialExpressionOneMinus, -750, 250)
    link(fres, facing)
    falloff = expr(mat, unreal.MaterialExpressionLinearInterpolate, -600, 250, const_a=0.08, const_b=1.0)
    link(facing, falloff, "Alpha")

    ci = expr(mat, unreal.MaterialExpressionMultiply, -600, -100)
    link(color, ci, "A")
    link(intensity, ci, "B")
    cio = expr(mat, unreal.MaterialExpressionMultiply, -450, -50)
    link(ci, cio, "A")
    link(opacity, cio, "B")
    final = expr(mat, unreal.MaterialExpressionMultiply, -300, 50)
    link(cio, final, "A")
    link(falloff, final, "B")

    to_prop(final, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    # Opacity is ignored for additive blending but keep it wired for clarity / blend mode changes.
    to_prop(opacity, unreal.MaterialProperty.MP_OPACITY)
    finish_material(mat)


def build_sprite_material(name, translucent):
    """Textured effect material for runtime-built sprites, beams and rings (Source/OpenUT2k4/ArenaArtFX.*).

    Unlit and two-sided. Parameters: texture "Base", vector "Tint", scalars "Intensity", "Opacity",
    flipbook "Columns" / "Rows" / "Frame" (frame index, row-major), "TileU" / "TileV" (texture repeats along
    the quad) and "PanU" / "PanV" (repeats per second). The additive version adds Base.rgb * Tint * Intensity
    * Opacity (times Base.a when "UseAlpha" is 1); the translucent version uses Base.a * Opacity as opacity.
    """
    mat = get_or_create_material(name)
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT if translucent
                            else unreal.BlendMode.BLEND_ADDITIVE)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property("two_sided", True)
    mat.set_editor_property("disable_depth_test", False)
    set_usages(mat)

    columns = scalar_param(mat, "Columns", 1.0, -2400, -300)
    rows = scalar_param(mat, "Rows", 1.0, -2400, -200)
    frame = scalar_param(mat, "Frame", 0.0, -2400, -100)
    tile_u = scalar_param(mat, "TileU", 1.0, -2400, 50)
    tile_v = scalar_param(mat, "TileV", 1.0, -2400, 150)
    pan_u = scalar_param(mat, "PanU", 0.0, -2400, 250)
    pan_v = scalar_param(mat, "PanV", 0.0, -2400, 350)

    # tiled = uv * (TileU, TileV) + time * (PanU, PanV)
    tc = expr(mat, unreal.MaterialExpressionTextureCoordinate, -2200, 0)
    tile = expr(mat, unreal.MaterialExpressionAppendVector, -2200, 100)
    link(tile_u, tile, "A")
    link(tile_v, tile, "B")
    tiled = expr(mat, unreal.MaterialExpressionMultiply, -2050, 50)
    link(tc, tiled, "A")
    link(tile, tiled, "B")
    pan = expr(mat, unreal.MaterialExpressionAppendVector, -2200, 300)
    link(pan_u, pan, "A")
    link(pan_v, pan, "B")
    time = expr(mat, unreal.MaterialExpressionTime, -2200, 400)
    pan_t = expr(mat, unreal.MaterialExpressionMultiply, -2050, 300)
    link(pan, pan_t, "A")
    link(time, pan_t, "B")
    moved = expr(mat, unreal.MaterialExpressionAdd, -1900, 100)
    link(tiled, moved, "A")
    link(pan_t, moved, "B")

    # flipbook cell: col = fmod(floor(Frame), Columns), row = floor(floor(Frame) / Columns)
    cell = expr(mat, unreal.MaterialExpressionFloor, -2200, -100)
    link(frame, cell)
    col = expr(mat, unreal.MaterialExpressionFmod, -2050, -200)
    link(cell, col, "A")
    link(columns, col, "B")
    row_f = expr(mat, unreal.MaterialExpressionDivide, -2050, -100)
    link(cell, row_f, "A")
    link(columns, row_f, "B")
    row = expr(mat, unreal.MaterialExpressionFloor, -1950, -100)
    link(row_f, row)
    offset = expr(mat, unreal.MaterialExpressionAppendVector, -1850, -150)
    link(col, offset, "A")
    link(row, offset, "B")
    dims = expr(mat, unreal.MaterialExpressionAppendVector, -1850, -300)
    link(columns, dims, "A")
    link(rows, dims, "B")
    shifted = expr(mat, unreal.MaterialExpressionAdd, -1750, 0)
    link(moved, shifted, "A")
    link(offset, shifted, "B")
    uv = expr(mat, unreal.MaterialExpressionDivide, -1650, 0)
    link(shifted, uv, "A")
    link(dims, uv, "B")

    base = expr(mat, unreal.MaterialExpressionTextureSampleParameter2D, -1450, 0, parameter_name="Base")
    default_tex = EAL.load_asset("/Engine/EngineResources/WhiteSquareTexture")
    if default_tex is not None:
        base.set_editor_property("texture", default_tex)
    link(uv, base, "UVs")

    tint = vec_param(mat, "Tint", (1.0, 1.0, 1.0, 1.0), -1450, -300)
    intensity = scalar_param(mat, "Intensity", 1.0, -1450, 250)
    opacity = scalar_param(mat, "Opacity", 1.0, -1450, 350)

    rgb = expr(mat, unreal.MaterialExpressionMultiply, -1150, -150)
    link(base, rgb, "A", "RGB")
    link(tint, rgb, "B", "RGB")
    lit = expr(mat, unreal.MaterialExpressionMultiply, -1000, -50)
    link(rgb, lit, "A")
    link(intensity, lit, "B")

    if translucent:
        to_prop(lit, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
        alpha = expr(mat, unreal.MaterialExpressionMultiply, -1000, 300)
        link(base, alpha, "A", "A")
        link(opacity, alpha, "B")
        to_prop(alpha, unreal.MaterialProperty.MP_OPACITY)
    else:
        use_alpha = scalar_param(mat, "UseAlpha", 0.0, -1450, 450)
        alpha_w = expr(mat, unreal.MaterialExpressionLinearInterpolate, -1150, 350, const_a=1.0)
        link(base, alpha_w, "B", "A")
        link(use_alpha, alpha_w, "Alpha")
        fade = expr(mat, unreal.MaterialExpressionMultiply, -1000, 250)
        link(opacity, fade, "A")
        link(alpha_w, fade, "B")
        final = expr(mat, unreal.MaterialExpressionMultiply, -850, 50)
        link(lit, final, "A")
        link(fade, final, "B")
        to_prop(final, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
        to_prop(opacity, unreal.MaterialProperty.MP_OPACITY)
    finish_material(mat)


# =============================================================================
# Audio synthesis (pure Python)
# =============================================================================

def n_samples(seconds):
    return int(seconds * SAMPLE_RATE)


def silence(seconds):
    return [0.0] * n_samples(seconds)


def env_exp(n, decay, attack=0.002):
    """Exponential decay envelope. decay = time constant in seconds."""
    a = max(1, int(attack * SAMPLE_RATE))
    out = []
    for i in range(n):
        t = i / SAMPLE_RATE
        g = math.exp(-t / decay)
        if i < a:
            g *= i / a
        out.append(g)
    return out


def env_adsr(n, attack, decay, sustain, release):
    a = max(1, int(attack * SAMPLE_RATE))
    d = max(1, int(decay * SAMPLE_RATE))
    r = max(1, int(release * SAMPLE_RATE))
    out = []
    for i in range(n):
        if i < a:
            g = i / a
        elif i < a + d:
            g = 1.0 - (1.0 - sustain) * (i - a) / d
        else:
            g = sustain
        if i > n - r:
            g *= max(0.0, (n - i) / r)
        out.append(g)
    return out


def noise(n, rng):
    return [rng.uniform(-1.0, 1.0) for _ in range(n)]


def osc(n, freq, shape="sine", phase=0.0, fm=None):
    """Oscillator. freq: number or callable(t)->Hz. fm: optional list of per-sample Hz offsets."""
    out = []
    ph = phase
    for i in range(n):
        t = i / SAMPLE_RATE
        f = freq(t) if callable(freq) else freq
        if fm is not None:
            f += fm[i]
        ph += f / SAMPLE_RATE
        ph -= math.floor(ph)
        if shape == "sine":
            v = math.sin(2.0 * math.pi * ph)
        elif shape == "square":
            v = 1.0 if ph < 0.5 else -1.0
        elif shape == "saw":
            v = 2.0 * ph - 1.0
        elif shape == "tri":
            v = 4.0 * abs(ph - 0.5) - 1.0
        else:
            raise ValueError(shape)
        out.append(v)
    return out


def fm_osc(n, carrier, ratio, index):
    """2-operator FM. carrier: Hz or callable(t); index: number or callable(t)."""
    out = []
    pc = 0.0
    pm = 0.0
    for i in range(n):
        t = i / SAMPLE_RATE
        fc = carrier(t) if callable(carrier) else carrier
        idx = index(t) if callable(index) else index
        pm += fc * ratio / SAMPLE_RATE
        pm -= math.floor(pm)
        pc += fc / SAMPLE_RATE
        pc -= math.floor(pc)
        out.append(math.sin(2.0 * math.pi * pc + idx * math.sin(2.0 * math.pi * pm)))
    return out


def svf(x, cutoff, q=0.707, mode="lp"):
    """TPT state-variable filter. cutoff: Hz or callable(t)."""
    out = []
    ic1 = 0.0
    ic2 = 0.0
    k = 1.0 / max(q, 0.05)
    nyq = SAMPLE_RATE * 0.49
    static_g = None
    if not callable(cutoff):
        static_g = math.tan(math.pi * min(max(cutoff, 10.0), nyq) / SAMPLE_RATE)
    for i, v in enumerate(x):
        if static_g is None:
            fc = min(max(cutoff(i / SAMPLE_RATE), 10.0), nyq)
            g = math.tan(math.pi * fc / SAMPLE_RATE)
        else:
            g = static_g
        a1 = 1.0 / (1.0 + g * (g + k))
        a2 = g * a1
        a3 = g * a2
        v3 = v - ic2
        v1 = a1 * ic1 + a2 * v3
        v2 = ic2 + a2 * ic1 + a3 * v3
        ic1 = 2.0 * v1 - ic1
        ic2 = 2.0 * v2 - ic2
        if mode == "lp":
            out.append(v2)
        elif mode == "bp":
            out.append(v1)
        elif mode == "hp":
            out.append(v - k * v1 - v2)
        else:
            raise ValueError(mode)
    return out


def mul(a, b):
    if isinstance(b, (int, float)):
        return [x * b for x in a]
    return [x * y for x, y in zip(a, b)]


def mix(*parts, length=None):
    """Mix (signal, gain[, offset_seconds]) tuples."""
    if length is None:
        length = 0
        for p in parts:
            off = n_samples(p[2]) if len(p) > 2 else 0
            length = max(length, off + len(p[0]))
    out = [0.0] * length
    for p in parts:
        sig, gain = p[0], p[1]
        off = n_samples(p[2]) if len(p) > 2 else 0
        for i, v in enumerate(sig):
            j = off + i
            if j >= length:
                break
            out[j] += v * gain
    return out


def softclip(x, drive=1.0):
    return [math.tanh(v * drive) for v in x]


def finalize(x, peak=0.89):
    # Remove DC, short fades to avoid clicks, normalise.
    if not x:
        return x
    mean = sum(x) / len(x)
    x = [v - mean for v in x]
    fi = min(len(x), n_samples(0.0015))
    fo = min(len(x), n_samples(0.008))
    for i in range(fi):
        x[i] *= i / fi
    for i in range(fo):
        x[len(x) - 1 - i] *= i / fo
    m = max(abs(v) for v in x) or 1.0
    return [v * peak / m for v in x]


def write_wav(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        frames = bytearray()
        for v in samples:
            iv = int(max(-1.0, min(1.0, v)) * 32767.0)
            frames += struct.pack("<h", iv)
        w.writeframes(bytes(frames))


def chime(freq, seconds, decay, rng=None, partials=((1.0, 1.0), (2.0, 0.35), (3.01, 0.15), (4.2, 0.08))):
    n = n_samples(seconds)
    out = [0.0] * n
    for ratio, amp in partials:
        o = osc(n, freq * ratio)
        e = env_exp(n, decay / ratio ** 0.5, attack=0.003)
        for i in range(n):
            out[i] += o[i] * e[i] * amp
    return out


def note(semitones_from_a4):
    return 440.0 * 2.0 ** (semitones_from_a4 / 12.0)


def exp_sweep(f0, f1, dur):
    def f(t):
        u = min(t / dur, 1.0)
        return f0 * (f1 / f0) ** u
    return f


# --- individual sounds --------------------------------------------------------

def snd_fire_hammer(rng):
    n = n_samples(0.55)
    thump = mul(osc(n, exp_sweep(140, 38, 0.25)), env_exp(n, 0.14))
    burst = mul(svf(noise(n, rng), exp_sweep(2500, 200, 0.2), 0.9), env_exp(n, 0.06))
    clank = mul(svf(noise(n, rng), 1800, 6.0, "bp"), env_exp(n, 0.03))
    return softclip(mix((thump, 1.0), (burst, 0.6), (clank, 0.5)), 1.6)


def snd_fire_rifle(rng):
    n = n_samples(0.28)
    crack = mul(svf(noise(n, rng), 1800, 0.7, "hp"), env_exp(n, 0.018, 0.0005))
    body = mul(svf(noise(n, rng), exp_sweep(3000, 400, 0.1)), env_exp(n, 0.05))
    thump = mul(osc(n, exp_sweep(180, 70, 0.08)), env_exp(n, 0.04))
    return softclip(mix((crack, 0.9), (body, 0.8), (thump, 0.7)), 1.4)


def snd_fire_goo(rng):
    n = n_samples(0.42)
    wob = lambda t: 260 * (1.0 - 0.55 * min(t / 0.3, 1.0)) * (1.0 + 0.18 * math.sin(2 * math.pi * 23 * t))
    blob = mul(fm_osc(n, wob, 0.5, lambda t: 2.5 * math.exp(-t / 0.12)), env_adsr(n, 0.01, 0.12, 0.4, 0.2))
    bubbles = mul(svf(noise(n, rng), lambda t: 500 + 400 * math.sin(2 * math.pi * 31 * t), 4.0, "bp"), env_exp(n, 0.12))
    return mix((blob, 0.8), (bubbles, 0.6))


def snd_fire_pulse(rng):
    n = n_samples(0.26)
    zap = mul(fm_osc(n, exp_sweep(1100, 380, 0.22), 1.5, lambda t: 4.0 * math.exp(-t / 0.05)), env_exp(n, 0.08, 0.001))
    hiss = mul(svf(noise(n, rng), 5000, 0.7, "hp"), env_exp(n, 0.02))
    return mix((zap, 1.0), (hiss, 0.25))


def snd_fire_beam(rng):
    n = n_samples(0.5)
    sweep = mul(osc(n, exp_sweep(2400, 260, 0.35), "saw"), env_exp(n, 0.16, 0.001))
    sweep = svf(sweep, exp_sweep(9000, 900, 0.4), 1.5)
    ring = mul(fm_osc(n, 660, 2.01, lambda t: 3.0 * math.exp(-t / 0.15)), env_exp(n, 0.2))
    crack = mul(svf(noise(n, rng), 3000, 0.7, "hp"), env_exp(n, 0.012, 0.0005))
    return mix((sweep, 0.7), (ring, 0.5), (crack, 0.6))


def snd_fire_link(rng):
    n = n_samples(0.3)
    vib = lambda t: 1.0 + 0.03 * math.sin(2 * math.pi * 17 * t)
    a = osc(n, lambda t: 220 * vib(t), "saw")
    b = osc(n, lambda t: 331 * vib(t), "saw")
    buzz = svf(mix((a, 0.6), (b, 0.5)), lambda t: 1800 + 900 * math.sin(2 * math.pi * 9 * t), 3.0)
    crackle = mul(svf(noise(n, rng), 6000, 0.7, "hp"), [0.3 + 0.7 * (rng.random() > 0.9) for _ in range(n)])
    return mul(mix((buzz, 0.9), (crackle, 0.15)), env_adsr(n, 0.01, 0.05, 0.8, 0.12))


def snd_fire_chaingun(rng):
    n = n_samples(0.11)
    crack = mul(svf(noise(n, rng), 2200, 0.8, "hp"), env_exp(n, 0.012, 0.0003))
    body = mul(svf(noise(n, rng), 1400), env_exp(n, 0.025))
    click = mul(osc(n, 3200, "square"), env_exp(n, 0.003, 0.0002))
    return softclip(mix((crack, 0.8), (body, 0.9), (click, 0.2)), 1.5)


def snd_fire_shard(rng):
    n = n_samples(0.38)
    burst = mul(svf(noise(n, rng), exp_sweep(6000, 600, 0.2)), env_exp(n, 0.05, 0.0005))
    metal = [0.0] * n
    for f, amp in ((523.0, 0.5), (1371.0, 0.4), (2217.0, 0.3), (3540.0, 0.2)):
        o = osc(n, f * rng.uniform(0.98, 1.02))
        e = env_exp(n, 0.06 + rng.random() * 0.05)
        for i in range(n):
            metal[i] += o[i] * e[i] * amp
    thump = mul(osc(n, exp_sweep(120, 50, 0.1)), env_exp(n, 0.06))
    return softclip(mix((burst, 0.9), (metal, 0.5), (thump, 0.8)), 1.3)


def snd_fire_rocket(rng):
    n = n_samples(0.7)
    whoosh = mul(svf(noise(n, rng), exp_sweep(300, 3500, 0.5), 1.2), env_adsr(n, 0.04, 0.2, 0.5, 0.35))
    thump = mul(osc(n, exp_sweep(110, 45, 0.15)), env_exp(n, 0.09))
    pop = mul(svf(noise(n, rng), 900), env_exp(n, 0.03))
    return softclip(mix((whoosh, 0.8), (thump, 0.9), (pop, 0.7)), 1.3)


def snd_fire_rail(rng):
    n = n_samples(0.85)
    crack = mul(svf(noise(n, rng), 1500, 0.7, "hp"), env_exp(n, 0.02, 0.0003))
    boom = mul(osc(n, exp_sweep(200, 55, 0.2)), env_exp(n, 0.12))
    tail = mul(svf(noise(n, rng), 4000, 0.7, "hp"), [(rng.random() > 0.93) * 1.0 for _ in range(n)])
    tail = mul(tail, env_exp(n, 0.25))
    zing = mul(fm_osc(n, exp_sweep(3000, 1200, 0.6), 0.5, 2.0), env_exp(n, 0.2))
    return softclip(mix((crack, 1.0), (boom, 0.9), (tail, 0.5), (zing, 0.25)), 1.5)


def explosion(rng, seconds, depth):
    n = n_samples(seconds)
    rumble = mul(svf(noise(n, rng), exp_sweep(2200 / depth, 120 / depth, seconds * 0.6), 0.8), env_exp(n, seconds * 0.28, 0.002))
    crack = mul(svf(noise(n, rng), 1200, 0.7, "hp"), env_exp(n, 0.03, 0.0005))
    sub = mul(osc(n, exp_sweep(75 / depth, 28, seconds * 0.4)), env_exp(n, seconds * 0.2, 0.003))
    debris = mul(svf(noise(n, rng), 2500, 2.0, "bp"),
                 [(rng.random() > 0.995) * 1.0 for _ in range(n)])
    debris = svf(debris, 3000)
    debris = mul(debris, env_exp(n, seconds * 0.35))
    return softclip(mix((rumble, 1.0), (crack, 0.5), (sub, 0.9), (debris, 0.8)), 1.8)


def snd_explosion(rng):
    return explosion(rng, 1.0, 1.0)


def snd_explosion_big(rng):
    return explosion(rng, 1.7, 1.6)


def snd_impact(rng):
    n = n_samples(0.16)
    thud = mul(osc(n, exp_sweep(220, 90, 0.05)), env_exp(n, 0.03))
    tick = mul(svf(noise(n, rng), 3000, 1.0, "bp"), env_exp(n, 0.01, 0.0003))
    return mix((thud, 0.8), (tick, 0.7))


def snd_pickup_health(rng):
    a = chime(note(3), 0.55, 0.25)   # C5
    b = chime(note(10), 0.45, 0.25)  # G5
    return mix((a, 0.7), (b, 0.7, 0.08))


def snd_pickup_shield(rng):
    n = n_samples(0.65)
    trem = [0.75 + 0.25 * math.sin(2 * math.pi * 14 * i / SAMPLE_RATE) for i in range(n)]
    tones = mix((osc(n, exp_sweep(note(-2), note(5), 0.4)), 0.5),
                (osc(n, exp_sweep(note(2), note(9), 0.4)), 0.4),
                (osc(n, exp_sweep(note(5), note(12), 0.4), "tri"), 0.3))
    shimmer = mul(svf(noise(n, rng), 7000, 3.0, "bp"), 0.15)
    return mul(mul(mix((tones, 1.0), (shimmer, 1.0)), trem), env_adsr(n, 0.04, 0.15, 0.7, 0.3))


def snd_pickup_weapon(rng):
    n = n_samples(0.18)
    clack = mul(svf(noise(n, rng), 2000, 3.0, "bp"), env_exp(n, 0.015, 0.0003))
    clack2 = mul(svf(noise(n, rng), 1300, 3.0, "bp"), env_exp(n, 0.02, 0.0003))
    c = chime(note(-2), 0.5, 0.18, partials=((1.0, 1.0), (2.76, 0.4), (5.4, 0.2)))
    return mix((clack, 0.9), (clack2, 0.8, 0.06), (c, 0.6, 0.05))


def snd_pickup_ammo(rng):
    n = n_samples(0.06)
    def click():
        return mul(svf(noise(n, rng), 2600, 4.0, "bp"), env_exp(n, 0.008, 0.0002))
    m = n_samples(0.08)
    return mix((click(), 1.0), (click(), 0.8, 0.07), (mul(osc(m, 1250), env_exp(m, 0.02)), 0.3, 0.1))


def snd_pickup_power(rng):
    parts = []
    for k, semi in enumerate((3, 7, 10, 15)):
        parts.append((chime(note(semi), 0.6, 0.22), 0.55, 0.07 * k))
    n = n_samples(0.95)
    sparkle = mul(svf(noise(n, rng), exp_sweep(3000, 10000, 0.9), 6.0, "bp"), env_adsr(n, 0.1, 0.3, 0.5, 0.4))
    parts.append((sparkle, 0.35))
    pad = mul(osc(n, note(-9), "tri"), env_adsr(n, 0.05, 0.3, 0.4, 0.4))
    parts.append((pad, 0.3))
    return mix(*parts)


def snd_jump(rng):
    n = n_samples(0.16)
    puff = mul(svf(noise(n, rng), 900, 1.0, "bp"), env_adsr(n, 0.01, 0.05, 0.3, 0.08))
    tone = mul(osc(n, exp_sweep(190, 300, 0.12)), env_exp(n, 0.05))
    return mix((puff, 0.7), (tone, 0.35))


def snd_dodge(rng):
    n = n_samples(0.32)
    whoosh = svf(noise(n, rng), lambda t: 400 + 2600 * math.sin(math.pi * min(t / 0.3, 1.0)), 2.0, "bp")
    return mul(whoosh, env_adsr(n, 0.05, 0.1, 0.6, 0.18))


def snd_land(rng):
    n = n_samples(0.2)
    thud = mul(osc(n, exp_sweep(110, 50, 0.1)), env_exp(n, 0.05))
    scuff = mul(svf(noise(n, rng), 700), env_exp(n, 0.04))
    return mix((thud, 1.0), (scuff, 0.6))


def snd_jump_pad(rng):
    n = n_samples(0.55)
    boing = mul(fm_osc(n, exp_sweep(140, 950, 0.45), 2.0, lambda t: 1.5 + 2.0 * math.exp(-t / 0.1)), env_adsr(n, 0.01, 0.2, 0.6, 0.2))
    whoosh = mul(svf(noise(n, rng), exp_sweep(500, 6000, 0.5), 1.5, "bp"), env_adsr(n, 0.05, 0.2, 0.5, 0.2))
    return mix((boing, 0.6), (whoosh, 0.5))


def vocal(rng, seconds, f0, f1, formants, breath=0.2):
    n = n_samples(seconds)
    src = osc(n, lambda t: (f0 + (f1 - f0) * min(t / seconds, 1.0)) * (1.0 + 0.02 * math.sin(2 * math.pi * 6 * t)), "saw")
    src = mix((src, 1.0), (noise(n, rng), breath))
    out = [0.0] * n
    for fc, q, g in formants:
        f = svf(src, fc, q, "bp")
        for i in range(n):
            out[i] += f[i] * g
    return out


def snd_hurt(rng):
    v = vocal(rng, 0.28, 170, 120, ((650, 5.0, 1.0), (1100, 6.0, 0.6), (2500, 8.0, 0.3)), 0.3)
    return mul(v, env_adsr(len(v), 0.01, 0.08, 0.6, 0.15))


def snd_death(rng):
    v = vocal(rng, 0.95, 160, 70, ((700, 5.0, 1.0), (1150, 6.0, 0.5), (2600, 8.0, 0.25)), 0.4)
    n = len(v)
    body = mul(svf(noise(n, rng), 300), env_exp(n, 0.1))
    return mul(mix((v, 1.0), (body, 0.4, 0.55)), env_adsr(n, 0.02, 0.3, 0.6, 0.45))


def snd_hit(rng):
    n = n_samples(0.09)
    return mix((mul(osc(n, 1800, "tri"), env_exp(n, 0.025, 0.0005)), 0.8),
               (mul(osc(n, 2700), env_exp(n, 0.015, 0.0005)), 0.4))


def snd_weapon_switch(rng):
    n = n_samples(0.05)
    def clk(fc):
        return mul(svf(noise(n, rng), fc, 5.0, "bp"), env_exp(n, 0.007, 0.0002))
    slide = mul(svf(noise(n_samples(0.08), rng), 1500, 1.0, "bp"), 0.12)
    return mix((clk(2400), 1.0), (slide, 1.0, 0.03), (clk(1600), 0.9, 0.12))


def two_tone(f_a, f_b, each, gap, reps, shape="square", cutoff=3000):
    parts = []
    t = 0.0
    for r in range(reps):
        for f in (f_a, f_b):
            n = n_samples(each)
            s = mul(svf(osc(n, f, shape), cutoff), env_adsr(n, 0.005, 0.03, 0.7, 0.03))
            parts.append((s, 0.6, t))
            t += each + gap
    return mix(*parts)


def snd_flag_taken(rng):
    return two_tone(note(2), note(9), 0.13, 0.02, 3, "square", 2500)


def snd_flag_captured(rng):
    parts = []
    seq = [(note(-2), 0.0), (note(2), 0.12), (note(5), 0.24), (note(10), 0.4)]
    for f, t in seq:
        d = 0.8 if t >= 0.4 else 0.3
        n = n_samples(d)
        s = mix((osc(n, f, "saw"), 0.5), (osc(n, f * 1.005, "saw"), 0.5), (osc(n, f * 2, "sine"), 0.3))
        s = svf(s, 3500, 0.8)
        parts.append((mul(s, env_adsr(n, 0.01, 0.1, 0.6, 0.3 if d > 0.5 else 0.08)), 0.5, t))
    return mix(*parts)


def snd_flag_returned(rng):
    return two_tone(note(9), note(2), 0.18, 0.03, 1, "tri", 4000)


def snd_combo(rng):
    n = n_samples(0.85)
    a = osc(n, exp_sweep(110, 880, 0.6), "saw")
    b = osc(n, exp_sweep(111.5, 892, 0.6), "saw")
    s = svf(mix((a, 0.5), (b, 0.5)), exp_sweep(400, 6000, 0.6), 4.0)
    sparkle = mul(svf(noise(n, rng), 8000, 4.0, "bp"), env_adsr(n, 0.3, 0.2, 0.6, 0.3))
    return mul(mix((s, 1.0), (sparkle, 0.3)), env_adsr(n, 0.02, 0.4, 0.7, 0.3))


def snd_menu_move(rng):
    n = n_samples(0.05)
    return mul(osc(n, 1500, "tri"), env_exp(n, 0.012, 0.0005))


def snd_menu_select(rng):
    n = n_samples(0.08)
    a = mul(osc(n, 990, "tri"), env_exp(n, 0.03, 0.0005))
    b = mul(osc(n, 1480, "tri"), env_exp(n, 0.03, 0.0005))
    return mix((a, 0.8), (b, 0.8, 0.06))


def snd_respawn(rng):
    n = n_samples(0.75)
    rise = mul(fm_osc(n, exp_sweep(200, 1200, 0.6), 1.5, 1.2), env_adsr(n, 0.1, 0.3, 0.6, 0.3))
    shimmer = mul(svf(noise(n, rng), exp_sweep(1000, 9000, 0.7), 5.0, "bp"), env_adsr(n, 0.2, 0.2, 0.5, 0.3))
    return mix((rise, 0.6), (shimmer, 0.4))


def snd_empty(rng):
    n = n_samples(0.08)
    click = mul(svf(noise(n, rng), 3500, 6.0, "bp"), env_exp(n, 0.006, 0.0002))
    tone = mul(osc(n, 600, "square"), env_exp(n, 0.008))
    return mix((click, 1.0), (svf(tone, 2000), 0.15))


def snd_countdown(rng):
    n = n_samples(0.22)
    s = mix((osc(n, 880), 0.8), (osc(n, 1760), 0.2))
    return mul(s, env_adsr(n, 0.005, 0.05, 0.7, 0.08))


def snd_spree(rng):
    n = n_samples(0.85)
    chord = [0.0] * n
    for semi in (-9, -5, -2, 3):
        f = note(semi)
        for det in (0.997, 1.003):
            o = osc(n, f * det, "saw")
            for i in range(n):
                chord[i] += o[i]
    chord = svf(chord, exp_sweep(5000, 1200, 0.8), 1.0)
    hit = mul(svf(noise(n, rng), 3000, 0.7, "hp"), env_exp(n, 0.03))
    return mul(mix((chord, 0.25), (hit, 0.5)), env_adsr(n, 0.005, 0.25, 0.55, 0.35))


SOUNDS = [
    ("S_Fire_Hammer", snd_fire_hammer),
    ("S_Fire_Rifle", snd_fire_rifle),
    ("S_Fire_Goo", snd_fire_goo),
    ("S_Fire_Pulse", snd_fire_pulse),
    ("S_Fire_Beam", snd_fire_beam),
    ("S_Fire_Link", snd_fire_link),
    ("S_Fire_Chaingun", snd_fire_chaingun),
    ("S_Fire_Shard", snd_fire_shard),
    ("S_Fire_Rocket", snd_fire_rocket),
    ("S_Fire_Rail", snd_fire_rail),
    ("S_Explosion", snd_explosion),
    ("S_ExplosionBig", snd_explosion_big),
    ("S_Impact", snd_impact),
    ("S_Pickup_Health", snd_pickup_health),
    ("S_Pickup_Shield", snd_pickup_shield),
    ("S_Pickup_Weapon", snd_pickup_weapon),
    ("S_Pickup_Ammo", snd_pickup_ammo),
    ("S_Pickup_Power", snd_pickup_power),
    ("S_Jump", snd_jump),
    ("S_Dodge", snd_dodge),
    ("S_Land", snd_land),
    ("S_JumpPad", snd_jump_pad),
    ("S_Hurt", snd_hurt),
    ("S_Death", snd_death),
    ("S_Hit", snd_hit),
    ("S_WeaponSwitch", snd_weapon_switch),
    ("S_Flag_Taken", snd_flag_taken),
    ("S_Flag_Captured", snd_flag_captured),
    ("S_Flag_Returned", snd_flag_returned),
    ("S_Combo", snd_combo),
    ("S_MenuMove", snd_menu_move),
    ("S_MenuSelect", snd_menu_select),
    ("S_Respawn", snd_respawn),
    ("S_Empty", snd_empty),
    ("S_Countdown", snd_countdown),
    ("S_Spree", snd_spree),
]


def build_audio():
    tmp = tempfile.mkdtemp(prefix="openut2k4_audio_")
    try:
        tasks = []
        for idx, (name, fn) in enumerate(SOUNDS):
            rng = random.Random(0x2026 + idx * 7919)  # deterministic output
            samples = finalize(fn(rng))
            path = os.path.join(tmp, name + ".wav")
            write_wav(path, samples)
            task = unreal.AssetImportTask()
            task.set_editor_property("filename", path)
            task.set_editor_property("destination_path", AUDIO_DIR)
            task.set_editor_property("destination_name", name)
            task.set_editor_property("automated", True)
            task.set_editor_property("replace_existing", True)
            task.set_editor_property("save", True)
            tasks.append((name, task, len(samples)))
        unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([t for _, t, _ in tasks])
        for name, task, count in tasks:
            asset_path = "%s/%s" % (AUDIO_DIR, name)
            imported = list(task.get_editor_property("imported_object_paths") or [])
            asset = EAL.load_asset(asset_path) if EAL.does_asset_exist(asset_path) else None
            if asset is None or not isinstance(asset, unreal.SoundWave):
                error("sound import failed: %s (imported=%s)" % (asset_path, imported))
                continue
            EAL.save_loaded_asset(asset, only_if_is_dirty=False)
            log("sound %s (%.2f s)" % (asset_path, count / float(SAMPLE_RATE)))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)



# =============================================================================
# Masters for UT2004 content (our own material graphs, no UT2004 data)
# =============================================================================

def build_particle_material(name, blend):
    """UE2 particle material for Source/OpenUT2k4/ArenaEmitters.* (UArenaParticleComponent).

    Unlit, two-sided; texture "Base" (UV 0) times the vertex colour, scalar "Intensity". blend "add": additive,
    Base.rgb * VertexColor.rgb * VertexColor.a; "alpha": translucent, opacity Base.a * VertexColor.a;
    "mod": modulate, lerp(1, Base.rgb * VertexColor.rgb, VertexColor.a).
    """
    mat = get_or_create_material(name)
    modes = {"add": unreal.BlendMode.BLEND_ADDITIVE, "alpha": unreal.BlendMode.BLEND_TRANSLUCENT,
             "mod": unreal.BlendMode.BLEND_MODULATE}
    mat.set_editor_property("blend_mode", modes[blend])
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property("two_sided", True)
    set_usages(mat)

    tc = expr(mat, unreal.MaterialExpressionTextureCoordinate, -1600, 0)
    base = expr(mat, unreal.MaterialExpressionTextureSampleParameter2D, -1400, 0, parameter_name="Base")
    default_tex = EAL.load_asset("/Engine/EngineResources/WhiteSquareTexture")
    if default_tex is not None:
        base.set_editor_property("texture", default_tex)
    link(tc, base, "UVs")
    vc = expr(mat, unreal.MaterialExpressionVertexColor, -1400, 300)
    intensity = scalar_param(mat, "Intensity", 1.0, -1400, 450)

    rgb = expr(mat, unreal.MaterialExpressionMultiply, -1100, 0)
    link(base, rgb, "A", "RGB")
    link(vc, rgb, "B", "")
    if blend == "add":
        faded = expr(mat, unreal.MaterialExpressionMultiply, -950, 50)
        link(rgb, faded, "A")
        link(vc, faded, "B", "A")
        lit = expr(mat, unreal.MaterialExpressionMultiply, -800, 50)
        link(faded, lit, "A")
        link(intensity, lit, "B")
        to_prop(lit, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    elif blend == "alpha":
        lit = expr(mat, unreal.MaterialExpressionMultiply, -800, 0)
        link(rgb, lit, "A")
        link(intensity, lit, "B")
        to_prop(lit, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
        alpha = expr(mat, unreal.MaterialExpressionMultiply, -800, 250)
        link(base, alpha, "A", "A")
        link(vc, alpha, "B", "A")
        to_prop(alpha, unreal.MaterialProperty.MP_OPACITY)
    else:
        mix = expr(mat, unreal.MaterialExpressionLinearInterpolate, -800, 50, const_a=1.0)
        link(rgb, mix, "B")
        link(vc, mix, "Alpha", "A")
        to_prop(mix, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    finish_material(mat)


def build_sky_face_material():
    """Unlit opaque face of the UE2 3D sky box cube (Source/OpenUT2k4/ArenaSkybox.*): texture "Base" (a render
    target with one face of the sky zone capture), UV 0, scalar "Intensity"."""
    mat = get_or_create_material("M_ArenaSkyFace")
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property("two_sided", True)
    set_usages(mat)
    tc = expr(mat, unreal.MaterialExpressionTextureCoordinate, -1000, 0)
    base = expr(mat, unreal.MaterialExpressionTextureSampleParameter2D, -800, 0, parameter_name="Base")
    default_tex = EAL.load_asset("/Engine/EngineResources/WhiteSquareTexture")
    if default_tex is not None:
        base.set_editor_property("texture", default_tex)
    link(tc, base, "UVs")
    intensity = scalar_param(mat, "Intensity", 1.0, -800, 300)
    lit = expr(mat, unreal.MaterialExpressionMultiply, -500, 0)
    link(base, lit, "A", "RGB")
    link(intensity, lit, "B")
    to_prop(lit, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    finish_material(mat)


def build_ut2004_master(name, blend, unlit, two_sided):
    """One base texture ("Base") with UT2004 TexScaler / TexRotator / TexPanner UVs and the parameters the
    runtime content provider sets (Source/OpenUT2k4/ArenaRuntimeMaterials.cpp, same graph as the editor
    importer's masters in Tools/ImportUT2004.py build_master). Two-sidedness cannot be changed on dynamic
    instances in cooked builds, hence the _2S variants."""
    mat = get_or_create_material(name, UT2004_MATERIAL_DIR)
    mat.set_editor_property("blend_mode", blend)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT if unlit
                            else unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    mat.set_editor_property("two_sided", two_sided)
    if blend == unreal.BlendMode.BLEND_MASKED:
        mat.set_editor_property("opacity_mask_clip_value", 0.5)
    set_usages(mat)

    base = expr(mat, unreal.MaterialExpressionTextureSampleParameter2D, -900, 0, parameter_name="Base")
    white = unreal.load_asset("/Engine/EngineResources/WhiteSquareTexture")
    if white is not None:
        base.set_editor_property("texture", white)
    # uv' = (u a + v c + e, u b + v d + f) + frac(time * pan); UVTransform = (a, b, c, d), UVOffsetPan = (e, f, pan u, pan v)
    tc = expr(mat, unreal.MaterialExpressionTextureCoordinate, -2100, 0)
    xf = vec_param(mat, "UVTransform", (1.0, 0.0, 0.0, 1.0), -2100, 150)
    op = vec_param(mat, "UVOffsetPan", (0.0, 0.0, 0.0, 0.0), -2100, 350)
    u = expr(mat, unreal.MaterialExpressionComponentMask, -1850, -50, r=True, g=False, b=False, a=False)
    v = expr(mat, unreal.MaterialExpressionComponentMask, -1850, 50, r=False, g=True, b=False, a=False)
    link(tc, u)
    link(tc, v)

    def pair(src, x, y, first, second):
        a = expr(mat, unreal.MaterialExpressionAppendVector, x, y)
        link(src, a, "A", first)
        link(src, a, "B", second)
        return a

    ab = pair(xf, -1850, 150, "R", "G")
    cd = pair(xf, -1850, 250, "B", "A")
    ef = pair(op, -1850, 350, "R", "G")
    pan = pair(op, -1850, 450, "B", "A")
    ua = expr(mat, unreal.MaterialExpressionMultiply, -1600, 0)
    link(u, ua, "A")
    link(ab, ua, "B")
    vc = expr(mat, unreal.MaterialExpressionMultiply, -1600, 150)
    link(v, vc, "A")
    link(cd, vc, "B")
    lin = expr(mat, unreal.MaterialExpressionAdd, -1450, 50)
    link(ua, lin, "A")
    link(vc, lin, "B")
    off = expr(mat, unreal.MaterialExpressionAdd, -1300, 100)
    link(lin, off, "A")
    link(ef, off, "B")
    tm = expr(mat, unreal.MaterialExpressionTime, -1600, 450)
    pt = expr(mat, unreal.MaterialExpressionMultiply, -1450, 400)
    link(tm, pt, "A")
    link(pan, pt, "B")
    pf = expr(mat, unreal.MaterialExpressionFrac, -1300, 400)
    link(pt, pf)
    uv = expr(mat, unreal.MaterialExpressionAdd, -1150, 150)
    link(off, uv, "A")
    link(pf, uv, "B")
    link(uv, base, "UVs")
    tint = vec_param(mat, "Tint", (1.0, 1.0, 1.0, 1.0), -900, -250)
    color = expr(mat, unreal.MaterialExpressionMultiply, -600, -100)
    link(base, color, "A", "RGB")
    link(tint, color, "B", "RGB")

    if unlit:
        intensity = scalar_param(mat, "Intensity", 1.0, -900, 250)
        em = expr(mat, unreal.MaterialExpressionMultiply, -400, 0)
        link(color, em, "A")
        link(intensity, em, "B")
        to_prop(em, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    else:
        to_prop(color, unreal.MaterialProperty.MP_BASE_COLOR)
        to_prop(scalar_param(mat, "Roughness", 0.8, -600, 150), unreal.MaterialProperty.MP_ROUGHNESS)
        to_prop(scalar_param(mat, "Specular", 0.3, -600, 250), unreal.MaterialProperty.MP_SPECULAR)
        emissive = scalar_param(mat, "Emissive", 0.0, -600, 350)
        # EmissiveAlpha: glow where the Base alpha is set (self-illumination masked by the diffuse alpha)
        em_alpha = scalar_param(mat, "EmissiveAlpha", 0.0, -600, 450)
        ea = expr(mat, unreal.MaterialExpressionMultiply, -500, 450)
        link(base, ea, "A", "A")
        link(em_alpha, ea, "B")
        em_sum = expr(mat, unreal.MaterialExpressionAdd, -450, 380)
        link(emissive, em_sum, "A")
        link(ea, em_sum, "B")
        em = expr(mat, unreal.MaterialExpressionMultiply, -400, 300)
        link(color, em, "A")
        link(em_sum, em, "B")
        to_prop(em, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    if blend in (unreal.BlendMode.BLEND_MASKED, unreal.BlendMode.BLEND_TRANSLUCENT):
        opacity = scalar_param(mat, "Opacity", 1.0, -900, 400)
        alpha = expr(mat, unreal.MaterialExpressionMultiply, -400, 450)
        link(base, alpha, "A", "A")
        link(opacity, alpha, "B")
        to_prop(alpha, unreal.MaterialProperty.MP_OPACITY_MASK if blend == unreal.BlendMode.BLEND_MASKED
                else unreal.MaterialProperty.MP_OPACITY)
    finish_material(mat)


def build_ut2004_masters():
    if not EAL.does_directory_exist(UT2004_MATERIAL_DIR):
        EAL.make_directory(UT2004_MATERIAL_DIR)
    specs = [
        ("M_ImportedOpaque", unreal.BlendMode.BLEND_OPAQUE, False),
        ("M_ImportedMasked", unreal.BlendMode.BLEND_MASKED, False),
        ("M_ImportedTranslucent", unreal.BlendMode.BLEND_TRANSLUCENT, False),
        ("M_ImportedAdditive", unreal.BlendMode.BLEND_ADDITIVE, True),
        ("M_ImportedModulate", unreal.BlendMode.BLEND_MODULATE, True),
    ]
    for name, blend, unlit in specs:
        build_ut2004_master(name, blend, unlit, False)
        build_ut2004_master(name + "_2S", blend, unlit, True)

# =============================================================================
# Maps
# =============================================================================

def build_map(name, game_mode_path):
    path = "%s/%s" % (MAPS_DIR, name)
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    ues = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    if EAL.does_asset_exist(path):
        if not les.load_level(path):
            error("could not load existing map " + path)
            return
        log("loaded existing map " + path)
    else:
        if not les.new_level(path, False):
            error("could not create map " + path)
            return
        log("created map " + path)

    world = ues.get_editor_world()
    if world is None:
        error("no editor world after opening " + path)
        return
    ws = world.get_world_settings()
    cls = None
    try:
        cls = unreal.load_class(None, game_mode_path)
    except Exception:
        cls = None
    if cls is not None:
        ws.set_editor_property("default_game_mode", cls)
        log("%s: default game mode = %s" % (path, game_mode_path))
    else:
        warn("%s: class %s not available, leaving game mode override unset (config aliases apply)"
             % (path, game_mode_path))
    if not les.save_current_level():
        error("could not save map " + path)


# =============================================================================

def main():
    for d in (MATERIAL_DIR, AUDIO_DIR, MAPS_DIR):
        if not EAL.does_directory_exist(d):
            EAL.make_directory(d)

    steps = [
        ("surface material", build_surface_material),
        ("glow material", build_glow_material),
        ("sprite material", lambda: build_sprite_material("M_ArenaSprite", False)),
        ("sprite alpha material", lambda: build_sprite_material("M_ArenaSpriteAlpha", True)),
        ("particle materials", lambda: [build_particle_material("M_ArenaParticle" + n, b)
                                        for n, b in (("Add", "add"), ("Alpha", "alpha"), ("Mod", "mod"))]),
        ("sky face material", build_sky_face_material),
        ("ut2004 masters", build_ut2004_masters),
        ("audio", build_audio),
        ("MainMenu map", lambda: build_map("MainMenu", "/Script/OpenUT2k4.ArenaMenuGameMode")),
        ("Arena map", lambda: build_map("Arena", "/Script/OpenUT2k4.ArenaGameMode")),
    ]
    # OPENUT2K4_GENERATE_ONLY="sprite,glow" rebuilds only the steps whose label contains one of the words.
    only = [w.strip() for w in os.environ.get("OPENUT2K4_GENERATE_ONLY", "").split(",") if w.strip()]
    if only:
        steps = [s for s in steps if any(w in s[0] for w in only)]
    for label, fn in steps:
        try:
            fn()
        except Exception as ex:
            import traceback
            error("%s failed: %s\n%s" % (label, ex, traceback.format_exc()))

    for d in (MATERIAL_DIR, AUDIO_DIR):
        EAL.save_directory(d, only_if_is_dirty=False, recursive=True)

    if _errors:
        raise RuntimeError("GenerateContent finished with %d error(s)" % len(_errors))
    log("done, all content generated")


main()
