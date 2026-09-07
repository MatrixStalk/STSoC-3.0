#pragma once

class IUIShader;

struct SUIRenderTransform
{
    float m00{1.f}, m01{}, m02{};
    float m10{}, m11{1.f}, m12{};

    void transform(float& x, float& y) const
    {
        const float source_x = x;
        const float source_y = y;
        x = m00 * source_x + m01 * source_y + m02;
        y = m10 * source_x + m11 * source_y + m12;
    }
};

class IUIRender
{
public:
    enum ePrimitiveType
    {
        ptNone = -1,
        ptTriList,
        ptTriStrip,
        ptLineStrip,
        ptLineList
    };

    enum ePointType
    {
        pttNone = -1,
        pttTL,
        pttLIT
    };

    enum CullMode
    {
        cmNONE = 0,
        cmCW,
        cmCCW,
    };

public:
    // virtual ~IUIRender() {;}

    virtual void CreateUIGeom() = 0;
    virtual void DestroyUIGeom() = 0;

    virtual void SetShader(IUIShader& shader) = 0;
    virtual void SetAlphaRef(int aref) = 0;

    virtual void SetAnimationAlpha(float alpha) = 0;
    virtual float GetAnimationAlpha() const = 0;
    virtual void SetAnimationOffset(float x, float y) = 0;
    virtual Fvector2 GetAnimationOffset() const = 0;
    virtual const SUIRenderTransform& GetAnimationTransform() const = 0;
    virtual void PushAnimationTransform(float alpha, float offset_x, float offset_y, float rotation, float pivot_x,
        float pivot_y) = 0;
    virtual void PushIdentityAnimationTransform() = 0;
    virtual void PopAnimationTransform() = 0;

    virtual void SetScissor(Irect* rect = nullptr) = 0;
    virtual void GetActiveTextureResolution(Fvector2& res) = 0;

    virtual void PushPoint(float x, float y, float z, u32 C, float u, float v) = 0;

    virtual void StartPrimitive(u32 iMaxVerts, ePrimitiveType primType, ePointType pointType) = 0;
    virtual void FlushPrimitive() = 0;

    virtual LPCSTR UpdateShaderName(LPCSTR tex_name, LPCSTR sh_name) = 0;

    virtual void CacheSetXformWorld(const Fmatrix& M) = 0;
    virtual void CacheSetCullMode(CullMode) = 0;
};
