// Vita3K emulator project
// FSR 1.0 upscaling as a fragment shader, the approach UE5 uses for its mobile FSR.
//
// The compute path in fsr_filter_easu.comp / fsr_filter_rcas.comp dispatches one workgroup
// per *output* pixel, so on a 1080x2412 phone it runs the upscale over 2.6M pixels for a
// 960x544 frame and clears the whole swapchain every frame. That costs more than the emulated
// game draws, which is what made it unusable on a phone.
//
// Running the same maths from the rasteriser removes both costs: the output pixels arrive
// for free with the screen-filter pass, and no swapchain clear is involved. What is left is
// proportional to the frame being upscaled.
//
// This is a mechanical port of the FSR 1.0 reference implementation in
// external/GPUOpen/ffx_fsr1.h (MIT), renamed to GLSL types. The only substantive change is
// in how the 12 taps are fetched: the reference implementation calls FsrEasuRF/GF/BF per
// tap, which is 36 texture reads for the 12 taps it uses. Here each tap is read once with
// texture() and its channels are used for R, G and B in turn, which is the same trick the
// MPV GLSL port uses and cuts the upscale to 12 reads.
//
// textureGather is deliberately avoided: OpImageGather is core SPIR-V but is only worth
// using when the driver folds four taps into one instruction, and the reference code's tap
// ordering (a,b / r,g) does not line up with what the fragment stage wants here.

#version 450

layout(location = 0) in vec2 uv_frag;

layout(binding = 0) uniform sampler2D fb;

// con0: xy = output-to-input pixel scale, zw = half-pixel offset (FsrEasuCon con0)
// con1: xy = 1 / source size in pixels (FsrEasuCon con1)
// sharp: RCAS sharpness in linear form, where 0 is the sharpest (FsrRcasCon)
layout(push_constant) uniform constants {
    vec4 con0;
    vec4 con1;
    vec2 pad;
    float sharp;
} pc;

layout(location = 0) out vec3 color_frag;

const vec3 kLuma = vec3(0.299, 0.587, 0.114);

float sat(float x) {
    return clamp(x, 0.0, 1.0);
}

float rcp(float x) {
    return 1.0 / max(x, 1e-8);
}

float rsq(float x) {
    return 1.0 / sqrt(max(x, 1e-8));
}

// One tap of the 12-tap kernel. Mirrors FsrEasuTapF().
void easuTap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len, float lob, float clp, vec3 c) {
    vec2 v;
    v.x = off.x * dir.x + off.y * dir.y;
    v.y = off.x * -dir.y + off.y * dir.x;
    v *= len;

    float d2 = v.x * v.x + v.y * v.y;
    d2 = min(d2, clp);

    float wB = 2.0 / 5.0 * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = 25.0 / 16.0 * wB - (25.0 / 16.0 - 1.0);

    float w = wB * wA;

    aC += c * w;
    aW += w;
}

// Accumulate direction and length. Mirrors FsrEasuSetF().
void easuSet(inout vec2 dir, inout float len, vec2 pp, bool biS, bool biT, bool biU, bool biV,
    float lA, float lB, float lC, float lD, float lE) {
    float w = 0.0;
    if (biS) w = (1.0 - pp.x) * (1.0 - pp.y);
    if (biT) w = pp.x * (1.0 - pp.y);
    if (biU) w = (1.0 - pp.x) * pp.y;
    if (biV) w = pp.x * pp.y;

    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = max(abs(dc), abs(cb));
    lenX = rcp(lenX);
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = sat(abs(dirX) * lenX);
    lenX *= lenX;
    len += lenX * w;

    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = max(abs(ec), abs(ca));
    lenY = rcp(lenY);
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = sat(abs(dirY) * lenY);
    lenY *= lenY;
    len += lenY * w;
}

void main() {
    // Output integer pixel position. The reference implementation assumes the output
    // coordinate and the input texture share an orientation, which holds here because
    // gl_FragCoord.y is 0 at the top just like row 0 of the frame.
    ivec2 ip = ivec2(gl_FragCoord.xy);

    // Position of 'f' and its fractional part. con0 is exactly what FsrEasuCon() produces:
    // xy is the output-to-input pixel scale and zw the half-pixel offset.
    vec2 pp = vec2(ip) * pc.con0.xy + pc.con0.zw;
    vec2 fp = floor(pp);
    pp -= fp;

    vec2 fTexel = (fp + 0.5) * pc.con1.xy;

    // 12 taps of the kernel, each read once and reused for R, G and B.
    //    b c
    //  e f g h
    //  i j k l
    //    n o
    vec3 b = texture(fb, fTexel + vec2(0.0, -1.0) * pc.con1.xy).rgb;
    vec3 c = texture(fb, fTexel + vec2(1.0, -1.0) * pc.con1.xy).rgb;
    vec3 e = texture(fb, fTexel + vec2(-1.0, 0.0) * pc.con1.xy).rgb;
    vec3 f = texture(fb, fTexel).rgb;
    vec3 g = texture(fb, fTexel + vec2(1.0, 0.0) * pc.con1.xy).rgb;
    vec3 h = texture(fb, fTexel + vec2(2.0, 0.0) * pc.con1.xy).rgb;
    vec3 i = texture(fb, fTexel + vec2(-1.0, 1.0) * pc.con1.xy).rgb;
    vec3 j = texture(fb, fTexel + vec2(0.0, 1.0) * pc.con1.xy).rgb;
    vec3 k = texture(fb, fTexel + vec2(1.0, 1.0) * pc.con1.xy).rgb;
    vec3 l = texture(fb, fTexel + vec2(2.0, 1.0) * pc.con1.xy).rgb;
    vec3 n = texture(fb, fTexel + vec2(0.0, 2.0) * pc.con1.xy).rgb;
    vec3 o = texture(fb, fTexel + vec2(1.0, 2.0) * pc.con1.xy).rgb;

    float bL = dot(b, kLuma);
    float cL = dot(c, kLuma);
    float eL = dot(e, kLuma);
    float fL = dot(f, kLuma);
    float gL = dot(g, kLuma);
    float hL = dot(h, kLuma);
    float iL = dot(i, kLuma);
    float jL = dot(j, kLuma);
    float kL = dot(k, kLuma);
    float lL = dot(l, kLuma);
    float nL = dot(n, kLuma);
    float oL = dot(o, kLuma);

    vec2 dir = vec2(0.0);
    float len = 0.0;
    easuSet(dir, len, pp, true, false, false, false, bL, eL, fL, gL, jL);
    easuSet(dir, len, pp, false, true, false, false, cL, fL, gL, hL, kL);
    easuSet(dir, len, pp, false, false, true, false, fL, iL, jL, kL, nL);
    easuSet(dir, len, pp, false, false, false, true, gL, jL, kL, lL, oL);

    // Normalize, with a cleanup near zero.
    vec2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < 1.0 / 32768.0;
    dirR = rsq(dirR);
    dirR = zro ? 1.0 : dirR;
    dir = zro ? vec2(1.0, 0.0) : dir;
    dir *= dirR;

    len *= 0.5;
    len *= len;

    float stretch = (dir.x * dir.x + dir.y * dir.y) * rcp(max(abs(dir.x), abs(dir.y)));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 + -0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = rcp(lob);

    vec3 min4 = min(min(min(j, l), k), f);
    vec3 max4 = max(max(max(j, l), k), f);

    vec3 aC = vec3(0.0);
    float aW = 0.0;
    easuTap(aC, aW, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, b);
    easuTap(aC, aW, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, c);
    easuTap(aC, aW, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, i);
    easuTap(aC, aW, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, j);
    easuTap(aC, aW, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, f);
    easuTap(aC, aW, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, e);
    easuTap(aC, aW, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, k);
    easuTap(aC, aW, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, l);
    easuTap(aC, aW, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, h);
    easuTap(aC, aW, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, g);
    easuTap(aC, aW, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, o);
    easuTap(aC, aW, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, n);

    vec3 result = min(max4, max(min4, aC * rcp(aW)));

    // RCAS, fused in rather than run as a second compute pass. It needs the cross around
    // the upscaled result, so it costs 4 more reads, but it saves writing an intermediate
    // image and reading it straight back. Mirrors FsrRcasF().
    vec2 texelRC = pc.con1.xy;
    vec3 bRC = texture(fb, uv_frag + vec2(0.0, -texelRC.y)).rgb;
    vec3 dRC = texture(fb, uv_frag + vec2(-texelRC.x, 0.0)).rgb;
    vec3 fRC = texture(fb, uv_frag + vec2(texelRC.x, 0.0)).rgb;
    vec3 hRC = texture(fb, uv_frag + vec2(0.0, texelRC.y)).rgb;

    // Luma times 2.
    float bLRC = bRC.b * 0.5 + (bRC.r * 0.5 + bRC.g);
    float dLRC = dRC.b * 0.5 + (dRC.r * 0.5 + dRC.g);
    float eLRC = result.b * 0.5 + (result.r * 0.5 + result.g);
    float fLRC = fRC.b * 0.5 + (fRC.r * 0.5 + fRC.g);
    float hLRC = hRC.b * 0.5 + (hRC.r * 0.5 + hRC.g);

    vec3 mn4 = min(min(bRC, dRC), min(fRC, hRC));
    vec3 mx4 = max(max(bRC, dRC), max(fRC, hRC));

    vec2 peakC = vec2(1.0, -4.0);
    vec3 hitMin = min(mn4, result) * (1.0 / (4.0 * mx4));
    vec3 hitMax = (peakC.x - max(mx4, result)) * (1.0 / (4.0 * mn4 + peakC.y));
    vec3 lobe3 = max(-hitMin, hitMax);

    const float rcasLimit = 0.25 - (1.0 / 16.0);
    float lobe = max(-rcasLimit, min(max(lobe3.r, max(lobe3.g, lobe3.b)), 0.0)) * pc.sharp;

    float rcpL = 1.0 / (4.0 * lobe + 1.0);
    color_frag = (lobe * (bRC + dRC + hRC + fRC) + result) * rcpL;
}