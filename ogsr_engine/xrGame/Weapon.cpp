// Weapon.cpp: implementation of the CWeapon class.
//
//////////////////////////////////////////////////////////////////////
#include "stdafx.h"

#include "Weapon.h"
#include "ParticlesObject.h"
#include "entity_alive.h"
#include "player_hud.h"
#include "inventory_item_impl.h"

#include "inventory.h"
#include "InventoryOwner.h"
#include "xrserver_objects_alife_items.h"

#include "actor.h"
#include "actoreffector.h"
#include "level.h"

#include "xr_level_controller.h"
#include "game_cl_base.h"
#include "../Include/xrRender/Kinematics.h"
#include "ai_object_location.h"
#include "clsid_game.h"
#include "object_broker.h"
#include "../xr_3da/LightAnimLibrary.h"
#include "game_object_space.h"
#include "script_game_object.h"

#include "GamePersistent.h"
#include "../xr_3da/x_ray.h"

#define ROTATION_TIME 0.25f

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

CWeapon::CWeapon(LPCSTR name)
{
    SetState(eHidden);
    SetNextState(eHidden);
    m_sub_state = eSubstateReloadBegin;
    m_idle_state = eIdle;
    m_bTriStateReload = false;
    SetDefaults();

    m_Offset.identity();
    m_StrapOffset.identity();

    iAmmoCurrent = -1;
    m_dwAmmoCurrentCalcFrame = 0;

    iAmmoElapsed = -1;
    iMagazineSize = -1;
    m_ammoType = 0;

    eHandDependence = hdNone;

    m_fZoomFactor = Core.Features.test(xrCore::Feature::ogse_wpn_zoom_system) ? 1.f : g_fov;

    m_fZoomRotationFactor = 0.f;

    m_pAmmo = nullptr;

    m_pFlameParticles2 = nullptr;
    m_sFlameParticles2 = nullptr;

    m_fCurrentCartirdgeDisp = 1.f;

    m_strap_bone0 = nullptr;
    m_strap_bone1 = nullptr;
    m_StrapOffset.identity();
    m_strapped_mode = false;
    m_can_be_strapped = false;
    m_ef_main_weapon_type = u32(-1);
    m_ef_weapon_type = u32(-1);
    m_UIScope = nullptr;
    m_set_next_ammoType_on_reload = u32(-1);
}

CWeapon::~CWeapon()
{
    if (g_player_hud)
        g_player_hud->clear_addon_hand_pose_sources(this);
    DestroyAddonVisuals();
    xr_delete(m_UIScope);

    laser_light_render.destroy();
    flashlight_render.destroy();
    flashlight_omni.destroy();
}

namespace
{
constexpr u32 legacy_addon_visual_count = 3;
constexpr u32 addon_visual_count = legacy_addon_visual_count + CWeapon::eCustomAddonCount;
constexpr LPCSTR legacy_addon_slot_names[legacy_addon_visual_count] = {"scope", "silencer", "launcher"};
constexpr LPCSTR fixed_custom_slot_names[CWeapon::eCustomAddonDynamicBegin] = {
    "magazine", "foregrip", "side_rail", "handguard", "stock", "load_grip", "muzzle", "pistolgrip", "receiver", "gas_block", "backup_scope"};

bool addon_dependency_list_contains(LPCSTR section, LPCSTR key, LPCSTR addon_section)
{
    if (!section || !key || !addon_section || !pSettings->section_exist(section) || !pSettings->line_exist(section, key))
        return false;

    LPCSTR value = pSettings->r_string(section, key);
    string128 item{};
    for (int i = 0, count = _GetItemCount(value); i < count; ++i)
    {
        _GetItem(value, i, item);
        if (!_stricmp(item, addon_section))
            return true;
    }
    return false;
}

bool addon_lists_as_incompatible(LPCSTR section, LPCSTR addon_section)
{
    // Keep the misspelled key for compatibility with configs which already
    // use the originally requested spelling.
    return addon_dependency_list_contains(section, "incompatible_addons", addon_section) ||
        addon_dependency_list_contains(section, "incopatible_addons", addon_section);
}

u16 find_addon_bone(IKinematics* model, LPCSTR bone_name)
{
    if (!model || !bone_name || !bone_name[0])
        return BI_NONE;

    u16 id = model->LL_BoneID(bone_name);
    if (id != BI_NONE)
        return id;

    for (u16 bone_id = 0; bone_id < model->LL_BoneCount(); ++bone_id)
    {
        LPCSTR candidate = model->LL_BoneName(bone_id);
        if (candidate && !_stricmp(candidate, bone_name))
            return bone_id;
    }
    return BI_NONE;
}

float arc9_ik_lerp(float from, float to, float delta)
{
    delta = clampr(delta, 0.f, 1.f);
    const float quartic = delta < 0.5f ? 8.f * delta * delta * delta * delta :
        1.f - powf(-2.f * delta + 2.f, 4.f) * 0.5f;
    const float qdelta = clampr(-quartic * quartic + 2.f * quartic, 0.f, 1.f);
    return from + (to - from) * qdelta;
}

// ARC9 evaluates LHIK/RHIK as a normalized animation timeline. Each entry is
// `time:weight`; stages are blended with InOutQuart followed by ARC9's qerp.
bool evaluate_addon_ik_timeline(LPCSTR value, float progress, float& result)
{
    if (!value || !value[0])
        return false;

    float previous_time = 0.f;
    float previous_weight = 0.f;
    bool parsed_any = false;
    string64 item{};
    for (int i = 0, count = _GetItemCount(value); i < count; ++i)
    {
        _GetItem(value, i, item);
        float stage_time = 0.f;
        float stage_weight = 0.f;
        if (sscanf(item, "%f:%f", &stage_time, &stage_weight) != 2)
            continue;

        stage_time = clampr(stage_time, 0.f, 1.f);
        stage_weight = clampr(stage_weight, 0.f, 1.f);
        parsed_any = true;
        if (progress <= stage_time)
        {
            const float duration = stage_time - previous_time;
            float delta = duration > EPS_S ? (progress - previous_time) / duration : 1.f;
            result = arc9_ik_lerp(previous_weight, stage_weight, delta);
            return true;
        }
        previous_time = stage_time;
        previous_weight = stage_weight;
    }

    if (parsed_any)
        result = previous_weight;
    return parsed_any;
}

void append_addon_bones(LPCSTR section, LPCSTR key, xr_vector<shared_str>& result)
{
    if (!section || !section[0] || !pSettings->section_exist(section) || !pSettings->line_exist(section, key))
        return;

    LPCSTR value = pSettings->r_string(section, key);
    string128 bone_name{};
    for (int i = 0, count = _GetItemCount(value); i < count; ++i)
    {
        _GetItem(value, i, bone_name);
        if (bone_name[0])
            result.emplace_back(bone_name);
    }
}

bool contains_addon_bone(const xr_vector<shared_str>& bones, LPCSTR bone_name)
{
    return std::find_if(bones.begin(), bones.end(), [bone_name](const shared_str& entry) { return !_stricmp(entry.c_str(), bone_name); }) != bones.end();
}

bool config_list_contains_motion(LPCSTR section, LPCSTR key, LPCSTR motion)
{
    if (!section || !section[0] || !motion || !motion[0] || !pSettings->section_exist(section) || !pSettings->line_exist(section, key))
        return false;

    LPCSTR value = pSettings->r_string(section, key);
    string128 entry{};
    for (int i = 0, count = _GetItemCount(value); i < count; ++i)
    {
        _GetItem(value, i, entry);
        if (entry[0] && !_stricmp(entry, motion))
            return true;
    }
    return false;
}

shared_str normalize_addon_bone(LPCSTR bone_name)
{
    xr_string normalized = bone_name ? bone_name : "";
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return shared_str(normalized.c_str());
}

Fvector read_addon_vector(LPCSTR addon_section, LPCSTR owner_section, LPCSTR slot, LPCSTR generic_key, LPCSTR specialized_suffix, bool hud)
{
    Fvector value{};
    string128 key{};
    xr_sprintf(key, "%s_attach_%s", slot, specialized_suffix);
    if (owner_section && pSettings->line_exist(owner_section, key))
        value = pSettings->r_fvector3(owner_section, key);

    // The addon section is authoritative. Owner-section values above are kept
    // only as a backwards-compatible fallback for old weapon configurations.
    if (pSettings->line_exist(addon_section, generic_key))
        value = pSettings->r_fvector3(addon_section, generic_key);

    xr_sprintf(key, "%s_attach_%s", hud ? "hud" : "world", specialized_suffix);
    if (pSettings->line_exist(addon_section, key))
        value = pSettings->r_fvector3(addon_section, key);
    return value;
}

float read_addon_scale(LPCSTR addon_section, LPCSTR owner_section, LPCSTR slot, bool hud)
{
    float value = 1.f;
    string128 key{};
    xr_sprintf(key, "%s_attach_scale", slot);
    if (owner_section && pSettings->line_exist(owner_section, key))
        value = pSettings->r_float(owner_section, key);

    if (pSettings->line_exist(addon_section, "attach_scale"))
        value = pSettings->r_float(addon_section, "attach_scale");

    xr_sprintf(key, "%s_attach_scale", hud ? "hud" : "world");
    if (pSettings->line_exist(addon_section, key))
        value = pSettings->r_float(addon_section, key);
    return clampr(value, 0.001f, 100.f);
}

LPCSTR read_addon_bone(LPCSTR addon_section, LPCSTR owner_section, LPCSTR slot, bool hud)
{
    LPCSTR bone_name = nullptr;
    string128 key{};

    // Legacy per-weapon/per-HUD configuration.
    xr_sprintf(key, "%s_attach_bone", slot);
    if (owner_section && pSettings->line_exist(owner_section, key))
        bone_name = pSettings->r_string(owner_section, key);

    // New single-section addon configuration. Mode-specific values override
    // the generic bone when world and HUD skeletons use different names.
    if (pSettings->line_exist(addon_section, "attach_bone"))
        bone_name = pSettings->r_string(addon_section, "attach_bone");

    xr_sprintf(key, "%s_attach_bone", hud ? "hud" : "world");
    if (pSettings->line_exist(addon_section, key))
        bone_name = pSettings->r_string(addon_section, key);

    return bone_name;
}

enum class EAddonAttachSpace
{
    Bone,
    Weapon,
    WeaponBone,
};

EAddonAttachSpace read_addon_attach_space(LPCSTR addon_section, bool hud)
{
    LPCSTR space = READ_IF_EXISTS(pSettings, r_string, addon_section, "attach_space", "bone");
    string64 key{};
    xr_sprintf(key, "%s_attach_space", hud ? "hud" : "world");
    if (pSettings->line_exist(addon_section, key))
        space = pSettings->r_string(addon_section, key);
    if (!_stricmp(space, "weapon") || !_stricmp(space, "root"))
        return EAddonAttachSpace::Weapon;
    if (!_stricmp(space, "weapon_bone") || !_stricmp(space, "model"))
        return EAddonAttachSpace::WeaponBone;
    return EAddonAttachSpace::Bone;
}

LPCSTR read_addon_visual_bone(LPCSTR addon_section, bool hud)
{
    LPCSTR bone_name = READ_IF_EXISTS(pSettings, r_string, addon_section, "visual_attach_bone", nullptr);
    string64 key{};
    xr_sprintf(key, "%s_visual_attach_bone", hud ? "hud" : "world");
    if (pSettings->line_exist(addon_section, key))
        bone_name = pSettings->r_string(addon_section, key);
    return bone_name;
}

LPCSTR read_addon_parent(LPCSTR addon_section, bool hud)
{
    LPCSTR parent = READ_IF_EXISTS(pSettings, r_string, addon_section, "attach_parent", nullptr);
    string64 key{};
    xr_sprintf(key, "%s_attach_parent", hud ? "hud" : "world");
    if (pSettings->line_exist(addon_section, key))
        parent = pSettings->r_string(addon_section, key);
    return parent;
}

bool addon_list_contains(LPCSTR provider_section, LPCSTR key, LPCSTR child_section)
{
    if (!provider_section || !provider_section[0] || !child_section || !child_section[0] ||
        !pSettings->section_exist(provider_section) || !pSettings->line_exist(provider_section, key))
        return false;

    LPCSTR value = pSettings->r_string(provider_section, key);
    string128 candidate{};
    for (int i = 0, count = _GetItemCount(value); i < count; ++i)
    {
        _GetItem(value, i, candidate);
        if (candidate[0] && !_stricmp(candidate, child_section))
            return true;
    }
    return false;
}

bool addon_class_matches(LPCSTR section, LPCSTR requested_class)
{
    if (!section || !requested_class || !pSettings->section_exist(section))
        return false;

    LPCSTR actual_class = READ_IF_EXISTS(pSettings, r_string, section, "addon_class",
        READ_IF_EXISTS(pSettings, r_string, section, "addon_slot", ""));
    if (!_stricmp(actual_class, requested_class))
        return true;
    if (!_stricmp(requested_class, "scope"))
    {
        const size_t class_length = xr_strlen(actual_class);
        const bool scope_suffix = class_length > 6 && !_stricmp(actual_class + class_length - 6, "_scope");
        return !_stricmp(actual_class, "sight") || !_stricmp(actual_class, "optic") || scope_suffix;
    }
    if (!_stricmp(requested_class, "grenade_launcher"))
        return !_stricmp(actual_class, "launcher") || !_stricmp(actual_class, "gl");
    return false;
}

void play_world_idle(IRenderVisual* visual, LPCSTR section)
{
    IKinematicsAnimated* animated = visual ? visual->dcast_PKinematicsAnimated() : nullptr;
    if (!animated)
        return;

    const MotionID idle = animated->ID_Cycle_Safe("idle");
    if (!idle.valid())
    {
        Msg("! Weapon addon [%s]: HUD world visual [%s] has no [idle] animation", section, visual->getDebugName().c_str());
        return;
    }

    animated->PlayCycle(idle, FALSE);
    animated->dcast_PKinematics()->CalculateBones_Invalidate();
}
} // namespace

void CWeapon::DestroyAddonVisuals()
{
    for (SAddonVisual& addon : m_addon_visuals)
    {
        if (addon.world)
            ::Render->model_Delete(addon.world);
        if (addon.hud)
            ::Render->model_Delete(addon.hud);
        addon.world = nullptr;
        addon.hud = nullptr;
        addon.world_reported = false;
        addon.hud_reported = false;
        addon.hud_pose_animation = nullptr;
        addon.section = nullptr;
        addon.editor_override[0] = false;
        addon.editor_override[1] = false;
    }
}

shared_str CWeapon::GetAddonVisualSection(u8 visual_index) const
{
    if (visual_index == 0)
        return IsScopeAttached() ? m_sScopeName : shared_str();
    if (visual_index == 1)
        return IsSilencerAttached() ? m_sSilencerName : shared_str();
    if (visual_index == 2)
        return IsGrenadeLauncherAttached() ? m_sGrenadeLauncherName : shared_str();
    if (visual_index >= addon_visual_count)
        return shared_str();
    return GetCustomAddonSection(static_cast<ECustomAddonSlot>(visual_index - legacy_addon_visual_count));
}

LPCSTR CWeapon::GetAddonVisualSlotName(u8 visual_index) const
{
    if (visual_index < legacy_addon_visual_count)
        return legacy_addon_slot_names[visual_index];
    if (visual_index >= addon_visual_count)
        return "";
    return GetCustomAddonSlotName(static_cast<ECustomAddonSlot>(visual_index - legacy_addon_visual_count));
}

void CWeapon::CollectAddonUISlots(xr_vector<SAddonUISlot>& slots) const
{
    slots.clear();
    IKinematics* weapon_model = smart_cast<IKinematics*>(Visual());
    if (!weapon_model)
        return;

    const Fmatrix weapon_transform = renderable_WorldTransform();
    for (u8 visual_index = 0; visual_index < addon_visual_count; ++visual_index)
    {
        shared_str section = GetAddonVisualSection(visual_index);
        bool available = false;
        if (visual_index == 0)
        {
            available = ScopeAttachable();
            if (!section.c_str() && !m_allScopeNames.empty())
                section = m_allScopeNames.front();
            if (!section.c_str())
                section = m_sScopeName;
        }
        else if (visual_index == 1)
        {
            available = SilencerAttachable();
            if (!section.c_str())
                section = m_sSilencerName;
        }
        else if (visual_index == 2)
        {
            available = GrenadeLauncherAttachable();
            if (!section.c_str())
                section = m_sGrenadeLauncherName;
        }
        else
        {
            const SCustomAddonSlot& custom = m_custom_addon_slots[visual_index - legacy_addon_visual_count];
            available = custom.name.c_str() && !custom.allowed.empty();
            if (!section.c_str() && !custom.allowed.empty())
                section = custom.allowed.front();
        }
        if (!available || !section.c_str() || !pSettings->section_exist(section))
            continue;

        IKinematics* attach_model = weapon_model;
        Fmatrix attach_transform = weapon_transform;
        LPCSTR owner_section = cNameSect().c_str();
        const shared_str parent = FindAddonParentSection(section.c_str(), false);
        LPCSTR requested_parent = read_addon_parent(section.c_str(), false);
        const bool requires_parent = requested_parent && requested_parent[0] && _stricmp(requested_parent, "weapon") &&
            _stricmp(requested_parent, "root");
        if (requires_parent && !parent.c_str())
            continue;
        if (parent.c_str())
        {
            for (u8 parent_index = 0; parent_index < addon_visual_count; ++parent_index)
            {
                const shared_str parent_candidate = GetAddonVisualSection(parent_index);
                if (!parent_candidate.c_str() || _stricmp(parent_candidate.c_str(), parent.c_str()))
                    continue;
                IRenderVisual* parent_visual = m_addon_visuals[parent_index].world;
                IKinematics* parent_model = parent_visual ? parent_visual->dcast_PKinematics() : nullptr;
                if (parent_model)
                {
                    attach_model = parent_model;
                    attach_transform = m_addon_visuals[parent_index].world_transform;
                    owner_section = parent.c_str();
                }
                break;
            }
        }

        const EAddonAttachSpace attach_space = read_addon_attach_space(section.c_str(), false);
        LPCSTR bone_name = read_addon_bone(section.c_str(), owner_section, GetAddonVisualSlotName(visual_index), false);
        if (attach_space != EAddonAttachSpace::Weapon && (!bone_name || !bone_name[0]))
        {
            if (visual_index == 0)
                bone_name = m_sWpn_scope_bones.empty() ? nullptr : m_sWpn_scope_bones.front().c_str();
            else if (visual_index == 1)
                bone_name = m_sWpn_silencer_bone.c_str();
            else if (visual_index == 2)
                bone_name = m_sWpn_launcher_bone.c_str();
        }
        const u16 bone_id = attach_space == EAddonAttachSpace::Weapon ? BI_NONE : find_addon_bone(attach_model, bone_name);
        if (attach_space != EAddonAttachSpace::Weapon && bone_id == BI_NONE)
            continue;

        Fmatrix anchor = attach_transform;
        if (bone_id != BI_NONE)
            anchor.mul_43(attach_transform, attach_model->LL_GetTransform(bone_id));

        const Fvector position = read_addon_vector(
            section.c_str(), owner_section, GetAddonVisualSlotName(visual_index), "attach_position", "position", false);
        Fmatrix offset;
        offset.translate(position);
        anchor.mulB_43(offset);

        SAddonUISlot& ui_slot = slots.emplace_back();
        ui_slot.visual_index = visual_index;
        ui_slot.name = GetAddonVisualSlotName(visual_index);
        ui_slot.installed_section = GetAddonVisualSection(visual_index);
        ui_slot.world_position = anchor.c;
    }
}

bool CWeapon::AddonSectionOffers(LPCSTR provider_section, LPCSTR child_section) const
{
    if (!provider_section || !child_section || !pSettings->section_exist(child_section) ||
        !pSettings->line_exist(child_section, "addon_slot"))
        return false;

    LPCSTR child_slot = pSettings->r_string(child_section, "addon_slot");
    string64 key{};
    xr_sprintf(key, "%s_addons", child_slot);
    return addon_list_contains(provider_section, key, child_section);
}

shared_str CWeapon::FindAddonParentSection(LPCSTR child_section, bool hud_mode) const
{
    if (!child_section || !child_section[0])
        return shared_str();

    // An explicit parent may name either its addon section or its slot.
    LPCSTR configured_parent = read_addon_parent(child_section, hud_mode);
    if (configured_parent && configured_parent[0] && _stricmp(configured_parent, "weapon") && _stricmp(configured_parent, "root"))
    {
        for (u8 i = 0; i < addon_visual_count; ++i)
        {
            const shared_str installed = GetAddonVisualSection(i);
            if (!installed.c_str() || !_stricmp(installed.c_str(), child_section))
                continue;
            if (!_stricmp(configured_parent, installed.c_str()) || !_stricmp(configured_parent, GetAddonVisualSlotName(i)))
                return installed;
        }
        return shared_str();
    }

    // Without attach_parent, infer the installed provider which advertised the
    // child through its `<slot>_addons` list. Root-compatible addons remain on
    // the weapon even if another installed addon happens to advertise them.
    if (pSettings->line_exist(child_section, "addon_slot"))
    {
        LPCSTR slot_name = pSettings->r_string(child_section, "addon_slot");
        for (u8 slot = 0; slot < eCustomAddonCount; ++slot)
        {
            if (_stricmp(slot_name, GetCustomAddonSlotName(static_cast<ECustomAddonSlot>(slot))))
                continue;
            const auto& root_allowed = m_custom_addon_slots[slot].root_allowed;
            if (std::find(root_allowed.begin(), root_allowed.end(), child_section) != root_allowed.end())
                return shared_str();
            break;
        }
    }

    for (u8 i = 0; i < addon_visual_count; ++i)
    {
        const shared_str installed = GetAddonVisualSection(i);
        if (installed.c_str() && _stricmp(installed.c_str(), child_section) && AddonSectionOffers(installed.c_str(), child_section))
            return installed;
    }
    return shared_str();
}

bool CWeapon::GetAddonEditorTransform(u8 visual_index, bool hud_mode, shared_str& section, shared_str& slot, shared_str& parent,
    Fvector& position, Fvector& rotation, float& scale) const
{
    if (visual_index >= addon_visual_count)
        return false;
    section = GetAddonVisualSection(visual_index);
    if (!section.c_str() || !pSettings->section_exist(section))
        return false;

    slot = GetAddonVisualSlotName(visual_index);
    const bool config_hud_mode = AddonEditorUsesHudConfig(visual_index, hud_mode);
    parent = FindAddonParentSection(section.c_str(), config_hud_mode);
    const SAddonVisual& instance = m_addon_visuals[visual_index];
    const u8 mode = hud_mode ? 1 : 0;
    if (instance.editor_override[mode])
    {
        position = instance.editor_position[mode];
        rotation = instance.editor_rotation[mode];
        scale = instance.editor_scale[mode];
    }
    else
    {
        LPCSTR owner_section = config_hud_mode ? hud_sect.c_str() : cNameSect().c_str();
        position = read_addon_vector(
            section.c_str(), owner_section, GetAddonVisualSlotName(visual_index), "attach_position", "position", config_hud_mode);
        rotation = read_addon_vector(
            section.c_str(), owner_section, GetAddonVisualSlotName(visual_index), "attach_rotation", "rotation", config_hud_mode);
        scale = read_addon_scale(section.c_str(), owner_section, GetAddonVisualSlotName(visual_index), config_hud_mode);
    }
    return true;
}

bool CWeapon::AddonEditorUsesHudConfig(u8 visual_index, bool hud_mode) const
{
    if (hud_mode)
        return true;

    const shared_str section = GetAddonVisualSection(visual_index);
    return section.c_str() && READ_IF_EXISTS(
        pSettings, r_bool, section, "use_hud_model_as_world", UsesHudModelAsWorld());
}

bool CWeapon::SetAddonEditorTransform(u8 visual_index, bool hud_mode, const Fvector& position, const Fvector& rotation, float scale)
{
    if (visual_index >= addon_visual_count || !GetAddonVisualSection(visual_index).c_str())
        return false;
    const u8 mode = hud_mode ? 1 : 0;
    SAddonVisual& instance = m_addon_visuals[visual_index];
    instance.editor_position[mode] = position;
    instance.editor_rotation[mode] = rotation;
    instance.editor_scale[mode] = clampr(scale, 0.001f, 100.f);
    instance.editor_override[mode] = true;
    return true;
}

void CWeapon::ResetAddonEditorTransform(u8 visual_index, bool hud_mode)
{
    if (visual_index < addon_visual_count)
        m_addon_visuals[visual_index].editor_override[hud_mode ? 1 : 0] = false;
}

float CWeapon::GetCurrentHudMotionProgress() const
{
    if (m_dwMotionEndTm <= m_dwMotionStartTm)
        return GetState() == eIdle ? 1.f : 0.f;
    return clampr(float(Device.dwTimeGlobal - m_dwMotionStartTm) / float(m_dwMotionEndTm - m_dwMotionStartTm), 0.f, 1.f);
}

bool CWeapon::PreviewHandPoseIKEditorMotion(LPCSTR motion)
{
    if (!motion || !motion[0] || !GetHUDmode() || !AnimationExist(motion))
        return false;
    m_hand_pose_ik_editor_preview_motion = motion;
    PlayHUDMotion(motion, true, eIdle, false);
    return true;
}

void CWeapon::CollectHandPoseIKEditorMotions(u8 visual_index, xr_vector<shared_str>& motions) const
{
    motions.clear();
    if (visual_index >= addon_visual_count)
        return;

    const shared_str section = GetAddonVisualSection(visual_index);
    if (!section.c_str() || !pSettings->section_exist(section) || !pSettings->line_exist(section, "hud_hand_pose"))
        return;

    const auto append_unique = [&motions](LPCSTR motion) {
        if (!motion || !motion[0])
            return;
        const auto found = std::find_if(motions.begin(), motions.end(), [motion](const shared_str& candidate) {
            return !_stricmp(candidate.c_str(), motion);
        });
        if (found == motions.end())
            motions.emplace_back(motion);
    };
    append_unique(GetCurrentHudMotion());

    constexpr LPCSTR prefixes[] = {"hand_pose_ik_timeline_", "hand_pose_ik_time_"};
    const LPCSTR sections[] = {section.c_str(), hud_sect.c_str()};
    for (LPCSTR config_section : sections)
    {
        if (!config_section || !config_section[0] || !pSettings->section_exist(config_section))
            continue;
        for (const auto& line : pSettings->r_section(config_section).Ordered_Data)
        {
            LPCSTR key = line.first.c_str();
            for (LPCSTR prefix : prefixes)
            {
                const size_t prefix_length = xr_strlen(prefix);
                if (!_strnicmp(key, prefix, prefix_length))
                    append_unique(key + prefix_length);
            }
        }
    }
    for (const SHandPoseIKEditorOverride& entry : m_hand_pose_ik_editor_overrides)
    {
        if (entry.visual_index == visual_index)
            append_unique(entry.motion.c_str());
    }
}

bool CWeapon::GetHandPoseIKEditorState(u8 visual_index, LPCSTR motion, SHandPoseIKEditorState& state) const
{
    if (visual_index >= addon_visual_count)
        return false;
    const shared_str section = GetAddonVisualSection(visual_index);
    if (!section.c_str() || !pSettings->section_exist(section) || !pSettings->line_exist(section, "hud_hand_pose"))
        return false;

    if (!motion || !motion[0])
        motion = GetCurrentHudMotion();
    if (!motion || !motion[0])
        return false;

    state = {};
    state.section = section;
    state.motion = motion;
    state.blend_in = READ_IF_EXISTS(pSettings, r_float, section, "hud_hand_pose_blend_in", 0.18f);
    state.blend_out = READ_IF_EXISTS(pSettings, r_float, section, "hud_hand_pose_blend_out", 0.1f);
    state.hold_between = READ_IF_EXISTS(pSettings, r_bool, section, "hud_hand_pose_hold_between_animations", true);
    state.ik_time = READ_IF_EXISTS(pSettings, r_float, section, "hud_hand_pose_ik_time", 0.8f);
    LPCSTR timeline = READ_IF_EXISTS(pSettings, r_string, section, "hud_hand_pose_ik_timeline", nullptr);

    string256 key{};
    xr_sprintf(key, "hand_pose_ik_time_%s", motion);
    if (pSettings->line_exist(section, key))
        state.ik_time = pSettings->r_float(section, key);
    if (pSettings->line_exist(hud_sect, key))
        state.ik_time = pSettings->r_float(hud_sect, key);

    xr_sprintf(key, "hand_pose_ik_timeline_%s", motion);
    if (pSettings->line_exist(section, key))
        timeline = pSettings->r_string(section, key);
    if (pSettings->line_exist(hud_sect, key))
        timeline = pSettings->r_string(hud_sect, key);
    state.timeline = timeline;
    state.override_weapon = config_list_contains_motion(section.c_str(), "hud_hand_pose_override_animations", motion) ||
        config_list_contains_motion(hud_sect.c_str(), "hand_pose_override_animations", motion);

    for (const SHandPoseIKEditorOverride& entry : m_hand_pose_ik_editor_overrides)
    {
        if (entry.visual_index != visual_index || _stricmp(entry.motion.c_str(), motion))
            continue;
        state.timeline = entry.timeline;
        state.blend_in = entry.blend_in;
        state.blend_out = entry.blend_out;
        state.ik_time = entry.ik_time;
        state.hold_between = entry.hold_between;
        state.override_weapon = entry.override_weapon;
        state.runtime_override = true;
        break;
    }
    return true;
}

bool CWeapon::SetHandPoseIKEditorState(u8 visual_index, const SHandPoseIKEditorState& state)
{
    SHandPoseIKEditorState current;
    if (!GetHandPoseIKEditorState(visual_index, state.motion.c_str(), current))
        return false;

    auto found = std::find_if(m_hand_pose_ik_editor_overrides.begin(), m_hand_pose_ik_editor_overrides.end(),
        [visual_index, &state](const SHandPoseIKEditorOverride& entry) {
            return entry.visual_index == visual_index && !_stricmp(entry.motion.c_str(), state.motion.c_str());
        });
    if (found == m_hand_pose_ik_editor_overrides.end())
    {
        m_hand_pose_ik_editor_overrides.emplace_back();
        found = m_hand_pose_ik_editor_overrides.end() - 1;
    }
    found->visual_index = visual_index;
    found->motion = state.motion;
    found->timeline = state.timeline;
    found->blend_in = _max(state.blend_in, 0.f);
    found->blend_out = _max(state.blend_out, 0.f);
    found->ik_time = clampr(state.ik_time, -1.f, 1.f);
    found->hold_between = state.hold_between;
    found->override_weapon = state.override_weapon;
    return true;
}

void CWeapon::ResetHandPoseIKEditorState(u8 visual_index, LPCSTR motion)
{
    const auto found = std::find_if(m_hand_pose_ik_editor_overrides.begin(), m_hand_pose_ik_editor_overrides.end(),
        [visual_index, motion](const SHandPoseIKEditorOverride& entry) {
            return entry.visual_index == visual_index && motion && !_stricmp(entry.motion.c_str(), motion);
        });
    if (found != m_hand_pose_ik_editor_overrides.end())
        m_hand_pose_ik_editor_overrides.erase(found);
}

const shared_str& CWeapon::GetCustomAddonSection(ECustomAddonSlot slot) const
{
    static const shared_str empty;
    if (slot >= eCustomAddonCount)
        return empty;

    const SCustomAddonSlot& data = m_custom_addon_slots[slot];
    if (!data.installed_index || data.installed_index > data.allowed.size())
        return empty;
    return data.allowed[data.installed_index - 1];
}

LPCSTR CWeapon::GetCustomAddonSlotName(ECustomAddonSlot slot) const
{
    if (slot >= eCustomAddonCount)
        return "";
    LPCSTR name = m_custom_addon_slots[slot].name.c_str();
    return name ? name : "";
}

const xr_vector<shared_str>& CWeapon::GetCustomAddonAllowed(ECustomAddonSlot slot) const
{
    static const xr_vector<shared_str> empty;
    return slot < eCustomAddonCount ? m_custom_addon_slots[slot].allowed : empty;
}

u8 CWeapon::GetCustomAddonIndex(ECustomAddonSlot slot) const
{
    return slot < eCustomAddonCount ? m_custom_addon_slots[slot].installed_index : 0;
}

shared_str CWeapon::GetInstalledAddonByClass(LPCSTR addon_class) const
{
    if (!addon_class || !addon_class[0])
        return shared_str();
    if (!_stricmp(addon_class, "scope") && IsScopeAttached())
        return m_sScopeName;
    if (!_stricmp(addon_class, "silencer") && IsSilencerAttached())
        return m_sSilencerName;
    if (!_stricmp(addon_class, "grenade_launcher") && IsGrenadeLauncherAttached())
        return m_sGrenadeLauncherName;

    for (u8 slot = 0; slot < eCustomAddonCount; ++slot)
    {
        const shared_str& section = GetCustomAddonSection(static_cast<ECustomAddonSlot>(slot));
        if (section.c_str() && addon_class_matches(section.c_str(), addon_class))
            return section;
    }
    return shared_str();
}

shared_str CWeapon::GetInstalledSilencerSection() const
{
    if (IsSilencerAttached())
        return m_sSilencerName;
    for (u8 slot = 0; slot < eCustomAddonCount; ++slot)
    {
        const shared_str& section = GetCustomAddonSection(static_cast<ECustomAddonSlot>(slot));
        if (!section.c_str())
            continue;
        const bool is_silencer = READ_IF_EXISTS(
            pSettings, r_bool, section, "is_silencer", addon_class_matches(section.c_str(), "silencer"));
        if (is_silencer)
            return section;
    }
    return shared_str();
}

bool CWeapon::IsAddonSectionInstalled(LPCSTR addon_section) const
{
    if (!addon_section || !addon_section[0])
        return false;

    for (u8 visual_index = 0; visual_index < addon_visual_count; ++visual_index)
    {
        const shared_str installed = GetAddonVisualSection(visual_index);
        if (installed.c_str() && !_stricmp(installed.c_str(), addon_section))
            return true;
    }
    return false;
}

bool CWeapon::AddonRequirementsSatisfied(LPCSTR addon_section) const
{
    if (!addon_section || !addon_section[0] || !pSettings->section_exist(addon_section))
        return false;

    if (pSettings->line_exist(addon_section, "required_addons"))
    {
        LPCSTR value = pSettings->r_string(addon_section, "required_addons");
        string128 required{};
        for (int i = 0, count = _GetItemCount(value); i < count; ++i)
        {
            _GetItem(value, i, required);
            if (!IsAddonSectionInstalled(required))
                return false;
        }
    }

    for (u8 visual_index = 0; visual_index < addon_visual_count; ++visual_index)
    {
        const shared_str installed = GetAddonVisualSection(visual_index);
        if (!installed.c_str())
            continue;
        if (addon_lists_as_incompatible(addon_section, installed.c_str()) ||
            addon_lists_as_incompatible(installed.c_str(), addon_section))
            return false;
    }
    return true;
}

bool CWeapon::CanDetachAddonSection(LPCSTR addon_section) const
{
    if (!addon_section || !addon_section[0])
        return false;

    for (u8 visual_index = 0; visual_index < addon_visual_count; ++visual_index)
    {
        const shared_str installed = GetAddonVisualSection(visual_index);
        if (!installed.c_str() || !_stricmp(installed.c_str(), addon_section))
            continue;
        if (addon_dependency_list_contains(installed.c_str(), "required_addons", addon_section))
            return false;
    }
    return true;
}

bool CWeapon::IsScopeFunctional() const { return GetInstalledAddonByClass("scope").c_str() != nullptr; }
bool CWeapon::IsAimingThroughAddonScope() const
{
    return IsZoomed() && !IsRotatingToZoom() && !IsGrenadeMode() && IsScopeFunctional();
}
bool CWeapon::IsSilencerFunctional() const { return GetInstalledSilencerSection().c_str() != nullptr; }
bool CWeapon::IsGrenadeLauncherFunctional() const { return GetInstalledAddonByClass("grenade_launcher").c_str() != nullptr; }

bool CWeapon::HasCriticalAddonComponents() const
{
    for (const shared_str& addon_class : m_critical_addon_classes)
    {
        bool installed = false;
        if (!_stricmp(addon_class.c_str(), "silencer"))
            installed = IsSilencerFunctional();
        else
            installed = GetInstalledAddonByClass(addon_class.c_str()).c_str() != nullptr;
        if (!installed)
            return false;
    }
    return true;
}

bool CWeapon::GetAddonHudAimTransform(bool alternate, Fmatrix& transform, bool& use_bone_rotation) const
{
    if (!HudItemData())
        return false;

    for (u8 visual_index = 0; visual_index < addon_visual_count; ++visual_index)
    {
        shared_str section;
        if (visual_index == 0 && IsScopeAttached())
            section = m_sScopeName;
        else if (visual_index >= legacy_addon_visual_count)
            section = GetCustomAddonSection(static_cast<ECustomAddonSlot>(visual_index - legacy_addon_visual_count));
        if (!section.c_str() || !addon_class_matches(section.c_str(), "scope") ||
            !READ_IF_EXISTS(pSettings, r_bool, section, "aim_from_bone", true))
            continue;

        const SAddonVisual& instance = m_addon_visuals[visual_index];
        IKinematics* model = instance.hud ? instance.hud->dcast_PKinematics() : nullptr;
        if (!model || !instance.hud_local_transform_valid)
            continue;
        LPCSTR key = alternate ? "alt_aim_bone" : "aim_bone";
        LPCSTR bone_name = READ_IF_EXISTS(
            pSettings, r_string, section, key, alternate ? "mod_alt_aim_camera" : "mod_aim_camera");
        const u16 bone_id = find_addon_bone(model, bone_name);
        if (bone_id == BI_NONE)
            continue;

        // A reticle/mesh bone normally supplies only the optical centre. Its
        // inherited Source-model basis can point along an arbitrary axis (and
        // commonly includes the 90-degree visual attachment correction), so
        // treating it as a camera basis turns the whole weapon sideways.
        // Dedicated camera bones retain full position + rotation alignment.
        LPCSTR rotation_key = alternate ? "alt_aim_rotation_from_bone" : "aim_rotation_from_bone";
        const bool camera_bone = !_stricmp(bone_name, "mod_aim_camera") ||
            !_stricmp(bone_name, "mod_alt_aim_camera");
        use_bone_rotation = READ_IF_EXISTS(pSettings, r_bool, section, rotation_key, camera_bone);

        model->CalculateBones();
        // hud_local_transform and the addon render transform are captured from
        // the same render pass. Reconstructing this from the current HUD item
        // transform used to combine a previous render matrix with one of the
        // two player_hud update passes, making the resulting ADS pose depend on
        // camera movement and occasionally sending the weapon far off-screen.
        transform.mul_43(instance.hud_local_transform, model->LL_GetTransform(bone_id));
        return true;
    }
    return false;
}

bool CWeapon::CanAttachCustomAddon(const CInventoryItem* item) const
{
    if (!item)
        return false;

    const shared_str section = item->object().cNameSect();
    if (!pSettings->section_exist(section) || !pSettings->line_exist(section, "addon_slot"))
        return false;
    if (!AddonRequirementsSatisfied(section.c_str()))
        return false;

    LPCSTR slot_name = pSettings->r_string(section, "addon_slot");
    for (u8 slot = 0; slot < eCustomAddonCount; ++slot)
    {
        const SCustomAddonSlot& data = m_custom_addon_slots[slot];
        if (_stricmp(slot_name, GetCustomAddonSlotName(static_cast<ECustomAddonSlot>(slot))) || data.installed_index)
            continue;
        if (std::find(data.allowed.begin(), data.allowed.end(), section) == data.allowed.end())
            continue;

        LPCSTR configured_parent = READ_IF_EXISTS(pSettings, r_string, section, "attach_parent", nullptr);
        const bool wants_weapon = !configured_parent || !configured_parent[0] || !_stricmp(configured_parent, "weapon") || !_stricmp(configured_parent, "root");
        if (wants_weapon && std::find(data.root_allowed.begin(), data.root_allowed.end(), section) != data.root_allowed.end())
            return true;

        for (u8 provider_index = 0; provider_index < addon_visual_count; ++provider_index)
        {
            const shared_str provider = GetAddonVisualSection(provider_index);
            if (!provider.c_str() || !AddonSectionOffers(provider.c_str(), section.c_str()))
                continue;
            if (data.provider.c_str() && _stricmp(data.provider.c_str(), provider.c_str()))
                continue;
            if (!configured_parent || !configured_parent[0] || !_stricmp(configured_parent, provider.c_str()) ||
                !_stricmp(configured_parent, GetAddonVisualSlotName(provider_index)))
                return true;
        }
        return false;
    }
    return false;
}

bool CWeapon::CanDetachCustomAddon(LPCSTR item_section) const
{
    if (!item_section || !item_section[0])
        return false;
    if (!CanDetachAddonSection(item_section))
        return false;
    for (u8 slot = 0; slot < eCustomAddonCount; ++slot)
    {
        const shared_str& installed = GetCustomAddonSection(static_cast<ECustomAddonSlot>(slot));
        if (!installed.c_str() || _stricmp(installed.c_str(), item_section))
            continue;

        // A parent must be stripped from the leaves inward. This avoids
        // silently deleting a child inventory item when its visual anchor goes
        // away and keeps save/network state deterministic.
        for (u8 child_index = 0; child_index < addon_visual_count; ++child_index)
        {
            const shared_str child = GetAddonVisualSection(child_index);
            if (!child.c_str() || !_stricmp(child.c_str(), item_section))
                continue;
            const shared_str parent = FindAddonParentSection(child.c_str());
            if (parent.c_str() && !_stricmp(parent.c_str(), item_section))
                return false;
        }
        return true;
    }
    return false;
}

bool CWeapon::AttachCustomAddon(const CInventoryItem* item)
{
    if (!CanAttachCustomAddon(item))
        return false;

    const shared_str section = item->object().cNameSect();
    LPCSTR slot_name = pSettings->r_string(section, "addon_slot");
    for (u8 slot = 0; slot < eCustomAddonCount; ++slot)
    {
        SCustomAddonSlot& data = m_custom_addon_slots[slot];
        if (_stricmp(slot_name, GetCustomAddonSlotName(static_cast<ECustomAddonSlot>(slot))) || data.installed_index)
            continue;
        const auto it = std::find(data.allowed.begin(), data.allowed.end(), section);
        if (it == data.allowed.end())
            continue;

        LPCSTR configured_parent = READ_IF_EXISTS(pSettings, r_string, section, "attach_parent", nullptr);
        const bool wants_weapon = !configured_parent || !configured_parent[0] || !_stricmp(configured_parent, "weapon") || !_stricmp(configured_parent, "root");
        bool valid_target = wants_weapon && std::find(data.root_allowed.begin(), data.root_allowed.end(), section) != data.root_allowed.end();
        if (!valid_target)
        {
            for (u8 provider_index = 0; provider_index < addon_visual_count; ++provider_index)
            {
                const shared_str provider = GetAddonVisualSection(provider_index);
                if (!provider.c_str() || !AddonSectionOffers(provider.c_str(), section.c_str()))
                    continue;
                if (data.provider.c_str() && _stricmp(data.provider.c_str(), provider.c_str()))
                    continue;
                if (!configured_parent || !configured_parent[0] || !_stricmp(configured_parent, provider.c_str()) ||
                    !_stricmp(configured_parent, GetAddonVisualSlotName(provider_index)))
                {
                    valid_target = true;
                    break;
                }
            }
        }
        if (!valid_target)
            continue;
        data.installed_index = static_cast<u8>(std::distance(data.allowed.begin(), it) + 1);
        return true;
    }
    return false;
}

bool CWeapon::DetachCustomAddon(LPCSTR item_section)
{
    if (!CanDetachCustomAddon(item_section))
        return false;

    for (u8 slot = 0; slot < eCustomAddonCount; ++slot)
    {
        const shared_str& installed = GetCustomAddonSection(static_cast<ECustomAddonSlot>(slot));
        if (installed.c_str() && !_stricmp(installed.c_str(), item_section))
        {
            m_custom_addon_slots[slot].installed_index = 0;
            return true;
        }
    }
    return false;
}

void CWeapon::UpdateAddonReplacementVisibility(bool hud_mode)
{
    const bool config_hud_mode = hud_mode || (!hud_mode && UsesHudModelAsWorld());
    IKinematics* model = nullptr;
    LPCSTR owner_section = config_hud_mode ? hud_sect.c_str() : cNameSect().c_str();
    if (hud_mode)
    {
        attachable_hud_item* hud_item = GetHUDmode() ? HudItemData() : nullptr;
        if (!hud_item)
            return;
        model = hud_item->m_model;
    }
    else
        model = smart_cast<IKinematics*>(Visual());

    if (!model)
        return;

    bool active[addon_visual_count] = {IsScopeAttached(), IsSilencerAttached(), IsGrenadeLauncherAttached()};
    shared_str sections[addon_visual_count] = {m_sScopeName, m_sSilencerName, m_sGrenadeLauncherName};
    for (u8 slot = 0; slot < eCustomAddonCount; ++slot)
    {
        sections[legacy_addon_visual_count + slot] = GetCustomAddonSection(static_cast<ECustomAddonSlot>(slot));
        active[legacy_addon_visual_count + slot] = sections[legacy_addon_visual_count + slot].c_str() != nullptr;
    }
    string_unordered_map<shared_str, bool> replacement_bones;

    for (u32 i = 0; i < addon_visual_count; ++i)
    {
        xr_vector<shared_str> bones;
        string128 key{};
        xr_sprintf(key, "%s_replaced_bones", GetAddonVisualSlotName(i));
        append_addon_bones(owner_section, key, bones);

        const bool nested_addon = sections[i].c_str() && FindAddonParentSection(sections[i].c_str(), config_hud_mode).c_str();
        if (sections[i].c_str() && pSettings->section_exist(sections[i]) && !nested_addon)
        {
            append_addon_bones(sections[i].c_str(), "replaced_bones", bones);
            append_addon_bones(sections[i].c_str(), config_hud_mode ? "replaced_hud_bones" : "replaced_world_bones", bones);
        }

        for (const shared_str& bone : bones)
        {
            const shared_str normalized = normalize_addon_bone(bone.c_str());
            auto [it, inserted] = replacement_bones.emplace(normalized, active[i]);
            if (!inserted)
                it->second = it->second || active[i];
        }
    }

    // Also remember replacement bones from currently uninstalled compatible
    // custom addons. Otherwise an addon-level replaced_bones entry could be
    // hidden on attach but would be unknown (and stay hidden) after detach.
    for (u8 slot = 0; slot < eCustomAddonCount; ++slot)
    {
        for (const shared_str& compatible_section : m_custom_addon_slots[slot].allowed)
        {
            xr_vector<shared_str> bones;
            append_addon_bones(compatible_section.c_str(), "replaced_bones", bones);
            append_addon_bones(compatible_section.c_str(), config_hud_mode ? "replaced_hud_bones" : "replaced_world_bones", bones);
            for (const shared_str& bone : bones)
                replacement_bones.emplace(normalize_addon_bone(bone.c_str()), false);
        }
    }

    const xr_vector<shared_str>& permanently_hidden = config_hud_mode ? hud_hidden_bones : hidden_bones;
    for (const auto& [bone_name, should_hide] : replacement_bones)
    {
        const u16 bone_id = find_addon_bone(model, bone_name.c_str());
        if (bone_id == BI_NONE)
            continue;

        const bool always_hidden = contains_addon_bone(permanently_hidden, bone_name.c_str());
        bool visible = !should_hide && !always_hidden;

        // If a replacement list contains one of the legacy embedded addon
        // bones, preserve its normal visibility policy after the external
        // model is detached instead of incorrectly revealing it.
        const auto& scope_bones = config_hud_mode ? m_sHud_wpn_scope_bones : m_sWpn_scope_bones;
        if (contains_addon_bone(scope_bones, bone_name.c_str()))
            visible = visible && IsScopeAttached() && m_eScopeStatus != ALife::eAddonDisabled;

        const shared_str& silencer_bone = config_hud_mode ? m_sHud_wpn_silencer_bone : m_sWpn_silencer_bone;
        if (silencer_bone.c_str() && !_stricmp(silencer_bone.c_str(), bone_name.c_str()))
            visible = visible && IsSilencerAttached() && m_eSilencerStatus != ALife::eAddonDisabled;

        const shared_str& launcher_bone = config_hud_mode ? m_sHud_wpn_launcher_bone : m_sWpn_launcher_bone;
        if (launcher_bone.c_str() && !_stricmp(launcher_bone.c_str(), bone_name.c_str()))
            visible = visible && IsGrenadeLauncherAttached() && m_eGrenadeLauncherStatus != ALife::eAddonDisabled;

        // Hide only the embedded mesh assigned to this bone. Recursive hiding
        // could also disable child anchors used by a separately rendered addon.
        model->LL_SetBoneVisible(bone_id, visible, FALSE);
    }
}

void CWeapon::RenderAddonVisuals(u32 context_id, IRenderable* root, bool hud_mode, bool ui_preview)
{
    if (g_pGameLevel && Level().is_removing_objects())
        return;

    const bool config_hud_mode = hud_mode || (!hud_mode && UsesHudModelAsWorld());
    bool active[addon_visual_count] = {IsScopeAttached(), IsSilencerAttached(), IsGrenadeLauncherAttached()};
    shared_str sections[addon_visual_count] = {m_sScopeName, m_sSilencerName, m_sGrenadeLauncherName};
    for (u8 slot = 0; slot < eCustomAddonCount; ++slot)
    {
        sections[legacy_addon_visual_count + slot] = GetCustomAddonSection(static_cast<ECustomAddonSlot>(slot));
        active[legacy_addon_visual_count + slot] = sections[legacy_addon_visual_count + slot].c_str() != nullptr;
    }
    const shared_str active_scope_section = GetInstalledAddonByClass("scope");

    IKinematics* weapon_model = nullptr;
    Fmatrix weapon_transform;
    LPCSTR owner_section = config_hud_mode ? hud_sect.c_str() : cNameSect().c_str();
    if (hud_mode)
    {
        attachable_hud_item* hud_item = GetHUDmode() ? HudItemData() : nullptr;
        if (!hud_item)
            return;
        weapon_model = hud_item->m_model;
        weapon_transform = hud_item->m_item_transform;
    }
    else
    {
        weapon_model = smart_cast<IKinematics*>(Visual());
        weapon_transform = renderable_WorldTransform();
    }

    if (!weapon_model)
        return;

    bool processed[addon_visual_count]{};
    for (u32 pass = 0; pass < addon_visual_count; ++pass)
    {
        bool progressed = false;
        for (u32 i = 0; i < addon_visual_count; ++i)
        {
            if (processed[i] || !active[i] || !sections[i].c_str() || !pSettings->section_exist(sections[i]))
                continue;

            const bool addon_hud_as_world = !hud_mode && READ_IF_EXISTS(
                pSettings, r_bool, sections[i], "use_hud_model_as_world", UsesHudModelAsWorld());
            const bool visual_config_hud = hud_mode || addon_hud_as_world;
            const shared_str parent_section = FindAddonParentSection(sections[i].c_str(), visual_config_hud);
            LPCSTR requested_parent = read_addon_parent(sections[i].c_str(), visual_config_hud);
            const bool requires_parent = requested_parent && requested_parent[0] && _stricmp(requested_parent, "weapon") && _stricmp(requested_parent, "root");
            if (requires_parent && !parent_section.c_str())
                continue;
            s32 parent_index = -1;
            if (parent_section.c_str())
            {
                for (u32 candidate = 0; candidate < addon_visual_count; ++candidate)
                {
                    if (sections[candidate].c_str() && !_stricmp(sections[candidate].c_str(), parent_section.c_str()))
                    {
                        parent_index = static_cast<s32>(candidate);
                        break;
                    }
                }
                if (parent_index < 0 || !processed[parent_index])
                    continue;
            }

        SAddonVisual& instance = m_addon_visuals[i];
        if (instance.section != sections[i])
        {
            if (instance.world)
                ::Render->model_Delete(instance.world);
            if (instance.hud)
                ::Render->model_Delete(instance.hud);
            instance.world = nullptr;
            instance.hud = nullptr;
            instance.world_reported = false;
            instance.hud_reported = false;
            instance.hud_pose_animation = nullptr;
            instance.hud_local_transform_valid = false;
            instance.editor_override[0] = false;
            instance.editor_override[1] = false;
            instance.section = sections[i];
        }

        string128 visual_key{};
        xr_sprintf(visual_key, "%s_attach_visual", visual_config_hud ? "hud" : "world");
        LPCSTR visual_name = pSettings->line_exist(sections[i], visual_key) ? pSettings->r_string(sections[i], visual_key) : nullptr;
        if ((!visual_name || !visual_name[0]) && pSettings->line_exist(sections[i], "attach_visual"))
            visual_name = pSettings->r_string(sections[i], "attach_visual");
        if ((!visual_name || !visual_name[0]) && addon_hud_as_world && pSettings->line_exist(sections[i], "item_visual"))
            visual_name = pSettings->r_string(sections[i], "item_visual");
        if ((!visual_name || !visual_name[0]) && addon_hud_as_world && pSettings->line_exist(sections[i], "hud"))
        {
            LPCSTR addon_hud_section = pSettings->r_string(sections[i], "hud");
            if (pSettings->section_exist(addon_hud_section))
            {
                if (pSettings->line_exist(addon_hud_section, "item_visual"))
                    visual_name = pSettings->r_string(addon_hud_section, "item_visual");
                else if (pSettings->line_exist(addon_hud_section, "visual"))
                    visual_name = pSettings->r_string(addon_hud_section, "visual");
            }
        }
        if ((!visual_name || !visual_name[0]) && addon_hud_as_world && pSettings->line_exist(sections[i], "world_attach_visual"))
            visual_name = pSettings->r_string(sections[i], "world_attach_visual");
        if (!visual_name || !visual_name[0])
            continue;

        IRenderVisual*& visual = hud_mode ? instance.hud : instance.world;
        bool visual_created = false;
        if (!visual)
        {
            // HUD shaders are compiled differently from ordinary world
            // shaders. Loading an external addon without this flag makes the
            // model submit successfully but remain invisible in the HUD pass.
            const bool previous_hud_loading = ::Render->hud_loading;
            ::Render->hud_loading = hud_mode;
            visual = ::Render->model_Create(visual_name);
            ::Render->hud_loading = previous_hud_loading;
            visual_created = visual != nullptr;
            if (visual && hud_mode)
                visual->MarkAsHot(false);
            if (visual && addon_hud_as_world)
                play_world_idle(visual, sections[i].c_str());
        }
        if (!visual)
            continue;

        // Restore any parent-mesh bones which one of its compatible children
        // may have hidden on the previous frame. Active children hide their
        // replacement bones again below after the hierarchy is resolved.
        if (IKinematics* own_model = visual->dcast_PKinematics())
        {
            for (u8 child_slot = 0; child_slot < eCustomAddonCount; ++child_slot)
            {
                for (const shared_str& candidate : m_custom_addon_slots[child_slot].allowed)
                {
                    if (!AddonSectionOffers(sections[i].c_str(), candidate.c_str()))
                        continue;
                    xr_vector<shared_str> parent_bones;
                    append_addon_bones(candidate.c_str(), "replaced_bones", parent_bones);
                    append_addon_bones(candidate.c_str(), config_hud_mode ? "replaced_hud_bones" : "replaced_world_bones", parent_bones);
                    for (const shared_str& bone : parent_bones)
                    {
                        const u16 bone_id = find_addon_bone(own_model, bone.c_str());
                        if (bone_id != BI_NONE)
                            own_model->LL_SetBoneVisible(bone_id, TRUE, FALSE);
                    }
                }
            }
        }

        IKinematics* attach_model = weapon_model;
        Fmatrix attach_transform = weapon_transform;
        LPCSTR attach_owner_section = owner_section;
        if (parent_index >= 0)
        {
            SAddonVisual& parent_instance = m_addon_visuals[parent_index];
            IRenderVisual* parent_visual = hud_mode ? parent_instance.hud : parent_instance.world;
            attach_model = parent_visual ? parent_visual->dcast_PKinematics() : nullptr;
            attach_transform = hud_mode ? parent_instance.hud_transform : parent_instance.world_transform;
            attach_owner_section = parent_section.c_str();
            if (!attach_model)
                continue;
            xr_vector<shared_str> parent_bones;
            append_addon_bones(sections[i].c_str(), "replaced_bones", parent_bones);
            append_addon_bones(sections[i].c_str(), config_hud_mode ? "replaced_hud_bones" : "replaced_world_bones", parent_bones);
            for (const shared_str& bone : parent_bones)
            {
                const u16 replacement_bone_id = find_addon_bone(attach_model, bone.c_str());
                if (replacement_bone_id != BI_NONE)
                    attach_model->LL_SetBoneVisible(replacement_bone_id, FALSE, FALSE);
            }
        }

        LPCSTR hand_pose = hud_mode ? READ_IF_EXISTS(pSettings, r_string, sections[i], "hud_hand_pose", nullptr) : nullptr;
        if (visual_created && hand_pose && hand_pose[0])
        {
            // Bone scaling does not remove a skinned c_arms mesh: mixed-weight
            // triangles may stretch towards the collapsed bones. Keep this an
            // explicit compatibility option only. ARC9 normally uses a second,
            // completely non-rendered proxy model for hand-pose sampling.
            if (READ_IF_EXISTS(pSettings, r_bool, sections[i], "hud_hand_pose_hide_visual_arms", false))
            {
                IKinematics* pose_model = visual->dcast_PKinematics();
                constexpr LPCSTR arm_roots[] = {
                    "valvebiped.bip01_l_clavicle",
                    "valvebiped.bip01_r_clavicle",
                    "base humanlcollarbone",
                    "base humanrcollarbone",
                };
                for (LPCSTR arm_root : arm_roots)
                {
                    const u16 arm_bone = find_addon_bone(pose_model, arm_root);
                    if (arm_bone != BI_NONE)
                        pose_model->LL_SetBoneVisible(arm_bone, FALSE, TRUE);
                }
            }
        }

        // ARC9 uses the attachment as an animation proxy. The default pose is
        // usually idle, but a weapon motion may select a matching proxy cycle.
        if (hud_mode && hand_pose && hand_pose[0])
        {
            LPCSTR pose_animation = READ_IF_EXISTS(pSettings, r_string, sections[i], "hud_hand_pose_animation", "idle");
            if (m_current_motion.c_str() && m_current_motion.size())
            {
                string256 pose_animation_key{};
                // Attachment sections commonly use the same anm_* aliases as
                // weapon HUD sections (anm_reload = reload, etc.). Keep the
                // attachment skeleton on the weapon motion instead of leaving
                // additional handguard/foregrip bones in idle.
                if (pSettings->line_exist(sections[i], m_current_motion.c_str()))
                    pose_animation = pSettings->r_string(sections[i], m_current_motion.c_str());

                xr_sprintf(pose_animation_key, "hud_hand_pose_animation_%s", m_current_motion.c_str());
                if (pSettings->line_exist(sections[i], pose_animation_key))
                    pose_animation = pSettings->r_string(sections[i], pose_animation_key);

                xr_sprintf(pose_animation_key, "hand_pose_animation_%s", m_current_motion.c_str());
                if (pSettings->line_exist(hud_sect, pose_animation_key))
                    pose_animation = pSettings->r_string(hud_sect, pose_animation_key);
            }

            if (instance.hud_pose_animation != pose_animation)
            {
                IKinematicsAnimated* animated = visual->dcast_PKinematicsAnimated();
                if (animated && animated->ID_Cycle_Safe(pose_animation).valid())
                {
                    animated->PlayCycle(pose_animation);
                    instance.hud_pose_animation = pose_animation;
                }
                else
                    Msg("! Weapon addon [%s]: HUD hand pose animation [%s] not found in [%s]", sections[i].c_str(), pose_animation, visual_name);
            }
        }

        const EAddonAttachSpace attach_space = read_addon_attach_space(sections[i].c_str(), visual_config_hud);
        const bool needs_target_bone = attach_space != EAddonAttachSpace::Weapon;
        LPCSTR bone_name = read_addon_bone(sections[i].c_str(), attach_owner_section, GetAddonVisualSlotName(i), visual_config_hud);
        if (needs_target_bone && (!bone_name || !bone_name[0]))
        {
            if (i == 0)
            {
                const auto& scope_bones = config_hud_mode ? m_sHud_wpn_scope_bones : m_sWpn_scope_bones;
                bone_name = scope_bones.empty() ? nullptr : scope_bones.front().c_str();
            }
            else if (i == 1)
                bone_name = (config_hud_mode ? m_sHud_wpn_silencer_bone : m_sWpn_silencer_bone).c_str();
            else if (i == 2)
                bone_name = (config_hud_mode ? m_sHud_wpn_launcher_bone : m_sWpn_launcher_bone).c_str();
        }

        const u16 bone_id = needs_target_bone ? find_addon_bone(attach_model, bone_name) : BI_NONE;
        if (needs_target_bone && bone_id == BI_NONE)
        {
            bool& reported = hud_mode ? instance.hud_reported : instance.world_reported;
            if (!reported)
            {
                Msg("! Weapon addon [%s]: target bone [%s] not found in [%s] model of [%s]", sections[i].c_str(), bone_name ? bone_name : "<empty>",
                    hud_mode ? "HUD" : "world", cNameSect().c_str());
                reported = true;
            }
            continue;
        }

        Fvector position = read_addon_vector(
            sections[i].c_str(), attach_owner_section, GetAddonVisualSlotName(i), "attach_position", "position", visual_config_hud);
        Fvector rotation = read_addon_vector(
            sections[i].c_str(), attach_owner_section, GetAddonVisualSlotName(i), "attach_rotation", "rotation", visual_config_hud);
        float addon_scale = read_addon_scale(sections[i].c_str(), attach_owner_section, GetAddonVisualSlotName(i), visual_config_hud);
        const u8 editor_mode = hud_mode ? 1 : 0;
        if (instance.editor_override[editor_mode])
        {
            position = instance.editor_position[editor_mode];
            rotation = instance.editor_rotation[editor_mode];
            addon_scale = instance.editor_scale[editor_mode];
        }
        rotation.mul(PI / 180.f);

        Fmatrix offset;
        offset.setHPB(rotation.x, rotation.y, rotation.z);
        offset.i.mul(addon_scale);
        offset.j.mul(addon_scale);
        offset.k.mul(addon_scale);
        offset.translate_over(position);

        LPCSTR visual_bone_name = read_addon_visual_bone(sections[i].c_str(), visual_config_hud);
        IKinematics* visual_model = visual->dcast_PKinematics();

        // Keep the authored models\\collimsight material on the reticle and
        // switch only its mesh bone. Its depth-tested pass is seen through the
        // models\\transparent lens, while opaque sight geometry still masks it.
        // HUD rendering follows the real addon-scope ADS state; the attachment
        // preview deliberately forces the reticle on.
        if (visual_model && addon_class_matches(sections[i].c_str(), "scope"))
        {
            const bool is_active_scope = active_scope_section.c_str() &&
                !_stricmp(active_scope_section.c_str(), sections[i].c_str());
            const bool reticle_visible = ui_preview ||
                (hud_mode && is_active_scope && IsAimingThroughAddonScope());
            LPCSTR reticle_bone = READ_IF_EXISTS(pSettings, r_string, sections[i], "collimator_bone", nullptr);
            if (!reticle_bone || !reticle_bone[0])
                reticle_bone = READ_IF_EXISTS(pSettings, r_string, sections[i], "aim_bone", "reticle");
            const u16 reticle_bone_id = find_addon_bone(visual_model, reticle_bone);
            if (reticle_bone_id != BI_NONE)
                visual_model->LL_SetBoneVisible(reticle_bone_id, reticle_visible, FALSE);
        }

        // Source/ValveBiped attachment exports can contain an arm hierarchy
        // plus a separate `root` which actually owns the visible addon mesh.
        // Their model origin belongs to the animation rig, so attaching that
        // origin to the weapon bone applies the rig offset a second time.
        // Automatically use the additional root as the visual anchor while
        // preserving explicit visual_attach_bone settings.
        if ((!visual_bone_name || !visual_bone_name[0]) && visual_model && attach_space == EAddonAttachSpace::WeaponBone)
        {
            const u16 root_bone = find_addon_bone(visual_model, "root");
            if (root_bone != BI_NONE && root_bone != visual_model->LL_GetBoneRoot())
                visual_bone_name = "root";
        }
        bool visual_bone_found = !visual_bone_name || !visual_bone_name[0];
        Fmatrix result;
        if (attach_space == EAddonAttachSpace::Weapon)
            result.mul_43(attach_transform, offset);
        else if (attach_space == EAddonAttachSpace::WeaponBone)
        {
            // Attach the addon's origin directly to the current weapon-bone
            // transform. Hidden replacement bones keep their animated
            // mTransform, so no skinning/bind-pose compensation is needed.
            Fmatrix bone_transform;
            bone_transform.mul_43(attach_transform, attach_model->LL_GetTransform(bone_id));
            result.mul_43(bone_transform, offset);

            if (visual_bone_name && visual_bone_name[0])
            {
                const u16 visual_bone_id = find_addon_bone(visual_model, visual_bone_name);
                if (visual_bone_id != BI_NONE)
                {
                    visual_bone_found = true;
                    Fmatrix inverse_visual_bone, aligned_result;
                    inverse_visual_bone.invert(visual_model->LL_GetTransform(visual_bone_id));
                    aligned_result.mul_43(result, inverse_visual_bone);
                    result = aligned_result;
                }
            }
        }
        else
        {
            Fmatrix bone_transform;
            bone_transform.mul_43(attach_transform, attach_model->LL_GetTransform(bone_id));
            result.mul_43(bone_transform, offset);

            // Optional bone-to-bone alignment. This preserves following of the
            // animated weapon bone while compensating for a non-zero anchor in
            // the separately exported addon model:
            // visual_world * visual_anchor = weapon_bone_world * offset.
            const u16 visual_bone_id = find_addon_bone(visual_model, visual_bone_name);
            if (visual_bone_id != BI_NONE)
            {
                visual_bone_found = true;
                Fmatrix inverse_visual_bone, aligned_result;
                inverse_visual_bone.invert(visual_model->LL_GetTransform(visual_bone_id));
                aligned_result.mul_43(result, inverse_visual_bone);
                result = aligned_result;
            }
        }

        bool& reported = hud_mode ? instance.hud_reported : instance.world_reported;
        if (!reported)
        {
            LPCSTR space_name = attach_space == EAddonAttachSpace::Weapon ? "weapon" :
                (attach_space == EAddonAttachSpace::WeaponBone ? "weapon_bone" : "bone");
            const float transform_scale = _max(result.i.magnitude(), _max(result.j.magnitude(), result.k.magnitude()));
            Fvector render_center;
            result.transform_tiny(render_center, visual->getVisData().sphere.P);
            const float render_radius = visual->getVisData().sphere.R * transform_scale;
            Msg("* Weapon addon [%s]: mode [%s], visual [%s], hud visual [%s], type [%u], %s space, target [%s], visual bone [%s], addon scale [%g], "
                "matrix scale [%g], origin [%g, %g, %g], sphere [%g, %g, %g; r=%g]",
                sections[i].c_str(), hud_mode ? "HUD" : "world", visual_name, visual->isHudVisual() ? "yes" : "no", visual->getType(), space_name,
                bone_name ? bone_name : "<root>", visual_bone_name ? visual_bone_name : "<origin>", addon_scale, transform_scale, result.c.x, result.c.y,
                result.c.z, render_center.x, render_center.y, render_center.z, render_radius);
            if (!visual_bone_found)
                Msg("! Weapon addon [%s]: visual bone [%s] not found in [%s]", sections[i].c_str(), visual_bone_name, visual_name);
            reported = true;
        }
        if (hud_mode && hand_pose && hand_pose[0] && g_player_hud)
        {
            IKinematics* pose_source = visual->dcast_PKinematics();
            if (pose_source)
            {
                Fmatrix weapon_inverse, source_to_weapon;
                weapon_inverse.invert(weapon_transform);
                source_to_weapon.mul_43(weapon_inverse, result);

                float blend_in = READ_IF_EXISTS(pSettings, r_float, sections[i], "hud_hand_pose_blend_in", 0.18f);
                float blend_out = READ_IF_EXISTS(pSettings, r_float, sections[i], "hud_hand_pose_blend_out", 0.1f);
                bool hold_between = READ_IF_EXISTS(pSettings, r_bool, sections[i], "hud_hand_pose_hold_between_animations", true);
                float ik_time = READ_IF_EXISTS(pSettings, r_float, sections[i], "hud_hand_pose_ik_time", 0.8f);
                LPCSTR ik_timeline = READ_IF_EXISTS(pSettings, r_string, sections[i], "hud_hand_pose_ik_timeline", nullptr);
                string256 motion_ik_key{};
                string256 motion_timeline_key{};
                if (m_current_motion.c_str() && m_current_motion.size())
                {
                    xr_sprintf(motion_ik_key, "hand_pose_ik_time_%s", m_current_motion.c_str());
                    if (pSettings->line_exist(sections[i], motion_ik_key))
                        ik_time = pSettings->r_float(sections[i], motion_ik_key);
                    if (pSettings->line_exist(hud_sect, motion_ik_key))
                        ik_time = pSettings->r_float(hud_sect, motion_ik_key);

                    xr_sprintf(motion_timeline_key, "hand_pose_ik_timeline_%s", m_current_motion.c_str());
                    if (pSettings->line_exist(sections[i], motion_timeline_key))
                        ik_timeline = pSettings->r_string(sections[i], motion_timeline_key);
                    if (pSettings->line_exist(hud_sect, motion_timeline_key))
                        ik_timeline = pSettings->r_string(hud_sect, motion_timeline_key);
                }

                bool motion_overrides_weapon = m_current_motion.c_str() && m_current_motion.size() &&
                    (config_list_contains_motion(sections[i].c_str(), "hud_hand_pose_override_animations", m_current_motion.c_str()) ||
                        config_list_contains_motion(hud_sect.c_str(), "hand_pose_override_animations", m_current_motion.c_str()));
                if (m_current_motion.c_str() && m_current_motion.size())
                {
                    const auto editor_override = std::find_if(m_hand_pose_ik_editor_overrides.begin(), m_hand_pose_ik_editor_overrides.end(),
                        [i, this](const SHandPoseIKEditorOverride& entry) {
                            return entry.visual_index == i && !_stricmp(entry.motion.c_str(), m_current_motion.c_str());
                        });
                    if (editor_override != m_hand_pose_ik_editor_overrides.end())
                    {
                        blend_in = editor_override->blend_in;
                        blend_out = editor_override->blend_out;
                        hold_between = editor_override->hold_between;
                        ik_time = editor_override->ik_time;
                        ik_timeline = editor_override->timeline.c_str();
                        motion_overrides_weapon = editor_override->override_weapon;
                    }
                }
                const bool editor_preview = m_hand_pose_ik_editor_preview_motion.c_str() && m_current_motion.c_str() &&
                    !_stricmp(m_hand_pose_ik_editor_preview_motion.c_str(), m_current_motion.c_str()) && m_dwMotionEndTm > m_dwMotionStartTm;
                float target_weight = GetState() == eIdle || motion_overrides_weapon ||
                    (hold_between && m_dwMotionEndTm <= m_dwMotionStartTm) ? 1.f : 0.f;
                bool timeline_driven = false;
                if (!motion_overrides_weapon && (editor_preview || GetState() != eIdle) && m_dwMotionEndTm > m_dwMotionStartTm)
                {
                    const float motion_progress = clampr(float(Device.dwTimeGlobal - m_dwMotionStartTm) /
                        float(m_dwMotionEndTm - m_dwMotionStartTm), 0.f, 1.f);
                    timeline_driven = evaluate_addon_ik_timeline(ik_timeline, motion_progress, target_weight);
                    if (!timeline_driven && ik_time >= 0.f)
                    {
                        // ARC9-like default timeline: release the authored grip
                        // during the opening 10%, keep the action authoritative,
                        // then ease back to the grip from ik_time to the end.
                        const float release_end = _min(0.1f, clampr(ik_time, 0.f, 1.f));
                        if (release_end > EPS_S && motion_progress < release_end)
                            target_weight = arc9_ik_lerp(1.f, 0.f, motion_progress / release_end);
                        else if (motion_progress < clampr(ik_time, 0.f, 1.f))
                            target_weight = 0.f;
                        else
                        {
                            const float return_start = clampr(ik_time, 0.f, 1.f);
                            target_weight = arc9_ik_lerp(0.f, 1.f,
                                (motion_progress - return_start) / _max(1.f - return_start, EPS_S));
                        }
                        timeline_driven = true;
                    }
                }

                // A timeline already supplies a smooth, eased weight every
                // frame. Applying the legacy time filter again would lag it.
                const float effective_blend_in = timeline_driven || motion_overrides_weapon ? 0.f : blend_in;
                const float effective_blend_out = timeline_driven || motion_overrides_weapon ? 0.f : blend_out;

                if (!_stricmp(hand_pose, "right") || !_stricmp(hand_pose, "both"))
                    g_player_hud->set_addon_hand_pose_source(0, pose_source, source_to_weapon, this, target_weight, effective_blend_in, effective_blend_out);
                if (!_stricmp(hand_pose, "left") || !_stricmp(hand_pose, "both"))
                    g_player_hud->set_addon_hand_pose_source(1, pose_source, source_to_weapon, this, target_weight, effective_blend_in, effective_blend_out);
            }
        }
        if (hud_mode)
        {
            instance.hud_transform = result;
            Fmatrix inverse_weapon;
            if (inverse_weapon.invert_b(weapon_transform))
            {
                instance.hud_local_transform.mul_43(inverse_weapon, result);
                instance.hud_local_transform_valid = true;
            }
            else
                instance.hud_local_transform_valid = false;
        }
        else
            instance.world_transform = result;
        ::Render->add_Visual(context_id, root, visual, result);
        processed[i] = true;
        progressed = true;
        }
        if (!progressed)
            break;
    }
}

void CWeapon::Hit(SHit* pHDS) { inherited::Hit(pHDS); }

void CWeapon::UpdateXForm()
{
    if (Device.dwFrame != dwXF_Frame)
    {
        dwXF_Frame = Device.dwFrame;

        // Get access to entity and its visual
        CEntityAlive* E = smart_cast<CEntityAlive*>(H_Parent());

        if (!E)
            return;

        const CInventoryOwner* parent = smart_cast<const CInventoryOwner*>(E);
        if (!parent || parent && parent->use_simplified_visual())
            return;

        if (parent->attached(this))
            return;

        IKinematics* V = smart_cast<IKinematics*>(E->Visual());
        VERIFY(V);

        // Get matrices
        int boneL{BI_NONE}, boneR{BI_NONE}, boneR2{BI_NONE};

        E->g_WeaponBones(boneL, boneR, boneR2);

        if ((HandDependence() == hd1Hand) || (GetState() == eReload) || (!E->g_Alive()))
            boneL = boneR2;

        // KRodin: видимо такое случается иногда у некоторых визуалов нпс. Например если создать нпс с визуалом монстра наверно.
        if (boneL == BI_NONE || boneR == BI_NONE)
            return;

        if (auto pActor = smart_cast<CActor*>(H_Parent()); pActor && pActor->cam_Active() != pActor->cam_FirstEye())
        {
            // https://www.gameru.net/forum/index.php?s=&showtopic=23443&view=findpost&p=1677678
            V->CalculateBones_Invalidate();
            V->CalculateBones(true);
        }
        else
        {
            V->CalculateBones();
        }

        const Fmatrix& mL = V->LL_GetTransform(u16(boneL));
        const Fmatrix& mR = V->LL_GetTransform(u16(boneR));
        // Calculate
        Fmatrix mRes;
        Fvector R, D, N;
        D.sub(mL.c, mR.c);

        if (fis_zero(D.magnitude()))
        {
            mRes.set(E->XFORM());
            mRes.c.set(mR.c);
        }
        else
        {
            D.normalize();
            R.crossproduct(mR.j, D);

            N.crossproduct(D, R);
            N.normalize();

            mRes.set(R, N, D, mR.c);
            mRes.mulA_43(E->XFORM());
        }

        UpdatePosition(mRes);
    }
}

void CWeapon::UpdateFireDependencies_internal()
{
    if (skip_updated_frame == Device.dwFrame || Device.dwFrame != dwFP_Frame)
    {
        dwFP_Frame = Device.dwFrame;

        UpdateXForm();

        if (GetHUDmode())
        {
            HudItemData()->setup_firedeps(m_current_firedeps);
            VERIFY(_valid(m_current_firedeps.m_FireParticlesXForm));
        }
        else
        {
            // 3rd person or no parent
            Fmatrix& parent = XFORM();
            Fvector& fp = vLoadedFirePoint;
            Fvector& fp2 = vLoadedFirePoint2;
            Fvector& sp = vLoadedShellPoint;

            parent.transform_tiny(m_current_firedeps.vLastFP, fp);
            parent.transform_tiny(m_current_firedeps.vLastFP2, fp2);
            parent.transform_tiny(m_current_firedeps.vLastSP, sp);
            parent.transform_tiny(m_current_firedeps.vLastShootPoint, fp);

            m_current_firedeps.vLastFD.set(0.f, 0.f, 1.f);
            parent.transform_dir(m_current_firedeps.vLastFD);

            m_current_firedeps.m_FireParticlesXForm.set(parent);
            VERIFY(_valid(m_current_firedeps.m_FireParticlesXForm));
        }
    }
}

void CWeapon::ForceUpdateFireParticles()
{
    if (!GetHUDmode())
    { // update particlesXFORM real bullet direction

        if (!H_Parent())
            return;

        Fvector p, d;
        smart_cast<CEntity*>(H_Parent())->g_fireParams(this, p, d);

        Fmatrix _pxf;
        _pxf.k = d;
        _pxf.i.crossproduct(Fvector().set(0.0f, 1.0f, 0.0f), _pxf.k);
        _pxf.j.crossproduct(_pxf.k, _pxf.i);
        _pxf.c = XFORM().c;

        m_current_firedeps.m_FireParticlesXForm.set(_pxf);
    }
}

constexpr const char* wpn_scope_def_bone = "wpn_scope";
constexpr const char* wpn_silencer_def_bone = "wpn_silencer";
constexpr const char* wpn_launcher_def_bone_shoc = "wpn_launcher";
constexpr const char* wpn_launcher_def_bone_cop = "wpn_grenade_launcher";

void CWeapon::Load(LPCSTR section)
{
    inherited::Load(section);
    CShootingObject::Load(section);

    m_critical_addon_classes.clear();
    if (pSettings->line_exist(section, "critical_addon_classes"))
    {
        LPCSTR value = pSettings->r_string(section, "critical_addon_classes");
        string128 addon_class{};
        for (int i = 0, count = _GetItemCount(value); i < count; ++i)
        {
            _GetItem(value, i, addon_class);
            if (addon_class[0])
                m_critical_addon_classes.emplace_back(addon_class);
        }
    }

    for (u8 slot = 0; slot < eCustomAddonCount; ++slot)
    {
        SCustomAddonSlot& data = m_custom_addon_slots[slot];
        data.name = slot < eCustomAddonDynamicBegin ? fixed_custom_slot_names[slot] : nullptr;
        data.provider = nullptr;
        data.root_allowed.clear();
        data.allowed.clear();
        data.installed_index = 0;

        if (slot >= eCustomAddonDynamicBegin)
            continue;

        string128 key{};
        xr_sprintf(key, "%s_addons", GetCustomAddonSlotName(static_cast<ECustomAddonSlot>(slot)));
        if (!pSettings->line_exist(section, key))
            continue;

        LPCSTR value = pSettings->r_string(section, key);
        string128 addon_section{};
        for (int i = 0, count = _GetItemCount(value); i < count; ++i)
        {
            _GetItem(value, i, addon_section);
            ASSERT_FMT(pSettings->section_exist(addon_section), "Addon section [%s] from [%s] does not exist", addon_section, key);
            ASSERT_FMT(data.allowed.size() < u8(-1), "Too many addons in [%s]", key);
            data.root_allowed.emplace_back(addon_section);
            data.allowed.emplace_back(addon_section);
        }
    }

    // Root weapons may expose arbitrary named mounting points. Fixed names
    // keep their stable save indices; names such as sight_rear, sight_front,
    // scope, silencer and grenade_launcher use the remaining named slots.
    const auto& weapon_data = pSettings->r_section(section).Ordered_Data;
    for (const auto& line : weapon_data)
    {
        const xr_string key = line.first.c_str();
        constexpr LPCSTR suffix = "_addons";
        constexpr size_t suffix_length = 7;
        if (key.size() <= suffix_length || key.compare(key.size() - suffix_length, suffix_length, suffix) ||
            key == "highlight_addons" || key == "preinstalled_addons")
            continue;

        const xr_string slot_name = key.substr(0, key.size() - suffix_length);
        bool known_slot = false;
        for (u8 slot = 0; slot < eCustomAddonCount; ++slot)
        {
            if (m_custom_addon_slots[slot].name.c_str() && !_stricmp(m_custom_addon_slots[slot].name.c_str(), slot_name.c_str()))
            {
                known_slot = true;
                break;
            }
        }
        if (known_slot)
            continue;

        u8 target_index = u8(-1);
        for (u8 slot = eCustomAddonDynamicBegin; slot < eCustomAddonCount; ++slot)
            if (!m_custom_addon_slots[slot].name.c_str())
            {
                target_index = slot;
                break;
            }
        R_ASSERT3(target_index != u8(-1), "Too many root addon slots; increase CWeapon::eCustomAddonCount", section);

        SCustomAddonSlot& target = m_custom_addon_slots[target_index];
        target.name = slot_name.c_str();
        LPCSTR value = line.second.c_str();
        string128 child{};
        for (int i = 0, count = _GetItemCount(value); i < count; ++i)
        {
            _GetItem(value, i, child);
            ASSERT_FMT(pSettings->section_exist(child), "Addon section [%s] from [%s] does not exist", child, key.c_str());
            ASSERT_FMT(pSettings->line_exist(child, "addon_slot") &&
                    !_stricmp(pSettings->r_string(child, "addon_slot"), slot_name.c_str()),
                "Addon [%s] must use addon_slot = %s", child, slot_name.c_str());
            target.root_allowed.emplace_back(child);
            target.allowed.emplace_back(child);
        }
    }

    // Build a deterministic superset of all recursively reachable addon
    // sections. Installed indices can then be serialized exactly as before;
    // actual attach permission is checked against the currently installed
    // parent chain in CanAttachCustomAddon().
    xr_vector<shared_str> providers;
    for (const SCustomAddonSlot& slot : m_custom_addon_slots)
        providers.insert(providers.end(), slot.root_allowed.begin(), slot.root_allowed.end());
    for (u32 provider_index = 0; provider_index < providers.size(); ++provider_index)
    {
        const shared_str provider = providers[provider_index];
        if (!provider.c_str() || !pSettings->section_exist(provider))
            continue;
        const auto& provider_data = pSettings->r_section(provider).Ordered_Data;
        for (const auto& line : provider_data)
        {
            const xr_string key = line.first.c_str();
            constexpr LPCSTR suffix = "_addons";
            constexpr size_t suffix_length = 7;
            if (key.size() <= suffix_length || key.compare(key.size() - suffix_length, suffix_length, suffix))
                continue;

            const xr_string slot_name = key.substr(0, key.size() - suffix_length);
            u8 target_index = u8(-1);
            for (u8 slot = 0; slot < eCustomAddonDynamicBegin; ++slot)
            {
                if (!_stricmp(slot_name.c_str(), GetCustomAddonSlotName(static_cast<ECustomAddonSlot>(slot))))
                {
                    target_index = slot;
                    break;
                }
            }
            if (target_index == u8(-1))
            {
                for (u8 slot = eCustomAddonDynamicBegin; slot < eCustomAddonCount; ++slot)
                {
                    SCustomAddonSlot& candidate = m_custom_addon_slots[slot];
                    if (candidate.name.c_str() && !_stricmp(candidate.name.c_str(), slot_name.c_str()) &&
                        candidate.provider.c_str() && !_stricmp(candidate.provider.c_str(), provider.c_str()))
                    {
                        target_index = slot;
                        break;
                    }
                    if (target_index == u8(-1) && !candidate.name.c_str())
                        target_index = slot;
                }
                R_ASSERT3(target_index != u8(-1), "Too many named addon slots; increase CWeapon::eCustomAddonCount", provider.c_str());
                SCustomAddonSlot& dynamic_slot = m_custom_addon_slots[target_index];
                if (!dynamic_slot.name.c_str())
                {
                    dynamic_slot.name = slot_name.c_str();
                    dynamic_slot.provider = provider;
                }
            }

            LPCSTR value = line.second.c_str();
            string128 child{};
            for (int i = 0, count = _GetItemCount(value); i < count; ++i)
            {
                _GetItem(value, i, child);
                ASSERT_FMT(pSettings->section_exist(child), "Child addon section [%s] from [%s]:%s does not exist", child, provider.c_str(), key.c_str());
                SCustomAddonSlot& target_slot = m_custom_addon_slots[target_index];
                if (std::find(target_slot.allowed.begin(), target_slot.allowed.end(), child) != target_slot.allowed.end())
                    continue;
                ASSERT_FMT(target_slot.allowed.size() < u8(-1), "Too many recursively reachable addons in [%s]", key.c_str());
                target_slot.allowed.emplace_back(child);
                providers.emplace_back(child);
            }
        }
    }

    auto preinstall = [this](LPCSTR addon_section) {
        if (!addon_section || !addon_section[0])
            return;
        for (u8 slot = 0; slot < eCustomAddonCount; ++slot)
        {
            SCustomAddonSlot& target = m_custom_addon_slots[slot];
            const auto found = std::find(target.allowed.begin(), target.allowed.end(), addon_section);
            if (found == target.allowed.end())
                continue;
            const u8 installed_index = static_cast<u8>(std::distance(target.allowed.begin(), found) + 1);
            if (target.installed_index == installed_index)
                return;
            ASSERT_FMT(!target.installed_index, "More than one preinstalled addon configured for slot [%s]", target.name.c_str());
            target.installed_index = installed_index;
            return;
        }
        ASSERT_FMT(false, "Preinstalled addon [%s] is not reachable from weapon [%s]", addon_section, cNameSect().c_str());
    };

    for (u8 slot = 0; slot < eCustomAddonCount; ++slot)
    {
        LPCSTR slot_name = m_custom_addon_slots[slot].name.c_str();
        if (!slot_name)
            continue;
        string128 key{};
        xr_sprintf(key, "%s_installed", slot_name);
        if (pSettings->line_exist(section, key))
            preinstall(pSettings->r_string(section, key));
    }
    if (pSettings->line_exist(section, "preinstalled_addons"))
    {
        LPCSTR value = pSettings->r_string(section, "preinstalled_addons");
        string128 addon_section{};
        for (int i = 0, count = _GetItemCount(value); i < count; ++i)
        {
            _GetItem(value, i, addon_section);
            preinstall(addon_section);
        }
    }

    if (pSettings->line_exist(section, "flame_particles_2"))
        m_sFlameParticles2 = pSettings->r_string(section, "flame_particles_2");

    if (!m_bForcedParticlesHudMode)
        m_bParticlesHudMode = !!pSettings->line_exist(hud_sect, "item_visual");

#ifdef DEBUG
    {
        Fvector pos, ypr;
        pos = pSettings->r_fvector3(section, "position");
        ypr = pSettings->r_fvector3(section, "orientation");
        ypr.mul(PI / 180.f);

        m_Offset.setHPB(ypr.x, ypr.y, ypr.z);
        m_Offset.translate_over(pos);
    }

    m_StrapOffset = m_Offset;
    if (pSettings->line_exist(section, "strap_position") && pSettings->line_exist(section, "strap_orientation"))
    {
        Fvector pos, ypr;
        pos = pSettings->r_fvector3(section, "strap_position");
        ypr = pSettings->r_fvector3(section, "strap_orientation");
        ypr.mul(PI / 180.f);

        m_StrapOffset.setHPB(ypr.x, ypr.y, ypr.z);
        m_StrapOffset.translate_over(pos);
    }
#endif

    // load ammo classes
    m_ammoTypes.clear();
    LPCSTR S = pSettings->r_string(section, "ammo_class");
    if (S && S[0])
    {
        string128 _ammoItem;
        int count = _GetItemCount(S);
        for (int it = 0; it < count; ++it)
        {
            _GetItem(S, it, _ammoItem);
            m_ammoTypes.push_back(_ammoItem);
        }
    }

    iAmmoElapsed = pSettings->r_s32(section, "ammo_elapsed");
    iMagazineSize = pSettings->r_s32(section, "ammo_mag_size");
    m_configuredMagazineSize = iMagazineSize;

    ////////////////////////////////////////////////////
    // дисперсия стрельбы

    //подбрасывание камеры во время отдачи
    camMaxAngle = pSettings->r_float(section, "cam_max_angle");
    camMaxAngle = deg2rad(camMaxAngle);
    camRelaxSpeed = pSettings->r_float(section, "cam_relax_speed");
    camRelaxSpeed = deg2rad(camRelaxSpeed);
    if (pSettings->line_exist(section, "cam_relax_speed_ai"))
    {
        camRelaxSpeed_AI = pSettings->r_float(section, "cam_relax_speed_ai");
        camRelaxSpeed_AI = deg2rad(camRelaxSpeed_AI);
    }
    else
    {
        camRelaxSpeed_AI = camRelaxSpeed;
    }

    //	camDispersion		= pSettings->r_float		(section,"cam_dispersion"	);
    //	camDispersion		= deg2rad					(camDispersion);

    camMaxAngleHorz = pSettings->r_float(section, "cam_max_angle_horz");
    camMaxAngleHorz = deg2rad(camMaxAngleHorz);
    camStepAngleHorz = pSettings->r_float(section, "cam_step_angle_horz");
    camStepAngleHorz = deg2rad(camStepAngleHorz);
    camDispertionFrac = READ_IF_EXISTS(pSettings, r_float, section, "cam_dispertion_frac", 0.7f);

    modernRecoil = SModernRecoilParams{};
    modernRecoil.enabled = READ_IF_EXISTS(pSettings, r_bool, section, "cam_recoil_modern", true);
    modernRecoil.recoil = READ_IF_EXISTS(pSettings, r_float, section, "arc9_recoil", modernRecoil.recoil);
    modernRecoil.recoil_up = READ_IF_EXISTS(pSettings, r_float, section, "arc9_recoil_up", modernRecoil.recoil_up);
    modernRecoil.recoil_side = READ_IF_EXISTS(pSettings, r_float, section, "arc9_recoil_side", modernRecoil.recoil_side);
    modernRecoil.recoil_random_up = READ_IF_EXISTS(pSettings, r_float, section, "arc9_recoil_random_up", modernRecoil.recoil_random_up);
    modernRecoil.recoil_random_side = READ_IF_EXISTS(pSettings, r_float, section, "arc9_recoil_random_side", modernRecoil.recoil_random_side);
    modernRecoil.recoil_auto_control = READ_IF_EXISTS(pSettings, r_float, section, "arc9_recoil_auto_control", modernRecoil.recoil_auto_control);
    modernRecoil.recoil_pattern_drift = READ_IF_EXISTS(pSettings, r_float, section, "arc9_recoil_pattern_drift", modernRecoil.recoil_pattern_drift);
    modernRecoil.recoil_per_shot = READ_IF_EXISTS(pSettings, r_float, section, "arc9_recoil_per_shot", modernRecoil.recoil_per_shot);
    modernRecoil.recoil_dissipation_rate = READ_IF_EXISTS(pSettings, r_float, section, "arc9_recoil_dissipation_rate", modernRecoil.recoil_dissipation_rate);
    modernRecoil.recoil_reset_time = READ_IF_EXISTS(pSettings, r_float, section, "arc9_recoil_reset_time", modernRecoil.recoil_reset_time);
    modernRecoil.recoil_full_reset_time = READ_IF_EXISTS(pSettings, r_float, section, "arc9_recoil_full_reset_time", modernRecoil.recoil_full_reset_time);
    modernRecoil.camera_recoil_scale = READ_IF_EXISTS(pSettings, r_float, section, "arc9_camera_recoil_scale", modernRecoil.camera_recoil_scale);
    modernRecoil.camera_impulse_duration = READ_IF_EXISTS(pSettings, r_float, section, "arc9_camera_impulse_time", modernRecoil.camera_impulse_duration);
    modernRecoil.camera_max_pitch = READ_IF_EXISTS(pSettings, r_float, section, "arc9_camera_max_pitch", modernRecoil.camera_max_pitch);
    modernRecoil.camera_max_yaw = READ_IF_EXISTS(pSettings, r_float, section, "arc9_camera_max_yaw", modernRecoil.camera_max_yaw);

    modernRecoil.visual_recoil = READ_IF_EXISTS(pSettings, r_float, section, "arc9_visual_recoil", modernRecoil.visual_recoil);
    modernRecoil.visual_recoil_up = READ_IF_EXISTS(pSettings, r_float, section, "arc9_visual_recoil_up", modernRecoil.visual_recoil_up);
    modernRecoil.visual_recoil_up_semi = READ_IF_EXISTS(pSettings, r_float, section, "arc9_visual_recoil_up_semi", modernRecoil.visual_recoil_up_semi);
    modernRecoil.visual_recoil_side = READ_IF_EXISTS(pSettings, r_float, section, "arc9_visual_recoil_side", modernRecoil.visual_recoil_side);
    modernRecoil.visual_recoil_side_semi = READ_IF_EXISTS(pSettings, r_float, section, "arc9_visual_recoil_side_semi", modernRecoil.visual_recoil_side_semi);
    modernRecoil.visual_recoil_roll = READ_IF_EXISTS(pSettings, r_float, section, "arc9_visual_recoil_roll", modernRecoil.visual_recoil_roll);
    modernRecoil.visual_recoil_punch = READ_IF_EXISTS(pSettings, r_float, section, "arc9_visual_recoil_punch", modernRecoil.visual_recoil_punch);
    modernRecoil.visual_recoil_punch_sights = READ_IF_EXISTS(pSettings, r_float, section, "arc9_visual_recoil_punch_sights", modernRecoil.visual_recoil_punch_sights);
    modernRecoil.visual_recoil_spring_constant = READ_IF_EXISTS(pSettings, r_float, section, "arc9_visual_recoil_spring_constant", modernRecoil.visual_recoil_spring_constant);
    modernRecoil.visual_recoil_spring_magnitude = READ_IF_EXISTS(pSettings, r_float, section, "arc9_visual_recoil_spring_magnitude", modernRecoil.visual_recoil_spring_magnitude);
    modernRecoil.visual_recoil_spring_damping = READ_IF_EXISTS(pSettings, r_float, section, "arc9_visual_recoil_spring_damping", modernRecoil.visual_recoil_spring_damping);
    modernRecoil.visual_recoil_bump_up = READ_IF_EXISTS(pSettings, r_float, section, "arc9_visual_recoil_bump_up", modernRecoil.visual_recoil_bump_up);
    modernRecoil.visual_recoil_bump_up_hip = READ_IF_EXISTS(pSettings, r_float, section, "arc9_visual_recoil_bump_up_hip", modernRecoil.visual_recoil_bump_up_hip);
    modernRecoil.visual_recoil_position_bump = READ_IF_EXISTS(pSettings, r_float, section, "arc9_visual_recoil_position_bump", modernRecoil.visual_recoil_position_bump);
    modernRecoil.visual_recoil_scale = READ_IF_EXISTS(pSettings, r_float, section, "arc9_visual_recoil_scale", modernRecoil.visual_recoil_scale);
    if (pSettings->line_exist(section, "arc9_visual_recoil_center"))
        modernRecoil.visual_recoil_center = pSettings->r_fvector3(section, "arc9_visual_recoil_center");
    modernRecoil.shots_to_full_auto = READ_IF_EXISTS(pSettings, r_u32, section, "arc9_shots_to_full_auto", modernRecoil.shots_to_full_auto);
    modernRecoil.subtle_visual_recoil = READ_IF_EXISTS(pSettings, r_float, section, "arc9_subtle_visual_recoil", modernRecoil.subtle_visual_recoil);
    modernRecoil.subtle_visual_recoil_direction = READ_IF_EXISTS(pSettings, r_float, section, "arc9_subtle_visual_recoil_direction", modernRecoil.subtle_visual_recoil_direction);
    modernRecoil.subtle_visual_recoil_speed = READ_IF_EXISTS(pSettings, r_float, section, "arc9_subtle_visual_recoil_speed", modernRecoil.subtle_visual_recoil_speed);
    modernRecoil.zoom_multiplier = READ_IF_EXISTS(pSettings, r_float, section, "cam_recoil_zoom_k", modernRecoil.zoom_multiplier);
    modernRecoil.crouch_multiplier = READ_IF_EXISTS(pSettings, r_float, section, "cam_recoil_crouch_k", modernRecoil.crouch_multiplier);

    modernRecoil.recoil = _max(modernRecoil.recoil, 0.f);
    modernRecoil.recoil_auto_control = _max(modernRecoil.recoil_auto_control, 0.f);
    modernRecoil.recoil_per_shot = _max(modernRecoil.recoil_per_shot, 0.01f);
    modernRecoil.recoil_dissipation_rate = _max(modernRecoil.recoil_dissipation_rate, 0.f);
    modernRecoil.recoil_reset_time = _max(modernRecoil.recoil_reset_time, 0.f);
    modernRecoil.recoil_full_reset_time = _max(modernRecoil.recoil_full_reset_time, modernRecoil.recoil_reset_time);
    modernRecoil.camera_recoil_scale = _max(modernRecoil.camera_recoil_scale, 0.f);
    modernRecoil.camera_impulse_duration = _max(modernRecoil.camera_impulse_duration, 0.01f);
    modernRecoil.camera_max_pitch = _max(modernRecoil.camera_max_pitch, 0.1f);
    modernRecoil.camera_max_yaw = _max(modernRecoil.camera_max_yaw, 0.1f);
    modernRecoil.visual_recoil_spring_constant = _max(modernRecoil.visual_recoil_spring_constant, 0.f);
    modernRecoil.visual_recoil_spring_magnitude = _max(modernRecoil.visual_recoil_spring_magnitude, 0.f);
    modernRecoil.visual_recoil_spring_damping = _max(modernRecoil.visual_recoil_spring_damping, 0.f);
    modernRecoil.visual_recoil_scale = _max(modernRecoil.visual_recoil_scale, 0.f);
    modernRecoil.subtle_visual_recoil_speed = _max(modernRecoil.subtle_visual_recoil_speed, 0.01f);
    modernRecoil.zoom_multiplier = _max(modernRecoil.zoom_multiplier, 0.f);
    modernRecoil.crouch_multiplier = _max(modernRecoil.crouch_multiplier, 0.f);
    //  [8/2/2005]
    // m_fParentDispersionModifier = READ_IF_EXISTS(pSettings, r_float, section, "parent_dispersion_modifier",1.0f);
    m_fPDM_disp_base = READ_IF_EXISTS(pSettings, r_float, section, "PDM_disp_base", 1.0f);
    m_fPDM_disp_vel_factor = READ_IF_EXISTS(pSettings, r_float, section, "PDM_disp_vel_factor", 1.0f);
    m_fPDM_disp_accel_factor = READ_IF_EXISTS(pSettings, r_float, section, "PDM_disp_accel_factor", 1.0f);
    m_fPDM_disp_crouch = READ_IF_EXISTS(pSettings, r_float, section, "PDM_disp_crouch", 1.0f);
    m_fPDM_disp_crouch_no_acc = READ_IF_EXISTS(pSettings, r_float, section, "PDM_disp_crouch_no_acc", 1.0f);
    //  [8/2/2005]

    fireDispersionConditionFactor = pSettings->r_float(section, "fire_dispersion_condition_factor");
    misfireProbability = pSettings->r_float(section, "misfire_probability");
    misfireConditionK = READ_IF_EXISTS(pSettings, r_float, section, "misfire_condition_k", 1.0f);
    conditionDecreasePerShot = pSettings->r_float(section, "condition_shot_dec");
    conditionDecreasePerShotOnHit = READ_IF_EXISTS(pSettings, r_float, section, "condition_shot_dec_on_hit", 0.f);
    conditionDecreasePerShotSilencer = READ_IF_EXISTS(pSettings, r_float, section, "condition_shot_dec_silencer", conditionDecreasePerShot);

    vLoadedFirePoint = pSettings->r_fvector3(section, "fire_point");

    if (pSettings->line_exist(section, "fire_point2"))
        vLoadedFirePoint2 = pSettings->r_fvector3(section, "fire_point2");
    else
        vLoadedFirePoint2 = vLoadedFirePoint;

    // hands
    eHandDependence = EHandDependence(pSettings->r_s32(section, "hand_dependence"));
    m_bIsSingleHanded = true;
    if (pSettings->line_exist(section, "single_handed"))
        m_bIsSingleHanded = !!pSettings->r_bool(section, "single_handed");
    //
    m_fMinRadius = pSettings->r_float(section, "min_radius");
    m_fMaxRadius = pSettings->r_float(section, "max_radius");

    // информация о возможных апгрейдах и их визуализации в инвентаре
    m_eScopeStatus = (ALife::EWeaponAddonStatus)pSettings->r_s32(section, "scope_status");
    m_eSilencerStatus = (ALife::EWeaponAddonStatus)pSettings->r_s32(section, "silencer_status");
    m_eGrenadeLauncherStatus = (ALife::EWeaponAddonStatus)pSettings->r_s32(section, "grenade_launcher_status");

    if (m_eSilencerStatus == ALife::eAddonPermanent)
        m_bLightShotEnabled = false;

    m_bZoomEnabled = !!pSettings->r_bool(section, "zoom_enabled");
    m_bUseScopeZoom = !!READ_IF_EXISTS(pSettings, r_bool, section, "use_scope_zoom", false);
    m_bUseScopeGrenadeZoom = !!READ_IF_EXISTS(pSettings, r_bool, section, "use_scope_grenade_zoom", false);
    m_bScopeShowIndicators = !!READ_IF_EXISTS(pSettings, r_bool, section, "scope_show_indicators", true);
    m_bIgnoreScopeTexture = !!READ_IF_EXISTS(pSettings, r_bool, section, "ignore_scope_texture", false);

    m_fZoomRotateTime = READ_IF_EXISTS(pSettings, r_float, hud_sect, "zoom_rotate_time", ROTATION_TIME);

    m_bScopeDynamicZoom = false;
    m_fScopeZoomFactor = 0;
    m_fRTZoomFactor = 0;

    m_fZoomFactor = CurrentZoomFactor();

    m_allScopeNames.clear();
    m_highlightAddons.clear();
    if (m_eScopeStatus == ALife::eAddonAttachable)
    {
        m_sScopeName = pSettings->r_string(section, "scope_name");
        m_iScopeX = pSettings->r_s32(section, "scope_x");
        m_iScopeY = pSettings->r_s32(section, "scope_y");

        m_allScopeNames.push_back(m_sScopeName);
        if (pSettings->line_exist(section, "scope_names"))
        {
            LPCSTR S = pSettings->r_string(section, "scope_names");
            if (S && S[0])
            {
                string128 _scopeItem;
                int count = _GetItemCount(S);
                for (int it = 0; it < count; ++it)
                {
                    _GetItem(S, it, _scopeItem);
                    m_allScopeNames.push_back(_scopeItem);
                    m_highlightAddons.push_back(_scopeItem);
                }
            }
        }
    }

    if (m_eSilencerStatus == ALife::eAddonAttachable)
    {
        m_sSilencerName = pSettings->r_string(section, "silencer_name");
        m_iSilencerX = pSettings->r_s32(section, "silencer_x");
        m_iSilencerY = pSettings->r_s32(section, "silencer_y");
    }

    if (m_eGrenadeLauncherStatus == ALife::eAddonAttachable)
    {
        m_sGrenadeLauncherName = pSettings->r_string(section, "grenade_launcher_name");
        m_iGrenadeLauncherX = pSettings->r_s32(section, "grenade_launcher_x");
        m_iGrenadeLauncherY = pSettings->r_s32(section, "grenade_launcher_y");
    }

    // Кости мировой модели оружия
    if (pSettings->line_exist(section, "scope_bone"))
    {
        const char* S = pSettings->r_string(section, "scope_bone");
        if (S && strlen(S))
        {
            const int count = _GetItemCount(S);
            string128 _scope_bone{};
            for (int it = 0; it < count; ++it)
            {
                _GetItem(S, it, _scope_bone);
                m_sWpn_scope_bones.push_back(_scope_bone);
            }
        }
        else
            m_sWpn_scope_bones.push_back(wpn_scope_def_bone);
    }
    else
        m_sWpn_scope_bones.push_back(wpn_scope_def_bone);
    m_sWpn_silencer_bone = READ_IF_EXISTS(pSettings, r_string, section, "silencer_bone", wpn_silencer_def_bone);
    m_sWpn_launcher_bone = READ_IF_EXISTS(pSettings, r_string, section, "launcher_bone", wpn_launcher_def_bone_shoc);
    m_sWpn_laser_bone = READ_IF_EXISTS(pSettings, r_string, section, "laser_ray_bones", "");
    m_sWpn_flashlight_bone = READ_IF_EXISTS(pSettings, r_string, section, "torch_cone_bones", "");

    if (pSettings->line_exist(section, "hidden_bones"))
    {
        const char* S = pSettings->r_string(section, "hidden_bones");
        if (S && strlen(S))
        {
            const int count = _GetItemCount(S);
            string128 _hidden_bone{};
            for (int it = 0; it < count; ++it)
            {
                _GetItem(S, it, _hidden_bone);
                hidden_bones.push_back(_hidden_bone);
            }
        }
    }

    append_addon_bones(section, "empty_hide_bones", empty_hide_bones);
    append_addon_bones(section, "hide_bones_when_empty", empty_hide_bones);

    // Кости худовой модели оружия - если не прописаны, используются имена из конфига мировой модели.
    if (pSettings->line_exist(hud_sect, "scope_bone"))
    {
        const char* S = pSettings->r_string(hud_sect, "scope_bone");
        if (S && strlen(S))
        {
            const int count = _GetItemCount(S);
            string128 _scope_bone{};
            for (int it = 0; it < count; ++it)
            {
                _GetItem(S, it, _scope_bone);
                m_sHud_wpn_scope_bones.push_back(_scope_bone);
            }
        }
        else
            m_sHud_wpn_scope_bones = m_sWpn_scope_bones;
    }
    else
        m_sHud_wpn_scope_bones = m_sWpn_scope_bones;
    m_sHud_wpn_silencer_bone = READ_IF_EXISTS(pSettings, r_string, hud_sect, "silencer_bone", m_sWpn_silencer_bone);
    m_sHud_wpn_launcher_bone = READ_IF_EXISTS(pSettings, r_string, hud_sect, "launcher_bone", m_sWpn_launcher_bone);
    m_sHud_wpn_laser_bone = READ_IF_EXISTS(pSettings, r_string, hud_sect, "laser_ray_bones", m_sWpn_laser_bone);
    m_sHud_wpn_flashlight_bone = READ_IF_EXISTS(pSettings, r_string, hud_sect, "torch_cone_bones", m_sWpn_flashlight_bone);

    if (pSettings->line_exist(hud_sect, "hidden_bones"))
    {
        const char* S = pSettings->r_string(hud_sect, "hidden_bones");
        if (S && strlen(S))
        {
            const int count = _GetItemCount(S);
            string128 _hidden_bone{};
            for (int it = 0; it < count; ++it)
            {
                _GetItem(S, it, _hidden_bone);
                hud_hidden_bones.push_back(_hidden_bone);
            }
        }
    }
    else
        hud_hidden_bones = hidden_bones;

    hud_empty_hide_bones = empty_hide_bones;
    if (pSettings->line_exist(hud_sect, "empty_hide_bones") || pSettings->line_exist(hud_sect, "hide_bones_when_empty"))
    {
        hud_empty_hide_bones.clear();
        append_addon_bones(hud_sect.c_str(), "empty_hide_bones", hud_empty_hide_bones);
        append_addon_bones(hud_sect.c_str(), "hide_bones_when_empty", hud_empty_hide_bones);
    }

    //Можно и из конфига прицела читать и наоборот! Пока так.
    m_fZoomHudFov = 0.0f;
    m_f3dssHudFov = 0.0f;
    m_fScopeInertionFactor = m_fControlInertionFactor;

    InitAddons();

    m_bHideCrosshairInZoom = true;
    if (pSettings->line_exist(hud_sect, "zoom_hide_crosshair"))
        m_bHideCrosshairInZoom = !!pSettings->r_bool(hud_sect, "zoom_hide_crosshair");

    m_bZoomInertionAllow =
        READ_IF_EXISTS(pSettings, r_bool, hud_sect, "allow_zoom_inertion", IS_OGSR_GA ? true : READ_IF_EXISTS(pSettings, r_bool, "features", "default_allow_zoom_inertion", true));
    m_bScopeZoomInertionAllow = READ_IF_EXISTS(pSettings, r_bool, hud_sect, "allow_scope_zoom_inertion",
                                               IS_OGSR_GA ? true : READ_IF_EXISTS(pSettings, r_bool, "features", "default_allow_scope_zoom_inertion", true));

    m_bCanBeLowered = READ_IF_EXISTS(pSettings, r_bool, section, "can_be_lowered", false);
    m_fSafeModeRotateTime = READ_IF_EXISTS(pSettings, r_float, section, "weapon_lower_speed", 1.f);
    m_safemode_anm[0].name = READ_IF_EXISTS(pSettings, r_string, hud_sect, "safemode_anm", nullptr);
    m_safemode_anm[1].name = READ_IF_EXISTS(pSettings, r_string, hud_sect, "safemode_anm2", nullptr);
    m_safemode_anm[0].speed = READ_IF_EXISTS(pSettings, r_float, hud_sect, "safemode_anm_speed", 1.f);
    m_safemode_anm[1].speed = READ_IF_EXISTS(pSettings, r_float, hud_sect, "safemode_anm_speed2", 1.f);
    m_safemode_anm[0].power = READ_IF_EXISTS(pSettings, r_float, hud_sect, "safemode_anm_power", 1.f);
    m_safemode_anm[1].power = READ_IF_EXISTS(pSettings, r_float, hud_sect, "safemode_anm_power2", 1.f);

    //////////////////////////////////////////////////////////

    m_bHasTracers = READ_IF_EXISTS(pSettings, r_bool, section, "tracers", true);
    m_u8TracerColorID = READ_IF_EXISTS(pSettings, r_u8, section, "tracers_color_ID", u8(-1));

    string256 temp;
    for (int i = egdNovice; i < egdCount; ++i)
    {
        strconcat(sizeof(temp), temp, "hit_probability_", get_token_name(difficulty_type_token, i));
        m_hit_probability[i] = READ_IF_EXISTS(pSettings, r_float, section, temp, 1.f);
    }

    if (pSettings->line_exist(section, "highlight_addons"))
    {
        LPCSTR S = pSettings->r_string(section, "highlight_addons");
        if (S && S[0])
        {
            string128 _addonItem;
            int count = _GetItemCount(S);
            for (int it = 0; it < count; ++it)
            {
                _GetItem(S, it, _addonItem);
                ASSERT_FMT(pSettings->section_exist(_addonItem), "Section [%s] not found!", _addonItem);
                m_highlightAddons.emplace_back(_addonItem);
            }
        }
    }

    if (!laser_light_render && pSettings->line_exist(section, "laser_light_section"))
    {
        has_laser = true;

        laserdot_attach_bone = READ_IF_EXISTS(pSettings, r_string, section, "laserdot_attach_bone", "");
        laserdot_attach_offset =
            Fvector{READ_IF_EXISTS(pSettings, r_float, section, "laserdot_attach_offset_x", 0.0f), READ_IF_EXISTS(pSettings, r_float, section, "laserdot_attach_offset_y", 0.0f),
                    READ_IF_EXISTS(pSettings, r_float, section, "laserdot_attach_offset_z", 0.0f)};
        laserdot_world_attach_offset = Fvector{READ_IF_EXISTS(pSettings, r_float, section, "laserdot_world_attach_offset_x", 0.0f),
                                               READ_IF_EXISTS(pSettings, r_float, section, "laserdot_world_attach_offset_y", 0.0f),
                                               READ_IF_EXISTS(pSettings, r_float, section, "laserdot_world_attach_offset_z", 0.0f)};

        constexpr bool b_r2 = true;

        const char* m_light_section = pSettings->r_string(section, "laser_light_section");

        laser_lanim = LALib.FindItem(READ_IF_EXISTS(pSettings, r_string, m_light_section, "color_animator", ""));

        laser_light_render = ::Render->light_create();
        laser_light_render->set_type(IRender_Light::SPOT);
        laser_light_render->set_shadow(false);
        laser_light_render->set_moveable(true);

        const Fcolor clr = READ_IF_EXISTS(pSettings, r_fcolor, m_light_section, b_r2 ? "color_r2" : "color", (Fcolor{1.0f, 0.0f, 0.0f, 1.0f}));
        laser_fBrightness = clr.intensity();
        laser_light_render->set_color(clr);
        const float range = READ_IF_EXISTS(pSettings, r_float, m_light_section, b_r2 ? "range_r2" : "range", 100.f);
        laser_light_render->set_range(range);
        laser_light_render->set_cone(deg2rad(READ_IF_EXISTS(pSettings, r_float, m_light_section, "spot_angle", 1.f)));
        laser_light_render->set_texture(READ_IF_EXISTS(pSettings, r_string, m_light_section, "spot_texture", nullptr));
    }

    if (!flashlight_render && pSettings->line_exist(section, "flashlight_section"))
    {
        has_flashlight = true;

        flashlight_attach_bone = pSettings->r_string(section, "torch_light_bone");
        flashlight_attach_offset = Fvector{pSettings->r_float(section, "torch_attach_offset_x"), pSettings->r_float(section, "torch_attach_offset_y"),
                                           pSettings->r_float(section, "torch_attach_offset_z")};
        flashlight_omni_attach_offset = Fvector{pSettings->r_float(section, "torch_omni_attach_offset_x"), pSettings->r_float(section, "torch_omni_attach_offset_y"),
                                                pSettings->r_float(section, "torch_omni_attach_offset_z")};
        flashlight_world_attach_offset = Fvector{pSettings->r_float(section, "torch_world_attach_offset_x"), pSettings->r_float(section, "torch_world_attach_offset_y"),
                                                 pSettings->r_float(section, "torch_world_attach_offset_z")};
        flashlight_omni_world_attach_offset =
            Fvector{pSettings->r_float(section, "torch_omni_world_attach_offset_x"), pSettings->r_float(section, "torch_omni_world_attach_offset_y"),
                    pSettings->r_float(section, "torch_omni_world_attach_offset_z")};

        constexpr bool b_r2 = true;

        const char* m_light_section = pSettings->r_string(section, "flashlight_section");

        flashlight_lanim = LALib.FindItem(READ_IF_EXISTS(pSettings, r_string, m_light_section, "color_animator", ""));

        flashlight_render = ::Render->light_create();
        flashlight_render->set_type(IRender_Light::SPOT);
        flashlight_render->set_shadow(ParentIsActor());
        flashlight_render->set_moveable(true);

        const Fcolor clr = READ_IF_EXISTS(pSettings, r_fcolor, m_light_section, b_r2 ? "color_r2" : "color", (Fcolor{0.6f, 0.55f, 0.55f, 1.0f}));

        bool useVolumetric = READ_IF_EXISTS(pSettings, r_bool, m_light_section, "volumetric_enabled", false);
        if (useVolumetric)
        {
            flashlight_render->set_volumetric(useVolumetric);

            float volIntensity = READ_IF_EXISTS(pSettings, r_float, m_light_section, "volumetric_intensity", 0.2f);
            volIntensity = std::clamp(volIntensity, 0.f, 1.f);
            flashlight_render->set_volumetric_intensity(volIntensity);

            float volDistance = READ_IF_EXISTS(pSettings, r_float, m_light_section, "volumetric_distance", 1.f);
            volDistance = std::clamp(volDistance, 0.f, 1.f);
            flashlight_render->set_volumetric_distance(volDistance);
        }

        flashlight_fBrightness = clr.intensity();
        flashlight_render->set_color(clr);
        const float range = READ_IF_EXISTS(pSettings, r_float, m_light_section, b_r2 ? "range_r2" : "range", 50.f);
        flashlight_render->set_range(range);
        flashlight_render->set_cone(deg2rad(READ_IF_EXISTS(pSettings, r_float, m_light_section, "spot_angle", 60.f)));
        flashlight_render->set_texture(READ_IF_EXISTS(pSettings, r_string, m_light_section, "spot_texture", nullptr));

        flashlight_omni = ::Render->light_create();
        flashlight_omni->set_type(
            (IRender_Light::LT)(READ_IF_EXISTS(pSettings, r_u8, m_light_section, "omni_type",
                                               2))); // KRodin: вообще omni это обычно поинт, но поинт светит во все стороны от себя, поэтому тут спот используется по умолчанию.
        flashlight_omni->set_shadow(false);
        flashlight_omni->set_moveable(true);

        const Fcolor oclr = READ_IF_EXISTS(pSettings, r_fcolor, m_light_section, b_r2 ? "omni_color_r2" : "omni_color", (Fcolor{1.0f, 1.0f, 1.0f, 0.0f}));
        flashlight_omni->set_color(oclr);
        const float orange = READ_IF_EXISTS(pSettings, r_float, m_light_section, b_r2 ? "omni_range_r2" : "omni_range", 0.25f);
        flashlight_omni->set_range(orange);
    }

    dof_transition_time = READ_IF_EXISTS(pSettings, r_float, section, "dof_transition_time", 0.6f);
    dof_params_zoom = (READ_IF_EXISTS(pSettings, r_fvector4, section, "dof_zoom_params", (Fvector4{0, 0, 0, 1.6}))); //(Fvector4{0.1, 0.4, 0, 1.6})
    dof_params_reload = (READ_IF_EXISTS(pSettings, r_fvector4, section, "dof_reload_params", (Fvector4{0, 0, 1, 0})));

    dont_interrupt_shot_anm = READ_IF_EXISTS(pSettings, r_bool, section, "dont_interrupt_shot_anm", false);
    is_gunslinger_weapon = READ_IF_EXISTS(pSettings, r_bool, section, "is_gunslinger_weapon", false);

    if (pSettings->line_exist(section, "bullet_textures_in_model"))
    {
        const char* str = pSettings->r_string(section, "bullet_textures_in_model");
        for (int i{}, count = _GetItemCount(str); i < count;)
        {
            xr_string bullet_tex;
            _GetItem(str, i++, bullet_tex);
            bullet_textures_in_model.emplace_back(std::move(bullet_tex));
        }
    }
    if (pSettings->line_exist(section, "bullet_textures_for_ammos"))
    {
        const char* str = pSettings->r_string(section, "bullet_textures_for_ammos");
        for (int i{}, count = _GetItemCount(str); i < count;)
        {
            xr_string ammo_section, bullet_tex;
            _GetItem(str, i++, ammo_section);
            ASSERT_FMT(i < count, "Incorrect [bullet_textures_for_ammos] in section [%s]", section);
            _GetItem(str, i++, bullet_tex);
            bullet_textures_for_ammos.emplace(std::move(ammo_section), std::move(bullet_tex));
        }
    }
}

void CWeapon::LoadFireParams(LPCSTR section, LPCSTR prefix)
{
    camDispersion = pSettings->r_float(section, "cam_dispersion");
    camDispersion = deg2rad(camDispersion);

    if (pSettings->line_exist(section, "cam_dispersion_inc"))
    {
        camDispersionInc = pSettings->r_float(section, "cam_dispersion_inc");
        camDispersionInc = deg2rad(camDispersionInc);
    }
    else
        camDispersionInc = 0;

    CShootingObject::LoadFireParams(section, prefix);
}

BOOL CWeapon::net_Spawn(CSE_Abstract* DC)
{
    BOOL bResult = inherited::net_Spawn(DC);

    auto E = smart_cast<CSE_ALifeItemWeapon*>(DC);

    // iAmmoCurrent					= E->a_current;
    iAmmoElapsed = E->a_elapsed;

    m_flagsAddOnState = E->m_addon_flags.get();
    m_ammoType = E->ammo_type;
    for (u8 slot = 0; slot < eCustomAddonCount; ++slot)
    {
        const u8 index = E->m_custom_addon_indices[slot];
        if (index != u8(-1))
            m_custom_addon_slots[slot].installed_index = index <= m_custom_addon_slots[slot].allowed.size() ? index : 0;
    }

    SetState(E->wpn_state);
    SetNextState(E->wpn_state);

    if (m_ammoType >= m_ammoTypes.size())
    {
        Msg("! [%s]: %s: wrong m_ammoType[%u/%u]", __FUNCTION__, cName().c_str(), m_ammoType, m_ammoTypes.size() - 1);
        m_ammoType = 0;
        auto se_obj = alife_object();
        if (se_obj)
        {
            auto W = smart_cast<CSE_ALifeItemWeapon*>(se_obj);
            W->ammo_type = m_ammoType;
        }
    }

    m_DefaultCartridge.Load(*m_ammoTypes[m_ammoType], u8(m_ammoType));
    if (iAmmoElapsed)
    {
        // нож автоматически заряжается двумя патронами, хотя
        // размер магазина у него 0. Что бы зря не ругаться, проверим
        // что в конфиге размер магазина не нулевой.
        if (iMagazineSize && iAmmoElapsed > (iMagazineSize + 1))
        {
            Msg("! [%s]: %s: wrong iAmmoElapsed[%u/%u]", __FUNCTION__, cName().c_str(), iAmmoElapsed, iMagazineSize);
            iAmmoElapsed = iMagazineSize;
            auto se_obj = alife_object();
            if (se_obj)
            {
                auto W = smart_cast<CSE_ALifeItemWeapon*>(se_obj);
                W->a_elapsed = iAmmoElapsed;
            }
        }
        m_fCurrentCartirdgeDisp = m_DefaultCartridge.m_kDisp;
        for (int i = 0; i < iAmmoElapsed; ++i)
            m_magazine.push_back(m_DefaultCartridge);
    }

    UpdateAddonsVisibility();
    InitAddons();

    VERIFY((u32)iAmmoElapsed == m_magazine.size());

    if (m_bLightShotEnabled)
        Light_Create();

    return bResult;
}

void CWeapon::net_Destroy()
{
    inherited::net_Destroy();

    //удалить объекты партиклов
    StopFlameParticles();
    StopFlameParticles2();
    StopLight();
    Light_Destroy();

    m_magazine.clear();
    m_magazine.shrink_to_fit();
}

BOOL CWeapon::IsUpdating()
{
    bool bIsActiveItem = m_pCurrentInventory && m_pCurrentInventory->ActiveItem() == this;
    return bIsActiveItem || bWorking || IsPending() || getVisible();
}

void CWeapon::net_Export(CSE_Abstract* E)
{
    inherited::net_Export(E);

    CSE_ALifeInventoryItem* itm = smart_cast<CSE_ALifeInventoryItem*>(E);
    itm->m_fCondition = m_fCondition;

    CSE_ALifeItemWeapon* wpn = smart_cast<CSE_ALifeItemWeapon*>(E);
    wpn->wpn_flags = IsUpdating() ? 1 : 0;
    wpn->a_elapsed = u16(iAmmoElapsed);
    wpn->m_addon_flags.flags = m_flagsAddOnState;
    wpn->ammo_type = (u8)m_ammoType;
    wpn->wpn_state = (u8)GetState();
    wpn->m_bZoom = (u8)m_bZoomMode;
    for (u8 slot = 0; slot < eCustomAddonCount; ++slot)
        wpn->m_custom_addon_indices[slot] = m_custom_addon_slots[slot].installed_index;
}

void CWeapon::save(NET_Packet& output_packet)
{
    inherited::save(output_packet);
    save_data(iAmmoElapsed, output_packet);
    constexpr u8 custom_addon_save_marker = 1 << 7;
    constexpr u8 extended_custom_addon_save_marker = 1 << 6;
    const u8 saved_addon_state = m_flagsAddOnState | custom_addon_save_marker | extended_custom_addon_save_marker;
    save_data(saved_addon_state, output_packet);
    save_data(m_ammoType, output_packet);
    constexpr u8 named_custom_addon_save_marker = 1 << 7;
    const u8 saved_zoom_state = (m_bZoomMode ? 1 : 0) | named_custom_addon_save_marker;
    save_data(saved_zoom_state, output_packet);
    for (const SCustomAddonSlot& slot : m_custom_addon_slots)
        save_data(slot.installed_index, output_packet);
}

void CWeapon::load(IReader& input_packet)
{
    inherited::load(input_packet);
    load_data(iAmmoElapsed, input_packet);
    constexpr u8 custom_addon_save_marker = 1 << 7;
    constexpr u8 extended_custom_addon_save_marker = 1 << 6;
    u8 saved_addon_state{};
    load_data(saved_addon_state, input_packet);
    const bool has_custom_addon_data = !!(saved_addon_state & custom_addon_save_marker);
    const bool has_extended_custom_addon_data = !!(saved_addon_state & extended_custom_addon_save_marker);
    m_flagsAddOnState = saved_addon_state & ~(custom_addon_save_marker | extended_custom_addon_save_marker);
    load_data(m_ammoType, input_packet);
    constexpr u8 named_custom_addon_save_marker = 1 << 7;
    u8 saved_zoom_state{};
    load_data(saved_zoom_state, input_packet);
    const bool has_named_custom_addon_data = !!(saved_zoom_state & named_custom_addon_save_marker);
    m_bZoomMode = !!(saved_zoom_state & 1);
    for (u8 slot_index = 0; slot_index < eCustomAddonCount; ++slot_index)
    {
        const bool has_slot_data = has_custom_addon_data &&
            (has_named_custom_addon_data || slot_index < 3 || (has_extended_custom_addon_data && slot_index < 4));
        if (!has_slot_data)
            continue; // Keep config defaults which older save formats could not serialize.

        u8 index{};
        load_data(index, input_packet);
        SCustomAddonSlot& slot = m_custom_addon_slots[slot_index];
        slot.installed_index = index <= slot.allowed.size() ? index : 0;
    }
    UpdateAddonsVisibility();
    InitAddons();

    if (m_bZoomMode)
        OnZoomIn();
    else
        OnZoomOut();
}

void CWeapon::OnEvent(NET_Packet& P, u16 type)
{
    switch (type)
    {
    case GE_WPN_STATE_CHANGE: {
        u8 state;
        P.r_u8(state);
        P.r_u8(m_sub_state);
        u8 NextAmmo = P.r_u8();
        if (NextAmmo == u8(-1))
            m_set_next_ammoType_on_reload = u32(-1);
        else
            m_set_next_ammoType_on_reload = u8(NextAmmo);

        OnStateSwitch(u32(state), GetState());
    }
    break;
    default: {
        inherited::OnEvent(P, type);
    }
    break;
    }
};

void CWeapon::shedule_Update(u32 dT)
{
    // Inherited
    inherited::shedule_Update(dT);
}

void CWeapon::OnH_B_Independent(bool just_before_destroy)
{
    RemoveShotEffector();

    inherited::OnH_B_Independent(just_before_destroy);

    //завершить принудительно все процессы что шли
    FireEnd();
    SetPending(FALSE);
    SwitchState(eIdle);

    m_strapped_mode = false;
    OnZoomOut();
    m_fZoomRotationFactor = 0.f;
    UpdateXForm();

    if (ParentIsActor())
        Actor()->set_safemode(false);
}

void CWeapon::OnH_A_Independent()
{
    inherited::OnH_A_Independent();
    Light_Destroy();
};

void CWeapon::OnH_A_Chield()
{
    inherited::OnH_A_Chield();

    UpdateAddonsVisibility();
};

void CWeapon::OnActiveItem()
{
    inherited::OnActiveItem();
    //если мы занружаемся и оружие было в руках
    SetState(eIdle);
    SetNextState(eIdle);
}

void CWeapon::OnHiddenItem()
{
    inherited::OnHiddenItem();
    SetState(eHidden);
    SetNextState(eHidden);
    m_set_next_ammoType_on_reload = u32(-1);
}

void CWeapon::OnStateSwitch(u32 S, u32 oldState)
{
    inherited::OnStateSwitch(S, oldState);

    if (CActor* actor = smart_cast<CActor*>(H_Parent()); actor && actor->inventory().ActiveItem() == this && S != eIdle)
        actor->set_safemode(false);
}

bool CWeapon::NeedBlendAnm()
{
    if (GetState() == eIdle)
    {
        if (const CActor* actor = smart_cast<const CActor*>(H_Parent()); actor && actor->is_safemode())
            return true;
    }

    /*if (IsZoomed() && psDeviceFlags2.test(rsAimSway))
        return true;*/

    return inherited::NeedBlendAnm();
}

void CWeapon::OnH_B_Chield()
{
    inherited::OnH_B_Chield();

    OnZoomOut();
    m_set_next_ammoType_on_reload = u32(-1);
}

void CWeapon::OnBeforeDrop()
{
    if (auto io = smart_cast<CActor*>(H_Parent()); io && this == io->inventory().ActiveItem())
        shader_exports.set_dof_params(0.f, 0.f, 0.f, 0.f);

    inherited::OnBeforeDrop();
}

u8 CWeapon::idle_state()
{
    auto* actor = smart_cast<CActor*>(H_Parent());

    if (actor)
    {
        u32 st = actor->get_state();
        if (st & mcSprint)
            return eSubstateIdleSprint;
        else if (st & mcAnyAction && !(st & mcJump) && !(st & mcFall))
            return eSubstateIdleMoving;
    }

    return eIdle;
}

float CWeapon::dof_zoom_effect{}, CWeapon::dof_reload_effect{};

void CWeapon::UpdateCL()
{
    inherited::UpdateCL();

    UpdateHUDAddonsVisibility();

    UpdateVisualBullets();

    //подсветка от выстрела
    UpdateLight();

    //нарисовать партиклы
    UpdateFlameParticles();
    UpdateFlameParticles2();

    VERIFY(smart_cast<IKinematics*>(Visual()));

    CInventoryItem* pActorItem{};
    auto pActor = smart_cast<CActor*>(H_Parent());
    if (pActor)
    {
        pActorItem = pActor->inventory().ActiveItem();
    }

    if (GetState() == eIdle)
    {
        if (pActor && pActorItem == this && IsPending() && !pActor->is_safemode())
            SetPending(FALSE);

        auto state = idle_state();
        if (m_idle_state != state)
        {
            m_idle_state = state;
            if (GetNextState() != eMagEmpty && GetNextState() != eReload)
            {
                SwitchState(eIdle);
            }
        }

        if (pActor && pActorItem == this)
        {
            if (psActorFlags.test(AF_DOF_ZOOM) && m_bZoomMode && dof_zoom_effect < 1.f && !UseScopeTexture() && pActor->active_cam() == ACTOR_DEFS::eacFirstEye)
                UpdateDof(dof_zoom_effect, Is3dssEnabled() ? dof_params_reload : dof_params_zoom, false);
            else if (dof_zoom_effect > 0.f && (!m_bZoomMode || pActor->active_cam() != ACTOR_DEFS::eacFirstEye))
                UpdateDof(dof_zoom_effect, Is3dssEnabled() ? dof_params_reload : dof_params_zoom, true);

            if (dof_reload_effect > 0.f)
                UpdateDof(dof_reload_effect, dof_params_reload, true);
        }
    }
    else if (GetState() == eReload)
    {
        if (pActor && pActorItem == this)
        {
            if (psActorFlags.test(AF_DOF_RELOAD) && dof_reload_effect < 1.f && pActor->active_cam() == ACTOR_DEFS::eacFirstEye)
                UpdateDof(dof_reload_effect, dof_params_reload, false);
            else if (dof_reload_effect > 0.f && pActor->active_cam() != ACTOR_DEFS::eacFirstEye)
                UpdateDof(dof_reload_effect, dof_params_reload, true);
        }

        m_idle_state = eIdle;
    }
    else
    {
        if (pActor && pActorItem == this)
        {
            if (dof_reload_effect > 0.f)
                UpdateDof(dof_reload_effect, dof_params_reload, true);

            if (dof_zoom_effect > 0.f && (!m_bZoomMode || pActor->active_cam() != ACTOR_DEFS::eacFirstEye))
                UpdateDof(dof_zoom_effect, Is3dssEnabled() ? dof_params_reload : dof_params_zoom, true);
        }

        m_idle_state = eIdle;
    }

    UpdateLaser();
    UpdateFlashlight();
}

void CWeapon::UpdateDof(float& type, const Fvector4& params_type, const bool desire)
{
    if (desire)
        type -= Device.fTimeDelta / dof_transition_time;
    else
        type += Device.fTimeDelta / dof_transition_time;

    shader_exports.set_dof_params(params_type.x * type, params_type.y * type, params_type.z * type, params_type.w * type);
    clamp(type, 0.f, 1.f);
}

void CWeapon::UpdateLaser()
{
    if (laser_light_render)
    {
        auto io = smart_cast<CInventoryOwner*>(H_Parent());
        if (!laser_light_render->get_active() && IsLaserOn() && (!H_Parent() || (io && this == io->inventory().ActiveItem())))
        {
            laser_light_render->set_active(true);
            UpdateAddonsVisibility();
        }
        else if (laser_light_render->get_active() && (!IsLaserOn() || !(!H_Parent() || (io && this == io->inventory().ActiveItem()))))
        {
            laser_light_render->set_active(false);
            UpdateAddonsVisibility();
        }

        if (laser_light_render->get_active())
        {
            laser_pos = get_LastFP();
            Fvector laser_dir = get_LastFD();

            if (GetHUDmode())
            {
                if (laserdot_attach_bone.size())
                {
                    GetBoneOffsetPosDir(laserdot_attach_bone, laser_pos, laser_dir, laserdot_attach_offset);
                    CorrectDirFromWorldToHud(laser_dir);
                }
            }
            else
            {
                XFORM().transform_tiny(laser_pos, laserdot_world_attach_offset);
            }

            Fmatrix laserXForm;
            laserXForm.identity();
            laserXForm.k.set(laser_dir);
            Fvector::generate_orthonormal_basis_normalized(laserXForm.k, laserXForm.j, laserXForm.i);

            laser_light_render->set_position(laser_pos);
            laser_light_render->set_rotation(laserXForm.k, laserXForm.i);

            // calc color animator
            if (laser_lanim)
            {
                int frame;
                const u32 clr = laser_lanim->CalculateBGR(Device.fTimeGlobal, frame);

                Fcolor fclr{(float)color_get_B(clr), (float)color_get_G(clr), (float)color_get_R(clr), 1.f};
                fclr.mul_rgb(laser_fBrightness / 255.f);
                laser_light_render->set_color(fclr);
            }
        }
    }
}

void CWeapon::UpdateFlashlight()
{
    if (flashlight_render)
    {
        auto io = smart_cast<CInventoryOwner*>(H_Parent());
        if (!flashlight_render->get_active() && IsFlashlightOn() && (!H_Parent() || (io && this == io->inventory().ActiveItem())))
        {
            flashlight_render->set_active(true);
            flashlight_omni->set_active(true);
            UpdateAddonsVisibility();
        }
        else if (flashlight_render->get_active() && (!IsFlashlightOn() || !(!H_Parent() || (io && this == io->inventory().ActiveItem()))))
        {
            flashlight_render->set_active(false);
            flashlight_omni->set_active(false);
            UpdateAddonsVisibility();
        }

        if (flashlight_render->get_active())
        {
            Fvector flashlight_pos_omni, flashlight_dir, flashlight_dir_omni;

            if (GetHUDmode())
            {
                GetBoneOffsetPosDir(flashlight_attach_bone, flashlight_pos, flashlight_dir, flashlight_attach_offset);
                CorrectDirFromWorldToHud(flashlight_dir);

                GetBoneOffsetPosDir(flashlight_attach_bone, flashlight_pos_omni, flashlight_dir_omni, flashlight_omni_attach_offset);
                CorrectDirFromWorldToHud(flashlight_dir_omni);
            }
            else
            {
                flashlight_dir = get_LastFD();
                XFORM().transform_tiny(flashlight_pos, flashlight_world_attach_offset);

                flashlight_dir_omni = get_LastFD();
                XFORM().transform_tiny(flashlight_pos_omni, flashlight_omni_world_attach_offset);
            }

            Fmatrix flashlightXForm;
            flashlightXForm.identity();
            flashlightXForm.k.set(flashlight_dir);
            Fvector::generate_orthonormal_basis_normalized(flashlightXForm.k, flashlightXForm.j, flashlightXForm.i);
            flashlight_render->set_position(flashlight_pos);
            flashlight_render->set_rotation(flashlightXForm.k, flashlightXForm.i);

            Fmatrix flashlightomniXForm;
            flashlightomniXForm.identity();
            flashlightomniXForm.k.set(flashlight_dir_omni);
            Fvector::generate_orthonormal_basis_normalized(flashlightomniXForm.k, flashlightomniXForm.j, flashlightomniXForm.i);
            flashlight_omni->set_position(flashlight_pos_omni);
            flashlight_omni->set_rotation(flashlightomniXForm.k, flashlightomniXForm.i);

            // calc color animator
            if (flashlight_lanim)
            {
                int frame;
                const u32 clr = flashlight_lanim->CalculateBGR(Device.fTimeGlobal, frame);

                Fcolor fclr{(float)color_get_B(clr), (float)color_get_G(clr), (float)color_get_R(clr), 1.f};
                fclr.mul_rgb(flashlight_fBrightness / 255.f);
                flashlight_render->set_color(fclr);
                flashlight_omni->set_color(fclr);
            }
        }
    }
}

void CWeapon::renderable_Render(u32 context_id, IRenderable* root)
{
    UpdateXForm();

    //нарисовать подсветку
    RenderLight();

    const bool hud_render = root && root->renderable_HUD() && GetHUDmode();
    bool render_world_weapon = !hud_render && (!H_Parent() || !IsHidden());
    if (!render_world_weapon && !hud_render && H_Parent())
    {
        const CInventoryOwner* owner = smart_cast<const CInventoryOwner*>(H_Parent());
        const CInventoryItem* self = smart_cast<const CInventoryItem*>(this);
        render_world_weapon = owner && owner->attached(self);
    }

    if (render_world_weapon)
    {
        Fmatrix world_transform = renderable_WorldTransform();
        ::Render->add_Visual(context_id, root, Visual(), world_transform);
        Visual()->getVisData().hom_frame = Device.dwFrame;
        RenderAddonVisuals(context_id, root, false);
    }
}

void CWeapon::renderable_RenderUI(u32 context_id, IRenderable* root)
{
    if (!Visual())
        return;

    Fmatrix world_transform = renderable_WorldTransform();
    ::Render->add_Visual(context_id, root, Visual(), world_transform);
    RenderAddonVisuals(context_id, root, false, true);
}

void CWeapon::render_hud_mode(u32 context_id, IRenderable* root)
{
    RenderLight();

    inherited::render_hud_mode(context_id, root);
    RenderAddonVisuals(context_id, root, true);
}

bool CWeapon::need_renderable() { return !(IsZoomed() && UseScopeTexture() && !IsRotatingToZoom()); }

void CWeapon::signal_HideComplete()
{
    if (H_Parent())
        setVisible(FALSE);
    SetPending(FALSE);
}

void CWeapon::SetDefaults()
{
    bWorking2 = false;
    SetPending(FALSE);

    m_flags.set(FUsingCondition, TRUE);
    m_flagsAddOnState = 0;
    m_bZoomMode = false;
}

void CWeapon::UpdatePosition(const Fmatrix& trans)
{
    Position().set(trans.c);
    XFORM().mul(trans, m_strapped_mode ? m_StrapOffset : m_Offset);
    VERIFY(!fis_zero(DET(renderable.xform)));
}

bool CWeapon::Action(s32 cmd, u32 flags)
{
    if (inherited::Action(cmd, flags))
        return true;

    CActor* actor = smart_cast<CActor*>(H_Parent());

    switch (cmd)
    {
    case kWPN_FIRE: {
        //если оружие чем-то занято, то ничего не делать
        {
            if (flags & CMD_START)
            {
                if (IsPending())
                    return false;
                if (actor && actor->is_safemode())
                {
                    actor->set_safemode(false);
                    return true;
                }
                FireStart();
            }
            else
                FireEnd();
        };
    }
        return true;
    case kWPN_NEXT: {
        if (IsPending())
        {
            return false;
        }

        if (psActorFlags.test(AF_LOCK_RELOAD) && ParentIsActor() && g_actor->get_state() & mcSprint)
            return true;

        if (flags & CMD_START)
        {
            u32 l_newType = m_ammoType;
            bool b1, b2;
            do
            {
                l_newType = (l_newType + 1) % m_ammoTypes.size();
                b1 = l_newType != m_ammoType;
                b2 = unlimited_ammo() ? false : (!m_pCurrentInventory->GetAmmo(*m_ammoTypes[l_newType], ParentIsActor()));
            } while (b1 && b2);

            if (l_newType != m_ammoType)
            {
                m_set_next_ammoType_on_reload = l_newType;

                Reload();
            }
        }
    }
        return true;

    case kWPN_ZOOM: {
        const u32 state = GetState();
        const bool bPending = IsPending();
        const bool zoom_out_request = IsZoomed() &&
            (!(flags & CMD_START) || psActorFlags.is(AF_WPN_AIM_TOGGLE));
        if (IsZoomEnabled() &&
            (zoom_out_request || state == eFire || state == eFire2 || state == eMagEmpty || state == eIdle || !bPending))
        {
            if (flags & CMD_START)
            {
                if (psActorFlags.is(AF_WPN_AIM_TOGGLE) && IsZoomed())
                {
                    OnZoomOut();
                    if (!bPending)
                        SwitchState(eIdle);
                }
                else
                {
                    if (actor && actor->is_safemode())
                        actor->set_safemode(false);
                    OnZoomIn();
                    if (!bPending)
                        SwitchState(eIdle);
                }
            }
            else if (IsZoomed() && !psActorFlags.is(AF_WPN_AIM_TOGGLE))
            {
                OnZoomOut();
                if (!bPending)
                    SwitchState(eIdle);
            }
            return true;
        }
        else
            return false;
    }

    case kWPN_ZOOM_INC:
    case kWPN_ZOOM_DEC: {
        if (IsZoomEnabled() && IsZoomed() && m_bScopeDynamicZoom && IsScopeFunctional() && (flags & CMD_START))
        {
            // если в режиме ПГ - не будем давать использовать динамический зум
            if (IsGrenadeMode())
                return false;

            ZoomChange(cmd == kWPN_ZOOM_INC);

            return true;
        }
        else
            return false;
    }
    case kSAFEMODE:
        if (actor && (flags & CMD_START) && !IsPending() && m_bCanBeLowered)
        {
            const bool lower = !actor->is_safemode();
            actor->set_safemode(lower);
            SetPending(TRUE);

            if (!lower && m_safemode_anm[1].name)
                PlayBlendAnm(m_safemode_anm[1].name, m_safemode_anm[1].speed, m_safemode_anm[1].power, false);
            else if (lower && m_safemode_anm[0].name)
                PlayBlendAnm(m_safemode_anm[0].name, m_safemode_anm[0].speed, m_safemode_anm[0].power, false);
        }
        return true;
    }
    return false;
}

void CWeapon::GetZoomData(const float scope_factor, float& delta, float& min_zoom_factor)
{
    float def_fov = Core.Features.test(xrCore::Feature::ogse_wpn_zoom_system) ? 1.f : g_fov;
    float delta_factor_total = def_fov - scope_factor;
    VERIFY(delta_factor_total > 0);
    min_zoom_factor = def_fov - delta_factor_total * m_fMinZoomK;
    delta = (delta_factor_total * (1 - m_fMinZoomK)) / m_fZoomStepCount;
}

void CWeapon::ZoomChange(bool inc)
{
    bool wasChanged = false;

    if (Is3dssEnabled())
    {
// Simp: переменную кратность в новых прицелах сделал на скриптах, мб в будущем верну в двиг, но пока так.
        return;
    }
    else
    {
        float delta, min_zoom_factor;
        GetZoomData(m_fScopeZoomFactor, delta, min_zoom_factor);

        const float currentZoomFactor = m_fZoomFactor;

        if (Core.Features.test(xrCore::Feature::ogse_wpn_zoom_system))
        {
            m_fZoomFactor += delta * (inc ? 1 : -1);
            clamp(m_fZoomFactor, min_zoom_factor, m_fScopeZoomFactor);
        }
        else
        {
            m_fZoomFactor -= delta * (inc ? 1 : -1);
            clamp(m_fZoomFactor, m_fScopeZoomFactor, min_zoom_factor);
        }

        wasChanged = !fsimilar(currentZoomFactor, m_fZoomFactor);

        if (H_Parent() && !IsRotatingToZoom())
            m_fRTZoomFactor = m_fZoomFactor; // store current
    }

    if (wasChanged)
    {
        OnZoomChanged();
    }
}

void CWeapon::SpawnAmmo(u32 boxCurr, LPCSTR ammoSect, u32 ParentID)
{
    if (!m_ammoTypes.size())
        return;

    if (!ammoSect)
        ammoSect = m_ammoTypes.front().c_str();

    CSE_Abstract* D = F_entity_Create(ammoSect);

    if (auto l_pA = smart_cast<CSE_ALifeItemAmmo*>(D))
    {
        l_pA->m_boxSize = (u16)pSettings->r_s32(ammoSect, "box_size");
        D->s_name = ammoSect;
        D->set_name_replace("");
        D->s_gameid = u8(GameID());
        D->s_RP = 0xff;
        D->ID = 0xffff;
        if (ParentID == 0xffffffff)
            D->ID_Parent = (u16)H_Parent()->ID();
        else
            D->ID_Parent = (u16)ParentID;

        D->ID_Phantom = 0xffff;
        D->s_flags.assign(M_SPAWN_OBJECT_LOCAL);
        D->RespawnTime = 0;
        l_pA->m_tNodeID = ai_location().level_vertex_id();

        if (boxCurr == 0xffffffff)
            boxCurr = l_pA->m_boxSize;

        while (boxCurr)
        {
            l_pA->a_elapsed = (u16)(boxCurr > l_pA->m_boxSize ? l_pA->m_boxSize : boxCurr);
            NET_Packet P;
            D->Spawn_Write(P, TRUE);
            Level().Send(P, net_flags(TRUE));

            if (boxCurr > l_pA->m_boxSize)
                boxCurr -= l_pA->m_boxSize;
            else
                boxCurr = 0;
        }
    };
    F_entity_Destroy(D);
}

int CWeapon::GetAmmoCurrent(bool use_item_to_spawn) const
{
    int l_count = iAmmoElapsed;
    if (!m_pCurrentInventory)
        return l_count;

    //чтоб не делать лишних пересчетов
    if (m_pCurrentInventory->ModifyFrame() <= m_dwAmmoCurrentCalcFrame)
        return l_count + iAmmoCurrent;

    m_dwAmmoCurrentCalcFrame = Device.dwFrame;
    iAmmoCurrent = 0;

    for (int i = 0; i < (int)m_ammoTypes.size(); ++i)
    {
        iAmmoCurrent += GetAmmoCount_forType(m_ammoTypes[i]);

        if (!use_item_to_spawn)
            continue;

        if (!inventory_owner().item_to_spawn())
            continue;

        iAmmoCurrent += inventory_owner().ammo_in_box_to_spawn();
    }
    return l_count + iAmmoCurrent;
}

int CWeapon::GetAmmoCount(u8 ammo_type, u32 max) const
{
    VERIFY(m_pInventory);
    R_ASSERT(ammo_type < m_ammoTypes.size());

    return GetAmmoCount_forType(m_ammoTypes[ammo_type], max);
}

int CWeapon::GetAmmoCount_forType(shared_str const& ammo_type, u32 max) const
{
    u32 res = 0;
    auto callback = [&](const auto pIItem) -> bool {
        auto* ammo = smart_cast<CWeaponAmmo*>(pIItem);
        if (ammo->cNameSect() == ammo_type)
            res += ammo->m_boxCurr;
        return (max > 0 && res >= max);
    };

    m_pCurrentInventory->IterateAmmo(false, callback);
    if (max == 0 || res < max)
        if (!smart_cast<const CActor*>(H_Parent()) || !psActorFlags.test(AF_AMMO_ON_BELT))
            m_pCurrentInventory->IterateAmmo(true, callback);

    return res;
}

float CWeapon::GetConditionMisfireProbability() const
{
    if (GetCondition() > 0.95f)
        return 0.0f;

    float mis = misfireProbability + powf(1.f - GetCondition(), 3.f) * misfireConditionK;
    clamp(mis, 0.0f, 0.99f);
    return mis;
}

BOOL CWeapon::CheckForMisfire()
{
    if (!smart_cast<CActor*>(H_Parent())) // KRodin: НПС не нужны осечки.
        return FALSE;

    float rnd = ::Random.randF(0.f, 1.f);
    float mp = GetConditionMisfireProbability();
    if (rnd < mp)
    {
        FireEnd();
        SwitchMisfire(true);
        return TRUE;
    }
    else
        return FALSE;
}

void CWeapon::Reload() {}

void CWeapon::DeviceSwitch() {}

bool CWeapon::IsGrenadeLauncherAttached() const
{
    return (CSE_ALifeItemWeapon::eAddonAttachable == m_eGrenadeLauncherStatus && 0 != (m_flagsAddOnState & CSE_ALifeItemWeapon::eWeaponAddonGrenadeLauncher)) ||
        CSE_ALifeItemWeapon::eAddonPermanent == m_eGrenadeLauncherStatus;
}

bool CWeapon::IsScopeAttached() const
{
    return (CSE_ALifeItemWeapon::eAddonAttachable == m_eScopeStatus && 0 != (m_flagsAddOnState & CSE_ALifeItemWeapon::eWeaponAddonScope)) ||
        CSE_ALifeItemWeapon::eAddonPermanent == m_eScopeStatus;
}

bool CWeapon::IsSilencerAttached() const
{
    return (CSE_ALifeItemWeapon::eAddonAttachable == m_eSilencerStatus && 0 != (m_flagsAddOnState & CSE_ALifeItemWeapon::eWeaponAddonSilencer)) ||
        CSE_ALifeItemWeapon::eAddonPermanent == m_eSilencerStatus;
}

bool CWeapon::GrenadeLauncherAttachable() const { return (CSE_ALifeItemWeapon::eAddonAttachable == m_eGrenadeLauncherStatus); }
bool CWeapon::ScopeAttachable() const { return (CSE_ALifeItemWeapon::eAddonAttachable == m_eScopeStatus); }
bool CWeapon::SilencerAttachable() const { return (CSE_ALifeItemWeapon::eAddonAttachable == m_eSilencerStatus); }

void CWeapon::UpdateHUDAddonsVisibility()
{
    if (!GetHUDmode())
        return;

    if (ScopeAttachable())
        HudItemData()->set_bone_visible(m_sHud_wpn_scope_bones, IsScopeAttached());

    if (m_eScopeStatus == ALife::eAddonDisabled)
        HudItemData()->set_bone_visible(m_sHud_wpn_scope_bones, FALSE, TRUE);
    else if (m_eScopeStatus == ALife::eAddonPermanent)
        HudItemData()->set_bone_visible(m_sHud_wpn_scope_bones, TRUE, TRUE);

    if (SilencerAttachable())
        HudItemData()->set_bone_visible(m_sHud_wpn_silencer_bone, IsSilencerAttached());

    if (m_eSilencerStatus == ALife::eAddonDisabled)
        HudItemData()->set_bone_visible(m_sHud_wpn_silencer_bone, FALSE, TRUE);
    else if (m_eSilencerStatus == ALife::eAddonPermanent)
        HudItemData()->set_bone_visible(m_sHud_wpn_silencer_bone, TRUE, TRUE);

    if (!HudItemData()->has_bone(m_sHud_wpn_launcher_bone) && HudItemData()->has_bone(wpn_launcher_def_bone_cop))
        m_sHud_wpn_launcher_bone = wpn_launcher_def_bone_cop;

    if (GrenadeLauncherAttachable())
        HudItemData()->set_bone_visible(m_sHud_wpn_launcher_bone, IsGrenadeLauncherAttached());

    if (m_eGrenadeLauncherStatus == ALife::eAddonDisabled)
        HudItemData()->set_bone_visible(m_sHud_wpn_launcher_bone, FALSE, TRUE);
    else if (m_eGrenadeLauncherStatus == ALife::eAddonPermanent)
        HudItemData()->set_bone_visible(m_sHud_wpn_launcher_bone, TRUE, TRUE);

    if (m_sHud_wpn_laser_bone.size() && has_laser)
        HudItemData()->set_bone_visible(m_sHud_wpn_laser_bone, IsLaserOn(), TRUE);

    if (m_sHud_wpn_flashlight_bone.size() && has_flashlight)
        HudItemData()->set_bone_visible(m_sHud_wpn_flashlight_bone, IsFlashlightOn(), TRUE);

    for (const shared_str& bone_name : hud_hidden_bones)
        HudItemData()->set_bone_visible(bone_name, FALSE, TRUE);

    UpdateAddonReplacementVisibility(true);
    UpdateEmptyBonesVisibility();
    callback(GameObject::eOnUpdateHUDAddonsVisibiility)();
}

void CWeapon::UpdateEmptyBonesVisibility()
{
    const bool visible = iAmmoElapsed > 0;

    if (IKinematics* world_model = smart_cast<IKinematics*>(Visual()))
    {
        for (const shared_str& bone_name : empty_hide_bones)
        {
            const u16 bone_id = find_addon_bone(world_model, bone_name.c_str());
            if (bone_id != BI_NONE)
                world_model->LL_SetBoneVisible(bone_id, visible, TRUE);
        }
    }

    if (GetHUDmode() && HudItemData() && HudItemData()->m_model)
    {
        IKinematics* hud_model = HudItemData()->m_model;
        for (const shared_str& bone_name : hud_empty_hide_bones)
        {
            const u16 bone_id = find_addon_bone(hud_model, bone_name.c_str());
            if (bone_id != BI_NONE)
                hud_model->LL_SetBoneVisible(bone_id, visible, TRUE);
        }
    }
}

void CWeapon::UpdateAddonsVisibility()
{
    if (g_player_hud)
        g_player_hud->clear_addon_hand_pose_sources(this);

    auto pWeaponVisual = smart_cast<IKinematics*>(Visual());
    VERIFY(pWeaponVisual);

    UpdateHUDAddonsVisibility();

    ///////////////////////////////////////////////////////////////////
    u16 bone_id{};

    for (const auto& sbone : m_sWpn_scope_bones)
    {
        bone_id = pWeaponVisual->LL_BoneID(sbone);

        if (bone_id != BI_NONE)
        {
            if (ScopeAttachable())
            {
                if (IsScopeAttached())
                {
                    if (!pWeaponVisual->LL_GetBoneVisible(bone_id))
                        pWeaponVisual->LL_SetBoneVisible(bone_id, TRUE, TRUE);
                }
                else
                {
                    if (pWeaponVisual->LL_GetBoneVisible(bone_id))
                        pWeaponVisual->LL_SetBoneVisible(bone_id, FALSE, TRUE);
                }
            }

            if (m_eScopeStatus == CSE_ALifeItemWeapon::eAddonDisabled && bone_id != BI_NONE && pWeaponVisual->LL_GetBoneVisible(bone_id))
                pWeaponVisual->LL_SetBoneVisible(bone_id, FALSE, TRUE);
            else if (m_eScopeStatus == CSE_ALifeItemWeapon::eAddonPermanent && bone_id != BI_NONE && !pWeaponVisual->LL_GetBoneVisible(bone_id))
                pWeaponVisual->LL_SetBoneVisible(bone_id, TRUE, TRUE);
        }
    }
    ///////////////////////////////////////////////////////////////////

    bone_id = pWeaponVisual->LL_BoneID(m_sWpn_silencer_bone);

    if (bone_id != BI_NONE)
    {
        if (SilencerAttachable())
        {
            if (IsSilencerAttached())
            {
                if (!pWeaponVisual->LL_GetBoneVisible(bone_id))
                    pWeaponVisual->LL_SetBoneVisible(bone_id, TRUE, TRUE);
            }
            else
            {
                if (pWeaponVisual->LL_GetBoneVisible(bone_id))
                    pWeaponVisual->LL_SetBoneVisible(bone_id, FALSE, TRUE);
            }
        }
    }

    if (m_eSilencerStatus == CSE_ALifeItemWeapon::eAddonDisabled && bone_id != BI_NONE && pWeaponVisual->LL_GetBoneVisible(bone_id))
        pWeaponVisual->LL_SetBoneVisible(bone_id, FALSE, TRUE);
    else if (m_eSilencerStatus == CSE_ALifeItemWeapon::eAddonPermanent && bone_id != BI_NONE && !pWeaponVisual->LL_GetBoneVisible(bone_id))
        pWeaponVisual->LL_SetBoneVisible(bone_id, TRUE, TRUE);

    ///////////////////////////////////////////////////////////////////

    bone_id = pWeaponVisual->LL_BoneID(m_sWpn_launcher_bone);

    if (bone_id != BI_NONE)
    {
        if (GrenadeLauncherAttachable())
        {
            if (IsGrenadeLauncherAttached())
            {
                if (!pWeaponVisual->LL_GetBoneVisible(bone_id))
                    pWeaponVisual->LL_SetBoneVisible(bone_id, TRUE, TRUE);
            }
            else
            {
                if (pWeaponVisual->LL_GetBoneVisible(bone_id))
                    pWeaponVisual->LL_SetBoneVisible(bone_id, FALSE, TRUE);
            }
        }
    }

    if (m_eGrenadeLauncherStatus == CSE_ALifeItemWeapon::eAddonDisabled && bone_id != BI_NONE && pWeaponVisual->LL_GetBoneVisible(bone_id))
        pWeaponVisual->LL_SetBoneVisible(bone_id, FALSE, TRUE);
    else if (m_eGrenadeLauncherStatus == CSE_ALifeItemWeapon::eAddonPermanent && bone_id != BI_NONE && !pWeaponVisual->LL_GetBoneVisible(bone_id))
        pWeaponVisual->LL_SetBoneVisible(bone_id, TRUE, TRUE);

    ///////////////////////////////////////////////////////////////////

    if (m_sWpn_laser_bone.size() && has_laser)
    {
        bone_id = pWeaponVisual->LL_BoneID(m_sWpn_laser_bone);

        if (bone_id != BI_NONE)
        {
            const bool laser_on = IsLaserOn();
            if (pWeaponVisual->LL_GetBoneVisible(bone_id) && !laser_on)
                pWeaponVisual->LL_SetBoneVisible(bone_id, FALSE, TRUE);
            else if (!pWeaponVisual->LL_GetBoneVisible(bone_id) && laser_on)
                pWeaponVisual->LL_SetBoneVisible(bone_id, TRUE, TRUE);
        }
    }

    ///////////////////////////////////////////////////////////////////

    if (m_sWpn_flashlight_bone.size() && has_flashlight)
    {
        bone_id = pWeaponVisual->LL_BoneID(m_sWpn_flashlight_bone);

        if (bone_id != BI_NONE)
        {
            const bool flashlight_on = IsFlashlightOn();
            if (pWeaponVisual->LL_GetBoneVisible(bone_id) && !flashlight_on)
                pWeaponVisual->LL_SetBoneVisible(bone_id, FALSE, TRUE);
            else if (!pWeaponVisual->LL_GetBoneVisible(bone_id) && flashlight_on)
                pWeaponVisual->LL_SetBoneVisible(bone_id, TRUE, TRUE);
        }
    }

    ///////////////////////////////////////////////////////////////////

    for (const auto& bone_name : hidden_bones)
    {
        bone_id = pWeaponVisual->LL_BoneID(bone_name);
        if (bone_id != BI_NONE && pWeaponVisual->LL_GetBoneVisible(bone_id))
            pWeaponVisual->LL_SetBoneVisible(bone_id, FALSE, TRUE);
    }

    ///////////////////////////////////////////////////////////////////

    UpdateAddonReplacementVisibility(false);
    UpdateEmptyBonesVisibility();
    callback(GameObject::eOnUpdateAddonsVisibiility)();

    pWeaponVisual->CalculateBones_Invalidate();
    pWeaponVisual->CalculateBones();
}

bool CWeapon::Activate(bool now)
{
    VisMask new_mask;
    new_mask.set_all();

    const auto K = smart_cast<IKinematics*>(Visual());
    K->LL_SetBonesVisible(new_mask);

    UpdateAddonsVisibility();
    return inherited::Activate(now);
}

void CWeapon::InitAddons() {}

float CWeapon::CurrentZoomFactor()
{
    if (Is3dssEnabled())
        return Core.Features.test(xrCore::Feature::ogse_wpn_zoom_system) ? 1.0f : m_fIronSightZoomFactor; // no change to main fov zoom when use second vp
    else if (IsScopeFunctional())
        return m_fScopeZoomFactor;
    else
        return m_fIronSightZoomFactor;
}

void CWeapon::OnZoomIn()
{
    m_bZoomMode = true;

    // если в режиме ПГ - не будем давать включать динамический зум
    if (m_bScopeDynamicZoom && !IsGrenadeMode() && !Is3dssEnabled())
        m_fZoomFactor = m_fRTZoomFactor;
    else
        m_fZoomFactor = CurrentZoomFactor();

    if (IsScopeFunctional() && !IsGrenadeMode())
    {
        if (!m_bScopeZoomInertionAllow)
            AllowHudInertion(FALSE);
    }
    else if (!m_bZoomInertionAllow)
        AllowHudInertion(FALSE);

    if (smart_cast<CActor*>(H_Parent()))
        g_actor->callback(GameObject::eOnActorWeaponZoomIn)(lua_game_object());

    g_player_hud->updateMovementLayerState();
}

void CWeapon::OnZoomOut()
{
    m_fZoomFactor = Core.Features.test(xrCore::Feature::ogse_wpn_zoom_system) ? 1.f : g_fov;

    if (m_bZoomMode)
    {
        SprintType = false;
        m_bZoomMode = false;

        if (smart_cast<CActor*>(H_Parent()))
        {
            g_actor->callback(GameObject::eOnActorWeaponZoomOut)(lua_game_object());
        }
    }

    AllowHudInertion(TRUE);

    ResetSubStateTime();

    g_player_hud->updateMovementLayerState();
}

bool CWeapon::UseScopeTexture()
{
    return !Is3dssEnabled() && m_UIScope; // только если есть текстура прицела - для простого создания коллиматоров
}

void CWeapon::SwitchState(u32 S)
{
    SetNextState(S); // Very-very important line of code!!! :)
    if (CHudItem::object().Local() && !CHudItem::object().getDestroy() /* && (S!=NEXT_STATE)*/
        && m_pCurrentInventory)
    {
        // !!! Just single entry for given state !!!
        NET_Packet P;
        CHudItem::object().u_EventGen(P, GE_WPN_STATE_CHANGE, CHudItem::object().ID());
        P.w_u8(u8(S));
        P.w_u8(u8(m_sub_state));
        P.w_u8(u8(m_set_next_ammoType_on_reload & 0xff));
        CHudItem::object().u_EventSend(P, net_flags(TRUE, TRUE, FALSE, TRUE));
    }
}

void CWeapon::OnMagazineEmpty() { VERIFY((u32)iAmmoElapsed == m_magazine.size()); }

void CWeapon::reinit()
{
    CShootingObject::reinit();
    CHudItemObject::reinit();
}

void CWeapon::reload(LPCSTR section)
{
    CShootingObject::reload(section);
    CHudItemObject::reload(section);

    m_can_be_strapped = true;
    m_strapped_mode = false;

    if (pSettings->line_exist(section, "strap_bone0"))
        m_strap_bone0 = pSettings->r_string(section, "strap_bone0");
    else
        m_can_be_strapped = false;

    if (pSettings->line_exist(section, "strap_bone1"))
        m_strap_bone1 = pSettings->r_string(section, "strap_bone1");
    else
        m_can_be_strapped = false;

    if (m_eScopeStatus == ALife::eAddonAttachable)
    {
        m_addon_holder_range_modifier = READ_IF_EXISTS(pSettings, r_float, m_sScopeName, "holder_range_modifier", m_holder_range_modifier);
        m_addon_holder_fov_modifier = READ_IF_EXISTS(pSettings, r_float, m_sScopeName, "holder_fov_modifier", m_holder_fov_modifier);
    }
    else
    {
        m_addon_holder_range_modifier = m_holder_range_modifier;
        m_addon_holder_fov_modifier = m_holder_fov_modifier;
    }

    {
        Fvector pos, ypr;
        pos = pSettings->r_fvector3(section, "position");
        ypr = pSettings->r_fvector3(section, "orientation");
        ypr.mul(PI / 180.f);

        m_Offset.setHPB(ypr.x, ypr.y, ypr.z);
        m_Offset.translate_over(pos);
    }

    m_StrapOffset = m_Offset;
    if (pSettings->line_exist(section, "strap_position") && pSettings->line_exist(section, "strap_orientation"))
    {
        Fvector pos, ypr;
        pos = pSettings->r_fvector3(section, "strap_position");
        ypr = pSettings->r_fvector3(section, "strap_orientation");
        ypr.mul(PI / 180.f);

        m_StrapOffset.setHPB(ypr.x, ypr.y, ypr.z);
        m_StrapOffset.translate_over(pos);
    }
    else
        m_can_be_strapped = false;

    m_ef_main_weapon_type = READ_IF_EXISTS(pSettings, r_u32, section, "ef_main_weapon_type", u32(-1));
    m_ef_weapon_type = READ_IF_EXISTS(pSettings, r_u32, section, "ef_weapon_type", u32(-1));
}

void CWeapon::create_physic_shell()
{
    if (PHForceAutoGeneratedCollision())
    {
        CPHShellSimpleCreator::CreatePhysicsShell();
        return;
    }

    // xrKrodin: Временный? "фикс" для оружия из ганслингера, валяющегося на земле. По непонятным причинам (много костей или хз от чего ещё) в некоторых случаях при рассчетах
    // физики происходят краши в ode которые исправить невозможно.
    if (is_gunslinger_weapon || IS_OGSR_GA)
    {
        auto vis = smart_cast<IKinematics*>(Visual());
        auto& rootBoneData = vis->LL_GetData(vis->LL_GetBoneRoot());
        m_pPhysicsShell = P_build_SimpleShell(this, rootBoneData.mass, false);
        m_pPhysicsShell->SetMaterial(rootBoneData.game_mtl_idx);
    }
    else
        CPhysicsShellHolder::create_physic_shell();
}

void CWeapon::activate_physic_shell() { CPhysicsShellHolder::activate_physic_shell(); }

void CWeapon::setup_physic_shell() { CPhysicsShellHolder::setup_physic_shell(); }

bool CWeapon::can_kill() const
{
    if (GetAmmoCurrent(true) || m_ammoTypes.empty())
        return (true);

    return (false);
}

CInventoryItem* CWeapon::can_kill(CInventory* inventory) const
{
    if (GetAmmoElapsed() || m_ammoTypes.empty())
        return (const_cast<CWeapon*>(this));

    TIItemContainer::iterator I = inventory->m_all.begin();
    TIItemContainer::iterator E = inventory->m_all.end();
    for (; I != E; ++I)
    {
        CInventoryItem* inventory_item = smart_cast<CInventoryItem*>(*I);
        if (!inventory_item)
            continue;

        xr_vector<shared_str>::const_iterator i = std::find(m_ammoTypes.begin(), m_ammoTypes.end(), inventory_item->object().cNameSect());
        if (i != m_ammoTypes.end())
            return (inventory_item);
    }

    return (0);
}

const CInventoryItem* CWeapon::can_kill(const xr_vector<const CGameObject*>& items) const
{
    if (m_ammoTypes.empty())
        return (this);

    xr_vector<const CGameObject*>::const_iterator I = items.begin();
    xr_vector<const CGameObject*>::const_iterator E = items.end();
    for (; I != E; ++I)
    {
        const CInventoryItem* inventory_item = smart_cast<const CInventoryItem*>(*I);
        if (!inventory_item)
            continue;

        xr_vector<shared_str>::const_iterator i = std::find(m_ammoTypes.begin(), m_ammoTypes.end(), inventory_item->object().cNameSect());
        if (i != m_ammoTypes.end())
            return (inventory_item);
    }

    return (0);
}

bool CWeapon::ready_to_kill() const { return (!IsMisfire() && ((GetState() == eIdle) || (GetState() == eFire) || (GetState() == eFire2)) && GetAmmoElapsed()); }

// Получить индекс текущих координат худа
u8 CWeapon::GetCurrentHudOffsetIdx() const
{
    if (const CActor* actor = smart_cast<const CActor*>(H_Parent()); actor && m_bCanBeLowered && actor->is_safemode())
        return hud_item_measures::m_hands_offset_type_lowered;

    if (IsZoomed())
    {
        const bool has_gl = GrenadeLauncherAttachable() && IsGrenadeLauncherAttached();
        const bool has_scope = IsScopeFunctional();
        //const bool has_aim_alt = AimAlt && is_second_zoom_offset_enabled;

        if (IsGrenadeMode())
        {
            if (m_bUseScopeGrenadeZoom && has_scope)
                return hud_item_measures::m_hands_offset_type_gl_scope;
            else
                return hud_item_measures::m_hands_offset_type_gl;
        }
        else if (has_gl)
        {
            if (m_bUseScopeZoom && has_scope)
                return hud_item_measures::m_hands_offset_type_gl_normal_scope;
            else
                return hud_item_measures::m_hands_offset_type_aim_gl_normal;
        }
        else
        {
            if (m_bUseScopeZoom && has_scope)
                return hud_item_measures::m_hands_offset_type_aim_scope;
            //else if (has_aim_alt)
            //    return hud_item_measures::m_hands_offset_type_alt_aim;
            else
                return hud_item_measures::m_hands_offset_type_aim;
        }
    }

    return hud_item_measures::m_hands_offset_type_normal;
}

void CWeapon::SetAmmoElapsed(int ammo_count)
{
    iAmmoElapsed = ammo_count;

    u32 uAmmo = u32(iAmmoElapsed);

    if (uAmmo != m_magazine.size())
    {
        if (uAmmo > m_magazine.size())
        {
            CCartridge l_cartridge;
            l_cartridge.Load(*m_ammoTypes[m_ammoType], u8(m_ammoType));
            while (uAmmo > m_magazine.size())
                m_magazine.push_back(l_cartridge);
        }
        else
        {
            while (uAmmo < m_magazine.size())
                m_magazine.pop_back();
        };
    };

    UpdateEmptyBonesVisibility();
}

u32 CWeapon::ef_main_weapon_type() const
{
    VERIFY(m_ef_main_weapon_type != u32(-1));
    return (m_ef_main_weapon_type);
}

u32 CWeapon::ef_weapon_type() const
{
    VERIFY(m_ef_weapon_type != u32(-1));
    return (m_ef_weapon_type);
}

bool CWeapon::IsNecessaryItem(const shared_str& item_sect) { return (std::find(m_ammoTypes.begin(), m_ammoTypes.end(), item_sect) != m_ammoTypes.end()); }

void CWeapon::modify_holder_params(float& range, float& fov) const
{
    if (!IsScopeFunctional())
    {
        inherited::modify_holder_params(range, fov);
        return;
    }
    range *= m_addon_holder_range_modifier;
    fov *= m_addon_holder_fov_modifier;
}

void CWeapon::OnDrawUI()
{
    if (IsZoomed() && ZoomHideCrosshair())
    {
        if (UseScopeTexture() && !IsRotatingToZoom())
        {
            m_UIScope->Update();
            m_UIScope->DrawWithAnimation();
        }
    }
}

bool CWeapon::IsHudModeNow() { return (HudItemData() != nullptr); }

bool CWeapon::unlimited_ammo()
{
    if (m_pCurrentInventory)
        return inventory_owner().unlimited_ammo() && m_DefaultCartridge.m_flags.test(CCartridge::cfCanBeUnlimited);
    else
        return false;
};

LPCSTR CWeapon::GetCurrentAmmo_ShortName()
{
    if (m_magazine.empty())
        return ("");
    CCartridge& l_cartridge = m_magazine.back();
    return *(l_cartridge.m_InvShortName);
}

float CWeapon::GetMagazineWeight(const decltype(CWeapon::m_magazine)& mag) const
{
    float res = 0;
    const char* last_type = nullptr;
    float last_ammo_weight = 0;
    for (auto& c : mag)
    {
        // Usually ammos in mag have same type, use this fact to improve performance
        if (last_type != c.m_ammoSect.c_str())
        {
            last_type = c.m_ammoSect.c_str();
            last_ammo_weight = c.Weight();
        }
        res += last_ammo_weight;
    }
    return res;
}

float CWeapon::Weight() const
{
    float res = CInventoryItemObject::Weight();
    if (GrenadeLauncherAttachable() && IsGrenadeLauncherAttached())
        res += pSettings->r_float(GetGrenadeLauncherName(), "inv_weight");
    if (ScopeAttachable() && IsScopeAttached())
        res += pSettings->r_float(GetScopeName(), "inv_weight");
    if (SilencerAttachable() && IsSilencerAttached())
        res += pSettings->r_float(GetSilencerName(), "inv_weight");
    for (u8 slot = 0; slot < eCustomAddonCount; ++slot)
    {
        const shared_str& addon = GetCustomAddonSection(static_cast<ECustomAddonSlot>(slot));
        if (addon.c_str())
            res += READ_IF_EXISTS(pSettings, r_float, addon, "inv_weight", 0.f);
    }
    res += GetMagazineWeight(m_magazine);

    return res;
}

u32 CWeapon::Cost() const
{
    u32 res = m_cost;

    if (Core.Features.test(xrCore::Feature::wpn_cost_include_addons))
    {
        if (GrenadeLauncherAttachable() && IsGrenadeLauncherAttached())
            res += pSettings->r_u32(GetGrenadeLauncherName(), "cost");
        if (ScopeAttachable() && IsScopeAttached())
            res += pSettings->r_u32(GetScopeName(), "cost");
        if (SilencerAttachable() && IsSilencerAttached())
            res += pSettings->r_u32(GetSilencerName(), "cost");
        for (u8 slot = 0; slot < eCustomAddonCount; ++slot)
        {
            const shared_str& addon = GetCustomAddonSection(static_cast<ECustomAddonSlot>(slot));
            if (addon.c_str())
                res += READ_IF_EXISTS(pSettings, r_u32, addon, "cost", 0u);
        }
    }
    return res;
}

void CWeapon::Hide(bool now)
{
    if (now)
    {
        OnStateSwitch(eHidden, GetState());
        SetState(eHidden);
        StopHUDSounds();
    }
    else
        SwitchState(eHiding);

    OnZoomOut();
}

void CWeapon::Show(bool now)
{
    if (now)
    {
        StopCurrentAnimWithoutCallback();
        OnStateSwitch(eIdle, GetState());
        SetState(eIdle);
        StopHUDSounds();
    }
    else
        SwitchState(eShowing);
}

bool CWeapon::show_crosshair() { return psActorFlags.test(AF_CROSSHAIR_DBG) || !(IsZoomed() && ZoomHideCrosshair()); }

bool CWeapon::show_indicators() { return !(IsZoomed() && (UseScopeTexture() || !m_bScopeShowIndicators)); }

float CWeapon::GetConditionToShow() const
{
    return (GetCondition()); // powf(GetCondition(),4.0f));
}

bool CWeapon::ParentIsActor() const
{
    return smart_cast<const CActor*>(H_Parent()) != nullptr;
}

const float& CWeapon::hit_probability() const
{
    VERIFY((g_SingleGameDifficulty >= egdNovice) && (g_SingleGameDifficulty <= egdMaster));
#pragma todo("WTF???")
    return (m_hit_probability[egdNovice]);
}

bool CWeapon::Is3dssEnabled() const
{
    const auto& zoom_params = shader_exports.get_custom_params("s3ds_param_2");
    return !fis_zero(zoom_params.w) && !IsGrenadeMode() && psActorFlags.test(AF_3D_SCOPES);
}

// Чувствительность мышки с оружием в руках во время прицеливания
float CWeapon::GetControlInertionFactor() const
{
    const float fInertionFactor = inherited::GetControlInertionFactor();

    if (IsZoomed() && Is3dssEnabled() && !IsRotatingToZoom())
    {
        if (m_bScopeDynamicZoom)
        {
            const auto& zoom_params = shader_exports.get_custom_params("s3ds_param_2");
            const float& max_zoom = zoom_params.y;
            const float& current_zoom = zoom_params.w;
            const float res = fInertionFactor + ((m_fScopeInertionFactor - fInertionFactor) * (current_zoom / max_zoom));
            // Msg("--[%s] current inertion: [%g], fInertionFactor: [%g], m_fScopeInertionFactor: [%g], current zoom: [%g], max_zoom: [%g]", __FUNCTION__, res, fInertionFactor,
            // m_fScopeInertionFactor, current_zoom, max_zoom);
            return res;
        }
        else
            return m_fScopeInertionFactor;
    }

    return fInertionFactor;
}

float CWeapon::GetHudFov()
{
    const float last_nw_hf = inherited::GetHudFov();

    if (m_fZoomRotationFactor > 0.0f)
    {
        if (Is3dssEnabled() && m_f3dssHudFov > 0.0f)
        {
            // В линзе зума
            const float fDiff = last_nw_hf - m_f3dssHudFov;
            return m_f3dssHudFov + (fDiff * (1 - m_fZoomRotationFactor));
        }
        if ((m_eScopeStatus == CSE_ALifeItemWeapon::eAddonDisabled || IsScopeFunctional()) && !IsGrenadeMode() && m_fZoomHudFov > 0.0f)
        {
            // В процессе зума
            const float fDiff = last_nw_hf - m_fZoomHudFov;
            return m_fZoomHudFov + (fDiff * (1 - m_fZoomRotationFactor));
        }
    }

    return last_nw_hf;
}

void CWeapon::OnBulletHit()
{
    if (!fis_zero(conditionDecreasePerShotOnHit))
        ChangeCondition(-conditionDecreasePerShotOnHit);
}

// По ef_weapon_type тут проверяем, пулемёт ли это. Это костыль чтоб при смене типа патронов не играла анимация reload_empty, которая выглядит в данном случае неправильно.
bool CWeapon::IsPartlyReloading() const { return (ef_weapon_type() == 10 || m_set_next_ammoType_on_reload == u32(-1)) && GetAmmoElapsed() > 0 && !IsMisfire(); }

void CWeapon::SaveAttachableParams()
{
    const char* sect_name = cNameSect().c_str();
    string_path buff;
    FS.update_path(buff, "$logs$", make_string("_world\\%s.ltx", sect_name).c_str());

    CInifile pHudCfg(buff, FALSE, FALSE, TRUE);

    sprintf_s(buff, "%f,%f,%f", m_Offset.c.x, m_Offset.c.y, m_Offset.c.z);
    pHudCfg.w_string(sect_name, "position", buff);

    Fvector ypr;
    m_Offset.getHPB(ypr.x, ypr.y, ypr.z);
    ypr.mul(180.f / PI);
    sprintf_s(buff, "%f,%f,%f", ypr.x, ypr.y, ypr.z);
    pHudCfg.w_string(sect_name, "orientation", buff);

    if (pSettings->line_exist(sect_name, "strap_position") && pSettings->line_exist(sect_name, "strap_orientation"))
    {
        sprintf_s(buff, "%f,%f,%f", m_StrapOffset.c.x, m_StrapOffset.c.y, m_StrapOffset.c.z);
        pHudCfg.w_string(sect_name, "strap_position", buff);
        m_StrapOffset.getHPB(ypr.x, ypr.y, ypr.z);
        ypr.mul(180.f / PI);
        sprintf_s(buff, "%f,%f,%f", ypr.x, ypr.y, ypr.z);
        pHudCfg.w_string(sect_name, "strap_orientation", buff);
    }

    Msg("--[%s] data saved to [%s]", __FUNCTION__, pHudCfg.fname());
}

void CWeapon::UpdateVisualBullets()
{
    if (!bullet_update)
        return;

    if (bHasBulletsToHide)
    {
        const int AE = GetAmmoElapsed();

        last_hide_bullet = AE >= bullet_cnt ? bullet_cnt : (AE == 0 ? -1 : bullet_cnt - AE - 1);
    }

    HUD_VisualBulletUpdate();
}

void CWeapon::HUD_VisualBulletUpdate(bool force, int force_idx)
{
    if (!GetHUDmode())
        return;

    if (!bHasBulletsToHide)
        return;

    bool hide = true;

    // Msg("Print %d bullets", last_hide_bullet);

    if (last_hide_bullet == bullet_cnt || force)
        hide = false;

    for (auto b = 0; b < bullet_cnt; b++)
    {
        const auto bone_id = HudItemData()->m_model->LL_BoneID(bullets_bones[b]);

        if (bone_id != BI_NONE)
            HudItemData()->set_bone_visible(bullets_bones[b], !hide);

        if (b == last_hide_bullet)
            hide = false;
    }
}

void CWeapon::ParseCurrentItem(CGameFont* F) { F->OutNext("WEAPON IN STRAPPED MODE: [%d]", m_strapped_mode); }

void CWeapon::update_visual_bullet_textures(const bool forced)
{
    if (IsGrenadeMode())
        return;

    if (bullet_textures_in_model.empty())
        return;

    if (!GetHUDmode())
        return;

    const u32 id = m_set_next_ammoType_on_reload != u32(-1) ? m_set_next_ammoType_on_reload : m_ammoType;
    const auto& current_ammo_sect = m_ammoTypes[id];
    const auto bullet_texrure_find_it = bullet_textures_for_ammos.find(current_ammo_sect);
    ASSERT_FMT(bullet_texrure_find_it != bullet_textures_for_ammos.end(), "!!Can't find [%s] in [bullet_textures_for_ammos] of [%s]", current_ammo_sect.c_str(),
               cNameSect().c_str());
    const auto& bullet_texrure_name = bullet_texrure_find_it->second;

    if (!forced && current_bullet_texture == bullet_texrure_name)
        return;

    for (const auto& tex_name : bullet_textures_in_model)
    {
        const auto textures = Device.m_pRender->GetResourceManager()->FindTexture(tex_name.c_str());
        if (textures.empty())
        {
            Msg("!![%s] can't find texture [%s] for [%s]", __FUNCTION__, tex_name.c_str(), cNameSect().c_str());
            continue;
        }

        auto* tex = textures.front();
        tex->Unload();
        tex->Load(bullet_texrure_name.c_str());
        current_bullet_texture = bullet_texrure_name;
        //Msg("--[%s] replaced texture [%s] --> [%s] for [%s]", __FUNCTION__, tex_name.c_str(), current_bullet_texture.c_str(), cNameSect().c_str());
    }
}

void CWeapon::on_a_hud_attach()
{
    inherited::on_a_hud_attach();

    VisMask new_mask;
    new_mask.set_all();

    IKinematics* K = HudItemData()->m_model;
    K->LL_SetBonesVisible(new_mask);
}

void CWeapon::on_b_hud_detach()
{
    if (g_player_hud)
        g_player_hud->clear_addon_hand_pose_sources(this);
    inherited::on_b_hud_detach();
}
