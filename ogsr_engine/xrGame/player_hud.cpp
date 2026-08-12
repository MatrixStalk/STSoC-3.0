#include "StdAfx.h"
#include "player_hud.h"
#include "physic_item.h"
#include "actor.h"
#include "ActorEffector.h"
#include "HudItem.h"
#include "ui_base.h"
#include "level.h"
#include "weapon.h"

player_hud* g_player_hud{};

namespace
{
constexpr LPCSTR source_root_bone = "valvebiped.bip01_spine4";
constexpr LPCSTR source_l_clavicle = "valvebiped.bip01_l_clavicle";
constexpr LPCSTR source_r_clavicle = "valvebiped.bip01_r_clavicle";
constexpr LPCSTR source_r_thumb0 = "valvebiped.bip01_r_finger0";
constexpr LPCSTR source_r_thumb01 = "valvebiped.bip01_r_finger01";
constexpr LPCSTR source_r_thumb02 = "valvebiped.bip01_r_finger02";
constexpr LPCSTR source_camera_bone = "camera";

bool is_source_hud_skeleton(const IKinematics* skeleton)
{
    return skeleton && skeleton->LL_BoneID(source_root_bone) != BI_NONE && skeleton->LL_BoneID(source_l_clavicle) != BI_NONE &&
        skeleton->LL_BoneID(source_r_clavicle) != BI_NONE;
}

u16 hud_bone_id(const IKinematics* skeleton, LPCSTR bone_name)
{
    if (!skeleton || !bone_name)
        return BI_NONE;

    string256 normalized_name;
    xr_strcpy(normalized_name, bone_name);
    _strlwr(normalized_name);

    if (const u16 bone_id = skeleton->LL_BoneID(normalized_name); bone_id != BI_NONE)
        return bone_id;

    struct bone_alias
    {
        LPCSTR legacy;
        LPCSTR source;
    };
    static constexpr bone_alias aliases[] = {
        {"l_clavicle", source_l_clavicle},
        {"r_clavicle", source_r_clavicle},
        {"l_hand", "valvebiped.bip01_l_hand"},
        {"r_hand", "valvebiped.bip01_r_hand"},
        {"r_finger0", source_r_thumb0},
        {"r_finger01", source_r_thumb01},
        {"r_finger02", source_r_thumb02},
    };

    for (const bone_alias& alias : aliases)
    {
        if (!xr_strcmp(normalized_name, alias.legacy))
            return skeleton->LL_BoneID(alias.source);
    }

    return BI_NONE;
}

bool is_source_arm_bone(LPCSTR bone_name)
{
    return bone_name && !strncmp(bone_name, "valvebiped.bip01_", xr_strlen("valvebiped.bip01_"));
}

} // namespace

float CalculateMotionStartSeconds(float fStartFromTime, float fMotionLength)
{
    R_ASSERT(fStartFromTime >= -1.0f);
    return (abs(fStartFromTime) * fMotionLength);
}

namespace
{
void setup_hud_blend(CBlend* blend, const motion_params& params, const float speed)
{
    R_ASSERT(blend);
    blend->speed *= speed;
    blend->timeCurrent = CalculateMotionStartSeconds(params.start_k, blend->timeTotal);
    if (params.stop_at_end)
        blend->stop_at_end = TRUE;
}
} // namespace

player_hud_motion* player_hud_motion_container::find_motion(const shared_str& name)
{
    auto it = m_anims.find(name);
    return it != m_anims.end() ? &it->second : nullptr;
}

void player_hud_motion_container::load(
    bool has_separated_hands, bool merge_skeleton, IKinematicsAnimated* model, IKinematicsAnimated* animatedHudItem, const shared_str& sect)
{
    m_anims.clear();
    string512 buff;
    MotionID motion_ID;

    for (const auto& [name, anm] : pSettings->r_section(sect).Data)
    {
        if ((strstr(name.c_str(), "anm_") == name.c_str() || strstr(name.c_str(), "anim_") == name.c_str())
            && !strstr(name.c_str(), "_speed_k") && !strstr(name.c_str(), "_start_k") && !strstr(name.c_str(), "_stop_k") &&
            !strstr(name.c_str(), "_stop_at_end") && !strstr(name.c_str(), "_effector"))
        {
            player_hud_motion pm;
            if (has_separated_hands)
            {
                if (_GetItemCount(anm.c_str()) == 1)
                {
                    pm.m_base_name = anm;
                    pm.m_additional_name = anm;
                }
                else
                {
                    R_ASSERT2(_GetItemCount(anm.c_str()) == 2, anm.c_str());
                    string512 str_item;
                    _GetItem(anm.c_str(), 0, str_item);
                    pm.m_base_name = str_item;
                    _GetItem(anm.c_str(), 1, str_item);
                    pm.m_additional_name = str_item;
                }
            }
            else
            {
                string512 str_item;
                _GetItem(anm.c_str(), 0, str_item);
                pm.m_base_name = str_item;
                pm.m_additional_name = str_item;
            }

            string128 speed_param;
            xr_strconcat(speed_param, name.c_str(), "_speed_k");
            if (pSettings->line_exist(sect, speed_param))
            {
                const float k = pSettings->r_float(sect, speed_param);
                if (!fsimilar(k, 1.f) && k > 0.001f)
                    pm.params.speed_k = k;
            }

            string128 stop_param;
            xr_strconcat(stop_param, name.c_str(), "_stop_k");
            if (pSettings->line_exist(sect, stop_param))
            {
                const float k = pSettings->r_float(sect, stop_param);
                if (k < 1.f && k > 0.001f)
                    pm.params.stop_k = k;
            }

            string128 stop_at_end_param;
            xr_strconcat(stop_at_end_param, name.c_str(), "_stop_at_end");
            const bool is_idle_alias = !strncmp(name.c_str(), "anm_idle", xr_strlen("anm_idle")) ||
                !strncmp(name.c_str(), "anim_idle", xr_strlen("anim_idle"));
            const bool is_idle_transition = is_idle_alias && (strstr(name.c_str(), "_start") || strstr(name.c_str(), "_end"));
            pm.params.stop_at_end =
                READ_IF_EXISTS(pSettings, r_bool, sect, stop_at_end_param, merge_skeleton && (!is_idle_alias || is_idle_transition));

            string128 start_param;
            xr_strconcat(start_param, name.c_str(), "_start_k");
            if (pSettings->line_exist(sect, start_param))
            {
                const float k = pSettings->r_float(sect, start_param);
                if (k < 1.f && k > 0.001f)
                    pm.params.start_k = k;
            }

            IKinematicsAnimated* final_model{};
            if (merge_skeleton && animatedHudItem)
                final_model = animatedHudItem;
            else if (model && has_separated_hands)
                final_model = model;
            else if (animatedHudItem && !has_separated_hands)
                final_model = animatedHudItem;

            R_ASSERT3(final_model, "No animated skeleton is available for HUD motions", sect.c_str());

            for (u32 i = 0; i <= 8; ++i)
            {
                if (i == 0)
                    xr_strcpy(buff, pm.m_base_name.c_str());
                else
                    xr_sprintf(buff, "%s%d", pm.m_base_name.c_str(), i);

                motion_ID = final_model->ID_Cycle_Safe(buff);
                if (motion_ID.valid())
                {
                    auto& Anim = pm.m_animations.emplace_back();
                    Anim.mid = motion_ID;
                    Anim.name = buff;
                    string128 eff_param;
                    Anim.eff_name = READ_IF_EXISTS(pSettings, r_string, sect, xr_strconcat(eff_param, name.c_str(), "_effector"), nullptr);
                }
            }

            if (pm.m_base_name != pm.m_additional_name)
            {
                for (u32 i = 0; i <= 8; ++i)
                {
                    if (i == 0)
                        xr_strcpy(buff, pm.m_additional_name.c_str());
                    else
                        xr_sprintf(buff, "%s%d", pm.m_additional_name.c_str(), i);

                    motion_ID = animatedHudItem->ID_Cycle_Safe(buff);
                    if (motion_ID.valid())
                    {
                        auto& Anim = pm.m_additional_animations.emplace_back();
                        Anim.mid = motion_ID;
                        Anim.name = buff;
                    }
                }
                if (pm.m_additional_animations.empty())
                    pm.m_additional_name = pm.m_base_name;
            }

            if (pm.m_animations.empty())
            {
                if (has_separated_hands)
                    FATAL("[%s] motion [%s](%s) not found in section [%s]", __FUNCTION__, pm.m_base_name.c_str(), name.c_str(), sect.c_str());
                else
                    continue;
            }
            m_anims.emplace(name, std::move(pm));
        }
    }
}

Fvector& attachable_hud_item::hands_attach_pos() { return m_measures.m_hands_attach[0]; }
Fvector& attachable_hud_item::hands_attach_rot() { return m_measures.m_hands_attach[1]; }
Fvector& attachable_hud_item::hands_offset_pos() { return m_measures.m_hands_offset[hud_item_measures::m_hands_offset_pos][m_parent_hud_item->GetCurrentHudOffsetIdx()]; }
Fvector& attachable_hud_item::hands_offset_rot() { return m_measures.m_hands_offset[hud_item_measures::m_hands_offset_rot][m_parent_hud_item->GetCurrentHudOffsetIdx()]; }

void player_hud::copy_source_bone(u16 target_idx, CBoneInstance* target_bone)
{
    IKinematics* source = m_source_skeletons[target_idx];
    if (!source)
        return;

    const u16 source_bone_id = static_cast<u16>(target_bone->get_param(0));
    const u16 target_bone_id = static_cast<u16>(target_bone->get_param(2));
    IKinematics* target = target_idx == 0 ? m_model_kinematics : m_model_2_kinematics;
    if (target && source_bone_id < source->LL_BoneCount() && target_bone_id < target->LL_BoneCount())
    {
        const CBoneData& source_data = source->LL_GetData(source_bone_id);
        const CBoneData& target_data = target->LL_GetData(target_bone_id);
        const u16 source_parent_id = source_data.GetParentID();
        const u16 target_parent_id = target_data.GetParentID();

        Fmatrix source_local;
        if (source_parent_id != BI_NONE)
        {
            Fmatrix source_parent_inverse;
            source_parent_inverse.invert(source->LL_GetBoneInstance(source_parent_id).mTransform);
            source_local.mul_43(source_parent_inverse, source->LL_GetBoneInstance(source_bone_id).mTransform);
        }
        else
        {
            source_local.set(source->LL_GetBoneInstance(source_bone_id).mTransform);
        }

        Fmatrix source_bind_inverse, source_delta;
        source_bind_inverse.invert(source_data.bind_transform);
        source_delta.mul_43(source_local, source_bind_inverse);
        source_delta.c.set(0.f, 0.f, 0.f);

        Fmatrix source_parent_bind_world, target_parent_bind_world;
        source_parent_bind_world.identity();
        target_parent_bind_world.identity();
        if (source_parent_id != BI_NONE)
            source_parent_bind_world.invert(source->LL_GetData(source_parent_id).m2b_transform);
        if (target_parent_id != BI_NONE)
            target_parent_bind_world.invert(target->LL_GetData(target_parent_id).m2b_transform);
        source_parent_bind_world.c.set(0.f, 0.f, 0.f);
        target_parent_bind_world.c.set(0.f, 0.f, 0.f);

        Fmatrix target_parent_bind_inverse, source_to_target_basis, target_to_source_basis;
        target_parent_bind_inverse.invert(target_parent_bind_world);
        source_to_target_basis.mul_43(target_parent_bind_inverse, source_parent_bind_world);
        target_to_source_basis.invert(source_to_target_basis);

        Fmatrix corrected_delta_tmp, corrected_delta;
        corrected_delta_tmp.mul_43(source_to_target_basis, source_delta);
        corrected_delta.mul_43(corrected_delta_tmp, target_to_source_basis);
        corrected_delta.c.set(0.f, 0.f, 0.f);

        Fmatrix target_local;
        target_local.mul_43(corrected_delta, target_data.bind_transform);

        LPCSTR target_bone_name = target->LL_BoneName(target_bone_id);
        const bool is_merge_root = target_bone_name && !xr_strcmp(target_bone_name, source_root_bone);
        if (is_merge_root)
        {
            Fvector root_translation_delta = source_local.c;
            root_translation_delta.sub(source_data.bind_transform.c);
            source_to_target_basis.transform_dir(root_translation_delta);
            target_local.c.set(target_data.bind_transform.c);
            target_local.c.add(root_translation_delta);
        }
        else
        {
            target_local.c.set(target_data.bind_transform.c);
        }

        if (target_parent_id != BI_NONE)
            target_bone->mTransform.mul_43(target->LL_GetBoneInstance(target_parent_id).mTransform, target_local);
        else
            target_bone->mTransform.set(target_local);
    }

    if (target_idx == 0)
    {
        switch (static_cast<u16>(target_bone->get_param(1)))
        {
        case 1: Thumb0Callback(target_bone); break;
        case 2: Thumb01Callback(target_bone); break;
        case 3: Thumb02Callback(target_bone); break;
        }
    }
}

bool player_hud::camera_bone_rotation(Fmatrix& rotation) const
{
    rotation.identity();
    IKinematics* source = m_source_skeletons[0];
    if (!m_source_skeleton_mode || !source)
        return false;
    const u16 camera_bone_id = source->LL_BoneID(source_camera_bone);
    if (camera_bone_id == BI_NONE)
        return false;
    const CBoneData& camera_data = source->LL_GetData(camera_bone_id);
    Fmatrix camera_delta;
    camera_delta.mul_43(source->LL_GetBoneInstance(camera_bone_id).mTransform, camera_data.m2b_transform);
    Fquaternion camera_rotation;
    camera_rotation.set(camera_delta);
    rotation.rotation(camera_rotation);
    rotation.c.set(0.f, 0.f, 0.f);
    return true;
}
