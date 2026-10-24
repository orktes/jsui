// The JSUI texture is premultiplied RGBA holding sRGB-encoded values in a
// non-sRGB format: composite it with ONE, ONE_MINUS_SRC_ALPHA, decoding the
// colour to linear first in Linear projects. Used by JsuiView's RawImage; it lives in a
// Resources folder so players include it.
Shader "Hidden/JSUI/Premultiplied"
{
    Properties
    {
        _MainTex ("Texture", 2D) = "white" {}
    }
    SubShader
    {
        Tags { "Queue"="Transparent" "IgnoreProjector"="True" "RenderType"="Transparent" "PreviewType"="Plane" }
        Cull Off
        Lighting Off
        ZWrite Off
        ZTest [unity_GUIZTestMode]
        Blend One OneMinusSrcAlpha

        Pass
        {
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"

            struct appdata { float4 vertex : POSITION; float4 color : COLOR; float2 uv : TEXCOORD0; };
            struct v2f { float4 pos : SV_POSITION; fixed4 color : COLOR; float2 uv : TEXCOORD0; };

            sampler2D _MainTex;
            float4 _MainTex_ST;

            v2f vert(appdata v)
            {
                v2f o;
                o.pos = UnityObjectToClipPos(v.vertex);
                o.uv = TRANSFORM_TEX(v.uv, _MainTex);
                o.color = v.color;
                return o;
            }

            fixed4 frag(v2f i) : SV_Target
            {
                // The Graphic's colour tints; its alpha fades the whole UI.
                float4 c = tex2D(_MainTex, i.uv);
            #if !defined(UNITY_COLORSPACE_GAMMA)
                // Unpremultiply, decode, premultiply.
                if (c.a > 0) c.rgb = GammaToLinearSpace(c.rgb / c.a) * c.a;
            #endif
                return float4(c.rgb * i.color.rgb, c.a) * i.color.a;
            }
            ENDCG
        }
    }
}
