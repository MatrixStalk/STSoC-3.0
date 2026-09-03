#include "stdafx.h"
#include "UIWeaponModWnd.h"

#include "UIInventoryWnd.h"
#include "UIXmlInit.h"
#include "UICursor.h"
#include "UIListBoxItem.h"
#include "../Weapon.h"
#include "../inventory.h"
#include "../scope.h"
#include "../silencer.h"
#include "../grenadelauncher.h"
#include "../string_table.h"
#include "../hudmanager.h"
#include "../level.h"
#include "../../Include/xrRender/Kinematics.h"

#include <dinput.h>

namespace
{
constexpr u32 choice_attach = 1;
constexpr u32 choice_detach = 2;
}

void CUIWeaponPreview::Draw()
{
    m_owner->DrawPreview();
}

bool CUIWeaponPreview::OnMouse(float x, float y, EUIMessages mouse_action)
{
    return m_owner->OnPreviewMouse(x, y, mouse_action);
}

CUIWeaponModWnd::CUIWeaponModWnd(CUIInventoryWnd* owner) : m_owner(owner), m_preview(this)
{
    Init();
    Show(false);
}

CUIWeaponModWnd::~CUIWeaponModWnd()
{
    for (SSlotControl& slot : m_slots)
    {
        DetachChild(slot.button);
        xr_delete(slot.button);
    }
    m_slots.clear();
}

void CUIWeaponModWnd::Init()
{
    CUIXml xml;
    const bool loaded = xml.Init(CONFIG_PATH, UI_PATH, "weapon_modification.xml");
    R_ASSERT3(loaded, "file parsing error", xml.m_xml_file_name);
    CUIXmlInit xml_init;

    xml_init.InitWindow(xml, "main", 0, this);

    AttachChild(&m_background);
    xml_init.InitStatic(xml, "background", 0, &m_background);

    AttachChild(&m_preview);
    xml_init.InitWindow(xml, "preview", 0, &m_preview);
    m_rotation_sensitivity = xml.ReadAttribFlt("preview", 0, "rotation_sensitivity", 0.01f);
    m_wheel_step = xml.ReadAttribFlt("preview", 0, "wheel_step", 0.1f);
    m_min_scale = xml.ReadAttribFlt("preview", 0, "min_scale", 0.5f);
    m_max_scale = xml.ReadAttribFlt("preview", 0, "max_scale", 2.f);
    m_default_scale = xml.ReadAttribFlt("preview", 0, "initial_scale", 1.f);
    m_default_rotation.set(
        deg2rad(xml.ReadAttribFlt("preview", 0, "initial_pitch", 0.f)),
        deg2rad(xml.ReadAttribFlt("preview", 0, "initial_yaw", 0.f)), 0.f);
    m_default_offset.set(
        xml.ReadAttribFlt("preview", 0, "initial_offset_x", 0.f),
        xml.ReadAttribFlt("preview", 0, "initial_offset_y", 0.f),
        xml.ReadAttribFlt("preview", 0, "initial_offset_z", 0.f));
    m_default_center_bone = xml.ReadAttrib("preview", 0, "center_bone", "");
    m_scale = m_default_scale;
    m_rotation = m_default_rotation;
    m_offset = m_default_offset;

    AttachChild(&m_title);
    xml_init.InitStatic(xml, "title", 0, &m_title);
    AttachChild(&m_help);
    xml_init.InitStatic(xml, "help", 0, &m_help);

    AttachChild(&m_apply);
    xml_init.Init3tButton(xml, "apply", 0, &m_apply);
    m_apply.SetMessageTarget(this);
    AttachChild(&m_cancel);
    xml_init.Init3tButton(xml, "cancel", 0, &m_cancel);
    m_cancel.SetMessageTarget(this);

    AttachChild(&m_choices);
    m_choices.Init(0.f, 0.f, 320.f, 300.f);
    m_choices.SetMessageTarget(this);
    m_choices.Hide();

    if (xml.NavigateToNode("action_sounds", 0))
    {
        XML_NODE* stored_root = xml.GetLocalRoot();
        xml.SetLocalRoot(xml.NavigateToNode("action_sounds", 0));
        create_ui_snd(m_sounds[eSoundOpen], xml.Read("open", 0, nullptr));
        create_ui_snd(m_sounds[eSoundClose], xml.Read("close", 0, nullptr));
        create_ui_snd(m_sounds[eSoundSelect], xml.Read("select", 0, nullptr));
        create_ui_snd(m_sounds[eSoundAttach], xml.Read("attach", 0, nullptr));
        create_ui_snd(m_sounds[eSoundDetach], xml.Read("detach", 0, nullptr));
        create_ui_snd(m_sounds[eSoundApply], xml.Read("apply", 0, nullptr));
        xml.SetLocalRoot(stored_root);
    }
}

void CUIWeaponModWnd::PlaySound(ESound sound)
{
    if (m_sounds[sound]._handle())
        m_sounds[sound].play(nullptr, sm_2D);
}

void CUIWeaponModWnd::Open(CWeapon* weapon, CInventory* inventory)
{
    m_weapon = weapon;
    m_inventory = inventory;
    m_rotation = m_default_rotation;
    m_offset = m_default_offset;
    m_scale = m_default_scale;
    m_center_bone = m_default_center_bone;
    m_center_bone_id = BI_NONE;
    if (m_weapon)
    {
        LPCSTR section = m_weapon->CInventoryItem::object().cNameSect().c_str();
        if (pSettings->line_exist(section, "attachment_editor_position"))
            m_offset = pSettings->r_fvector3(section, "attachment_editor_position");
        if (pSettings->line_exist(section, "attachment_editor_rotation"))
        {
            m_rotation = pSettings->r_fvector3(section, "attachment_editor_rotation");
            m_rotation.mul(PI / 180.f);
        }
        m_scale = READ_IF_EXISTS(pSettings, r_float, section, "attachment_editor_scale", m_scale);
        if (pSettings->line_exist(section, "attachment_editor_center_bone"))
            m_center_bone = pSettings->r_string(section, "attachment_editor_center_bone");

        if (IKinematics* model = smart_cast<IKinematics*>(m_weapon->CInventoryItem::object().Visual());
            model && m_center_bone.c_str() && m_center_bone.c_str()[0])
        {
            m_center_bone_id = model->LL_BoneID(m_center_bone.c_str());
            if (m_center_bone_id == BI_NONE)
                Msg("! Weapon attachment editor: center bone [%s] not found in [%s]", m_center_bone.c_str(), section);
        }
    }
    m_selected_visual_index = u8(-1);
    m_choices.Hide();
    RebuildSlots();
    Show(true);
    BringAllToTop();
    PlaySound(eSoundOpen);
}

void CUIWeaponModWnd::Close()
{
    const bool was_shown = IsShown();
    m_dragging = false;
    if (GetMouseCapturer() == &m_preview)
        SetMouseCapture(&m_preview, false);
    m_choices.Hide();
    Show(false);
    m_weapon = nullptr;
    m_inventory = nullptr;
    m_selected_visual_index = u8(-1);
    if (was_shown)
        PlaySound(eSoundClose);
}

void CUIWeaponModWnd::Reset()
{
    Close();
    inherited::Reset();
}

bool CUIWeaponModWnd::OnKeyboard(int dik, EUIMessages keyboard_action)
{
    if (m_choices.IsShown())
    {
        m_choices.OnKeyboard(dik, keyboard_action);
        return true;
    }
    if (keyboard_action == WINDOW_KEY_PRESSED && dik == DIK_ESCAPE)
    {
        Close();
        return true;
    }
    inherited::OnKeyboard(dik, keyboard_action);
    return true;
}

void CUIWeaponModWnd::SendMessage(CUIWindow* pWnd, s16 msg, void* pData)
{
    if (msg == BUTTON_CLICKED)
    {
        if (pWnd == &m_apply || pWnd == &m_cancel)
        {
            if (pWnd == &m_apply)
                PlaySound(eSoundApply);
            Close();
            m_owner->InitInventory_delayed();
            return;
        }
        for (const SSlotControl& slot : m_slots)
        {
            if (slot.button == pWnd)
            {
                PlaySound(eSoundSelect);
                OpenSlotChoices(slot.visual_index);
                return;
            }
        }
    }
    else if (pWnd == &m_choices && msg == PROPERTY_CLICKED)
    {
        ProcessChoice();
        return;
    }
    inherited::SendMessage(pWnd, msg, pData);
}

void CUIWeaponModWnd::RebuildSlots()
{
    for (SSlotControl& slot : m_slots)
    {
        DetachChild(slot.button);
        xr_delete(slot.button);
    }
    m_slots.clear();
    m_render_points.clear();
    if (!m_weapon)
        return;

    xr_vector<CWeapon::SAddonUISlot> weapon_slots;
    m_weapon->CollectAddonUISlots(weapon_slots);

    CUIXml xml;
    xml.Init(CONFIG_PATH, UI_PATH, "weapon_modification.xml");
    CUIXmlInit xml_init;
    for (const CWeapon::SAddonUISlot& weapon_slot : weapon_slots)
    {
        SSlotControl& control = m_slots.emplace_back();
        control.visual_index = weapon_slot.visual_index;
        control.installed_section = weapon_slot.installed_section;
        control.button = xr_new<CUI3tButton>();
        control.button->SetAutoDelete(false);
        control.button->SetWindowName(weapon_slot.name.c_str());
        control.button->SetMessageTarget(this);
        AttachChild(control.button);
        xml_init.Init3tButton(xml, "slot_marker", 0, control.button);
        control.button->SetText(weapon_slot.installed_section.c_str() ? "x" : "+");

        SUIModelRenderPoint& point = m_render_points.emplace_back();
        point.world_position = weapon_slot.world_position;
    }
}

void CUIWeaponModWnd::DrawPreview()
{
    if (!m_weapon || !g_pGameLevel || Level().is_removing_objects())
        return;

    CGameObject& weapon_object = m_weapon->CInventoryItem::object();
    if (weapon_object.getDestroy() || !weapon_object.Visual())
        return;

    xr_vector<CWeapon::SAddonUISlot> weapon_slots;
    m_weapon->CollectAddonUISlots(weapon_slots);
    if (weapon_slots.size() != m_slots.size())
        RebuildSlots();
    else
    {
        for (u32 i = 0; i < weapon_slots.size(); ++i)
        {
            m_slots[i].installed_section = weapon_slots[i].installed_section;
            m_slots[i].button->SetText(weapon_slots[i].installed_section.c_str() ? "x" : "+");
            m_render_points[i].world_position = weapon_slots[i].world_position;
        }
    }

    Frect rect;
    m_preview.GetAbsoluteRect(rect);
    SUIModelRenderParams params;
    UI()->ClientToScreenScaled(params.viewport.lt, rect.x1, rect.y1);
    UI()->ClientToScreenScaled(params.viewport.rb, rect.x2, rect.y2);
    params.rotation = m_rotation;
    params.offset = m_offset;
    params.scale = m_scale;
    params.points = &m_render_points;
    if (m_center_bone_id != BI_NONE)
    {
        if (IKinematics* model = smart_cast<IKinematics*>(weapon_object.Visual()))
        {
            const Fmatrix weapon_transform = m_weapon->renderable_WorldTransform();
            weapon_transform.transform_tiny(params.pivot, model->LL_GetTransform(m_center_bone_id).c);
            params.use_pivot = true;
        }
    }
    Render->RenderUIModel(smart_cast<IRenderable*>(&weapon_object), params);

    Fvector2 ui_scale{1.f, 1.f};
    UI()->ClientToScreenScaled(ui_scale);
    Frect root_rect;
    GetAbsoluteRect(root_rect);
    for (u32 i = 0; i < m_slots.size() && i < m_render_points.size(); ++i)
    {
        CUI3tButton* marker = m_slots[i].button;
        const SUIModelRenderPoint& point = m_render_points[i];
        marker->Show(point.visible);
        if (!point.visible)
            continue;
        Fvector2 position;
        position.set(point.screen_position.x / ui_scale.x - root_rect.x1 - marker->GetWidth() * 0.5f,
            point.screen_position.y / ui_scale.y - root_rect.y1 - marker->GetHeight() * 0.5f);
        marker->SetWndPos(position);
    }
}

bool CUIWeaponModWnd::OnPreviewMouse(float x, float y, EUIMessages mouse_action)
{
    const Fvector2 cursor = GetUICursor()->GetCursorPosition();
    switch (mouse_action)
    {
    case WINDOW_LBUTTON_DOWN:
        m_dragging = true;
        m_last_cursor = cursor;
        SetMouseCapture(&m_preview, true);
        return true;
    case WINDOW_LBUTTON_UP:
        if (m_dragging)
        {
            m_dragging = false;
            SetMouseCapture(&m_preview, false);
        }
        return true;
    case WINDOW_MOUSE_CAPTURE_LOST:
        m_dragging = false;
        return true;
    case WINDOW_MOUSE_MOVE:
        if (m_dragging)
        {
            Fvector2 delta;
            delta.sub(cursor, m_last_cursor);
            m_rotation.y += delta.x * m_rotation_sensitivity;
            m_rotation.x += delta.y * m_rotation_sensitivity;
            clamp(m_rotation.x, -PI_DIV_2, PI_DIV_2);
            m_last_cursor = cursor;
            return true;
        }
        break;
    case WINDOW_MOUSE_WHEEL_UP:
        m_scale = _min(m_scale + m_wheel_step, m_max_scale);
        return true;
    case WINDOW_MOUSE_WHEEL_DOWN:
        m_scale = _max(m_scale - m_wheel_step, m_min_scale);
        return true;
    default: break;
    }
    return false;
}

bool CUIWeaponModWnd::ItemMatchesSlot(CInventoryItem* item, u8 visual_index) const
{
    if (!item || !m_weapon || item == m_weapon || item->object().getDestroy() || !m_weapon->CanAttach(item))
        return false;
    if (visual_index == 0)
        return smart_cast<CScope*>(item) != nullptr;
    if (visual_index == 1)
        return smart_cast<CSilencer*>(item) != nullptr;
    if (visual_index == 2)
        return smart_cast<CGrenadeLauncher*>(item) != nullptr;
    if (!pSettings->line_exist(item->object().cNameSect(), "addon_slot"))
        return false;
    return !_stricmp(pSettings->r_string(item->object().cNameSect(), "addon_slot"),
        m_weapon->GetCustomAddonSlotName(static_cast<CWeapon::ECustomAddonSlot>(visual_index - 3)));
}

void CUIWeaponModWnd::OpenSlotChoices(u8 visual_index)
{
    if (!m_weapon || !m_inventory)
        return;
    m_selected_visual_index = visual_index;
    m_choices.RemoveAll();
    bool has_choices = false;

    const auto installed = std::find_if(m_slots.begin(), m_slots.end(), [visual_index](const SSlotControl& slot) {
        return slot.visual_index == visual_index;
    });
    if (installed != m_slots.end() && installed->installed_section.c_str() && m_weapon->CanDetach(installed->installed_section.c_str()))
    {
        LPCSTR inv_name = READ_IF_EXISTS(pSettings, r_string, installed->installed_section, "inv_name", installed->installed_section.c_str());
        string256 label{};
        xr_sprintf(label, "Detach: %s", CStringTable().translate(inv_name).c_str());
        m_choices.AddItem(label, nullptr, choice_detach);
        has_choices = true;
    }

    for (CInventoryItem* item : m_inventory->m_all)
    {
        if (!ItemMatchesSlot(item, visual_index))
            continue;
        m_choices.AddItem(item->m_nameShort.c_str(), item, choice_attach);
        has_choices = true;
    }
    if (!has_choices)
        m_choices.AddItem("No compatible attachments", nullptr, 0);

    m_choices.AutoUpdateSize();
    Frect root_rect;
    GetAbsoluteRect(root_rect);
    Fvector2 cursor = GetUICursor()->GetCursorPosition();
    cursor.sub(root_rect.lt);
    m_choices.Show(root_rect, cursor);
}

void CUIWeaponModWnd::ProcessChoice()
{
    CUIListBoxItem* choice = m_choices.GetClickedItem();
    if (!choice || !m_weapon)
        return;

    if (choice->GetTAG() == choice_attach)
    {
        CInventoryItem* addon = static_cast<CInventoryItem*>(choice->GetData());
        if (addon && ItemMatchesSlot(addon, m_selected_visual_index) && m_weapon->Attach(addon, true))
        {
            PlaySound(eSoundAttach);
        }
    }
    else if (choice->GetTAG() == choice_detach)
    {
        const auto slot = std::find_if(m_slots.begin(), m_slots.end(), [this](const SSlotControl& item) {
            return item.visual_index == m_selected_visual_index;
        });
        if (slot != m_slots.end() && slot->installed_section.c_str() &&
            m_weapon->Detach(slot->installed_section.c_str(), true))
        {
            PlaySound(eSoundDetach);
        }
    }

    m_owner->InitInventory_delayed();
    RebuildSlots();
}
