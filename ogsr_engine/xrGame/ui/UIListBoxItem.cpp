#include "StdAfx.h"
#include "UIListBoxItem.h"
#include "UIControlConfig.h"
#include "UIScrollView.h"
#include "../object_broker.h"

//. u32 CUIListBoxItem::uid_counter = 0;

CUIListBoxItem::CUIListBoxItem()
{
    txt_color = 0xffffffff;
    txt_color_s = 0xffffffff;
    //.	uid					= uid_counter++;
    tag = u32(-1);
    m_bTextureAvailable = false;
}

CUIListBoxItem::~CUIListBoxItem() { delete_data(fields); }

void CUIListBoxItem::SetTAG(u32 value) { tag = value; }

u32 CUIListBoxItem::GetTAG() { return tag; }

void CUIListBoxItem::Draw()
{
    m_bTextureAvailable = m_bSelected;

    u32 CurColor = GetTextColor();
    static const u32 enabled_alpha = clampr(UIControlConfig::ReadInt("list_box", "enabled_alpha", 255), 0, 255);
    static const u32 disabled_alpha = clampr(UIControlConfig::ReadInt("list_box", "disabled_alpha", 128), 0, 255);
    const u32 alpha = IsEnabled() ? enabled_alpha : disabled_alpha;
    u32 ResColor = (alpha << 24) | (CurColor & 0x00ffffff);
    SetTextColor(ResColor);

    CUILabel::Draw();
}

void CUIListBoxItem::OnFocusReceive()
{
    CUILabel::OnFocusReceive();
    GetMessageTarget()->SendMessage(this, LIST_ITEM_FOCUS_RECEIVED);
}

void CUIListBoxItem::InitDefault()
{
    InitTexture(UIControlConfig::ReadString("list_box", "default_texture", "ui_listline"));
}
bool CUIListBoxItem::OnDbClick()
{
    smart_cast<CUIScrollView*>(GetParent()->GetParent())->SetSelected(this);
    GetMessageTarget()->SendMessage(this, LIST_ITEM_DB_CLICKED, &tag);
    return false;
}

bool CUIListBoxItem::OnMouseDown(int mouse_btn)
{
    if (mouse_btn == MOUSE_1)
    {
        smart_cast<CUIScrollView*>(GetParent()->GetParent())->SetSelected(this);
        GetMessageTarget()->SendMessage(this, LIST_ITEM_SELECT, this);
        GetMessageTarget()->SendMessage(this, LIST_ITEM_CLICKED, this);
        return true;
    }
    else
        return false;
}

void CUIListBoxItem::SetSelected(bool b)
{
    CUISelectable::SetSelected(b);
    u32 col;
    if (b)
        col = txt_color_s;
    else
        col = txt_color;

    SetTextColor(col);
    for (u32 i = 0; i < fields.size(); i++)
        fields[i]->SetTextColor(col);
}

void CUIListBoxItem::SetTextColor(u32 color, u32 color_s)
{
    txt_color = color;
    txt_color_s = color_s;
    SetTextColor(color);
}

float CUIListBoxItem::FieldsLength()
{
    float c = 0;
    for (u32 i = 0; i < fields.size(); i++)
        c += fields[i]->GetWidth();
    return c;
}

CGameFont* CUIListBoxItem::GetFont() { return CUILinesOwner::GetFont(); }

CUIStatic* CUIListBoxItem::AddField(LPCSTR txt, float len, LPCSTR key)
{
    fields.push_back(xr_new<CUIStatic>());
    CUIStatic* st = fields.back();
    AttachChild(st);
    st->Init(FieldsLength(), 0, GetWidth(), len);
    st->SetFont(GetFont());
    st->SetTextAlignment(GetTextAlignment());
    st->SetVTextAlignment(m_lines.GetVTextAlignment());
    st->SetTextColor(GetTextColor());
    st->SetText(txt);
    st->SetWindowName(key);

    return st;
}

LPCSTR CUIListBoxItem::GetField(LPCSTR key)
{
    for (u32 i = 0; i < fields.size(); i++)
    {
        if (0 == xr_strcmp(fields[i]->WindowName(), key))
            return fields[i]->GetText();
    }
    return NULL;
}

void CUIListBoxItem::SetData(void* data) { pData = data; }

void* CUIListBoxItem::GetData() { return pData; }
