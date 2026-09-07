#pragma once

#include "../../Include/xrRender/UIRender.h"

class dxUIRender : public IUIRender
{
public:
    dxUIRender() : PrimitiveType(ptNone), m_PointType(pttNone) { m_animationOffset.set(0.f, 0.f); }

    virtual void CreateUIGeom();
    virtual void DestroyUIGeom();

    virtual void SetShader(IUIShader& shader);
    virtual void SetAlphaRef(int aref);

    void SetAnimationAlpha(float alpha) override { m_animationAlpha = alpha; }
    float GetAnimationAlpha() const override { return m_animationAlpha; }
    void SetAnimationOffset(float x, float y) override
    {
        m_animationTransform.m02 += x - m_animationOffset.x;
        m_animationTransform.m12 += y - m_animationOffset.y;
        m_animationOffset.set(x, y);
    }
    Fvector2 GetAnimationOffset() const override { return m_animationOffset; }
    const SUIRenderTransform& GetAnimationTransform() const override { return m_animationTransform; }
    void PushAnimationTransform(float alpha, float offset_x, float offset_y, float rotation, float pivot_x,
        float pivot_y) override;
    void PushIdentityAnimationTransform() override;
    void PopAnimationTransform() override;

    virtual void SetScissor(Irect* rect = nullptr);
    virtual void GetActiveTextureResolution(Fvector2& res);

    virtual void PushPoint(float x, float y, float z, u32 C, float u, float v);

    virtual void StartPrimitive(u32 iMaxVerts, ePrimitiveType primType, ePointType pointType);
    virtual void FlushPrimitive();

    virtual LPCSTR UpdateShaderName(LPCSTR tex_name, LPCSTR sh_name);

    virtual void CacheSetXformWorld(const Fmatrix& M);
    virtual void CacheSetCullMode(CullMode);

private:
    ref_geom hGeom_TL;
    ref_geom hGeom_LIT;

    ePrimitiveType PrimitiveType;
    ePointType m_PointType;

    float m_animationAlpha{1.f};
    Fvector2 m_animationOffset;
    SUIRenderTransform m_animationTransform;

    struct SAnimationRenderState
    {
        float alpha;
        Fvector2 offset;
        SUIRenderTransform transform;
        bool scissor_enabled;
        Irect scissor;
    };
    xr_vector<SAnimationRenderState> m_animationStack;
    bool m_animationScissorEnabled{};
    Irect m_animationScissor{};

    //	Vertex buffer attributes
    u32 m_iMaxVerts;
    u32 vOffset;

    FVF::TL* TL_start_pv;
    FVF::TL* TL_pv;

    FVF::LIT* LIT_start_pv;
    FVF::LIT* LIT_pv;
};

extern dxUIRender UIRenderImpl;
