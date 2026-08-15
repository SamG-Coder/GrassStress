cbuffer HudCB : register(b0)
{
    float2 g_Resolution;
    float g_Fps;
    float g_FrameMs;
    uint g_Blades;
    uint g_Patches;
    uint g_Width;
    uint g_Height;
    float g_TitleAlpha;
    float g_HudAlpha;
    float g_TimeOfDay;
    float g_Cinematic;
    float g_TempC;
    float g_Util;
    float g_PowerW;
    float g_FrameMsP1;
    float g_VramGiB;
    float g_Pad0;
    float g_Pad1;
    float g_Pad2;
    float g_DisplayFps;
    float g_DisplayMs;
    uint g_MfgMul;
    uint g_DlssMode;
    uint g_GpuChars[16];
};

struct VSOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID) {
    VSOut output;
    output.uv = float2((id << 1) & 2, id & 2);
    output.position = float4(output.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}

uint fontBit(uint ch, uint x, uint y) {
    // Compact 5x7 glyphs for 0-9, A-Z, space and a few punctuation marks.
    uint row = 0;
    if (ch == 32) row = 0;
    else if (ch == 37) {                                   // %
        if (y == 0) row = 17; else if (y == 1) row = 19; else if (y == 2) row = 2;
        else if (y == 3) row = 4; else if (y == 4) row = 8; else if (y == 5) row = 25;
        else row = 17;
    }
    else if (ch == 45) { if (y == 3) row = 14; }           // -
    else if (ch == 46) { if (y == 6) row = 4; }            // .
    else if (ch == 47) {                                   // /
        if (y == 0) row = 1; else if (y == 1) row = 2; else if (y == 2) row = 2;
        else if (y == 3) row = 4; else if (y == 4) row = 8; else if (y == 5) row = 8;
        else row = 16;
    }
    else if (ch == 58) { if (y == 2 || y == 5) row = 4; }  // :
    else if (ch == 215) {                                  // x as multiply lookalike via 'x'
        if (y == 1 || y == 5) row = 17; else if (y == 2 || y == 4) row = 10; else if (y == 3) row = 4;
    }
    else if (ch >= 48 && ch <= 57) {
        uint d = ch - 48;
        if (d == 0) {
            uint rows[7] = {14,17,17,17,17,17,14}; row = rows[y];
        } else if (d == 1) {
            uint rows[7] = {4,12,4,4,4,4,14}; row = rows[y];
        } else if (d == 2) {
            uint rows[7] = {14,17,1,2,4,8,31}; row = rows[y];
        } else if (d == 3) {
            uint rows[7] = {14,17,1,6,1,17,14}; row = rows[y];
        } else if (d == 4) {
            uint rows[7] = {2,6,10,18,31,2,2}; row = rows[y];
        } else if (d == 5) {
            uint rows[7] = {31,16,30,1,1,17,14}; row = rows[y];
        } else if (d == 6) {
            uint rows[7] = {14,16,16,30,17,17,14}; row = rows[y];
        } else if (d == 7) {
            uint rows[7] = {31,1,2,4,4,4,4}; row = rows[y];
        } else if (d == 8) {
            uint rows[7] = {14,17,17,14,17,17,14}; row = rows[y];
        } else {
            uint rows[7] = {14,17,17,15,1,1,14}; row = rows[y];
        }
    } else if (ch >= 65 && ch <= 90) {
        uint L = ch;
        if (L == 65) { uint r[7] = {14,17,17,31,17,17,17}; row = r[y]; }
        else if (L == 66) { uint r[7] = {30,17,17,30,17,17,30}; row = r[y]; }
        else if (L == 67) { uint r[7] = {14,17,16,16,16,17,14}; row = r[y]; }
        else if (L == 68) { uint r[7] = {30,17,17,17,17,17,30}; row = r[y]; }
        else if (L == 69) { uint r[7] = {31,16,16,30,16,16,31}; row = r[y]; }
        else if (L == 70) { uint r[7] = {31,16,16,30,16,16,16}; row = r[y]; }
        else if (L == 71) { uint r[7] = {14,17,16,19,17,17,14}; row = r[y]; }
        else if (L == 72) { uint r[7] = {17,17,17,31,17,17,17}; row = r[y]; }
        else if (L == 73) { uint r[7] = {14,4,4,4,4,4,14}; row = r[y]; }
        else if (L == 75) { uint r[7] = {17,18,20,24,20,18,17}; row = r[y]; }
        else if (L == 76) { uint r[7] = {16,16,16,16,16,16,31}; row = r[y]; }
        else if (L == 77) { uint r[7] = {17,27,21,21,17,17,17}; row = r[y]; }
        else if (L == 78) { uint r[7] = {17,25,21,19,17,17,17}; row = r[y]; }
        else if (L == 79) { uint r[7] = {14,17,17,17,17,17,14}; row = r[y]; }
        else if (L == 80) { uint r[7] = {30,17,17,30,16,16,16}; row = r[y]; }
        else if (L == 82) { uint r[7] = {30,17,17,30,20,18,17}; row = r[y]; }
        else if (L == 83) { uint r[7] = {15,16,16,14,1,1,30}; row = r[y]; }
        else if (L == 84) { uint r[7] = {31,4,4,4,4,4,4}; row = r[y]; }
        else if (L == 85) { uint r[7] = {17,17,17,17,17,17,14}; row = r[y]; }
        else if (L == 86) { uint r[7] = {17,17,17,17,17,10,4}; row = r[y]; }
        else if (L == 88) { uint r[7] = {17,17,10,4,10,17,17}; row = r[y]; }
        else if (L == 89) { uint r[7] = {17,17,10,4,4,4,4}; row = r[y]; }
        else if (L == 90) { uint r[7] = {31,1,2,4,8,16,31}; row = r[y]; }
    }
    return (row >> (4 - x)) & 1u;
}

float drawGlyph(float2 pixel, float2 origin, uint ch, float scale) {
    float2 local = (pixel - origin) / scale;
    if (local.x < 0 || local.y < 0 || local.x >= 6 || local.y >= 8) return 0;
    uint x = (uint)floor(local.x);
    uint y = (uint)floor(local.y);
    if (x > 4 || y > 6) return 0;
    return fontBit(ch, x, y);
}

float drawText(float2 pixel, float2 origin, uint chars[48], uint count, float scale) {
    float a = 0;
    [loop] for (uint i = 0; i < count; ++i) {
        a = max(a, drawGlyph(pixel, origin + float2(i * 6 * scale, 0), chars[i], scale));
    }
    return a;
}

void writeNumber(inout uint chars[48], inout uint count, uint value, uint digits) {
    uint pow10 = 1;
    [unroll] for (uint i = 1; i < 10; ++i) if (i < digits) pow10 *= 10;
    bool started = digits > 0;
    if (digits == 0) {
        if (value == 0) { chars[count++] = 48; return; }
        pow10 = 1000000000;
        while (pow10 > 1 && value < pow10) pow10 /= 10;
        started = true;
    }
    [loop] for (; pow10 > 0 && count < 48; pow10 /= 10) {
        uint digit = (value / pow10) % 10;
        if (started || digit != 0 || pow10 == 1) {
            started = true;
            chars[count++] = 48 + digit;
        }
    }
}

void writeStr(inout uint chars[48], inout uint count, uint a, uint b, uint c, uint d) {
    if (a) chars[count++] = a;
    if (b) chars[count++] = b;
    if (c) chars[count++] = c;
    if (d) chars[count++] = d;
}

float4 PSMain(VSOut input) : SV_Target {
    float2 pixel = input.uv * g_Resolution;
    float3 color = 0;
    float alpha = 0;

    float letter = saturate((18.0 - pixel.y) / 18.0) + saturate((pixel.y - (g_Resolution.y - 18.0)) / 18.0);
    color = lerp(color, float3(0, 0, 0), letter * .65);
    alpha = max(alpha, letter * .65 * g_HudAlpha);

    if (g_TitleAlpha > 0.002) {
        float veil = 0.42 * g_TitleAlpha;
        color = lerp(color, float3(0.01, 0.012, 0.01), veil);
        alpha = max(alpha, veil);

        uint line0[48]; uint n0 = 0;
        writeStr(line0, n0, 82, 84, 88, 32); // RTX
        // GPU name from packed chars
        [unroll] for (uint w = 0; w < 16; ++w) {
            uint packed = g_GpuChars[w];
            [unroll] for (uint b = 0; b < 4; ++b) {
                uint ch = (packed >> (b * 8)) & 255u;
                if (ch == 0) break;
                if (ch >= 97 && ch <= 122) ch -= 32;
                if (n0 < 48) line0[n0++] = ch;
            }
        }
        float2 center = g_Resolution * .5;
        float s0 = 4.0;
        float w0 = n0 * 6.0 * s0;
        float a0 = drawText(pixel, float2(center.x - w0 * .5, center.y - 78), line0, n0, s0);

        uint line1[48]; uint n1 = 0;
        writeNumber(line1, n1, g_Blades, 0);
        writeStr(line1, n1, 32, 86, 79, 76);
        writeStr(line1, n1, 85, 77, 69, 84);
        writeStr(line1, n1, 82, 73, 67, 32);
        writeStr(line1, n1, 71, 82, 65, 83);
        writeStr(line1, n1, 83, 32, 66, 76);
        writeStr(line1, n1, 65, 68, 69, 83);
        float s1 = 3.2;
        float w1 = n1 * 6.0 * s1;
        float a1 = drawText(pixel, float2(center.x - w1 * .5, center.y - 18), line1, n1, s1);

        uint line2[48]; uint n2 = 0;
        writeStr(line2, n2, 80, 65, 84, 72);
        writeStr(line2, n2, 32, 84, 82, 65);
        writeStr(line2, n2, 67, 69, 68, 32);
        writeStr(line2, n2, 83, 84, 82, 69);
        writeStr(line2, n2, 83, 83, 32, 84);
        writeStr(line2, n2, 69, 83, 84, 0);
        float s2 = 2.2;
        float w2 = n2 * 6.0 * s2;
        float a2 = drawText(pixel, float2(center.x - w2 * .5, center.y + 42), line2, n2, s2);

        float3 gold = float3(0.92, 0.82, 0.42);
        float3 white = float3(0.95, 0.96, 0.93);
        color = lerp(color, gold, a0 * g_TitleAlpha);
        color = lerp(color, white, a1 * g_TitleAlpha);
        color = lerp(color, float3(0.70, 0.86, 0.62), a2 * g_TitleAlpha);
        alpha = max(alpha, max(a0, max(a1, a2)) * g_TitleAlpha);
    }

    if (g_HudAlpha > 0.002) {
        uint statsA[48]; uint n = 0;
        writeNumber(statsA, n, g_Blades, 0);
        writeStr(statsA, n, 32, 66, 76, 65);
        writeStr(statsA, n, 68, 69, 83, 32);
        writeStr(statsA, n, 32, 80, 84, 32);
        writeStr(statsA, n, 66, 79, 85, 78);
        writeStr(statsA, n, 67, 69, 83, 0);
        float a = drawText(pixel, float2(28, g_Resolution.y - 78), statsA, n, 2.0);

        uint lineB[48]; uint nb = 0;
        uint fps = (uint)round(g_Fps);
        writeNumber(lineB, nb, fps, 0);
        writeStr(lineB, nb, 32, 80, 84, 32);
        writeStr(lineB, nb, 70, 80, 83, 32);
        uint ms10 = (uint)round(g_FrameMs * 10.0);
        writeNumber(lineB, nb, ms10 / 10, 0);
        lineB[nb++] = 46;
        writeNumber(lineB, nb, ms10 % 10, 1);
        writeStr(lineB, nb, 32, 77, 83, 32);
        uint disp = (uint)round(g_DisplayFps > .5 ? g_DisplayFps : g_Fps);
        writeNumber(lineB, nb, disp, 0);
        writeStr(lineB, nb, 32, 68, 73, 83);
        writeStr(lineB, nb, 80, 32, 70, 80);
        writeStr(lineB, nb, 83, 0, 0, 0);
        float b = drawText(pixel, float2(28, g_Resolution.y - 56), lineB, nb, 1.7);

        uint lineC[48]; uint nc = 0;
        writeNumber(lineC, nc, (uint)round(g_TempC), 0);
        writeStr(lineC, nc, 32, 67, 32, 32);
        writeNumber(lineC, nc, (uint)round(g_Util), 0);
        writeStr(lineC, nc, 32, 37, 32, 32);
        writeNumber(lineC, nc, (uint)round(g_PowerW), 0);
        writeStr(lineC, nc, 32, 87, 32, 32);
        uint vram10 = (uint)round(g_VramGiB * 10.0);
        writeNumber(lineC, nc, vram10 / 10, 0);
        lineC[nc++] = 46;
        writeNumber(lineC, nc, vram10 % 10, 1);
        writeStr(lineC, nc, 32, 71, 66, 32);
        if (g_DlssMode == 2) writeStr(lineC, nc, 82, 82, 32, 0);
        else if (g_DlssMode == 1) writeStr(lineC, nc, 83, 82, 32, 0);
        if (g_MfgMul > 1u) {
            writeNumber(lineC, nc, g_MfgMul, 0);
            lineC[nc++] = 88;
            writeStr(lineC, nc, 32, 0, 0, 0);
        }
        if (g_Pad1 > .5) {
            writeStr(lineC, nc, 67, 72, 79, 75);
            writeStr(lineC, nc, 73, 78, 71, 0);
        } else {
            writeStr(lineC, nc, 83, 84, 65, 66);
            writeStr(lineC, nc, 76, 69, 0, 0);
        }
        float c = drawText(pixel, float2(28, g_Resolution.y - 34), lineC, nc, 1.6);

        uint lineE[48]; uint ne = 0;
        writeStr(lineE, ne, 69, 88, 80, 32);
        uint exp = (uint)round(g_Pad2);
        writeNumber(lineE, ne, exp, 0);
        lineE[ne++] = 32;
        if (exp == 0) { writeStr(lineE, ne, 66, 65, 83, 69); writeStr(lineE, ne, 0, 0, 0, 0); }
        else if (exp == 1) { writeStr(lineE, ne, 82, 50, 32, 81); writeStr(lineE, ne, 77, 67, 0, 0); }
        else if (exp == 2) { writeStr(lineE, ne, 70, 73, 66, 69); writeStr(lineE, ne, 82, 0, 0, 0); }
        else if (exp == 3) { writeStr(lineE, ne, 72, 65, 83, 72); writeStr(lineE, ne, 71, 73, 0, 0); }
        else if (exp == 4) { writeStr(lineE, ne, 82, 69, 83, 84); writeStr(lineE, ne, 73, 82, 0, 0); }
        else if (exp == 5) { writeStr(lineE, ne, 80, 83, 70, 0); }
        else if (exp == 6) { writeStr(lineE, ne, 67, 65, 83, 67); writeStr(lineE, ne, 65, 68, 69, 83); }
        else { writeStr(lineE, ne, 83, 84, 65, 67); writeStr(lineE, ne, 75, 0, 0, 0); }
        float e = drawText(pixel, float2(28, g_Resolution.y - 14), lineE, ne, 1.5);

        float3 ink = g_Pad1 > .5 ? float3(1.0, 0.42, 0.28) : float3(0.93, 0.95, 0.88);
        color = lerp(color, ink, max(a, max(b, max(c, e))) * g_HudAlpha);
        alpha = max(alpha, max(a, max(b, max(c, e))) * g_HudAlpha);
    }

    return float4(color, saturate(alpha));
}
