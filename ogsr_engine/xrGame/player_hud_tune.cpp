#include "stdafx.h"
#include "player_hud.h"
#include "level.h"
#include "debug_renderer.h"
#include "../xr_3da/xr_input.h"
#include "../xr_3da/XR_IOConsole.h"
#include "../xr_3da/xr_ioc_cmd.h"
#include "HudManager.h"
#include "HudItem.h"
#include "Weapon.h"
#include <array>
#include <unordered_map>
#include <unordered_set>

enum HUD_ADJUST_MODE : int
{
    OFF,
    HUD_POS,
    HUD_ROT,
    ITM_POS,
    ITM_ROT,
    FIRE_POINT,
    FIRE_POINT2,
    SHELL_POINT,
    ADJUST_DELTA_POS,
    ADJUST_DELTA_ROT,
    LASETDOT_POS,
    FLASHLIGHT_POS,
    _HUD_ADJUST_MODES_COUNT_,

    // Separate modes used by bone_adjust_mode. They intentionally live outside
    // the hud_adjust_mode console range so the old command keeps its behaviour.
    BONE_POS = _HUD_ADJUST_MODES_COUNT_,
    BONE_ROT,
    BONE_ADJUST_DELTA_POS,
    BONE_ADJUST_DELTA_ROT,
};

static constexpr std::array<std::tuple<int, const char*>, _HUD_ADJUST_MODES_COUNT_> ADJUST_MODES_DB{{
    {DIK_NUMPAD0, ""},
    {DIK_NUMPAD1, "adjusting HUD POSITION"},
    {DIK_NUMPAD2, "adjusting HUD ROTATION"},
    {DIK_NUMPAD3, "adjusting ITEM POSITION"},
    {DIK_NUMPAD4, "adjusting ITEM ROTATION"},
    {DIK_NUMPAD5, "adjusting FIRE POINT"},
    {DIK_NUMPAD6, "adjusting FIRE POINT 2"},
    {DIK_NUMPAD7, "adjusting SHELL POINT"},
    {DIK_NUMPAD8, "adjusting pos STEP"},
    {DIK_NUMPAD9, "adjusting rot STEP"},
    {DIK_1, "adjusting LASER POINT"},
    {DIK_2, "adjusting FLASHLIGHT POINT"},
}};

int g_bHudAdjustMode = OFF;
int g_bHudAdjustItemIdx = 0;
float g_bHudAdjustDeltaPos = 0.0005f;
float g_bHudAdjustDeltaRot = 0.05f;

namespace
{
constexpr LPCSTR BONE_POSITION_PREFIX = "bone_position_";
constexpr LPCSTR BONE_ROTATION_PREFIX = "bone_rotation_";

struct hud_bone_adjustment
{
    Fvector position{};
    Fvector rotation{}; // degrees in config/editor, just like HUD orientation values
};

struct hud_bone_adjustment_section
{
    bool loaded{};
    string_unordered_map<shared_str, hud_bone_adjustment> bones;
};

struct bone_adjust_callback_context
{
    BoneCallback original_callback{};
    void* original_param{};
    shared_str section;
    shared_str bone_name;
    u16 target_idx{};
};

string_unordered_map<shared_str, hud_bone_adjustment_section> g_hud_bone_adjustments;
std::unordered_map<CBoneInstance*, bone_adjust_callback_context> g_bone_adjust_callbacks;
IKinematics* g_bone_adjust_models[2]{};
shared_str g_bone_adjust_bone;

bool is_bone_adjust_mode()
{
    return g_bHudAdjustMode == BONE_POS || g_bHudAdjustMode == BONE_ROT ||
        g_bHudAdjustMode == BONE_ADJUST_DELTA_POS || g_bHudAdjustMode == BONE_ADJUST_DELTA_ROT;
}

xr_string normalize_bone_name(LPCSTR bone_name)
{
    xr_string result = bone_name ? bone_name : "";

    while (!result.empty() && isspace(static_cast<unsigned char>(result.front())))
        result.erase(result.begin());
    while (!result.empty() && isspace(static_cast<unsigned char>(result.back())))
        result.pop_back();

    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return result;
}

hud_bone_adjustment_section& bone_adjust_section(const shared_str& section)
{
    auto& data = g_hud_bone_adjustments[section];
    if (data.loaded)
        return data;

    data.loaded = true;
    if (!pSettings->section_exist(section))
        return data;

    const size_t position_prefix_length = xr_strlen(BONE_POSITION_PREFIX);
    const size_t rotation_prefix_length = xr_strlen(BONE_ROTATION_PREFIX);

    for (const auto& [key, value] : pSettings->r_section(section).Data)
    {
        LPCSTR key_name = key.c_str();
        LPCSTR bone_name = nullptr;
        bool is_position = false;

        if (!strncmp(key_name, BONE_POSITION_PREFIX, position_prefix_length))
        {
            bone_name = key_name + position_prefix_length;
            is_position = true;
        }
        else if (!strncmp(key_name, BONE_ROTATION_PREFIX, rotation_prefix_length))
        {
            bone_name = key_name + rotation_prefix_length;
        }
        else
            continue;

        const xr_string normalized = normalize_bone_name(bone_name);
        if (normalized.empty())
            continue;

        auto& adjustment = data.bones[shared_str(normalized.c_str())];
        if (is_position)
            adjustment.position = pSettings->r_fvector3(section, key_name);
        else
            adjustment.rotation = pSettings->r_fvector3(section, key_name);
    }

    return data;
}

hud_bone_adjustment* find_bone_adjustment(const shared_str& section, LPCSTR bone_name)
{
    auto& data = bone_adjust_section(section);
    const xr_string normalized = normalize_bone_name(bone_name);
    if (normalized.empty())
        return nullptr;

    const auto it = data.bones.find(shared_str(normalized.c_str()));
    return it != data.bones.end() ? &it->second : nullptr;
}

hud_bone_adjustment& edit_bone_adjustment(const shared_str& section, LPCSTR bone_name)
{
    auto& data = bone_adjust_section(section);
    const xr_string normalized = normalize_bone_name(bone_name);
    return data.bones[shared_str(normalized.c_str())];
}

attachable_hud_item* bone_adjust_item_for_index(u16 idx)
{
    if (!g_player_hud)
        return nullptr;

    // Bone adjustment is a generic HUD-skeleton feature. It must not depend on
    // Source skeleton merge or on any particular naming convention.
    if (auto item = g_player_hud->attached_item(idx))
        return item;

    const u16 other_idx = idx ? 0 : 1;
    return g_player_hud->attached_item(other_idx);
}

u16 find_bone_id_ci(IKinematics* model, LPCSTR bone_name)
{
    if (!model || !bone_name || !bone_name[0])
        return BI_NONE;

    const u16 direct = model->LL_BoneID(bone_name);
    if (direct != BI_NONE)
        return direct;

    const u16 bone_count = model->LL_BoneCount();
    for (u16 bone_id = 0; bone_id < bone_count; ++bone_id)
    {
        LPCSTR candidate = model->LL_BoneName(bone_id);
        if (candidate && !_stricmp(candidate, bone_name))
            return bone_id;
    }

    return BI_NONE;
}

void append_model_bone_tips(IKinematics* model, IConsole_Command::vecTips& tips)
{
    if (!model)
        return;

    const u16 bone_count = model->LL_BoneCount();
    for (u16 bone_id = 0; bone_id < bone_count; ++bone_id)
    {
        LPCSTR bone_name = model->LL_BoneName(bone_id);
        if (!bone_name || !bone_name[0])
            continue;

        const xr_string normalized = normalize_bone_name(bone_name);
        bool duplicate = false;
        for (const shared_str& tip : tips)
        {
            if (!_stricmp(tip.c_str(), normalized.c_str()))
            {
                duplicate = true;
                break;
            }
        }

        if (!duplicate)
            tips.emplace_back(normalized.c_str());
    }
}

void BoneAdjustCallback(CBoneInstance* bone)
{
    const auto it = g_bone_adjust_callbacks.find(bone);
    if (it == g_bone_adjust_callbacks.end())
        return;

    // If this bone is also driven by Source merge/IK/another custom callback,
    // let it establish the animated pose first, then apply the user correction.
    const BoneCallback original = it->second.original_callback;
    if (original && original != BoneAdjustCallback)
        original(bone);

    hud_bone_adjustment* adjustment = find_bone_adjustment(it->second.section, it->second.bone_name.c_str());
    if (!adjustment)
        return;

    Fvector rotation_radians = adjustment->rotation;
    rotation_radians.mul(PI / 180.f);

    Fmatrix correction;
    correction.identity();
    correction.rotateX(rotation_radians.x);

    Fmatrix axis_rotation;
    axis_rotation.identity();
    axis_rotation.rotateY(rotation_radians.y);
    correction.mulA_43(axis_rotation);

    axis_rotation.identity();
    axis_rotation.rotateZ(rotation_radians.z);
    correction.mulA_43(axis_rotation);

    correction.translate_over(adjustment->position);
    bone->mTransform.mulB_43(correction);
}

void refresh_bone_adjust_callbacks()
{
    IKinematics* models[] = {
        g_player_hud ? g_player_hud->Model() : nullptr,
        g_player_hud ? g_player_hud->Model2() : nullptr,
    };

    // A HUD hands visual can be destroyed/recreated in-place. Never touch old
    // CBoneInstance pointers after the owning model changed.
    if (models[0] != g_bone_adjust_models[0] || models[1] != g_bone_adjust_models[1])
    {
        g_bone_adjust_callbacks.clear();
        g_bone_adjust_models[0] = models[0];
        g_bone_adjust_models[1] = models[1];
    }

    std::unordered_set<CBoneInstance*> desired;

    for (u16 target_idx = 0; target_idx < 2; ++target_idx)
    {
        IKinematics* model = models[target_idx];
        attachable_hud_item* item = bone_adjust_item_for_index(target_idx);
        if (!model || !item)
            continue;

        auto& section_data = bone_adjust_section(item->m_sect_name);
        for (const auto& [bone_name, adjustment] : section_data.bones)
        {
            const u16 bone_id = find_bone_id_ci(model, bone_name.c_str());
            if (bone_id == BI_NONE)
                continue;

            CBoneInstance& bone = model->LL_GetBoneInstance(bone_id);
            desired.insert(&bone);

            auto context_it = g_bone_adjust_callbacks.find(&bone);
            if (bone.callback() == BoneAdjustCallback && context_it != g_bone_adjust_callbacks.end())
            {
                context_it->second.section = item->m_sect_name;
                context_it->second.bone_name = bone_name;
                context_it->second.target_idx = target_idx;
                continue;
            }

            // refresh_source_skeleton_merge may have overwritten our wrapper.
            // Capture the fresh callback and wrap it again. A bone without an
            // existing callback is valid too: bone_adjust_mode must work on all
            // bones, not only ValveBiped/Source-merge bones.
            if (context_it != g_bone_adjust_callbacks.end())
                g_bone_adjust_callbacks.erase(context_it);

            const BoneCallback original = bone.callback();

            bone_adjust_callback_context context;
            context.original_callback = original == BoneAdjustCallback ? nullptr : original;
            context.original_param = original == BoneAdjustCallback ? nullptr : bone.callback_param();
            context.section = item->m_sect_name;
            context.bone_name = bone_name;
            context.target_idx = target_idx;
            g_bone_adjust_callbacks.emplace(&bone, context);

            bone.set_callback(bctCustom, BoneAdjustCallback, context.original_param, TRUE);
        }
    }

    for (auto it = g_bone_adjust_callbacks.begin(); it != g_bone_adjust_callbacks.end();)
    {
        if (desired.find(it->first) != desired.end())
        {
            ++it;
            continue;
        }

        CBoneInstance* bone = it->first;
        const bone_adjust_callback_context context = it->second;
        if (bone->callback() == BoneAdjustCallback)
        {
            if (bone_adjust_item_for_index(context.target_idx) && context.original_callback)
                bone->set_callback(bctCustom, context.original_callback, context.original_param, TRUE);
            else
                bone->reset_callback();
        }

        it = g_bone_adjust_callbacks.erase(it);
    }
}

void print_bone_adjustment(const shared_str& section, LPCSTR bone_name, const hud_bone_adjustment& adjustment)
{
    Log("####################################");
    Msg("[%s]", section.c_str());
    Msg("bone_position_%s = %g,%g,%g", bone_name, adjustment.position.x, adjustment.position.y, adjustment.position.z);
    Msg("bone_rotation_%s = %g,%g,%g", bone_name, adjustment.rotation.x, adjustment.rotation.y, adjustment.rotation.z);
    Log("####################################");
}

} // namespace

bool set_bone_adjust_mode(LPCSTR bone_name)
{
    const xr_string normalized = normalize_bone_name(bone_name);
    if (normalized.empty())
    {
        Msg("! usage: bone_adjust_mode <bone_name|off>");
        return false;
    }

    if (normalized == "off" || normalized == "0")
    {
        g_bHudAdjustMode = OFF;
        g_bone_adjust_bone = nullptr;
        Msg("bone adjust mode: OFF");
        return true;
    }

    if (!g_player_hud)
    {
        Msg("! bone_adjust_mode: player HUD is not available");
        return false;
    }

    u16 item_idx = static_cast<u16>(g_bHudAdjustItemIdx);
    attachable_hud_item* item = bone_adjust_item_for_index(item_idx);
    if (!item)
    {
        if (auto item0 = bone_adjust_item_for_index(0))
        {
            item_idx = 0;
            item = item0;
        }
        else if (auto item1 = bone_adjust_item_for_index(1))
        {
            item_idx = 1;
            item = item1;
        }
    }

    if (!item)
    {
        Msg("! bone_adjust_mode: no HUD item is attached");
        return false;
    }

    const u16 bone0 = find_bone_id_ci(g_player_hud->Model(), normalized.c_str());
    const u16 bone1 = find_bone_id_ci(g_player_hud->Model2(), normalized.c_str());
    if (bone0 == BI_NONE && bone1 == BI_NONE)
    {
        Msg("! bone_adjust_mode: bone [%s] not found in HUD hands skeleton", normalized.c_str());
        return false;
    }

    LPCSTR resolved_bone_name = normalized.c_str();
    if (bone0 != BI_NONE && g_player_hud->Model())
        resolved_bone_name = g_player_hud->Model()->LL_BoneName(bone0);
    else if (bone1 != BI_NONE && g_player_hud->Model2())
        resolved_bone_name = g_player_hud->Model2()->LL_BoneName(bone1);

    g_bHudAdjustItemIdx = item_idx;
    g_bone_adjust_bone = resolved_bone_name;
    edit_bone_adjustment(item->m_sect_name, g_bone_adjust_bone.c_str());
    g_bHudAdjustMode = BONE_POS;
    refresh_bone_adjust_callbacks();

    Msg("bone adjust mode: [%s] for [%s]", g_bone_adjust_bone.c_str(), item->m_sect_name.c_str());
    Msg("SHIFT+0 exit | SHIFT+1 position | SHIFT+2 rotation | SHIFT+8 pos step | SHIFT+9 rot step");
    return true;
}

namespace
{
class CCC_BoneAdjustMode final : public IConsole_Command
{
public:
    CCC_BoneAdjustMode(LPCSTR name) : IConsole_Command(name) { bEmptyArgsHandled = TRUE; }

    void Execute(LPCSTR args) override { set_bone_adjust_mode(args); }

    void Info(TInfo& info) override
    {
        strcpy_s(info, "adjust any HUD hands bone: bone_adjust_mode <bone_name|off>");
    }

    void fill_tips(vecTips& tips, u32 mode) override
    {
        add_LRU_to_tips(tips);

        bool has_off = false;
        for (const shared_str& tip : tips)
        {
            if (!_stricmp(tip.c_str(), "off"))
            {
                has_off = true;
                break;
            }
        }
        if (!has_off)
            tips.emplace_back("off");

        if (!g_player_hud)
            return;

        // Enumerate the actual loaded skeletons. ValveBiped, Base Human and
        // custom rigs are all handled identically and appear automatically.
        append_model_bone_tips(g_player_hud->Model(), tips);
        append_model_bone_tips(g_player_hud->Model2(), tips);
    }
};

void ensure_bone_adjust_command_registered()
{
    static CCC_BoneAdjustMode command("bone_adjust_mode");
    static bool registered{};
    if (!registered && Console)
    {
        Console->AddCommand(&command);
        registered = true;
    }
}
} // namespace

static bool is_attachable_item_tuning_mode()
{
    return pInput->iGetAsyncKeyState(DIK_LSHIFT) || pInput->iGetAsyncKeyState(DIK_Z) || pInput->iGetAsyncKeyState(DIK_X) || pInput->iGetAsyncKeyState(DIK_C);
}

static void tune_remap(const Ivector& in_values, Ivector& out_values)
{
    if (pInput->iGetAsyncKeyState(DIK_LSHIFT))
    {
        out_values = in_values;
    }
    else if (pInput->iGetAsyncKeyState(DIK_Z))
    { // strict by X
        out_values.x = in_values.y;
        out_values.y = 0;
        out_values.z = 0;
    }
    else if (pInput->iGetAsyncKeyState(DIK_X))
    { // strict by Y
        out_values.x = 0;
        out_values.y = in_values.y;
        out_values.z = 0;
    }
    else if (pInput->iGetAsyncKeyState(DIK_C))
    { // strict by Z
        out_values.x = 0;
        out_values.y = 0;
        out_values.z = in_values.y;
    }
    else
    {
        out_values.set(0, 0, 0);
    }
}

static void calc_cam_diff_pos(const Fmatrix& item_transform, const Fvector& diff, Fvector& res)
{
    Fmatrix cam_m;
    cam_m.i.set(Device.vCameraRight);
    cam_m.j.set(Device.vCameraTop);
    cam_m.k.set(Device.vCameraDirection);
    cam_m.c.set(Device.vCameraPosition);

    Fvector res1;
    cam_m.transform_dir(res1, diff);

    Fmatrix item_transform_i;
    item_transform_i.invert(item_transform);
    item_transform_i.transform_dir(res, res1);
}

void attachable_hud_item::tune(const Ivector& values)
{
    if (!is_attachable_item_tuning_mode())
        return;

    Fvector diff{};

    if (g_bHudAdjustMode == ITM_POS || g_bHudAdjustMode == ITM_ROT)
    {
        if (g_bHudAdjustMode == ITM_POS)
        {
            if (values.x)
                diff.x = (values.x > 0) ? g_bHudAdjustDeltaPos : -g_bHudAdjustDeltaPos;
            if (values.y)
                diff.y = (values.y > 0) ? g_bHudAdjustDeltaPos : -g_bHudAdjustDeltaPos;
            if (values.z)
                diff.z = (values.z < 0) ? g_bHudAdjustDeltaPos : -g_bHudAdjustDeltaPos;

            Fvector d;
            Fmatrix ancor_m;
            m_parent->calc_transform(m_attach_place_idx, Fidentity, ancor_m);
            calc_cam_diff_pos(ancor_m, diff, d);
            m_measures.m_item_attach[0].add(d);
        }
        else if (g_bHudAdjustMode == ITM_ROT)
        {
            if (values.x)
                diff.x = (values.x > 0) ? g_bHudAdjustDeltaRot : -g_bHudAdjustDeltaRot;
            if (values.y)
                diff.y = (values.y > 0) ? g_bHudAdjustDeltaRot : -g_bHudAdjustDeltaRot;
            if (values.z)
                diff.z = (values.z > 0) ? g_bHudAdjustDeltaRot : -g_bHudAdjustDeltaRot;

            Fvector d;
            Fmatrix ancor_m;
            m_parent->calc_transform(m_attach_place_idx, Fidentity, ancor_m);

            calc_cam_diff_pos(m_item_transform, diff, d);
            m_measures.m_item_attach[1].add(d);
        }

        if ((values.x) || (values.y) || (values.z))
        {
            Log("####################################");
            Msg("[%s]", m_sect_name.c_str());
            Msg("item_position = %g,%g,%g", m_measures.m_item_attach[0].x, m_measures.m_item_attach[0].y, m_measures.m_item_attach[0].z);
            Msg("item_orientation = %g,%g,%g", m_measures.m_item_attach[1].x, m_measures.m_item_attach[1].y, m_measures.m_item_attach[1].z);
            Log("####################################");
        }
    }

    if (g_bHudAdjustMode == FIRE_POINT || g_bHudAdjustMode == FIRE_POINT2 || g_bHudAdjustMode == SHELL_POINT || g_bHudAdjustMode == LASETDOT_POS ||
        g_bHudAdjustMode == FLASHLIGHT_POS)
    {
        if (values.x)
            diff.x = (values.x > 0) ? g_bHudAdjustDeltaPos : -g_bHudAdjustDeltaPos;
        if (values.y)
            diff.y = (values.y > 0) ? g_bHudAdjustDeltaPos : -g_bHudAdjustDeltaPos;
        if (values.z)
            diff.z = (values.z > 0) ? g_bHudAdjustDeltaPos : -g_bHudAdjustDeltaPos;

        if (g_bHudAdjustMode == FIRE_POINT)
            m_measures.m_fire_point_offset.add(diff);
        else if (g_bHudAdjustMode == FIRE_POINT2)
            m_measures.m_fire_point2_offset.add(diff);
        else if (g_bHudAdjustMode == SHELL_POINT)
            m_measures.m_shell_point_offset.add(diff);
        else if (g_bHudAdjustMode == LASETDOT_POS)
        {
            if (auto Wpn = smart_cast<CWeapon*>(m_parent_hud_item))
                Wpn->laserdot_attach_offset.add(diff);
        }
        else if (g_bHudAdjustMode == FLASHLIGHT_POS)
        {
            if (auto Wpn = smart_cast<CWeapon*>(m_parent_hud_item))
                Wpn->flashlight_attach_offset.add(diff);
        }

        if ((values.x) || (values.y) || (values.z))
        {
            Log("####################################");
            Msg("[%s]", m_sect_name.c_str());
            Msg("fire_point = %g,%g,%g", m_measures.m_fire_point_offset.x, m_measures.m_fire_point_offset.y, m_measures.m_fire_point_offset.z);
            Msg("fire_point2 = %g,%g,%g", m_measures.m_fire_point2_offset.x, m_measures.m_fire_point2_offset.y, m_measures.m_fire_point2_offset.z);
            Msg("shell_point = %g,%g,%g", m_measures.m_shell_point_offset.x, m_measures.m_shell_point_offset.y, m_measures.m_shell_point_offset.z);
            if (auto Wpn = smart_cast<CWeapon*>(m_parent_hud_item))
            {
                Msg("laserdot_attach_offset = %g,%g,%g", Wpn->laserdot_attach_offset.x, Wpn->laserdot_attach_offset.y, Wpn->laserdot_attach_offset.z);
                Msg("torch_attach_offset = %g,%g,%g", Wpn->flashlight_attach_offset.x, Wpn->flashlight_attach_offset.y, Wpn->flashlight_attach_offset.z);
            }
            Log("####################################");
        }
    }
}

void attachable_hud_item::debug_draw_firedeps()
{
    const bool bForce = g_bHudAdjustMode == ITM_POS || g_bHudAdjustMode == ITM_ROT;

    if (g_bHudAdjustMode == FIRE_POINT || g_bHudAdjustMode == FIRE_POINT2 || g_bHudAdjustMode == SHELL_POINT || g_bHudAdjustMode == LASETDOT_POS ||
        g_bHudAdjustMode == FLASHLIGHT_POS || bForce)
    {
        auto& render = Level().debug_renderer();

        firedeps fd;
        setup_firedeps(fd);

        if (g_bHudAdjustMode == FIRE_POINT || bForce)
        {
            render.draw_aabb(fd.vLastFP, 0.01f, 0.01f, 0.01f, D3DCOLOR_XRGB(255, 0, 0), true);
            render.draw_aabb(fd.vLastShootPoint, 0.01f, 0.01f, 0.01f, D3DCOLOR_XRGB(5, 107, 0), true);
        }
        else if (g_bHudAdjustMode == FIRE_POINT2)
        {
            render.draw_aabb(fd.vLastFP2, 0.01f, 0.01f, 0.01f, D3DCOLOR_XRGB(0, 0, 255), true);
        }
        else if (g_bHudAdjustMode == SHELL_POINT)
        {
            render.draw_aabb(fd.vLastSP, 0.01f, 0.01f, 0.01f, D3DCOLOR_XRGB(0, 255, 0), true);
        }
        else if (g_bHudAdjustMode == LASETDOT_POS)
        {
            if (auto Wpn = smart_cast<CWeapon*>(m_parent_hud_item))
                render.draw_aabb(Wpn->laser_pos, 0.01f, 0.01f, 0.01f, D3DCOLOR_XRGB(125, 0, 0));
        }
        else if (g_bHudAdjustMode == FLASHLIGHT_POS)
        {
            if (auto Wpn = smart_cast<CWeapon*>(m_parent_hud_item))
                render.draw_aabb(Wpn->flashlight_pos, 0.01f, 0.01f, 0.01f, D3DCOLOR_XRGB(0, 56, 125));
        }
    }
}

void player_hud::tune(const Ivector& _values)
{
    Ivector values;
    tune_remap(_values, values);

    if (is_bone_adjust_mode())
    {
        if (g_bHudAdjustMode == BONE_ADJUST_DELTA_POS || g_bHudAdjustMode == BONE_ADJUST_DELTA_ROT)
        {
            if (g_bHudAdjustMode == BONE_ADJUST_DELTA_POS && values.z)
                g_bHudAdjustDeltaPos += (values.z > 0) ? 0.001f : -0.001f;

            if (g_bHudAdjustMode == BONE_ADJUST_DELTA_ROT && values.z)
                g_bHudAdjustDeltaRot += (values.z > 0) ? 0.1f : -0.1f;
            return;
        }

        attachable_hud_item* item = bone_adjust_item_for_index(static_cast<u16>(g_bHudAdjustItemIdx));
        if (!item || !g_bone_adjust_bone.c_str())
            return;

        hud_bone_adjustment& adjustment = edit_bone_adjustment(item->m_sect_name, g_bone_adjust_bone.c_str());
        Fvector diff{};

        if (g_bHudAdjustMode == BONE_POS)
        {
            if (values.x)
                diff.x = (values.x > 0) ? g_bHudAdjustDeltaPos : -g_bHudAdjustDeltaPos;
            if (values.y)
                diff.y = (values.y < 0) ? g_bHudAdjustDeltaPos : -g_bHudAdjustDeltaPos;
            if (values.z)
                diff.z = (values.z < 0) ? g_bHudAdjustDeltaPos : -g_bHudAdjustDeltaPos;
            adjustment.position.add(diff);
        }
        else if (g_bHudAdjustMode == BONE_ROT)
        {
            if (values.x)
                diff.x = (values.x > 0) ? g_bHudAdjustDeltaRot : -g_bHudAdjustDeltaRot;
            if (values.y)
                diff.y = (values.y > 0) ? g_bHudAdjustDeltaRot : -g_bHudAdjustDeltaRot;
            if (values.z)
                diff.z = (values.z > 0) ? g_bHudAdjustDeltaRot : -g_bHudAdjustDeltaRot;
            adjustment.rotation.add(diff);
        }

        if (values.x || values.y || values.z)
            print_bone_adjustment(item->m_sect_name, g_bone_adjust_bone.c_str(), adjustment);
        return;
    }

    const bool is_16x9 = UI()->is_widescreen();

    if (g_bHudAdjustMode == HUD_POS || g_bHudAdjustMode == HUD_ROT)
    {
        Fvector diff{};

        float _curr_dr = g_bHudAdjustDeltaRot;

        if (!m_attached_items[g_bHudAdjustItemIdx])
            return;

        const u8 idx = m_attached_items[g_bHudAdjustItemIdx]->m_parent_hud_item->GetCurrentHudOffsetIdx();
        if (idx)
            _curr_dr /= 20.0f;

        Fvector& pos_ = (idx != 0) ? m_attached_items[g_bHudAdjustItemIdx]->hands_offset_pos() : m_attached_items[g_bHudAdjustItemIdx]->hands_attach_pos();
        Fvector& rot_ = (idx != 0) ? m_attached_items[g_bHudAdjustItemIdx]->hands_offset_rot() : m_attached_items[g_bHudAdjustItemIdx]->hands_attach_rot();

        if (g_bHudAdjustMode == HUD_POS)
        {
            if (values.x)
                diff.x = (values.x > 0) ? g_bHudAdjustDeltaPos : -g_bHudAdjustDeltaPos;
            if (values.y)
                diff.y = (values.y < 0) ? g_bHudAdjustDeltaPos : -g_bHudAdjustDeltaPos;
            if (values.z)
                diff.z = (values.z < 0) ? g_bHudAdjustDeltaPos : -g_bHudAdjustDeltaPos;

            pos_.add(diff);
        }
        else if (g_bHudAdjustMode == HUD_ROT)
        {
            if (values.x)
                diff.y = (values.x > 0) ? _curr_dr : -_curr_dr;
            if (values.y)
                diff.x = (values.y > 0) ? _curr_dr : -_curr_dr;
            if (values.z)
                diff.z = (values.z > 0) ? _curr_dr : -_curr_dr;

            rot_.add(diff);
        }

        if ((values.x) || (values.y) || (values.z))
        {
            if (idx == hud_item_measures::m_hands_offset_type_normal)
            {
                Log("####################################");
                Msg("[%s]", m_attached_items[g_bHudAdjustItemIdx]->m_sect_name.c_str());
                Msg("hands_position%s = %g,%g,%g", is_16x9 ? "_16x9" : "", pos_.x, pos_.y, pos_.z);
                Msg("hands_orientation%s = %g,%g,%g", is_16x9 ? "_16x9" : "", rot_.x, rot_.y, rot_.z);
                Log("####################################");
            }
            else if (idx == hud_item_measures::m_hands_offset_type_aim)
            {
                Log("####################################");
                Msg("[%s]", m_attached_items[g_bHudAdjustItemIdx]->m_sect_name.c_str());
                Msg("aim_hud_offset_pos%s = %g,%g,%g", is_16x9 ? "_16x9" : "", pos_.x, pos_.y, pos_.z);
                Msg("aim_hud_offset_rot%s = %g,%g,%g", is_16x9 ? "_16x9" : "", rot_.x, rot_.y, rot_.z);
                Log("####################################");
            }
            else if (idx == hud_item_measures::m_hands_offset_type_gl)
            {
                Log("####################################");
                Msg("[%s]", m_attached_items[g_bHudAdjustItemIdx]->m_sect_name.c_str());
                Msg("gl_hud_offset_pos%s = %g,%g,%g", is_16x9 ? "_16x9" : "", pos_.x, pos_.y, pos_.z);
                Msg("gl_hud_offset_rot%s\t = %g,%g,%g", is_16x9 ? "_16x9" : "", rot_.x, rot_.y, rot_.z);
                Log("####################################");
            }
            else if (idx == hud_item_measures::m_hands_offset_type_aim_scope)
            {
                Log("####################################");
                Msg("[%s]", m_attached_items[g_bHudAdjustItemIdx]->m_sect_name.c_str());
                Msg("scope_zoom_offset%s = %g,%g,%g", is_16x9 ? "_16x9" : "", pos_.x, pos_.y, pos_.z);
                Msg("scope_zoom_rotate_x%s = %g", is_16x9 ? "_16x9" : "", rot_.x);
                Msg("scope_zoom_rotate_y%s = %g", is_16x9 ? "_16x9" : "", rot_.y);
                Log("####################################");
            }
            else if (idx == hud_item_measures::m_hands_offset_type_gl_scope)
            {
                Log("####################################");
                Msg("[%s]", m_attached_items[g_bHudAdjustItemIdx]->m_sect_name.c_str());
                Msg("scope_grenade_zoom_offset%s = %g,%g,%g", is_16x9 ? "_16x9" : "", pos_.x, pos_.y, pos_.z);
                Msg("scope_grenade_zoom_rotate_x%s = %g", is_16x9 ? "_16x9" : "", rot_.x);
                Msg("scope_grenade_zoom_rotate_y%s = %g", is_16x9 ? "_16x9" : "", rot_.y);
                Log("####################################");
            }
            else if (idx == hud_item_measures::m_hands_offset_type_aim_gl_normal)
            {
                Log("####################################");
                Msg("[%s]", m_attached_items[g_bHudAdjustItemIdx]->m_sect_name.c_str());
                Msg("grenade_normal_zoom_offset%s = %g,%g,%g", is_16x9 ? "_16x9" : "", pos_.x, pos_.y, pos_.z);
                Msg("grenade_normal_zoom_rotate_x%s = %g", is_16x9 ? "_16x9" : "", rot_.x);
                Msg("grenade_normal_zoom_rotate_y%s = %g", is_16x9 ? "_16x9" : "", rot_.y);
                Log("####################################");
            }
            else if (idx == hud_item_measures::m_hands_offset_type_gl_normal_scope)
            {
                Log("####################################");
                Msg("[%s]", m_attached_items[g_bHudAdjustItemIdx]->m_sect_name.c_str());
                Msg("scope_grenade_normal_zoom_offset%s = %g,%g,%g", is_16x9 ? "_16x9" : "", pos_.x, pos_.y, pos_.z);
                Msg("scope_grenade_normal_zoom_rotate_x%s = %g", is_16x9 ? "_16x9" : "", rot_.x);
                Msg("scope_grenade_normal_zoom_rotate_y%s = %g", is_16x9 ? "_16x9" : "", rot_.y);
                Log("####################################");
            }
        }
    }
    else if (g_bHudAdjustMode == ADJUST_DELTA_POS || g_bHudAdjustMode == ADJUST_DELTA_ROT)
    {
        if (g_bHudAdjustMode == ADJUST_DELTA_POS && (values.z))
            g_bHudAdjustDeltaPos += (values.z > 0) ? 0.001f : -0.001f;

        if (g_bHudAdjustMode == ADJUST_DELTA_ROT && (values.z))
            g_bHudAdjustDeltaRot += (values.z > 0) ? 0.1f : -0.1f;
    }
    else if (auto hi = m_attached_items[g_bHudAdjustItemIdx])
        hi->tune(values);
}

void hud_draw_adjust_mode()
{
    ensure_bone_adjust_command_registered();

    // Keep config-defined bone corrections active even while the adjustment UI
    // itself is switched off.
    refresh_bone_adjust_callbacks();

    if (!g_bHudAdjustMode)
        return;

    if (is_bone_adjust_mode())
    {
        const char* text{};
        if (pInput->iGetAsyncKeyState(DIK_LSHIFT))
            text = "press SHIFT+0-return|1-bone_pos|2-bone_rot|8-pos_step|9-rot_step";
        else if (pInput->iGetAsyncKeyState(DIK_LCONTROL))
            text = "press CTRL+0-item idx 1|1-item idx 2";
        else if (g_bHudAdjustMode == BONE_POS)
            text = "adjusting BONE POSITION";
        else if (g_bHudAdjustMode == BONE_ROT)
            text = "adjusting BONE ROTATION";
        else if (g_bHudAdjustMode == BONE_ADJUST_DELTA_POS)
            text = "adjusting bone pos STEP";
        else if (g_bHudAdjustMode == BONE_ADJUST_DELTA_ROT)
            text = "adjusting bone rot STEP";

        CGameFont* F = UI()->Font()->pFontDI;
        F->SetAligment(CGameFont::alCenter);
        F->OutSetI(0.f, -0.8f);
        F->SetColor(D3DCOLOR_XRGB(125, 0, 0));
        F->OutNext(text ? text : "adjusting BONE");

        attachable_hud_item* item = bone_adjust_item_for_index(static_cast<u16>(g_bHudAdjustItemIdx));
        F->OutNext("for item: [%d] [%s]", g_bHudAdjustItemIdx, item ? item->m_sect_name.c_str() : "NOT FOUND");
        F->OutNext("bone: [%s]", g_bone_adjust_bone.c_str() ? g_bone_adjust_bone.c_str() : "NOT SELECTED");

        if (item && g_bone_adjust_bone.c_str())
        {
            if (hud_bone_adjustment* adjustment = find_bone_adjustment(item->m_sect_name, g_bone_adjust_bone.c_str()))
            {
                F->OutNext("bone_position = [%g, %g, %g]", adjustment->position.x, adjustment->position.y, adjustment->position.z);
                F->OutNext("bone_rotation(deg) = [%g, %g, %g]", adjustment->rotation.x, adjustment->rotation.y, adjustment->rotation.z);
            }
        }

        F->OutNext("delta values: dP=[%g], dR=[%g]", g_bHudAdjustDeltaPos, g_bHudAdjustDeltaRot);
        F->OutNext("[Z]-x axis, [X]-y axis, [C]-z axis    ||||||    [<---LEFT/RIGHT--->]-x axis, [UP/DOWN]-y axis, [PageUP/PageDown]-z axis");
        return;
    }

    const char* _text{};
    if (pInput->iGetAsyncKeyState(DIK_LSHIFT))
        _text =
            "press SHIFT+NUM 0-return|1-hud_pos|2-hud_rot|3-itm_pos|4-itm_rot|5-fire_point|6-fire_point2|7-shell_point|8-pos_step|9-rot_step    ||||||    press "
            "SHIFT+1-laser_point|2-flashlight_point";
    else if (pInput->iGetAsyncKeyState(DIK_LCONTROL))
        _text = "press CTRL+NUM 0-item idx 1|1-item idx 2";
    else
        _text = std::get<1>(ADJUST_MODES_DB.at(g_bHudAdjustMode));

    if (_text)
    {
        CGameFont* F = UI()->Font()->pFontDI;
        F->SetAligment(CGameFont::alCenter);
        F->OutSetI(0.f, -0.8f);
        F->SetColor(D3DCOLOR_XRGB(125, 0, 0));
        F->OutNext(_text);
        F->OutNext("for item: [%d] [%s]", g_bHudAdjustItemIdx,
                   g_player_hud->attached_item(u16(g_bHudAdjustItemIdx)) ? g_player_hud->attached_item(u16(g_bHudAdjustItemIdx))->m_sect_name.c_str() : "NOT FOUND");
        F->OutNext("delta values: dP=[%g], dR=[%g]", g_bHudAdjustDeltaPos, g_bHudAdjustDeltaRot);
        F->OutNext("[Z]-x axis, [X]-y axis, [C]-z axis    ||||||    [<---LEFT/RIGHT--->]-x axis, [UP/DOWN]-y axis, [PageUP/PageDown]-z axis");
    }
}

void hud_adjust_mode_keyb(int dik)
{
    if (!g_bHudAdjustMode) //Включать этот режим только через консоль
        return;

    if (is_bone_adjust_mode())
    {
        if (pInput->iGetAsyncKeyState(DIK_LSHIFT))
        {
            if (dik == DIK_NUMPAD0)
            {
                g_bHudAdjustMode = OFF;
                g_bone_adjust_bone = nullptr;
            }
            else if (dik == DIK_NUMPAD1)
                g_bHudAdjustMode = BONE_POS;
            else if (dik == DIK_NUMPAD2)
                g_bHudAdjustMode = BONE_ROT;
            else if (dik == DIK_NUMPAD8)
                g_bHudAdjustMode = BONE_ADJUST_DELTA_POS;
            else if (dik == DIK_NUMPAD9)
                g_bHudAdjustMode = BONE_ADJUST_DELTA_ROT;
            return;
        }
        else if (pInput->iGetAsyncKeyState(DIK_LCONTROL))
        {
            if (dik == DIK_NUMPAD0)
                g_bHudAdjustItemIdx = 0;
            else if (dik == DIK_NUMPAD1)
                g_bHudAdjustItemIdx = 1;
            return;
        }

        return;
    }

    if (pInput->iGetAsyncKeyState(DIK_LSHIFT))
    {
        int mode{};
        for (const auto& [key, str] : ADJUST_MODES_DB)
        {
            if (key == dik)
            {
                g_bHudAdjustMode = mode;
                return;
            }
            mode++;
        }
    }
    else if (pInput->iGetAsyncKeyState(DIK_LCONTROL))
    {
        if (dik == DIK_NUMPAD0)
            g_bHudAdjustItemIdx = 0;
        else if (dik == DIK_NUMPAD1)
            g_bHudAdjustItemIdx = 1;
    }
}
