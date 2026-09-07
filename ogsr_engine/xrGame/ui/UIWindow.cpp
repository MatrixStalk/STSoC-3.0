// UIWindow.cpp: implementation of the CUIWindow class.
//
//////////////////////////////////////////////////////////////////////
#include "stdafx.h"
#include "UIWindow.h"
#include "../UICursor.h"
#include "../MainMenu.h"
#include "HudManager.h"
#include "../Include/xrRender/DebugRender.h"

//#define LOG_ALL_WNDS
#ifdef LOG_ALL_WNDS
int ListWndCount = 0;
struct DBGList
{
    int num;
    bool closed;
};
xr_vector<DBGList> dbg_list_wnds;
void dump_list_wnd()
{
    Msg("------Total  wnds %d", dbg_list_wnds.size());
    xr_vector<DBGList>::iterator _it = dbg_list_wnds.begin();
    for (; _it != dbg_list_wnds.end(); ++_it)
        if (!(*_it).closed)
            Msg("--leak detected ---- wnd = %d", (*_it).num);
}
#else
void dump_list_wnd() {}
#endif

static xr_vector<std::pair<shared_str, Frect>> g_wnds_rects;
BOOL g_show_wnd_rect = FALSE;
BOOL g_show_wnd_rect2 = FALSE;
BOOL g_show_wnd_rect_text = FALSE;

namespace
{
bool g_bypass_window_animations = false;

Fvector2 animation_offset(CUIWindow::EAnimationPreset preset, float distance)
{
    Fvector2 result{};
    switch (preset)
    {
    case CUIWindow::EAnimationPreset::SlideLeft: result.x = -distance; break;
    case CUIWindow::EAnimationPreset::SlideRight: result.x = distance; break;
    case CUIWindow::EAnimationPreset::SlideUp: result.y = -distance; break;
    case CUIWindow::EAnimationPreset::SlideDown: result.y = distance; break;
    default: break;
    }
    return result;
}

float animation_rotation(CUIWindow::EAnimationPreset preset, float amount)
{
    if (preset == CUIWindow::EAnimationPreset::RotateClockwise)
        return amount;
    if (preset == CUIWindow::EAnimationPreset::RotateCounterClockwise)
        return -amount;
    return 0.f;
}

float animation_curve(CUIWindow::EAnimationPreset preset, float value)
{
    value = clampr(value, 0.f, 1.f);
    if (preset == CUIWindow::EAnimationPreset::Fade)
        return value;

    // Smootherstep keeps the first and last frames free of visible jumps.
    return value * value * value * (value * (value * 6.f - 15.f) + 10.f);
}
} // namespace

void clean_wnd_rects() { DRender->DestroyDebugShader(IDebugRender::dbgShaderWindow); }

static void add_rect_to_draw(const Frect& r, const shared_str& windowName) { g_wnds_rects.emplace_back(windowName, r); }

static void draw_rect(const Frect& r, const u32 color, const shared_str& name)
{
    DRender->SetDebugShader(IDebugRender::dbgShaderWindow);

    //.	UIRender->StartLineStrip	(5);
    UIRender->StartPrimitive(5, IUIRender::ptLineStrip, UI()->m_currentPointType);

    UIRender->PushPoint(r.lt.x, r.lt.y, 0, color, 0, 0);
    UIRender->PushPoint(r.rb.x - 1, r.lt.y, 0, color, 0, 0);
    UIRender->PushPoint(r.rb.x - 1, r.rb.y - 1, 0, color, 0, 0);
    UIRender->PushPoint(r.lt.x, r.rb.y - 1, 0, color, 0, 0);
    UIRender->PushPoint(r.lt.x, r.lt.y, 0, color, 0, 0);

    //.	UIRender->FlushLineStrip();
    UIRender->FlushPrimitive();

    if (g_show_wnd_rect_text && name.size())
    {
        CGameFont* F = UI()->Font()->pFontDI;
        const float x = r.lt.x - (r.lt.x >= 20 ? 20 : 0);
        const float y = r.lt.y > Device.dwHeight / 2 ? r.lt.y - F->CurrentHeight_() - 20 : r.rb.y + 20;
        F->Out(x, y, name.c_str());
        F->SetColor(D3DCOLOR_XRGB(255, 0, 255));
    }
}

void draw_wnds_rects()
{
    if (g_wnds_rects.empty())
        return;

    for (auto& [name, r] : g_wnds_rects)
    {
        UI()->ClientToScreenScaled(r.lt, r.lt.x, r.lt.y);
        UI()->ClientToScreenScaled(r.rb, r.rb.x, r.rb.y);
        draw_rect(r, color_rgba(255, 0, 0, 255), name);
    }

    g_wnds_rects.clear();
}

void CUIWindow::SetPPMode()
{
    m_bPP = true;
    MainMenu()->RegisterPPDraw(this);
    Show(false);
};

void CUIWindow::ResetPPMode()
{
    if (GetPPMode())
    {
        MainMenu()->UnregisterPPDraw(this);
        m_bPP = false;
    }
}

CUIWindow::CUIWindow()
{
    //.	m_dbg_flag.zero			();
    m_pFont = NULL;
    m_pParentWnd = NULL;
    Reset();
    m_pMessageTarget = NULL;
    m_pKeyboardCapturer = NULL;
    SetWndRect(0, 0, 0, 0);
    m_bAutoDelete = false;
    SetVisible(true);
    Enable(true);
    m_bCursorOverWindow = false;
    m_bCursorOverWindowChanged = false;
    m_bClickable = false;
    m_bPP = false;
    m_dwFocusReceiveTime = 0;
#ifdef LOG_ALL_WNDS
    ListWndCount++;
    m_dbg_id = ListWndCount;
    dbg_list_wnds.push_back(DBGList());
    dbg_list_wnds.back().num = m_dbg_id;
    dbg_list_wnds.back().closed = false;
#endif
}

CUIWindow::~CUIWindow()
{
    VERIFY(!(GetParent() && IsAutoDelete()));

    CUIWindow* parent = GetParent();
    if (parent)
        parent->DetachChild(this, true);

    DetachAll();

    if (GetPPMode())
        MainMenu()->UnregisterPPDraw(this);

#ifdef LOG_ALL_WNDS
    xr_vector<DBGList>::iterator _it = dbg_list_wnds.begin();
    bool bOK = false;
    for (; _it != dbg_list_wnds.end(); ++_it)
    {
        if ((*_it).num == m_dbg_id && !(*_it).closed)
        {
            bOK = true;
            (*_it).closed = true;
            dbg_list_wnds.erase(_it);
            break;
        }
        if ((*_it).num == m_dbg_id && (*_it).closed)
        {
            Msg("--CUIWindow [%d] already deleted", m_dbg_id);
            bOK = true;
        }
    }
    if (!bOK)
        Msg("CUIWindow::~CUIWindow.[%d] cannot find window in list", m_dbg_id);
#endif
}

void CUIWindow::Init(Frect* pRect) { SetWndRect(*pRect); }

void CUIWindow::Draw()
{
    for (WINDOW_LIST_it it = m_ChildWndList.begin(); m_ChildWndList.end() != it; ++it)
    {
        if (!(*it)->IsVisibleForRender())
            continue;

        if ((*it)->GetCustomDraw())
            continue;

        (*it)->DrawWithAnimation();
    }

    m_wasDrawn = true;

    if (g_show_wnd_rect2)
    {
        Frect r;
        GetAbsoluteRect(r);
        add_rect_to_draw(r, m_windowName);
    }
}

void CUIWindow::DrawWithAnimation()
{
    if (g_bypass_window_animations)
    {
        Draw();
        m_wasDrawn = true;
        return;
    }

    Frect rect;
    GetAbsoluteRect(rect);
    Fvector2 pivot;
    UI()->ClientToScreenScaled(pivot, (rect.x1 + rect.x2) * .5f, (rect.y1 + rect.y2) * .5f);
    UIRender->PushAnimationTransform(m_animationAlpha, m_animationOffset.x + m_motionOffset.x,
        m_animationOffset.y + m_motionOffset.y, m_animationRotation + m_rotation,
        pivot.x, pivot.y);
    Draw();
    UIRender->PopAnimationTransform();
    m_wasDrawn = true;
}

void CUIWindow::DrawWithoutAnimation()
{
    const bool previous_bypass = g_bypass_window_animations;
    g_bypass_window_animations = true;
    UIRender->PushIdentityAnimationTransform();
    Draw();
    UIRender->PopAnimationTransform();
    g_bypass_window_animations = previous_bypass;
    m_wasDrawn = true;
}

void CUIWindow::Draw(float x, float y)
{
    SetWndPos(x, y);
    DrawWithAnimation();
}

bool CUIWindow::CapturesFocusToo() { return GetMouseCapturer() ? GetMouseCapturer()->CapturesFocusToo() : true; }

void CUIWindow::UpdateFocus(bool focus_lost)
{
    bool cursor_on_window;
    if (focus_lost)
    {
        cursor_on_window = false;
    }
    else
    {
        Fvector2 temp = GetUICursor()->GetCursorPosition();
        Frect r;
        GetAbsoluteRect(r);
        cursor_on_window = !!r.in(temp);
        if (!cursor_on_window && (!GetMouseCapturer() || !GetMouseCapturer()->CapturesFocusToo()))
            focus_lost = true;
    }

    if (cursor_on_window && g_show_wnd_rect)
    {
        Frect r;
        GetAbsoluteRect(r);
        add_rect_to_draw(r, m_windowName);
    }

    // RECEIVE and LOST focus
    m_bCursorOverWindowChanged = (m_bCursorOverWindow != cursor_on_window);
    if (GetMouseCapturer() && GetMouseCapturer()->CapturesFocusToo())
        GetMouseCapturer()->UpdateFocus(focus_lost);
    else
    {
        for (auto it = m_ChildWndList.begin(); m_ChildWndList.end() != it; ++it)
            if ((*it)->IsShown())
                (*it)->UpdateFocus(focus_lost);
    }
}

void CUIWindow::CommitFocus(bool focus_lost)
{
    if (m_bCursorOverWindowChanged && m_bCursorOverWindow == focus_lost)
    {
        if (m_bCursorOverWindow)
            OnFocusLost();
        else
            OnFocusReceive();
        m_bCursorOverWindowChanged = false;
    }

    for (auto it = m_ChildWndList.begin(); m_ChildWndList.end() != it; ++it)
        if ((*it)->IsShown())
            (*it)->CommitFocus(focus_lost);
}

void CUIWindow::Update()
{
    UpdateAnimation();
    UpdateMotion();
    UpdateRotation();

    for (auto it = m_ChildWndList.begin(); m_ChildWndList.end() != it; ++it)
        if ((*it)->IsVisibleForRender())
            (*it)->Update();
}

CUIWindow::EMotionEffect CUIWindow::ParseMotionEffect(LPCSTR effect)
{
    if (!effect || !effect[0] || 0 == xr_strcmp(effect, "none"))
        return EMotionEffect::None;
    if (0 == xr_strcmp(effect, "parallax"))
        return EMotionEffect::Parallax;
    if (0 == xr_strcmp(effect, "panorama"))
        return EMotionEffect::Panorama;

    Msg("! Unknown UI motion effect [%s], using [none]", effect);
    return EMotionEffect::None;
}

void CUIWindow::SetMotionEffect(LPCSTR effect)
{
    m_motionEffect = ParseMotionEffect(effect);

    switch (m_motionEffect)
    {
    case EMotionEffect::Parallax:
        m_motionMouseStrength.set(8.f, 5.f);
        m_motionAutoStrength.set(1.2f, .8f);
        m_motionSpeed = .35f;
        m_motionSmoothing = 8.f;
        break;
    case EMotionEffect::Panorama:
        m_motionMouseStrength.set(14.f, 7.f);
        m_motionAutoStrength.set(8.f, 2.5f);
        m_motionSpeed = .1f;
        m_motionSmoothing = 4.f;
        break;
    default:
        m_motionMouseStrength.set(0.f, 0.f);
        m_motionAutoStrength.set(0.f, 0.f);
        m_motionSpeed = 0.f;
        m_motionOffset.set(0.f, 0.f);
        break;
    }
}

void CUIWindow::SetMotionMouseStrength(float x, float y) { m_motionMouseStrength.set(x, y); }
void CUIWindow::SetMotionAutoStrength(float x, float y) { m_motionAutoStrength.set(x, y); }
void CUIWindow::SetMotionSpeed(float cycles_per_second) { m_motionSpeed = _max(cycles_per_second, 0.f); }
void CUIWindow::SetMotionSmoothing(float smoothing) { m_motionSmoothing = _max(smoothing, 0.f); }
void CUIWindow::SetMotionPhase(float degrees) { m_motionPhase = degrees; }

void CUIWindow::UpdateMotion()
{
    if (m_motionEffect == EMotionEffect::None)
    {
        m_motionOffset.set(0.f, 0.f);
        return;
    }

    const Fvector2 cursor = GetUICursor()->GetCursorPosition();
    const float cursor_x = clampr(cursor.x / (UI_BASE_WIDTH * .5f) - 1.f, -1.f, 1.f);
    const float cursor_y = clampr(cursor.y / (UI_BASE_HEIGHT * .5f) - 1.f, -1.f, 1.f);
    const float phase = m_motionPhase * (PI / 180.f);
    const float angle = Device.dwTimeContinual * .001f * m_motionSpeed * PI_MUL_2 + phase;

    Fvector2 target;
    target.x = -cursor_x * m_motionMouseStrength.x + _sin(angle) * m_motionAutoStrength.x;
    target.y = -cursor_y * m_motionMouseStrength.y + _cos(angle * .83f + phase) * m_motionAutoStrength.y;

    const float dt = clampr(Device.fTimeDelta, 0.f, .1f);
    const float blend = m_motionSmoothing > 0.f ? 1.f - expf(-m_motionSmoothing * dt) : 1.f;
    m_motionOffset.x += (target.x - m_motionOffset.x) * blend;
    m_motionOffset.y += (target.y - m_motionOffset.y) * blend;
}

CUIWindow::EAnimationPreset CUIWindow::ParseAnimationPreset(LPCSTR preset)
{
    if (!preset || !preset[0] || 0 == xr_strcmp(preset, "curve_fade") || 0 == xr_strcmp(preset, "curve-fade"))
        return EAnimationPreset::CurveFade;
    if (0 == xr_strcmp(preset, "none") || 0 == xr_strcmp(preset, "instant"))
        return EAnimationPreset::None;
    if (0 == xr_strcmp(preset, "fade"))
        return EAnimationPreset::Fade;
    if (0 == xr_strcmp(preset, "slide_left") || 0 == xr_strcmp(preset, "slide-left"))
        return EAnimationPreset::SlideLeft;
    if (0 == xr_strcmp(preset, "slide_right") || 0 == xr_strcmp(preset, "slide-right"))
        return EAnimationPreset::SlideRight;
    if (0 == xr_strcmp(preset, "slide_up") || 0 == xr_strcmp(preset, "slide-up"))
        return EAnimationPreset::SlideUp;
    if (0 == xr_strcmp(preset, "slide_down") || 0 == xr_strcmp(preset, "slide-down"))
        return EAnimationPreset::SlideDown;
    if (0 == xr_strcmp(preset, "rotate_cw") || 0 == xr_strcmp(preset, "rotate-cw") ||
        0 == xr_strcmp(preset, "rotate_clockwise"))
        return EAnimationPreset::RotateClockwise;
    if (0 == xr_strcmp(preset, "rotate_ccw") || 0 == xr_strcmp(preset, "rotate-ccw") ||
        0 == xr_strcmp(preset, "rotate_counterclockwise"))
        return EAnimationPreset::RotateCounterClockwise;

    Msg("! Unknown UI animation preset [%s], using [curve_fade]", preset);
    return EAnimationPreset::CurveFade;
}

void CUIWindow::SetAnimationPreset(LPCSTR preset) { SetAnimationPresets(preset, preset); }

void CUIWindow::SetAnimationPresets(LPCSTR show_preset, LPCSTR hide_preset)
{
    m_showAnimation = ParseAnimationPreset(show_preset);
    m_hideAnimation = ParseAnimationPreset(hide_preset);

    if (m_animationState == EAnimationState::Showing && fis_zero(m_animationAlpha))
    {
        m_animationStartOffset = m_animationOffset = animation_offset(m_showAnimation, m_animationDistance);
        m_animationStartRotation = m_animationRotation =
            animation_rotation(m_showAnimation, m_animationRotationAmount);
    }

    if ((m_requestedVisible && m_showAnimation == EAnimationPreset::None) || (!m_requestedVisible && m_hideAnimation == EAnimationPreset::None))
        ShowImmediate(m_requestedVisible);
}

void CUIWindow::SetAnimationTimes(float show_time_ms, float hide_time_ms)
{
    m_showAnimationTime = _max(show_time_ms, 0.f);
    m_hideAnimationTime = _max(hide_time_ms, 0.f);
}

void CUIWindow::SetAnimationDelay(float delay_ms) { m_animationDelay = _max(delay_ms, 0.f); }

void CUIWindow::SetAnimationDistance(float distance)
{
    m_animationDistance = _max(distance, 0.f);
    if (m_animationState == EAnimationState::Showing && !m_animationStarted)
        m_animationStartOffset = m_animationOffset = animation_offset(m_showAnimation, m_animationDistance);
}

void CUIWindow::SetAnimationRotation(float degrees)
{
    m_animationRotationAmount = deg2rad(_abs(degrees));
    if (m_animationState == EAnimationState::Showing && !m_animationStarted)
        m_animationStartRotation = m_animationRotation =
            animation_rotation(m_showAnimation, m_animationRotationAmount);
}

void CUIWindow::SetRotation(float degrees) { m_rotation = deg2rad(degrees); }
float CUIWindow::GetRotation() const { return rad2deg(m_rotation); }
void CUIWindow::SetRotationSpeed(float degrees_per_second) { m_rotationTargetSpeed = deg2rad(degrees_per_second); }
void CUIWindow::SetRotationSmoothing(float smoothing) { m_rotationSmoothing = _max(smoothing, 0.f); }

void CUIWindow::UpdateRotation()
{
    if (fis_zero(m_rotationSpeed) && fis_zero(m_rotationTargetSpeed))
        return;

    const float dt = clampr(Device.fTimeDelta, 0.f, .1f);
    const float blend = m_rotationSmoothing > 0.f ? 1.f - expf(-m_rotationSmoothing * dt) : 1.f;
    m_rotationSpeed += (m_rotationTargetSpeed - m_rotationSpeed) * blend;
    if (fis_zero(m_rotationTargetSpeed) && _abs(m_rotationSpeed) < EPS_S)
        m_rotationSpeed = 0.f;
    m_rotation = angle_normalize(m_rotation + m_rotationSpeed * dt);
}

void CUIWindow::ShowImmediate(bool status)
{
    m_requestedVisible = status;
    m_animationState = EAnimationState::Idle;
    m_animationStarted = false;
    m_animationAlpha = status ? 1.f : 0.f;
    m_animationOffset.set(0.f, 0.f);
    m_animationRotation = 0.f;
    SetVisible(status);
    Enable(status);
}

void CUIWindow::Show(bool status)
{
    if (status == m_requestedVisible)
    {
        if (status)
        {
            SetVisible(true);
            Enable(true);
        }
        return;
    }

    // Layout code commonly hides optional tabs before their first frame. Those
    // windows must never flash while the interface is being assembled.
    if (!status && !m_wasDrawn)
    {
        ShowImmediate(false);
        return;
    }

    StartAnimation(status);
}

void CUIWindow::StartAnimation(bool show)
{
    m_requestedVisible = show;
    Enable(show);

    const EAnimationPreset preset = show ? m_showAnimation : m_hideAnimation;
    const float duration = show ? m_showAnimationTime : m_hideAnimationTime;
    if (preset == EAnimationPreset::None || fis_zero(duration))
    {
        ShowImmediate(show);
        return;
    }

    SetVisible(true);
    m_animationState = show ? EAnimationState::Showing : EAnimationState::Hiding;
    m_animationStarted = false;
    m_animationStartAlpha = m_animationAlpha;
    m_animationStartOffset = m_animationOffset;
    m_animationStartRotation = m_animationRotation;

    if (show && fis_zero(m_animationStartAlpha))
    {
        m_animationStartOffset = m_animationOffset = animation_offset(preset, m_animationDistance);
        m_animationStartRotation = m_animationRotation = animation_rotation(preset, m_animationRotationAmount);
    }

    m_animationTargetOffset = show ? Fvector2{} : animation_offset(preset, m_animationDistance);
    m_animationTargetRotation = show ? 0.f : animation_rotation(preset, m_animationRotationAmount);
}

void CUIWindow::UpdateAnimation()
{
    if (m_animationState == EAnimationState::Idle)
        return;

    const EAnimationPreset preset = m_animationState == EAnimationState::Showing ? m_showAnimation : m_hideAnimation;
    const float duration = m_animationState == EAnimationState::Showing ? m_showAnimationTime : m_hideAnimationTime;

    if (!m_animationStarted)
    {
        m_animationStartTime = Device.dwTimeContinual;
        m_animationStarted = true;
    }

    const float elapsed = static_cast<float>(Device.dwTimeContinual - m_animationStartTime);
    if (elapsed < m_animationDelay)
        return;

    const float progress = duration > 0.f ? (elapsed - m_animationDelay) / duration : 1.f;
    const float curve = animation_curve(preset, progress);
    const float target_alpha = m_animationState == EAnimationState::Showing ? 1.f : 0.f;
    m_animationAlpha = m_animationStartAlpha + (target_alpha - m_animationStartAlpha) * curve;
    m_animationOffset.x = m_animationStartOffset.x + (m_animationTargetOffset.x - m_animationStartOffset.x) * curve;
    m_animationOffset.y = m_animationStartOffset.y + (m_animationTargetOffset.y - m_animationStartOffset.y) * curve;
    m_animationRotation = m_animationStartRotation + (m_animationTargetRotation - m_animationStartRotation) * curve;

    if (progress < 1.f)
        return;

    const bool shown = m_animationState == EAnimationState::Showing;
    m_animationState = EAnimationState::Idle;
    m_animationStarted = false;
    m_animationAlpha = shown ? 1.f : 0.f;
    m_animationOffset.set(0.f, 0.f);
    m_animationRotation = 0.f;
    SetVisible(shown);
}

void CUIWindow::AttachChild(CUIWindow* pChild, bool bottom)
{
    if (!pChild)
        return;

    R_ASSERT(!IsChild(pChild));
    pChild->SetParent(this);
    if (bottom)
        m_ChildWndList.push_front(pChild);
    else
        m_ChildWndList.push_back(pChild);
}

void CUIWindow::DoDetachChild(CUIWindow* pChild, bool from_destructor)
{
    if (!pChild)
        return;

    if (GetMouseCapturer() == pChild)
        SetMouseCapture(pChild, false);

    pChild->SetParent(NULL);

    if (from_destructor && pChild->IsAutoDelete())
    {
        Msg("!![" __FUNCTION__ "] detaching autodelete window from destructor : [%s]", pChild->WindowName_script());
        // LogStackTrace("");
    }

    if (pChild->IsAutoDelete() && !from_destructor)
        xr_delete(pChild);
}

void CUIWindow::DetachChild(CUIWindow* pChild, bool from_destructor)
{
    if (!pChild)
        return;

    __try
    {
        m_ChildWndList.remove(pChild);
    }
    __except (ExceptStackTrace("Exception catched in m_ChildWndList.remove(pChild)"))
    {
        FATAL("Exception catched in m_ChildWndList.remove(pChild)! Please send logs and minidumps to the engine developers!");
    }

    DoDetachChild(pChild, from_destructor);
}

void CUIWindow::DetachAll()
{
    auto tmp_m_ChildWndList = m_ChildWndList; //-V826
    m_ChildWndList.clear();
    for (CUIWindow* pChild : tmp_m_ChildWndList)
        DoDetachChild(pChild);
}

void CUIWindow::GetAbsoluteRect(Frect& r)
{
    //.	Frect rect;

    if (GetParent() == NULL)
    {
        GetWndRect(r);
        return;
    }
    //.	rect = GetParent()->GetAbsoluteRect();
    GetParent()->GetAbsoluteRect(r);

    Frect rr;
    GetWndRect(rr);
    r.left += rr.left;
    r.top += rr.top;
    r.right = r.left + GetWidth();
    r.bottom = r.top + GetHeight();
    //.	return			rect;
}

//реакция на мышь
//координаты курсора всегда, кроме начального вызова
//задаются относительно текущего окна

bool CUIWindow::OnMouse(float x, float y, EUIMessages mouse_action)
{
    Frect wndRect = GetWndRect();

    cursor_pos.x = x;
    cursor_pos.y = y;

    if (GetParent() == NULL)
    {
        if (!wndRect.in(cursor_pos))
            return false;
        //получить координаты относительно окна
        cursor_pos.x -= wndRect.left;
        cursor_pos.y -= wndRect.top;
    }

    //если есть дочернее окно,захватившее мышь, то
    //сообщение направляем ему сразу
    if (GetMouseCapturer())
    {
        GetMouseCapturer()->OnMouse(cursor_pos.x - GetMouseCapturer()->GetWndRect().left, cursor_pos.y - GetMouseCapturer()->GetWndRect().top, mouse_action);
        return true;
    }

    // handle any action
    switch (mouse_action)
    {
    case WINDOW_MOUSE_MOVE: OnMouseMove(); break;
    case WINDOW_MOUSE_WHEEL_DOWN: OnMouseScroll(WINDOW_MOUSE_WHEEL_DOWN); break;
    case WINDOW_MOUSE_WHEEL_UP: OnMouseScroll(WINDOW_MOUSE_WHEEL_UP); break;
    case WINDOW_LBUTTON_DOWN:
        if (OnMouseDown(MOUSE_1))
            return true;
        break;
    case WINDOW_RBUTTON_DOWN:
        if (OnMouseDown(MOUSE_2))
            return true;
        break;
    case WINDOW_CBUTTON_DOWN:
        if (OnMouseDown(MOUSE_3))
            return true;
        break;
    case WINDOW_LBUTTON_UP:
        if (OnMouseUp(MOUSE_1))
            return true;
        break;
    case WINDOW_RBUTTON_UP:
        if (OnMouseUp(MOUSE_2))
            return true;
        break;
    case WINDOW_CBUTTON_UP:
        if (OnMouseUp(MOUSE_3))
            return true;
        break;
    case WINDOW_LBUTTON_DB_CLICK:
        if (OnDbClick())
            return true;
        break;
    default: break;
    }

    //Проверка на попадание мыши в окно,
    //происходит в обратном порядке, чем рисование окон
    //(последние в списке имеют высший приоритет)
    WINDOW_LIST::reverse_iterator it = m_ChildWndList.rbegin();

    for (; it != m_ChildWndList.rend(); ++it)
    {
        CUIWindow* w = (*it);
        if (!w->IsShown())
            continue;
        Frect wndRect = w->GetWndRect();
        if (wndRect.in(cursor_pos))
        {
            if (w->IsEnabled())
            {
                if (w->OnMouse(cursor_pos.x - w->GetWndRect().left, cursor_pos.y - w->GetWndRect().top, mouse_action))
                    return true;
            }
        }
        else if (w->IsEnabled() && w->CursorOverWindow())
        {
            if (w->OnMouse(cursor_pos.x - w->GetWndRect().left, cursor_pos.y - w->GetWndRect().top, mouse_action))
                return true;
        }
    }

    return false;
}

bool CUIWindow::HasChildMouseHandler()
{
    WINDOW_LIST::reverse_iterator it = m_ChildWndList.rbegin();

    for (; it != m_ChildWndList.rend(); ++it)
    {
        if ((*it)->m_bClickable)
        {
            Frect wndRect = (*it)->GetWndRect();
            if (wndRect.in(cursor_pos))
                return true;
        }
    }

    return false;
}

void CUIWindow::OnMouseMove() {}

void CUIWindow::OnMouseScroll(float iDirection) {}

bool CUIWindow::OnDbClick()
{
    if (GetMessageTarget())
        GetMessageTarget()->SendMessage(this, WINDOW_LBUTTON_DB_CLICK);
    return false;
}

bool CUIWindow::OnMouseDown(int mouse_btn) { return false; }

bool CUIWindow::OnMouseUp(int mouse_btn) { return false; }

void CUIWindow::OnFocusReceive()
{
    m_dwFocusReceiveTime = Device.dwTimeGlobal;
    m_bCursorOverWindow = true;
}

void CUIWindow::OnFocusLost()
{
    m_dwFocusReceiveTime = 0;
    m_bCursorOverWindow = false;
}

//Сообщение, посылаемое дочерним окном,
//о том, что окно хочет захватить мышь,
//все сообщения от нее будут направляться только
//ему в независимости от того где мышь
void CUIWindow::SetMouseCapture(CUIWindow* pChildWindow, bool capture_status)
{
    if (GetParent())
    {
        if (!m_pOrignMouseCapturer || m_pOrignMouseCapturer == pChildWindow)
            GetParent()->SetMouseCapture(this, capture_status);
    }

    if (capture_status)
    {
        //оповестить дочернее окно о потере фокуса мыши
        if (m_pMouseCapturer && m_pMouseCapturer != pChildWindow)
            m_pMouseCapturer->SendMessage(this, WINDOW_MOUSE_CAPTURE_LOST);
        m_pMouseCapturer = pChildWindow;
    }
    else
    {
        ASSERT_FMT_DBG((m_pMouseCapturer && m_pMouseCapturer == pChildWindow), "[%s]: [%s] trying to reset m_pMouseCapturer[%s]", __FUNCTION__, pChildWindow->WindowName().c_str(),
                       m_pMouseCapturer ? m_pMouseCapturer->WindowName().c_str() : "");
        m_pMouseCapturer = nullptr;
    }
}

CUIWindow* CUIWindow::GetMouseCapturer() { return m_pMouseCapturer; }

//реакция на клавиатуру
bool CUIWindow::OnKeyboard(int dik, EUIMessages keyboard_action)
{
    //если есть дочернее окно,захватившее клавиатуру, то сообщение направляем ему сразу
    if (m_pKeyboardCapturer)
    {
        if (m_pKeyboardCapturer->OnKeyboard(dik, keyboard_action))
        {
            return true;
        }
    }

    size_t processed = 0;
    auto iter = m_ChildWndList.rbegin();
    while (iter != m_ChildWndList.rend())
    {
        const auto size = m_ChildWndList.size();

        auto* Wnd = *(iter++);

        ASSERT_FMT_DBG(Wnd, "!![%s][%s] Child wnd is nullptr! Something strange!", __FUNCTION__, this->WindowName_script());

        if (Wnd && Wnd->IsEnabled())
        {
            if (Wnd->OnKeyboard(dik, keyboard_action))
            {
                return true;
            }
        }

        if (size != m_ChildWndList.size())
        {
            iter = m_ChildWndList.rbegin();
            std::advance(iter, processed);
        }
        else
        {
            processed++;
        }
    }

    return false;
}

bool CUIWindow::OnKeyboardHold(int dik)
{
    if (m_pKeyboardCapturer)
        if (m_pKeyboardCapturer->OnKeyboardHold(dik))
            return true;

    size_t processed = 0;
    auto iter = m_ChildWndList.rbegin();
    while (iter != m_ChildWndList.rend())
    {
        const auto size = m_ChildWndList.size();

        auto* Wnd = *(iter++);

        ASSERT_FMT_DBG(Wnd, "!![%s][%s] Child wnd is nullptr! Something strange!", __FUNCTION__, this->WindowName_script());

        if (Wnd && Wnd->IsEnabled())
        {
            if (Wnd->OnKeyboardHold(dik))
            {
                return true;
            }
        }

        if (size != m_ChildWndList.size())
        {
            iter = m_ChildWndList.rbegin();
            std::advance(iter, processed);
        }
        else
        {
            processed++;
        }
    }

    return false;
}

void CUIWindow::SetKeyboardCapture(CUIWindow* pChildWindow, bool capture_status)
{
    if (NULL != GetParent())
        GetParent()->SetKeyboardCapture(this, capture_status);

    if (capture_status)
    {
        //оповестить дочернее окно о потере фокуса клавиатуры
        if (NULL != m_pKeyboardCapturer)
            m_pKeyboardCapturer->SendMessage(this, WINDOW_KEYBOARD_CAPTURE_LOST);

        m_pKeyboardCapturer = pChildWindow;
    }
    else
        m_pKeyboardCapturer = NULL;
}

//обработка сообщений
void CUIWindow::SendMessage(CUIWindow* pWnd, s16 msg, void* pData)
{
    //оповестить дочерние окна
    for (WINDOW_LIST_it it = m_ChildWndList.begin(); m_ChildWndList.end() != it; ++it)
    {
        if ((*it)->IsEnabled())
            (*it)->SendMessage(pWnd, msg, pData);
    }
}

CUIWindow* CUIWindow::GetCurrentMouseHandler() { return GetTop()->GetChildMouseHandler(); }

CUIWindow* CUIWindow::GetChildMouseHandler()
{
    CUIWindow* pWndResult;
    WINDOW_LIST::reverse_iterator it = m_ChildWndList.rbegin();

    for (; it != m_ChildWndList.rend(); ++it)
    {
        Frect wndRect = (*it)->GetWndRect();
        // very strange code.... i can't understand difference between
        // first and second condition. I Got It from OnMouse() method;
        if (wndRect.in(cursor_pos))
        {
            if ((*it)->IsEnabled())
            {
                return pWndResult = (*it)->GetChildMouseHandler();
            }
        }
        else if ((*it)->IsEnabled() && (*it)->CursorOverWindow())
        {
            return pWndResult = (*it)->GetChildMouseHandler();
        }
    }

    return this;
}

//перемесчтить окно на вершину.
// false если такого дочернего окна нет
bool CUIWindow::BringToTop(CUIWindow* pChild)
{
    //найти окно в списке
    if (!IsChild(pChild))
        return false;

    //удалить со старого места
    m_ChildWndList.remove(pChild);

    //поместить на вершину списка
    m_ChildWndList.push_back(pChild);

    return true;
}

bool CUIWindow::BringToBottom(CUIWindow* pChild)
{
    if (!IsChild(pChild))
        return false;
    m_ChildWndList.remove(pChild);
    m_ChildWndList.push_front(pChild);
    return true;
}

//поднять на вершину списка всех родителей окна и его самого
void CUIWindow::BringAllToTop()
{
    if (GetParent() == NULL)
        return;
    else
    {
        GetParent()->BringToTop(this);
        GetParent()->BringAllToTop();
    }
}

//для перевода окна и потомков в исходное состояние
void CUIWindow::Reset() { m_pOrignMouseCapturer = m_pMouseCapturer = nullptr; }

void CUIWindow::ResetAll()
{
    //.	m_dbg_flag.set(128,TRUE);
    for (WINDOW_LIST_it it = m_ChildWndList.begin(); m_ChildWndList.end() != it; ++it)
    {
        (*it)->Reset();
    }
    //.	m_dbg_flag.set(128,FALSE);
}

CUIWindow* CUIWindow::GetMessageTarget() { return m_pMessageTarget ? m_pMessageTarget : GetParent(); }

bool CUIWindow::IsChild(CUIWindow* pPossibleChild) const
{
    WINDOW_LIST::const_iterator it = std::find(m_ChildWndList.begin(), m_ChildWndList.end(), pPossibleChild);
    return it != m_ChildWndList.end();
}

CUIWindow* CUIWindow::FindChild(const shared_str name, u32 max_nested)
{
    if (WindowName() == name)
        return this;

    if (0 == max_nested)
        return NULL;

    //.	m_dbg_flag.set(256,TRUE);
    WINDOW_LIST::const_iterator it = m_ChildWndList.begin();
    WINDOW_LIST::const_iterator it_e = m_ChildWndList.end();
    for (; it != it_e; ++it)
    {
        CUIWindow* pRes = (*it)->FindChild(name, max_nested - 1);
        if (pRes != NULL)
            return pRes;
    }

    //.	m_dbg_flag.set(256,FALSE);
    return NULL;
}

const shared_str CUIWindow::WindowName() const
{
    if (0 != m_windowName.size())
        return m_windowName;

    if (NULL == GetParent())
        return m_windowName;

    WINDOW_LIST& pcl = GetParent()->GetChildWndList();
    WINDOW_LIST::const_iterator it = pcl.begin();
    WINDOW_LIST::const_iterator it_e = pcl.end();

    int index = 0;
    for (; it != it_e; ++it)
    {
        if (this == (*it))
        {
            shared_str result;
            result.sprintf("%s.child_%d", GetParent()->WindowName().c_str(), index);
            return result;
        }
        index++;
    }
    return m_windowName;
}

void CUIWindow::SetWindowName(LPCSTR wn, BOOL ifnset)
{
    if (ifnset && 0 != m_windowName.size()) // alpet: имя обновить, только если оно не установленно ранее
        return;
    m_windowName = wn;
}

void CUIWindow::SetParent(CUIWindow* pNewParent)
{
    R_ASSERT(!(m_pParentWnd && m_pParentWnd->IsChild(this)));

    m_pParentWnd = pNewParent;
}

void CUIWindow::ShowChildren(bool show)
{
    //.	m_dbg_flag.set(512,TRUE);
    for (WINDOW_LIST_it it = m_ChildWndList.begin(); m_ChildWndList.end() != it; ++it)
        (*it)->Show(show);
    //.	m_dbg_flag.set(512,FALSE);
}

void CUIWindow::DetachFromParent()
{
    if (!m_pParentWnd)
        return;

    m_pParentWnd->DetachChild(this);
}

void CUIWindow::SortByPriority()
{
    m_ChildWndList.sort([](const auto& a, const auto& b) { return a->priority_index < b->priority_index; });
}
