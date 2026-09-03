#pragma once

#include "UIWindow.h"
#include "UIStatic.h"
#include "UI3tButton.h"
#include "UIPropertiesBox.h"
#include "../../xr_3da/Render.h"

class CInventory;
class CInventoryItem;
class CUIInventoryWnd;
class CWeapon;

class CUIWeaponModWnd;

class CUIWeaponPreview final : public CUIWindow
{
public:
    explicit CUIWeaponPreview(CUIWeaponModWnd* owner) : m_owner(owner) {}
    void Draw() override;
    bool OnMouse(float x, float y, EUIMessages mouse_action) override;

private:
    CUIWeaponModWnd* m_owner;
};

class CUIWeaponModWnd final : public CUIWindow
{
public:
    using inherited = CUIWindow;

    explicit CUIWeaponModWnd(CUIInventoryWnd* owner);
    ~CUIWeaponModWnd() override;

    void Init();
    void Open(CWeapon* weapon, CInventory* inventory);
    void Close();
    void Reset() override;
    bool OnKeyboard(int dik, EUIMessages keyboard_action) override;
    void SendMessage(CUIWindow* pWnd, s16 msg, void* pData = nullptr) override;

    void DrawPreview();
    bool OnPreviewMouse(float x, float y, EUIMessages mouse_action);

private:
    enum ESound
    {
        eSoundOpen,
        eSoundClose,
        eSoundSelect,
        eSoundAttach,
        eSoundDetach,
        eSoundApply,
        eSoundCount
    };

    struct SSlotControl
    {
        CUI3tButton* button{};
        u8 visual_index{};
        shared_str installed_section;
    };

    void RebuildSlots();
    void OpenSlotChoices(u8 visual_index);
    bool ItemMatchesSlot(CInventoryItem* item, u8 visual_index) const;
    void ProcessChoice();
    void PlaySound(ESound sound);

    CUIInventoryWnd* m_owner;
    CWeapon* m_weapon{};
    CInventory* m_inventory{};

    CUIStatic m_background;
    CUIStatic m_title;
    CUIStatic m_help;
    CUIWeaponPreview m_preview;
    CUI3tButton m_apply;
    CUI3tButton m_cancel;
    CUIPropertiesBox m_choices;

    xr_vector<SSlotControl> m_slots;
    xr_vector<SUIModelRenderPoint> m_render_points;
    u8 m_selected_visual_index{u8(-1)};

    Fvector m_rotation{};
    Fvector m_offset{};
    Fvector m_default_rotation{};
    Fvector m_default_offset{};
    float m_scale{1.f};
    float m_default_scale{1.f};
    float m_min_scale{0.5f};
    float m_max_scale{2.f};
    float m_wheel_step{0.1f};
    float m_rotation_sensitivity{0.01f};
    shared_str m_default_center_bone;
    shared_str m_center_bone;
    u16 m_center_bone_id{u16(-1)};
    bool m_dragging{};
    Fvector2 m_last_cursor{};
    ref_sound m_sounds[eSoundCount];
};
