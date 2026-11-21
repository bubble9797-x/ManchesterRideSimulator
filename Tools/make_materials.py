# Creates the project's runtime materials. Ground and walls use CC0 photo textures (Tools/import_textures.py
# imports them first); everything else is computed in the shader.
#
#   M_MRSurface  vehicles/props: Color, Metallic, Roughness, Emissive, Kind (micro detail), ClearCoat (lacquer gloss)
#   M_MRGround   map ground/roads (CC0 photo textures): Color, Roughness, Tex (surface, see below)
#   M_MRFacade   buildings: Color, Glass (0 masonry .. 1 curtain wall), FloorHeight, BaySpacing, Night, Wall (photo texture)
#   M_MRRider    rider clothes, hair and helmet: fabric / leather / paint patterns (see below)
#   M_MRSkin     rider skin (subsurface): pores, mottling, lips
#   M_MRFoliage  tree canopies (two-sided foliage, leaf clusters) and bark
#
# Run (editor closed):  UnrealEditor-Cmd.exe <project> -run=pythonscript -script="<this file>"
# Tools/make_materials.bat does exactly that.
import unreal

MEL = unreal.MaterialEditingLibrary
EAL = unreal.EditorAssetLibrary
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()
FOLDER = '/Game/MR/Materials'

NOISE = '''
struct FMRNoise
{
    float H(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }
    float V(float2 p)
    {
        float2 i = floor(p); float2 f = frac(p); f = f * f * (3.0 - 2.0 * f);
        return lerp(lerp(H(i), H(i + float2(1, 0)), f.x), lerp(H(i + float2(0, 1)), H(i + float2(1, 1)), f.x), f.y);
    }
    float Fbm(float2 p) { return V(p) * 0.5 + V(p * 2.03) * 0.3 + V(p * 4.11) * 0.2; }
};
FMRNoise N;
'''


def new_material(name):
    path = FOLDER + '/' + name
    if EAL.does_asset_exist(path):
        EAL.delete_asset(path)
    return TOOLS.create_asset(name, FOLDER, unreal.Material, unreal.MaterialFactoryNew())


def scalar(m, name, default, x, y):
    e = MEL.create_material_expression(m, unreal.MaterialExpressionScalarParameter, x, y)
    e.set_editor_property('parameter_name', name)
    e.set_editor_property('default_value', default)
    return e


def vector(m, name, default, x, y):
    e = MEL.create_material_expression(m, unreal.MaterialExpressionVectorParameter, x, y)
    e.set_editor_property('parameter_name', name)
    e.set_editor_property('default_value', unreal.LinearColor(*default))
    return e


def custom(m, code, inputs, out_type, x, y):
    """inputs: list of (name, expression, output_name)."""
    e = MEL.create_material_expression(m, unreal.MaterialExpressionCustom, x, y)
    e.set_editor_property('code', code)
    e.set_editor_property('output_type', out_type)
    ins = []
    for name, _, _ in inputs:
        ci = unreal.CustomInput()
        ci.set_editor_property('input_name', name)
        ins.append(ci)
    e.set_editor_property('inputs', ins)
    for name, src, out in inputs:
        MEL.connect_material_expressions(src, out, e, name)
    return e


def finish(m):
    MEL.layout_material_expressions(m)
    MEL.recompile_material(m)
    EAL.save_asset(m.get_path_name())
    unreal.log('MR: saved ' + m.get_path_name())


F3 = unreal.CustomMaterialOutputType.CMOT_FLOAT3
F1 = unreal.CustomMaterialOutputType.CMOT_FLOAT1

# ---------------------------------------------------------------- photo-texture helpers
# CC0 photo textures from ambientCG (Tools/textures_src, imported by Tools/import_textures.py).
# Sampled in Custom HLSL through the engine's shared wrap sampler, so a material can use more
# textures than it has sampler slots. Each surface keeps its own colour (the Color parameter or the
# building's vertex colour) and takes the photo's detail as a ratio to the photo's average colour,
# so a red cycle lane stays red and every terrace keeps its own brick shade.
TEX = '/Game/MR/Textures/T_'


def texobj(m, name, kind, x, y):
    e = MEL.create_material_expression(m, unreal.MaterialExpressionTextureObject, x, y)
    e.set_editor_property('texture', EAL.load_asset(TEX + name))
    if kind == 'n':
        e.set_editor_property('sampler_type', unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
    elif kind == 'r':
        e.set_editor_property('sampler_type', unreal.MaterialSamplerType.SAMPLERTYPE_GRAYSCALE)
    return e


def tex_inputs(m, sets, x0, y0):
    """sets: list of (prefix, ambientCG id). Returns custom-node inputs <prefix>C / N / R."""
    ins = []
    for i, (pre, aid) in enumerate(sets):
        ins.append((pre + 'C', texobj(m, aid + '_Color', 'c', x0, y0 + i * 300), ''))
        ins.append((pre + 'N', texobj(m, aid + '_NormalDX', 'n', x0, y0 + i * 300 + 100), ''))
        ins.append((pre + 'R', texobj(m, aid + '_Roughness', 'r', x0, y0 + i * 300 + 200), ''))
    return ins


def sample_block(pre, uv1, uv2, w):
    """HLSL that fills tc / tavg / tn / tr from texture set <pre>, blending two projections by w."""
    return f'''{{
        tc = lerp({pre}C.SampleGrad(SS, {uv1}, ddx({uv1}), ddy({uv1})).rgb, {pre}C.SampleGrad(SS, {uv2}, ddx({uv2}), ddy({uv2})).rgb, {w});
        tavg = {pre}C.SampleLevel(SS, float2(0.5, 0.5), 12.0).rgb;
        float2 nn = lerp({pre}N.SampleGrad(SS, {uv1}, ddx({uv1}), ddy({uv1})).rg, {pre}N.SampleGrad(SS, {uv2}, ddx({uv2}), ddy({uv2})).rg, {w}) * 2.0 - 1.0;
        tn = float3(nn, sqrt(saturate(1.0 - dot(nn, nn))));
        tr = lerp({pre}R.SampleGrad(SS, {uv1}, ddx({uv1}), ddy({uv1})).r, {pre}R.SampleGrad(SS, {uv2}, ddx({uv2}), ddy({uv2})).r, {w});
    }}'''


# ---------------------------------------------------------------- M_MRGround
# Tex: 0 asphalt, 1 worn/cracked asphalt, 2 concrete flags, 3 granite setts, 4 grass, 5 woodland
# floor, 6 dirt, 7 gravel / ballast, 8 water (procedural ripples, no photo).
GROUND_SETS = [('As', 'Road008A'), ('Aw', 'Road013A'), ('Fl', 'Concrete047A'), ('St', 'PavingStones119'),
               ('Gr', 'Grass004'), ('Wd', 'Ground106'), ('Dt', 'Ground048'), ('Gv', 'Gravel023')]
GROUND_TILE = [320.0, 320.0, 240.0, 130.0, 170.0, 240.0, 220.0, 190.0]
GROUND_LIB = NOISE + '''
SamplerState SS = View.MaterialTextureBilinearWrapedSampler;
float2 q = P.xy;  // cm
float tex = floor(Tex + 0.5);
float macro = N.Fbm(q / 900.0);
float tile = ''' + ' : '.join(f'tex < {i}.5 ? {t:.1f}' for i, t in enumerate(GROUND_TILE)) + ''' : 200.0;
float2 uv1 = q / tile;
float2 uv2 = float2(q.x * 0.6 - q.y * 0.8, q.x * 0.8 + q.y * 0.6) / (tile * 2.7) + 0.37; // larger, rotated copy hides repeats
float mixw = smoothstep(0.3, 0.7, N.Fbm(q / 650.0 + 11.0)) * 0.6;
float3 tc = Color.rgb, tavg = Color.rgb, tn = float3(0, 0, 1);
float tr = Roughness;
'''
for i, (pre, _) in enumerate(GROUND_SETS):
    GROUND_LIB += ('if' if i == 0 else 'else if') + f' (tex < {i}.5) ' + sample_block(pre, 'uv1', 'uv2', 'mixw') + '\n'
GROUND_LIB += '''
// Keep the surface's own colour; take the photo's detail. Then large-scale wear and repairs.
float3 c = tex < 7.5 ? Color.rgb * tc / max(tavg, float3(0.02, 0.02, 0.02)) : Color.rgb;
float r = tex < 7.5 ? tr : Roughness;
c *= lerp(0.85, 1.12, macro);
if (tex > 1.5 && tex < 2.5) {           // concrete flags: 60 cm slabs, each a slightly different shade, dark joints
    float2 g = frac(q / 60.0);
    float d = min(min(g.x, 1.0 - g.x), min(g.y, 1.0 - g.y)) * 60.0;
    float joint = 1.0 - smoothstep(0.4, 1.2, d);
    c *= lerp(0.88, 1.08, N.H(floor(q / 60.0))) * lerp(1.0, 0.55, joint);
    r = lerp(r, 1.0, joint * 0.5);
    tn.xy += float2(g.x < 0.5 ? 1.0 : -1.0, g.y < 0.5 ? 1.0 : -1.0) * joint * 0.25 * step(d, 1.2);
}
if (tex > 3.5 && tex < 4.5) {           // grass: dry, yellowed patches and clumps
    c = lerp(c, float3(0.16, 0.14, 0.08), smoothstep(0.72, 0.9, N.Fbm(q / 400.0)) * 0.5);
    c = lerp(c, c * float3(1.3, 1.12, 0.55), smoothstep(0.55, 0.8, N.Fbm(q / 180.0 + 3.0)) * 0.45);
}
if (tex > 7.5) {                        // water: dark with slow ripples
    c *= lerp(0.85, 1.15, N.Fbm(q / 250.0));
    float2 rp = float2(N.V(q / 40.0) - N.V(q / 40.0 + float2(0.7, 0)), N.V(q / 40.0) - N.V(q / 40.0 + float2(0, 0.7)));
    tn = normalize(float3(rp * 0.15, 1.0));
    r = 0.05;
}
'''
m = new_material('M_MRGround')
m.set_editor_property('tangent_space_normal', False)
wp = MEL.create_material_expression(m, unreal.MaterialExpressionWorldPosition, -1400, 0)
vn = MEL.create_material_expression(m, unreal.MaterialExpressionVertexNormalWS, -1400, 100)
col = vector(m, 'Color', (0.1, 0.1, 0.1, 1), -1400, 200)
rough = scalar(m, 'Roughness', 0.8, -1400, 300)
texp = scalar(m, 'Tex', 0.0, -1400, 400)
ins = [('P', wp, ''), ('VN', vn, ''), ('Color', col, ''), ('Roughness', rough, ''), ('Tex', texp, '')] + tex_inputs(m, GROUND_SETS, -1100, 0)
base = custom(m, GROUND_LIB + 'return c;', ins, F3, -400, 0)
rgh = custom(m, GROUND_LIB + 'return saturate(r);', ins, F1, -400, 250)
# Ground is near-flat with UVs = world XY, so the photo's tangent space is world X / Y.
nrm = custom(m, GROUND_LIB + 'return normalize(normalize(VN) * tn.z + float3(tn.x, tn.y, 0.0));', ins, F3, -400, 500)
MEL.connect_material_property(base, '', unreal.MaterialProperty.MP_BASE_COLOR)
MEL.connect_material_property(rgh, '', unreal.MaterialProperty.MP_ROUGHNESS)
MEL.connect_material_property(nrm, '', unreal.MaterialProperty.MP_NORMAL)
finish(m)

# ---------------------------------------------------------------- M_MRFacade
# Walls take a photo texture by Wall: 0 red brick, 1 stone (sandstone, or render on pale buildings),
# 2 concrete / render, 3 corrugated steel. Windows and roofs stay procedural (per storey / bay).
FACADE_SETS = [('Br', 'Bricks101'), ('Sn', 'Bricks084'), ('Cn', 'Concrete034'), ('Cs', 'CorrugatedSteel005')]
FACADE_LIB = NOISE + '''
SamplerState SS = View.MaterialTextureBilinearWrapedSampler;
float3 n = normalize(Nrm);
float roof = step(0.6, n.z);
float2 t = normalize(float2(-n.y, n.x) + 1e-4);
float u = dot(P.xy, t);
float v = P.z;
float bay = max(BaySpacing, 50.0), flr = max(FloorHeight, 200.0);
float2 cell = float2(floor(u / bay), floor(v / flr));
float fu = frac(u / bay), fv = frac(v / flr);
float w0 = lerp(0.22, 0.04, Glass), w1 = 1.0 - w0;
float h0 = lerp(0.32, 0.08, Glass), h1 = lerp(0.82, 0.96, Glass);
float win = (1.0 - roof) * step(w0, fu) * step(fu, w1) * step(h0, fv) * step(fv, h1);
float wall = N.Fbm(P.xy / 300.0 + P.z / 500.0);
float lit = step(0.7, N.H(cell + floor(P.xy / 4000.0))) * Night;
float wt = floor(Wall + 0.5);
float pale = step(0.33, dot(VC.rgb, float3(0.33, 0.33, 0.33)));
if (wt > 0.5 && wt < 1.5 && pale > 0.5) wt = 2.0; // pale stone-group buildings are rendered, not sandstone
float tile = wt < 0.5 ? 120.0 : wt < 1.5 ? 160.0 : wt < 2.5 ? 220.0 : 180.0;
float2 uv1 = float2(u, -v) / tile;
float2 uv2 = uv1 * 0.37 + 0.21;
float mixw = smoothstep(0.35, 0.65, wall) * 0.35;
float3 tc = VC.rgb, tavg = VC.rgb, tn = float3(0, 0, 1);
float tr = 0.85;
'''
for i, (pre, _) in enumerate(FACADE_SETS):
    FACADE_LIB += ('if' if i == 0 else 'else if') + f' (wt < {i}.5) ' + sample_block(pre, 'uv1', 'uv2', 'mixw') + '\n'
FACADE_LIB += '''
float3 wallC = VC.rgb * tc / max(tavg, float3(0.02, 0.02, 0.02)) * lerp(0.88, 1.08, wall);
wallC *= lerp(1.0, 0.8, smoothstep(0.0, 400.0, 400.0 - v) * 0.4); // grime near the ground
float3 slate = float3(0.065, 0.065, 0.075), tile2 = float3(0.21, 0.075, 0.045);
float3 copper = float3(0.17, 0.36, 0.3);
float3 roofBase = VCA < 0.25 ? tile2 : (VCA > 0.75 ? slate : copper);
float3 roofC = roofBase * lerp(0.8, 1.15, wall) * lerp(0.85, 1.0, step(0.12, frac(P.z / 12.0)));
float fw = 0.035 * (1.0 - Glass);
float inner = step(w0 + fw, fu) * step(fu, w1 - fw) * step(h0 + fw * 2.0, fv) * step(fv, h1 - fw);
float3 glassC = lerp(float3(0.03, 0.04, 0.05), float3(0.08, 0.1, 0.12), N.H(cell));
float3 frameC = lerp(float3(0.85, 0.85, 0.82), VC.rgb * 0.6, Glass);
float3 winC = lerp(frameC, glassC, inner);
'''
m = new_material('M_MRFacade')
m.set_editor_property('tangent_space_normal', False)
wp = MEL.create_material_expression(m, unreal.MaterialExpressionWorldPosition, -1400, 0)
nrmws = MEL.create_material_expression(m, unreal.MaterialExpressionVertexNormalWS, -1400, 100)
col = vector(m, 'Color', (0.36, 0.13, 0.08, 1), -1400, 200)
glass = scalar(m, 'Glass', 0.0, -1400, 350)
flh = scalar(m, 'FloorHeight', 320.0, -1400, 450)
bays = scalar(m, 'BaySpacing', 260.0, -1400, 550)
night = scalar(m, 'Night', 0.0, -1400, 650)
wallp = scalar(m, 'Wall', 0.0, -1400, 750)
vc = MEL.create_material_expression(m, unreal.MaterialExpressionVertexColor, -1400, 850)
ins = [('P', wp, ''), ('Nrm', nrmws, ''), ('Color', col, ''), ('Glass', glass, ''), ('FloorHeight', flh, ''), ('BaySpacing', bays, ''),
       ('Night', night, ''), ('Wall', wallp, ''), ('VC', vc, ''), ('VCA', vc, 'A')] + tex_inputs(m, FACADE_SETS, -1100, 0)
base = custom(m, FACADE_LIB + 'return lerp(lerp(wallC, roofC, roof), winC, win);', ins, F3, -400, 0)
rgh = custom(m, FACADE_LIB + 'return lerp(lerp(tr, lerp(0.55, 0.75, VCA), roof), 0.06, win);', ins, F1, -400, 250)
mtl = custom(m, FACADE_LIB + 'return win * 0.7 + (1.0 - win) * (1.0 - roof) * step(2.5, wt) * 0.6;', ins, F1, -400, 400)
emc = custom(m, FACADE_LIB + 'return win * lit * float3(1.0, 0.78, 0.5) * lerp(0.6, 1.1, N.H(cell * 1.7));', ins, F3, -400, 550)
# Walls: photo normal in the wall's frame (along the wall, down, out); roofs and glass stay flat.
nrm = custom(m, FACADE_LIB + 'float3 T = float3(t, 0.0); float3 B = float3(0, 0, -1); float k = (1.0 - roof) * (1.0 - win);'
             ' return normalize(lerp(n, T * tn.x + B * tn.y + n * tn.z, k));', ins, F3, -400, 700)
MEL.connect_material_property(base, '', unreal.MaterialProperty.MP_BASE_COLOR)
MEL.connect_material_property(rgh, '', unreal.MaterialProperty.MP_ROUGHNESS)
MEL.connect_material_property(mtl, '', unreal.MaterialProperty.MP_METALLIC)
MEL.connect_material_property(emc, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)
MEL.connect_material_property(nrm, '', unreal.MaterialProperty.MP_NORMAL)
finish(m)

# ---------------------------------------------------------------- M_MRRider / M_MRSkin
# The rider meshes have no UVs, so every pattern is computed from the body segment's own local
# position (cm). The patterns follow the body as it moves, and the fine detail fades out once it
# gets smaller than a couple of pixels, so it doesn't shimmer. Bumps go through the normal
# (screen-space height derivatives in local space, so no tangents are needed).
#
#   M_MRRider: Color, Roughness, Metallic, Emissive, Pattern, Wrinkle, WAxis, Detail
#     Pattern: 0 plain, 1 cotton knit (hoodie / T-shirt), 2 polyester jersey mesh, 3 Cordura textile,
#              4 denim / chino twill (Detail 1 = faded denim), 5 leather, 6 helmet paint,
#              7 hair, 8 stubble, 9 trainer mesh, 10 rubber
#     Wrinkle: crease strength; WAxis: the segment's long axis (creases run across it);
#     SegLen: segment length (cm), creases gather near the joints at each end
#   M_MRSkin (subsurface): Color, Roughness, Pattern (0 skin, 1 lips)
RIDER_LIB = '''
struct FMRR
{
    float H3(float3 p) { p = frac(p * 0.3183099 + 0.1); p *= 17.0; return frac(p.x * p.y * p.z * (p.x + p.y + p.z)); }
    float V3(float3 p)
    {
        float3 i = floor(p); float3 f = frac(p); f = f * f * (3.0 - 2.0 * f);
        return lerp(lerp(lerp(H3(i), H3(i + float3(1, 0, 0)), f.x), lerp(H3(i + float3(0, 1, 0)), H3(i + float3(1, 1, 0)), f.x), f.y),
                    lerp(lerp(H3(i + float3(0, 0, 1)), H3(i + float3(1, 0, 1)), f.x), lerp(H3(i + float3(0, 1, 1)), H3(i + float3(1, 1, 1)), f.x), f.y), f.z);
    }
    float F3(float3 p) { return V3(p) * 0.5 + V3(p * 2.03 + 1.7) * 0.3 + V3(p * 4.11 + 3.1) * 0.2; }
    // 1 while the feature (period in cm) spans several pixels, 0 once it is sub-pixel.
    float Fade(float period, float fw) { return saturate((period / fw - 1.5) * 0.5); }
    // 2D thread patterns (height 0..1), used tri-planar.
    float Weave(float2 q, float s)
    {
        float2 g = frac(q / s); float2 cell = floor(q / s);
        float over = fmod(abs(cell.x + cell.y), 2.0);
        return over < 1.0 ? sin(g.y * 3.14159) * (0.6 + 0.4 * sin(g.x * 3.14159)) : sin(g.x * 3.14159) * (0.6 + 0.4 * sin(g.y * 3.14159));
    }
    float Twill(float2 q, float s) { float t = frac((q.x + q.y * 0.5) / s); return 0.5 + 0.5 * sin(t * 6.28318); }
    float Knit(float2 q, float s)
    {
        float2 g = q / s; float col = frac(g.x);
        float row = frac(g.y * 0.5 + abs(col - 0.5));
        return sin(row * 3.14159) * sin(col * 3.14159);
    }
    float Holes(float2 q, float s)
    {
        float2 g = q / s; float row = floor(g.y);
        g.x += fmod(abs(row), 2.0) * 0.5;
        float2 f = frac(g) - 0.5;
        return 1.0 - smoothstep(0.22, 0.32, length(f));
    }
};
FMRR R;
float3 p = LP;
float3 n = normalize(LN);
float fw = max(length(max(abs(ddx(p)), abs(ddy(p)))), 1e-4); // cm per pixel
float3 tw = pow(abs(n), 4.0); tw /= (tw.x + tw.y + tw.z);
'''

RIDER_COMMON = RIDER_LIB + '''
float pat = floor(Pattern + 0.5);
float3 c = Color.rgb;
float r = Roughness;
float h = 0.0;
float sheen = 0.0;
float mott = R.F3(p / 6.0);
// Creases across the segment's long axis (elbows, knees, waist), patchy along it.
float fabric = (pat > 0.5 && pat < 5.5) ? 1.0 : 0.0;
float a = dot(p, normalize(WAxis.xyz + 1e-4));
// Broad, soft folds roughly 5 cm apart, gently bent, fading in and out along the garment.
float fold = 0.5 + 0.5 * sin((a + (R.V3(p / 7.0) - 0.5) * 3.0) * 1.25);
fold = fold * fold * smoothstep(0.3, 0.7, R.F3(p / 10.0 + 7.0));
// Bunched up near the joints at each end of the segment (SegLen = 0: evenly spread).
float jw = SegLen > 1.0 ? lerp(0.2, 1.0, max(1.0 - smoothstep(0.0, 0.3 * SegLen, a), smoothstep(0.7 * SegLen, SegLen, a))) : 1.0;
float crease = fold * Wrinkle * fabric * jw;
h -= crease * 0.6;
c *= lerp(1.0, 0.9, crease);
// Seams with a row of stitches: under each limb, and down each side of the torso.
if (fabric > 0.5) {
    float seam = 0.0;
    if (WAxis.z > 0.5) seam = (1.0 - smoothstep(0.03, 0.07, abs(n.x))) * step(0.6, abs(n.y));
    else if (SegLen > 1.0) seam = (1.0 - smoothstep(0.06, 0.16, abs(p.y))) * step(p.z, 0.0);
    float stitch = step(0.5, frac(a / 0.5)) * R.Fade(0.5, fw);
    h -= seam * 0.04;
    c *= lerp(1.0, lerp(0.78, 0.92, stitch), seam);
}
if (pat < 0.5) {
} else if (pat < 1.5) {        // cotton knit: stitch columns, slight fuzz and pilling, soft sheen
    float s = 0.4, fd = R.Fade(s, fw);
    float k = R.Knit(p.yz, s) * tw.x + R.Knit(p.xz, s) * tw.y + R.Knit(p.xy, s) * tw.z;
    h += k * 0.03 * fd;
    c *= lerp(1.0, lerp(0.8, 1.06, k), fd) * lerp(0.9, 1.08, mott) * lerp(0.94, 1.05, R.V3(p / 0.6)); // heathered yarn
    r = saturate(r + 0.06);
    sheen = 0.35;
} else if (pat < 2.5) {        // polyester jersey: fine pinhole mesh, slight shine
    float s = 0.3, fd = R.Fade(s, fw);
    float o = R.Holes(p.yz, s) * tw.x + R.Holes(p.xz, s) * tw.y + R.Holes(p.xy, s) * tw.z;
    h -= o * 0.015 * fd;
    c *= lerp(1.0, lerp(1.0, 0.78, o), fd) * lerp(0.96, 1.04, mott);
    r = saturate(r - 0.05 + 0.1 * (R.V3(p / 3.0) - 0.5));
    sheen = 0.2;
} else if (pat < 3.5) {        // Cordura: coarse basket weave, scuffed
    float s = 0.3, fd = R.Fade(s, fw);
    float w = R.Weave(p.yz, s) * tw.x + R.Weave(p.xz, s) * tw.y + R.Weave(p.xy, s) * tw.z;
    h += w * 0.03 * fd;
    c *= lerp(1.0, lerp(0.82, 1.06, w), fd) * lerp(0.9, 1.07, mott);
    r = saturate(r + 0.1 * (w - 0.5) * fd);
    sheen = 0.1;
} else if (pat < 4.5) {        // twill: diagonal ribs, slubby yarn; Detail = faded denim on the high points
    float s = 0.35, fd = R.Fade(s, fw);
    float t = R.Twill(p.yz, s) * tw.x + R.Twill(p.xz, s) * tw.y + R.Twill(p.xy, s) * tw.z;
    float slub = R.V3(p * float3(0.4, 3.0, 3.0));
    h += t * 0.02 * fd;
    c *= lerp(1.0, lerp(0.8, 1.1, t), fd) * lerp(0.9, 1.08, slub);
    float wear = saturate((1.0 - fold) * 0.6 * smoothstep(0.45, 0.8, R.F3(p / 12.0 + 3.0)) + mott * 0.25);
    c = lerp(c, c * 1.55 + 0.035, Detail * wear);
    sheen = 0.15;
} else if (pat < 5.5) {        // leather: pebbled grain, smoother and shinier where it creases
    float s = 0.25, fd = R.Fade(s, fw);
    float g = 1.0 - abs(R.V3(p / 0.12) * 2.0 - 1.0);
    h += g * 0.02 * fd;
    c *= lerp(0.92, 1.07, mott) * lerp(1.0, lerp(0.88, 1.04, g), fd);
    r = saturate(r + (g - 0.5) * 0.14 * fd - crease * 0.12);
} else if (pat < 6.5) {        // helmet paint: orange peel, metal flake, light scuffs
    float fl = step(0.93, R.H3(floor(p / 0.06))) * R.Fade(0.12, fw);
    float scuff = smoothstep(0.74, 0.86, R.F3(p / 2.0 + 3.0)) * 0.5;
    h += R.V3(p / 0.35) * 0.006;
    c *= (1.0 + fl * 0.3) * lerp(1.0, 0.9, scuff);
    r = saturate(r - fl * 0.06 + scuff * 0.3);
} else if (pat < 7.5) {        // hair: strands running down the head, clumps
    float fd = R.Fade(0.07, fw);
    float st = R.V3(p * float3(14.0, 14.0, 0.6));
    float cl = R.V3(p * float3(1.6, 1.6, 0.25));
    c *= lerp(0.7, 1.25, cl) * lerp(1.0, lerp(0.6, 1.3, st), fd);
    h += cl * 0.08 + st * 0.02 * fd;
    r = saturate(r - 0.15 + 0.2 * st * fd);
    sheen = 0.25;
} else if (pat < 8.5) {        // stubble: dark dots over skin
    float fd = R.Fade(0.09, fw);
    float d = step(0.55, R.H3(floor(p / 0.09)));
    c *= lerp(1.0, lerp(1.25, 0.6, d), fd);
} else if (pat < 9.5) {        // trainer upper: engineered mesh
    float s = 0.35, fd = R.Fade(s, fw);
    float o = R.Holes(p.yz, s) * tw.x + R.Holes(p.xz, s) * tw.y + R.Holes(p.xy, s) * tw.z;
    h -= o * 0.03 * fd;
    c *= lerp(1.0, lerp(1.0, 0.55, o), fd) * lerp(0.95, 1.05, mott);
    r = saturate(r + 0.05);
} else {                       // rubber
    c *= lerp(0.85, 1.1, R.V3(p / 0.3));
    r = saturate(r + (R.V3(p / 0.1) - 0.5) * 0.1);
}
float fres = pow(1.0 - saturate(abs(dot(n, normalize(CV.xyz)))), 4.0);
c = lerp(c, c * 1.7 + 0.02, sheen * fres);
'''

NORMAL_FROM_H = '''
float3 dpdx = ddx(p), dpdy = ddy(p);
float dhx = ddx(h), dhy = ddy(h);
float3 r1 = cross(dpdy, n), r2 = cross(n, dpdx);
float det = dot(dpdx, r1);
float3 grad = (det < 0.0 ? -1.0 : 1.0) * (dhx * r1 + dhy * r2);
return normalize(abs(det) * n - grad);'''

SKIN_COMMON = RIDER_LIB + '''
float3 c = Color.rgb;
float r = Roughness;
float lips = step(0.5, Pattern);
c *= lerp(0.92, 1.06, R.F3(p / 3.0));                                              // mottling
c = lerp(c, c * float3(1.08, 0.9, 0.88), smoothstep(0.55, 0.8, R.F3(p / 5.0 + 11.0)) * 0.6); // flushed patches
float fd = R.Fade(0.14, fw);
float pore = smoothstep(0.7, 0.95, R.V3(p / 0.07));
float h = -pore * 0.008 * fd + (R.V3(p / 0.6) - 0.5) * 0.01;
h += lips * sin(p.y * 40.0) * 0.004 * R.Fade(0.16, fw);                            // lip lines
c *= lerp(1.0, 0.94, pore * fd);
r = saturate(r + (R.V3(p / 1.2) - 0.5) * 0.12 - lips * 0.12);
'''


def rider_inputs(m, extra_params):
    wp = MEL.create_material_expression(m, unreal.MaterialExpressionWorldPosition, -1400, 0)
    lp = MEL.create_material_expression(m, unreal.MaterialExpressionTransformPosition, -1150, 0)
    lp.set_editor_property('transform_source_type', unreal.MaterialPositionTransformSource.TRANSFORMPOSSOURCE_WORLD)
    lp.set_editor_property('transform_type', unreal.MaterialPositionTransformSource.TRANSFORMPOSSOURCE_LOCAL)
    MEL.connect_material_expressions(wp, '', lp, '')
    nw = MEL.create_material_expression(m, unreal.MaterialExpressionVertexNormalWS, -1400, 100)
    ln = MEL.create_material_expression(m, unreal.MaterialExpressionTransform, -1150, 100)
    ln.set_editor_property('transform_source_type', unreal.MaterialVectorCoordTransformSource.TRANSFORMSOURCE_WORLD)
    ln.set_editor_property('transform_type', unreal.MaterialVectorCoordTransform.TRANSFORM_LOCAL)
    MEL.connect_material_expressions(nw, '', ln, '')
    cw = MEL.create_material_expression(m, unreal.MaterialExpressionCameraVectorWS, -1400, 200)
    lc = MEL.create_material_expression(m, unreal.MaterialExpressionTransform, -1150, 200)
    lc.set_editor_property('transform_source_type', unreal.MaterialVectorCoordTransformSource.TRANSFORMSOURCE_WORLD)
    lc.set_editor_property('transform_type', unreal.MaterialVectorCoordTransform.TRANSFORM_LOCAL)
    MEL.connect_material_expressions(cw, '', lc, '')
    ins = [('LP', lp, ''), ('LN', ln, ''), ('CV', lc, '')]
    return ins + extra_params


def normal_out(m, code, ins):
    nl = custom(m, code, ins, F3, -400, 500)
    tr = MEL.create_material_expression(m, unreal.MaterialExpressionTransform, -150, 500)
    tr.set_editor_property('transform_source_type', unreal.MaterialVectorCoordTransformSource.TRANSFORMSOURCE_LOCAL)
    tr.set_editor_property('transform_type', unreal.MaterialVectorCoordTransform.TRANSFORM_WORLD)
    MEL.connect_material_expressions(nl, '', tr, '')
    MEL.connect_material_property(tr, '', unreal.MaterialProperty.MP_NORMAL)


m = new_material('M_MRRider')
m.set_editor_property('tangent_space_normal', False)
col = vector(m, 'Color', (0.5, 0.5, 0.5, 1), -900, 0)
rough = scalar(m, 'Roughness', 0.6, -900, 100)
met = scalar(m, 'Metallic', 0.0, -900, 200)
emi = scalar(m, 'Emissive', 0.0, -900, 300)
pat = scalar(m, 'Pattern', 0.0, -900, 400)
wr = scalar(m, 'Wrinkle', 1.0, -900, 500)
wax = vector(m, 'WAxis', (1, 0, 0, 0), -900, 600)
det = scalar(m, 'Detail', 0.0, -900, 700)
sl = scalar(m, 'SegLen', 0.0, -900, 800)
ins = rider_inputs(m, [('Color', col, ''), ('Roughness', rough, ''), ('Pattern', pat, ''), ('Wrinkle', wr, ''), ('WAxis', wax, ''), ('Detail', det, ''), ('SegLen', sl, '')])
base = custom(m, RIDER_COMMON + 'return c;', ins, F3, -400, 0)
rgh = custom(m, RIDER_COMMON + 'return r;', ins, F1, -400, 250)
normal_out(m, RIDER_COMMON + NORMAL_FROM_H, ins)
mul = MEL.create_material_expression(m, unreal.MaterialExpressionMultiply, -400, 750)
MEL.connect_material_expressions(col, '', mul, 'A')
MEL.connect_material_expressions(emi, '', mul, 'B')
MEL.connect_material_property(base, '', unreal.MaterialProperty.MP_BASE_COLOR)
MEL.connect_material_property(rgh, '', unreal.MaterialProperty.MP_ROUGHNESS)
MEL.connect_material_property(met, '', unreal.MaterialProperty.MP_METALLIC)
MEL.connect_material_property(mul, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)
finish(m)

m = new_material('M_MRSkin')
m.set_editor_property('tangent_space_normal', False)
m.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_SUBSURFACE)
col = vector(m, 'Color', (0.6, 0.42, 0.33, 1), -900, 0)
rough = scalar(m, 'Roughness', 0.55, -900, 100)
pat = scalar(m, 'Pattern', 0.0, -900, 200)
ins = rider_inputs(m, [('Color', col, ''), ('Roughness', rough, ''), ('Pattern', pat, '')])
base = custom(m, SKIN_COMMON + 'return c;', ins, F3, -400, 0)
rgh = custom(m, SKIN_COMMON + 'return r;', ins, F1, -400, 250)
normal_out(m, SKIN_COMMON + NORMAL_FROM_H, ins)
# Light passing through skin comes out red (blood); thin parts (ears, nose, fingers) glow most.
sss = custom(m, 'return saturate(Color.rgb * float3(0.75, 0.26, 0.18));', [('Color', col, '')], F3, -400, 650)
op = MEL.create_material_expression(m, unreal.MaterialExpressionConstant, -400, 750)
op.set_editor_property('r', 0.1)
MEL.connect_material_property(base, '', unreal.MaterialProperty.MP_BASE_COLOR)
MEL.connect_material_property(rgh, '', unreal.MaterialProperty.MP_ROUGHNESS)
MEL.connect_material_property(sss, '', unreal.MaterialProperty.MP_SUBSURFACE_COLOR)
MEL.connect_material_property(op, '', unreal.MaterialProperty.MP_OPACITY)
finish(m)


# ---------------------------------------------------------------- M_MRSurface (vehicles and props)
# Lacquer (ClearCoat 0..1 makes the surface glossier: the Python API has no clear-coat pins) over a flaky base. Kind adds micro detail from the
# part's local position: 0 plain, 1 paint, 2 brushed alloy, 3 tyre rubber, 4 textured plastic /
# anodised, 5 seat cover. ClearCoat 0 behaves like ordinary lit (props, street furniture).
SURF_COMMON = RIDER_LIB + '''
float kind = floor(Kind + 0.5);
float3 c = Color.rgb;
float r = lerp(Roughness, Roughness * 0.35, saturate(ClearCoat)); // lacquered: glossy top coat
float h = 0.0;
if (kind < 0.5) {
} else if (kind < 1.5) {       // paint: orange peel under the lacquer, metal flake, slight flop in the colour
    h += R.V3(p / 0.6) * 0.004;
    c *= lerp(0.97, 1.03, R.F3(p / 30.0));
    float fl = step(0.9, R.H3(floor(p / 0.05))) * R.Fade(0.1, fw);
    c *= 1.0 + fl * 0.35;
} else if (kind < 2.5) {       // brushed / machined alloy: fine streaks, a few tool marks
    float st = R.V3(p * float3(0.25, 30.0, 30.0)) * 0.7 + R.V3(p * float3(30.0, 0.25, 30.0)) * 0.3;
    r = saturate(r + (st - 0.5) * 0.18);
    c *= lerp(0.92, 1.05, st);
    h += st * 0.004 * R.Fade(0.06, fw);
} else if (kind < 3.5) {       // tyre rubber: grain, lighter dusty scuffs, slightly shinier where worn
    float fd = R.Fade(0.3, fw);
    c = lerp(c, float3(0.075, 0.07, 0.065), smoothstep(0.55, 0.9, R.F3(p / 4.0)) * 0.6);
    h += R.V3(p / 0.15) * 0.01 * fd;
    r = saturate(r - 0.08 + 0.12 * R.V3(p / 2.0));
} else if (kind < 4.5) {       // textured plastic / anodised: fine stipple
    h += R.V3(p / 0.08) * 0.006 * R.Fade(0.16, fw);
    r = saturate(r + (R.V3(p / 3.0) - 0.5) * 0.1);
    c *= lerp(0.94, 1.05, R.F3(p / 5.0));
} else {                       // seat cover: leatherette grain
    float g = 1.0 - abs(R.V3(p / 0.12) * 2.0 - 1.0);
    h += g * 0.015 * R.Fade(0.25, fw);
    r = saturate(r + (g - 0.5) * 0.12);
    c *= lerp(0.9, 1.06, R.F3(p / 4.0));
}
'''

m = EAL.load_asset(FOLDER + '/M_MRSurface')
if m:
    EAL.delete_asset(FOLDER + '/M_MRSurface')
m = new_material('M_MRSurface')
m.set_editor_property('tangent_space_normal', False)
col = vector(m, 'Color', (0.5, 0.5, 0.5, 1), -900, 0)
met = scalar(m, 'Metallic', 0.0, -900, 100)
rough = scalar(m, 'Roughness', 0.5, -900, 200)
emi = scalar(m, 'Emissive', 0.0, -900, 300)
kind = scalar(m, 'Kind', 0.0, -900, 400)
cc = scalar(m, 'ClearCoat', 0.0, -900, 500)
ins = rider_inputs(m, [('Color', col, ''), ('Roughness', rough, ''), ('Kind', kind, ''), ('ClearCoat', cc, '')])
base = custom(m, SURF_COMMON + 'return c;', ins, F3, -400, 0)
rgh = custom(m, SURF_COMMON + 'return r;', ins, F1, -400, 250)
normal_out(m, SURF_COMMON + NORMAL_FROM_H, ins)
mul = MEL.create_material_expression(m, unreal.MaterialExpressionMultiply, -400, 750)
MEL.connect_material_expressions(col, '', mul, 'A')
MEL.connect_material_expressions(emi, '', mul, 'B')
MEL.connect_material_property(base, '', unreal.MaterialProperty.MP_BASE_COLOR)
MEL.connect_material_property(rgh, '', unreal.MaterialProperty.MP_ROUGHNESS)
MEL.connect_material_property(met, '', unreal.MaterialProperty.MP_METALLIC)
MEL.connect_material_property(mul, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)
finish(m)

# ---------------------------------------------------------------- M_MRFoliage (tree canopies and bark)
# Two-sided foliage shading (sunlight glows through the leaves). Pattern 0: leaf canopy, broken into
# leaf clusters with dark gaps between them; 1: bark with vertical furrows. Local position, so the
# pattern is stable however large the world coordinates are.
FOLIAGE_COMMON = RIDER_LIB + '''
float3 c = Color.rgb;
float r = 0.75;
float h = 0.0;
if (Pattern < 0.5) {
    float clump = R.F3(p / 45.0);                       // big leaf masses
    float leaf = R.V3(p / 6.0) * 0.6 + R.V3(p / 2.2) * 0.4; // leaf clusters
    float gap = smoothstep(0.42, 0.25, leaf);            // dark holes into the crown
    c *= lerp(0.6, 1.3, clump) * lerp(1.0, 0.25, gap) * lerp(0.85, 1.15, R.V3(p / 1.1));
    c = lerp(c, c * float3(1.25, 1.15, 0.55), smoothstep(0.62, 0.85, R.F3(p / 120.0 + 5.0)) * 0.5); // yellower trees / sunlit tops
    c *= lerp(0.65, 1.1, saturate(n.z * 0.5 + 0.5));      // self-shadowed underside
    h = clump * 30.0 + leaf * 9.0;
    r = 0.6 + 0.25 * leaf;
} else {
    float furrow = abs(sin(p.x * 0.35 + R.V3(p * float3(0.1, 0.1, 0.02)) * 6.0) * sin(p.y * 0.35));
    float lich = smoothstep(0.65, 0.8, R.F3(p / 20.0 + 9.0));
    c *= lerp(0.55, 1.15, furrow) * lerp(0.85, 1.1, R.V3(p * float3(0.5, 0.5, 0.08)));
    c = lerp(c, float3(0.1, 0.13, 0.06), lich * 0.6);
    h = furrow * 1.2;
    r = 0.9;
}
'''
m = new_material('M_MRFoliage')
m.set_editor_property('tangent_space_normal', False)
m.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)
col = vector(m, 'Color', (0.04, 0.075, 0.025, 1), -900, 0)
pat = scalar(m, 'Pattern', 0.0, -900, 100)
ins = rider_inputs(m, [('Color', col, ''), ('Pattern', pat, '')])
base = custom(m, FOLIAGE_COMMON + 'return c;', ins, F3, -400, 0)
rgh = custom(m, FOLIAGE_COMMON + 'return r;', ins, F1, -400, 250)
normal_out(m, FOLIAGE_COMMON + NORMAL_FROM_H, ins)
sss = custom(m, 'return Pattern < 0.5 ? saturate(Color.rgb * float3(1.6, 2.2, 0.8)) : float3(0, 0, 0);', [('Color', col, ''), ('Pattern', pat, '')], F3, -400, 650)
MEL.connect_material_property(base, '', unreal.MaterialProperty.MP_BASE_COLOR)
MEL.connect_material_property(rgh, '', unreal.MaterialProperty.MP_ROUGHNESS)
MEL.connect_material_property(sss, '', unreal.MaterialProperty.MP_SUBSURFACE_COLOR)
finish(m)


unreal.log('MR: materials done')
