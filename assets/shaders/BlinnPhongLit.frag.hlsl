struct Input
  {
      float3 fragPos : TEXCOORD0;
      float3 normal  : TEXCOORD1;
      float2 uv      : TEXCOORD2;
  };

  // Fragment-stage textures + samplers live in space2 (separate objects in HLSL).
  Texture2D    diffuseTex  : register(t0, space2);
  SamplerState diffuseSmp  : register(s0, space2);
  Texture2D    specularTex : register(t1, space2);
  SamplerState specularSmp : register(s1, space2);

  // Fragment-stage uniform buffers live in space3.
  cbuffer Lighting : register(b0, space3)
  {
      float3 viewPos;        float shininess;      // vec3 + float pack into one 16-byte slot
      float3 lightPos;       float _pad0;
      float3 lightAmbient;   float _pad1;
      float3 lightDiffuse;   float _pad2;
      float3 lightSpecular;  float _pad3;
  };

  float4 main(Input input) : SV_Target
  {
      float3 diffuseColor = diffuseTex.Sample(diffuseSmp, input.uv).rgb;

      // ambient
      float3 ambient = lightAmbient * diffuseColor;

      // diffuse
      float3 norm     = normalize(input.normal);
      float3 lightDir = normalize(lightPos - input.fragPos);
      float  diff     = max(dot(norm, lightDir), 0.0f);
      float3 diffuse  = lightDiffuse * diff * diffuseColor;

      // specular
      float3 viewDir    = normalize(viewPos - input.fragPos);
      float3 reflectDir = reflect(-lightDir, norm);
      float  spec       = pow(max(dot(viewDir, reflectDir), 0.0f), shininess);
      float3 specular   = lightSpecular * spec * specularTex.Sample(specularSmp, input.uv).rgb;

      float3 result = ambient + diffuse + specular;
      return float4(result, 1.0f);
  }