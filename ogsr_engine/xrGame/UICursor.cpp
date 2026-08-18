#include "stdafx.h"
#include "uicursor.h"
#include "ui/UIStatic.h"
#include "ui/UIXmlInit.h"

CUICursor::CUICursor()
{
    InitInternal();
    Device.seqRender.Add(this, 1);
}

CUICursor::~CUICursor() { Device.seqRender.Remove(this); }

void CUICursor::InitInternal()
{
    m_static = std::make_unique<CUIStatic>();

    CUIXml xml;
    if (xml.Init(CONFIG_PATH, UI_PATH, "ui_cursor.xml") && xml.NavigateToNode("cursor", 0))
    {
        CUIXmlInit::InitStatic(xml, "cursor", 0, m_static.get());

        const float legacy_scale_x = READ_IF_EXISTS(pSettings, r_float, "ui_tweaks", "cursor_scale_x", 1.f);
        const float legacy_scale_y = READ_IF_EXISTS(pSettings, r_float, "ui_tweaks", "cursor_scale_y", 1.f);
        const float scale_x = xml.ReadAttribFlt("cursor", 0, "scale_x", legacy_scale_x);
        const float scale_y = xml.ReadAttribFlt("cursor", 0, "scale_y", legacy_scale_y);
        const bool scale_with_kx = !!xml.ReadAttribInt("cursor", 0, "scale_with_kx", 1);

        Fvector2 size = m_static->GetWndSize();
        const float kx = scale_with_kx ? UI()->get_current_kx() : 1.f;
        size.x *= kx * scale_x;
        size.y *= scale_y;
        m_static->SetWndSize(size);

        m_hotspot.x = xml.ReadAttribFlt("cursor", 0, "hotspot_x", 0.f) * kx * scale_x;
        m_hotspot.y = xml.ReadAttribFlt("cursor", 0, "hotspot_y", 0.f) * scale_y;
        m_sensitivity = xml.ReadAttribFlt("cursor", 0, "sensitivity", 1.f);
    }
    else
    {
        Msg("! Cannot load [ui_cursor.xml], using the default cursor");
        m_static->InitTextureEx("ui\\ui_ani_cursor", "hud\\cursor");

        constexpr Frect rect{0.0f, 0.0f, 40.0f, 40.0f};
        m_static->SetOriginalRect(rect);

        Fvector2 size{rect.rb};
        size.x *= UI()->get_current_kx();
        size.x *= READ_IF_EXISTS(pSettings, r_float, "ui_tweaks", "cursor_scale_x", 1.f);
        size.y *= READ_IF_EXISTS(pSettings, r_float, "ui_tweaks", "cursor_scale_y", 1.f);
        m_static->SetWndSize(size);
        m_static->SetStretchTexture(true);
    }

    m_static->ShowImmediate(true);
}

void CUICursor::OnRender()
{
    static u32 last_frame{};
    if (last_frame == Device.dwFrame)
        return;
    last_frame = Device.dwFrame;

    if (!IsVisible())
        return;

#ifdef DEBUG
    static u32 last_render_frame = 0;
    VERIFY(last_render_frame != Device.dwFrame);
    last_render_frame = Device.dwFrame;

    if (bDebug)
    {
        CGameFont* F = UI()->Font()->pFontDI;
        F->SetAligment(CGameFont::alCenter);
        F->SetHeightI(0.02f);
        F->OutSetI(0.f, -0.9f);
        F->SetColor(0xffffffff);
        Fvector2 pt = GetCursorPosition();
        F->OutNext("%f-%f", pt.x, pt.y);
    }
#endif

    m_static->SetWndPos(vPos.x - m_hotspot.x, vPos.y - m_hotspot.y);
    m_static->Update();
    m_static->Draw();
}

Fvector2 CUICursor::GetCursorPosition() const { return vPos; }

Fvector2 CUICursor::GetCursorPositionDelta() const { return Fvector2{vPos.x - vPrevPos.x, vPos.y - vPrevPos.y}; }

void CUICursor::UpdateCursorPosition(const int _dx, const int _dy)
{
    vPrevPos = vPos;

    const u32 screen_size_x = GetSystemMetrics(SM_CXSCREEN);
    const u32 screen_size_y = GetSystemMetrics(SM_CYSCREEN);

    const bool m_b_use_win_cursor = (screen_size_y >= Device.dwHeight && screen_size_x >= Device.dwWidth);

    if (m_b_use_win_cursor)
    {
        Ivector2 pti{};

        GetCursorPos((LPPOINT)&pti);

        HWND hwnd = Device.m_hWnd;
        if (hwnd)
            ScreenToClient(hwnd, (LPPOINT)&pti);

        vPos.x = (float)pti.x * (UI_BASE_WIDTH / (float)Device.dwWidth);
        vPos.y = (float)pti.y * (UI_BASE_HEIGHT / (float)Device.dwHeight);
    }
    else
    {
        vPos.x += _dx * m_sensitivity;
        vPos.y += _dy * m_sensitivity;
    }

    clamp(vPos.x, 0.f, UI_BASE_WIDTH);
    clamp(vPos.y, 0.f, UI_BASE_HEIGHT);
}

void CUICursor::SetUICursorPosition(const Fvector2& pos)
{
    vPos = pos;

    const int x = iFloor(vPos.x / (UI_BASE_WIDTH / (float)Device.dwWidth));
    const int y = iFloor(vPos.y / (UI_BASE_HEIGHT / (float)Device.dwHeight));

    SetCursorPos(x, y);
}
