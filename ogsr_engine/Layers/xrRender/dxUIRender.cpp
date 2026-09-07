#include "stdafx.h"
#include "dxUIRender.h"

#include "dxUIShader.h"

dxUIRender UIRenderImpl;

void dxUIRender::PushAnimationTransform(float alpha, float offset_x, float offset_y, float rotation, float pivot_x,
    float pivot_y)
{
    m_animationStack.push_back(
        {m_animationAlpha, m_animationOffset, m_animationTransform, m_animationScissorEnabled, m_animationScissor});

    const float cosine = _cos(rotation);
    const float sine = _sin(rotation);
    SUIRenderTransform local;
    local.m00 = cosine;
    local.m01 = -sine;
    local.m02 = pivot_x - cosine * pivot_x + sine * pivot_y + offset_x;
    local.m10 = sine;
    local.m11 = cosine;
    local.m12 = pivot_y - sine * pivot_x - cosine * pivot_y + offset_y;

    const SUIRenderTransform parent = m_animationTransform;
    m_animationTransform.m00 = parent.m00 * local.m00 + parent.m01 * local.m10;
    m_animationTransform.m01 = parent.m00 * local.m01 + parent.m01 * local.m11;
    m_animationTransform.m02 = parent.m00 * local.m02 + parent.m01 * local.m12 + parent.m02;
    m_animationTransform.m10 = parent.m10 * local.m00 + parent.m11 * local.m10;
    m_animationTransform.m11 = parent.m10 * local.m01 + parent.m11 * local.m11;
    m_animationTransform.m12 = parent.m10 * local.m02 + parent.m11 * local.m12 + parent.m12;

    m_animationAlpha = _min(m_animationAlpha, alpha);
    m_animationOffset.set(m_animationTransform.m02, m_animationTransform.m12);
}

void dxUIRender::PushIdentityAnimationTransform()
{
    m_animationStack.push_back(
        {m_animationAlpha, m_animationOffset, m_animationTransform, m_animationScissorEnabled, m_animationScissor});
    m_animationAlpha = 1.f;
    m_animationOffset.set(0.f, 0.f);
    m_animationTransform = SUIRenderTransform{};
}

void dxUIRender::PopAnimationTransform()
{
    VERIFY(!m_animationStack.empty());
    const SAnimationRenderState& state = m_animationStack.back();
    m_animationAlpha = state.alpha;
    m_animationOffset = state.offset;
    m_animationTransform = state.transform;
    m_animationScissorEnabled = state.scissor_enabled;
    m_animationScissor = state.scissor;
    RCache.set_Scissor(m_animationScissorEnabled ? &m_animationScissor : nullptr);
    RCache.StateManager.OverrideScissoring(m_animationScissorEnabled, TRUE);
    m_animationStack.pop_back();
}

void dxUIRender::CreateUIGeom()
{
    hGeom_TL.create(FVF::F_TL, RImplementation.Vertex.Buffer(), nullptr);
    hGeom_LIT.create(FVF::F_LIT, RImplementation.Vertex.Buffer(), nullptr);
}

void dxUIRender::DestroyUIGeom()
{
    hGeom_TL = nullptr;
    hGeom_LIT = nullptr;
}

void dxUIRender::SetShader(IUIShader& shader)
{
    dxUIShader* pShader = smart_cast<dxUIShader*>(&shader);
    VERIFY(&pShader);
    VERIFY(pShader->hShader);
    RCache.set_Shader(pShader->hShader);
}

void dxUIRender::SetAlphaRef(int aref)
{
    // CHK_DX(HW.pDevice->SetRenderState(D3DRS_ALPHAREF,aref));
    RCache.set_AlphaRef(aref);
}

void dxUIRender::SetScissor(Irect* rect)
{
    Irect animated_rect;
    Irect* render_rect = rect;
    if (rect)
    {
        float x[4] = {float(rect->x1), float(rect->x2), float(rect->x2), float(rect->x1)};
        float y[4] = {float(rect->y1), float(rect->y1), float(rect->y2), float(rect->y2)};
        for (u32 i = 0; i < 4; ++i)
            m_animationTransform.transform(x[i], y[i]);
        animated_rect.set(iFloor(*std::min_element(x, x + 4)), iFloor(*std::min_element(y, y + 4)),
            iCeil(*std::max_element(x, x + 4)), iCeil(*std::max_element(y, y + 4)));
        render_rect = &animated_rect;
    }

    m_animationScissorEnabled = render_rect != nullptr;
    if (render_rect)
        m_animationScissor = *render_rect;
    RCache.set_Scissor(render_rect);
    RCache.StateManager.OverrideScissoring(m_animationScissorEnabled, TRUE);
}

void dxUIRender::GetActiveTextureResolution(Fvector2& res)
{
    CTexture* T = RCache.get_ActiveTexture(0);
    res.set(float(T->get_Width()), float(T->get_Height()));
}

LPCSTR dxUIRender::UpdateShaderName(LPCSTR tex_name, LPCSTR sh_name)
{
    string_path buff;
    const u32 v_dev = CAP_VERSION(HW.Caps.raster_major, HW.Caps.raster_minor);
    const u32 v_need = CAP_VERSION(2, 0);
    
    if ((v_dev >= v_need) && FS.exist(buff, fsgame::game_textures, tex_name, ".ogm"))
        return "hud\\movie";
    else
        return sh_name;
}

void dxUIRender::PushPoint(float x, float y, float z, u32 C, float u, float v)
{
    m_animationTransform.transform(x, y);
    C = subst_alpha(C, static_cast<u32>(color_get_A(C) * m_animationAlpha));

    //.	VERIFY(m_PointType==pttLIT);
    switch (m_PointType)
    {
    case pttLIT:
        LIT_pv->set(x, y, z, C, u, v);
        ++LIT_pv;
        break;
    case pttTL:
        TL_pv->set(x, y, C, u, v);
        ++TL_pv;
        break;
    }
}

void dxUIRender::StartPrimitive(u32 iMaxVerts, ePrimitiveType primType, ePointType pointType)
{
    VERIFY(PrimitiveType == ptNone);
    VERIFY(m_PointType == pttNone);

    m_iMaxVerts = iMaxVerts;
    PrimitiveType = primType;
    m_PointType = pointType;

    switch (m_PointType)
    {
    case pttLIT:
        LIT_start_pv = (FVF::LIT*)RImplementation.Vertex.Lock(m_iMaxVerts, hGeom_LIT.stride(), vOffset);
        LIT_pv = LIT_start_pv;
        break;
    case pttTL:
        TL_start_pv = (FVF::TL*)RImplementation.Vertex.Lock(m_iMaxVerts, hGeom_TL.stride(), vOffset);
        TL_pv = TL_start_pv;
        break;
    }
}

void dxUIRender::FlushPrimitive()
{
    u32 primCount = 0;
    _D3DPRIMITIVETYPE d3dPrimType = D3DPT_FORCE_DWORD;
    std::ptrdiff_t p_cnt = 0;

    switch (m_PointType)
    {
    case pttLIT:
        p_cnt = LIT_pv - LIT_start_pv;
        VERIFY(u32(p_cnt) <= m_iMaxVerts);

        RImplementation.Vertex.Unlock(u32(p_cnt), hGeom_LIT.stride());
        RCache.set_Geometry(hGeom_LIT);
        break;
    case pttTL:
        p_cnt = TL_pv - TL_start_pv;
        VERIFY(u32(p_cnt) <= m_iMaxVerts);

        RImplementation.Vertex.Unlock(u32(p_cnt), hGeom_TL.stride());
        RCache.set_Geometry(hGeom_TL);
        break;
    default: NODEFAULT;
    }

    //	Update data for primitive type
    switch (PrimitiveType)
    {
    case ptTriStrip:
        primCount = (u32)(p_cnt - 2);
        d3dPrimType = D3DPT_TRIANGLESTRIP;
        break;
    case ptTriList:
        primCount = (u32)(p_cnt / 3);
        d3dPrimType = D3DPT_TRIANGLELIST;
        break;
    case ptLineStrip:
        primCount = (u32)(p_cnt - 1);
        d3dPrimType = D3DPT_LINESTRIP;
        break;
    case ptLineList:
        primCount = (u32)(p_cnt / 2);
        d3dPrimType = D3DPT_LINELIST;
        break;
    default: NODEFAULT;
    }

    if (primCount > 0)
        RCache.Render(d3dPrimType, vOffset, primCount);

    PrimitiveType = ptNone;
    m_PointType = pttNone;
}

void dxUIRender::CacheSetXformWorld(const Fmatrix& M) { RCache.set_xform_world(M); }

void dxUIRender::CacheSetCullMode(CullMode m) { RCache.set_CullMode(CULL_NONE + m); }
