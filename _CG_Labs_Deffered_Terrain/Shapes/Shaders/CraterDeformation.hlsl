// Crater Deformation Compute Shader
// Applies parabolic crater deformation to the CraterMap texture
// When radiusUV >= 1.0, acts as a clear operation (sets all pixels to depth value)

cbuffer CraterParams : register(b0)
{
    float2 gCenterUV;
    float  gRadiusUV;
    float  gDepth;
};

RWTexture2D<float> gCraterMap : register(u0);

[numthreads(8, 8, 1)]
void CS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 texCoord = dispatchThreadID.xy;
    uint width, height;
    gCraterMap.GetDimensions(width, height);
    
    // Bounds check
    if (texCoord.x >= width || texCoord.y >= height)
        return;
    
    // Convert texel coordinate to UV space
    float2 texelUV = (float2(texCoord) + 0.5f) / float2(width, height);
    
    // If radius >= 1.0, this is a clear operation - set all pixels to depth
    if (gRadiusUV >= 1.0f)
    {
        gCraterMap[texCoord] = gDepth;
        return;
    }
    
    // Compute distance from crater center
    float2 delta = texelUV - gCenterUV;
    float distance = length(delta);
    
    // Apply parabolic falloff within radius
    if (distance < gRadiusUV)
    {
        float t = distance / gRadiusUV;
        float falloff = 1.0f - t * t;
        float deformation = gDepth * falloff;
        
        // Accumulate deformation
        gCraterMap[texCoord] += deformation;
    }
}
