// colortrans3.glsl
// libplacebo / mpv "custom shader" (HOOK)
// Intended use with ffmpeg:
//   -vf "libplacebo=custom_shader_path=colortrans3.glsl"
//
// Implements:
//   - optional reverse ColorTrans (BT.709 inverse rows)
//   - grade: clamp -> pow(gamma) -> +lift -> *gain -> *rgb_mult -> mpv-style saturation -> clamp
//
// Edit the constants below to tune.


//!HOOK MAIN
//!BIND HOOKED
//!DESC colortrans3: reverse ColorTrans

vec3 apply_saturation_mpv(vec3 c, float mpv_sat)
{
    float sat = 1.0 + (mpv_sat / 100.0);
    sat = clamp(sat, 0.0, 3.0);

    // Rec.709 luma
    float y = dot(c, vec3(0.2126, 0.7152, 0.0722));
    return mix(vec3(y), c, sat);
}

vec3 reverse_colortrans(vec3 rgb_in)
{
    // Reverse ColorTrans constants
    const float colortrans_y_offset_10b  = 200.0;
    const float colortrans_yoff_strength = 0.25; // try 0.10–0.30

    const float colortrans_yoff =
        (colortrans_y_offset_10b / 1023.0) * colortrans_yoff_strength;

    const float reverse_black_lift = 0.020;

    // Precomputed inverse (RGB-domain), BT.709-style
    const vec3 row0 = vec3( 1.17866031,  0.17460893,  0.01571472);
    const vec3 row1 = vec3(-0.11147506,  1.55408099, -0.07362197);
    const vec3 row2 = vec3( 0.03243649,  0.11275820,  1.22378927);

    // Remove (part of) camera Y offset as neutral RGB bias
    vec3 x = rgb_in - vec3(colortrans_yoff);

    vec3 y = vec3(
        dot(row0, x),
        dot(row1, x),
        dot(row2, x)
    );

    // Prevent black crush
    y += vec3(reverse_black_lift);
    return y;
}

vec3 apply_grade(vec3 c)
{
    // Defaults (baked)
    const float gamma_pow = 1.0;
    const float lift      = -0.15;
    const float gain      = 1.75;
    const vec3  rgb_mult  = vec3(1.0, 1.0, 1.0);

    // mpv-style saturation default in range [-100 .. +100]
    const float mpv_saturation_default = -22.5;

    // clamp input before gamma
    c = clamp(c, 0.0, 1.0);

    float g = clamp(gamma_pow, 0.10, 5.0);
    float l = clamp(lift,     -1.0,  1.0);
    float k = clamp(gain,      0.0, 10.0);
    vec3  m = clamp(rgb_mult,  vec3(0.0), vec3(4.0));

    vec3 y = pow(c, vec3(g));
    y += vec3(l);
    y *= k;
    y *= m;

    y = apply_saturation_mpv(y, mpv_saturation_default);
    return clamp(y, 0.0, 1.0);
}

vec4 hook()
{
    vec4 src = HOOKED_texOff(0);
    vec3 c = src.rgb;

    // Toggle reverse ColorTrans here:
    const bool REVERSE_COLORTRANS_ENABLE = true;
    if (REVERSE_COLORTRANS_ENABLE) {
        c = reverse_colortrans(c);
    }

    vec3 out_rgb = apply_grade(c);
    return vec4(out_rgb, src.a);
}
