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

    // Keep old HUD configs usable after replacing their mesh with a Source rig.
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


// Рассчитать стартовую секунду анимации --#SM+#--
float CalculateMotionStartSeconds(float fStartFromTime, float fMotionLength)
{
    R_ASSERT(fStartFromTime >= -1.0f);

    //if (fStartFromTime >= 0.0f)
    //{ 
    //    // Выставляем время в точных значениях
    //    clamp(fStartFromTime, 0.0f, fMotionLength);
    //    return abs(fStartFromTime);
    //}
    //else
    {   // Выставляем время в процентных значениях (от всей длины анимации)
        return (abs(fStartFromTime) * fMotionLength);
    }
}

namespace
{
void setup_hud_blend(CBlend* blend, const motion_params& params, const float speed)
{
    R_ASSERT(blend);

    blend->speed *= speed;
    blend->timeCurrent = CalculateMotionStartSeconds(params.start_k, blend->timeTotal);

    // A config-side *_stop_at_end flag must affect the actual skeleton track,
    // not only the CHudItem completion timer. Otherwise a cyclic OGF motion
    // wraps to its first frames before OnAnimationEnd switches the HUD state.
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

            // and load all motions for it

            for (u32 i = 0; i <= 8; ++i)
            {
                if (i == 0)
                    xr_strcpy(buff, pm.m_base_name.c_str());
                else
                    xr_sprintf(buff, "%s%d", pm.m_base_name.c_str(), i);

                {
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
            }

            if (pm.m_base_name != pm.m_additional_name)
            {
                // and additiona motions for it

                for (u32 i = 0; i <= 8; ++i)
                {
                    if (i == 0)
                        xr_strcpy(buff, pm.m_additional_name.c_str());
                    else
                        xr_sprintf(buff, "%s%d", pm.m_additional_name.c_str(), i);

                    {
                        motion_ID = animatedHudItem->ID_Cycle_Safe(buff);

                        if (motion_ID.valid())
                        {
                            auto& Anim = pm.m_additional_animations.emplace_back();
                            Anim.mid = motion_ID;
                            Anim.name = buff;

                            /*string128 eff_param;
                            Anim.eff_name = READ_IF_EXISTS(pSettings, r_string, sect, xr_strconcat(eff_param, name.c_str(), "_effector"), nullptr);*/
                        }
                    }
                }

                if (pm.m_additional_animations.empty())
                {
                    MsgDbg("additional motion [%s](%s) not found in section [%s], will use main!", pm.m_additional_name.c_str(), name.c_str(), sect.c_str());

                    pm.m_additional_name = pm.m_base_name;
                }
            }

            if (pm.m_animations.empty())
            {
                if (has_separated_hands)
                {
                    FATAL("[%s] motion [%s](%s) not found in section [%s]", __FUNCTION__, pm.m_base_name.c_str(), name.c_str(), sect.c_str());
                }
                else
                {
                    Msg("! [%s] motion [%s](%s) not found in section [%s]", __FUNCTION__, pm.m_base_name.c_str(), name.c_str(), sect.c_str());
                    continue;
                }
            }

            m_anims.emplace(name, std::move(pm));
        }
    }
}

Fvector& attachable_hud_item::hands_attach_pos() { return m_measures.m_hands_attach[0]; }
Fvector& attachable_hud_item::hands_attach_rot() { return m_measures.m_hands_attach[1]; }
Fvector& attachable_hud_item::hands_offset_pos() { return m_measures.m_hands_offset[hud_item_measures::m_hands_offset_pos][m_parent_hud_item->GetCurrentHudOffsetIdx()]; }

Fvector& attachable_hud_item::hands_offset_rot() { return m_measures.m_hands_offset[hud_item_measures::m_hands_offset_rot][m_parent_hud_item->GetCurrentHudOffsetIdx()]; }

void attachable_hud_item::set_bone_visible(const shared_str& bone_name, BOOL bVisibility, BOOL bSilent)
{
    u16 bone_id;
    BOOL bVisibleNow;
    bone_id = m_model->LL_BoneID(bone_name);
    if (bone_id == BI_NONE)
    {
        if (bSilent)
            return;
        FATAL("model [%s] has no bone [%s]", m_visual_name.c_str(), bone_name.c_str());
    }
    bVisibleNow = m_model->LL_GetBoneVisible(bone_id);
    if (bVisibleNow != bVisibility)
        m_model->LL_SetBoneVisible(bone_id, bVisibility, TRUE);
}

void attachable_hud_item::set_bone_visible(const xr_vector<shared_str>& bone_names, BOOL bVisibility, BOOL bSilent)
{
    for (const auto& bone_name : bone_names)
        set_bone_visible(bone_name, bVisibility, bSilent);
}

BOOL attachable_hud_item::get_bone_visible(const shared_str& bone_name)
{
    u16 bone_id = m_model->LL_BoneID(bone_name);
    return m_model->LL_GetBoneVisible(bone_id);
}

bool attachable_hud_item::has_bone(const shared_str& bone_name)
{
    u16 bone_id = m_model->LL_BoneID(bone_name);
    return (bone_id != BI_NONE);
}

void attachable_hud_item::update(bool bForce)
{
    if (!bForce && m_upd_firedeps_frame == Device.dwFrame)
        return;
    bool is_16x9 = UI()->is_widescreen();

    if (!!m_measures.m_prop_flags.test(hud_item_measures::e_16x9_mode_now) != is_16x9)
        m_measures.load(m_sect_name, m_model);

    Fvector ypr = m_measures.m_item_attach[1];
    ypr.mul(PI / 180.f);
    m_attach_offset.setHPB(ypr.x, ypr.y, ypr.z);
    m_attach_offset.translate_over(m_measures.m_item_attach[0]);

    m_parent->calc_transform(m_attach_place_idx, m_attach_offset, m_item_transform, m_merge_skeleton);
    m_upd_firedeps_frame = Device.dwFrame;

    IKinematicsAnimated* ka = m_model->dcast_PKinematicsAnimated();
    if (ka)
    {
        ka->UpdateTracks();
        ka->dcast_PKinematics()->CalculateBones_Invalidate();
        ka->dcast_PKinematics()->CalculateBones(TRUE);
    }
}

player_hud_motion* attachable_hud_item::find_motion(const shared_str& name)
{ 
    return m_hand_motions.find_motion(name); 
}

void attachable_hud_item::setup_firedeps(firedeps& fd)
{
    update(false);
    // fire point&direction
    if (m_measures.m_prop_flags.test(hud_item_measures::e_fire_point))
    {
        Fmatrix& fire_mat = m_model->LL_GetTransform(m_measures.m_fire_bone);
        fire_mat.transform_tiny(fd.vLastFP, m_measures.m_fire_point_offset);
        m_item_transform.transform_tiny(fd.vLastFP);
        fd.vLastFP.add(Device.vCameraPosition);

        // KRodin придумал костыль. Из-за того, что fire_point расположен сильно впереди ствола, попробуем точку вылета пули считать от позиции fire_point.z == -0.5, т.е. ближе к
        // актору, чтобы нельзя было стрелять сквозь стены.
        if (m_measures.useCopFirePoint)
        {
            fire_mat.transform_tiny(fd.vLastShootPoint, m_measures.m_shoot_point_offset);
            m_item_transform.transform_tiny(fd.vLastShootPoint);
            fd.vLastShootPoint.add(Device.vCameraPosition);
        }
        else //На ТЧ - стволах fire_point живет от стволов отдельной жизнью, поэтому если пытаться там править координаты - всё плывёт, оставим как есть.
            fd.vLastShootPoint = fd.vLastFP;

        fd.vLastFD.set(0.f, 0.f, 1.f);
        m_item_transform.transform_dir(fd.vLastFD);
        if (m_measures.useCopFirePoint)
            m_parent_hud_item->CorrectDirFromWorldToHud(fd.vLastFD);
        fd.vLastFD.normalize_safe();
        VERIFY(_valid(fd.vLastFD));
        VERIFY(_valid(fd.vLastFD));

        fd.m_FireParticlesXForm.identity();
        fd.m_FireParticlesXForm.k.set(fd.vLastFD);
        Fvector::generate_orthonormal_basis_normalized(fd.m_FireParticlesXForm.k, fd.m_FireParticlesXForm.j, fd.m_FireParticlesXForm.i);
        VERIFY(_valid(fd.m_FireParticlesXForm));
    }

    if (m_measures.m_prop_flags.test(hud_item_measures::e_fire_point2))
    {
        Fmatrix& fire_mat = m_model->LL_GetTransform(m_measures.m_fire_bone2);
        fire_mat.transform_tiny(fd.vLastFP2, m_measures.m_fire_point2_offset);
        m_item_transform.transform_tiny(fd.vLastFP2);
        fd.vLastFP2.add(Device.vCameraPosition);
        VERIFY(_valid(fd.vLastFP2));
        VERIFY(_valid(fd.vLastFP2));
    }

    if (m_measures.m_prop_flags.test(hud_item_measures::e_shell_point))
    {
        Fmatrix& fire_mat = m_model->LL_GetTransform(m_measures.m_shell_bone);
        fire_mat.transform_tiny(fd.vLastSP, m_measures.m_shell_point_offset);
        m_item_transform.transform_tiny(fd.vLastSP);
        fd.vLastSP.add(Device.vCameraPosition);
        VERIFY(_valid(fd.vLastSP));
        VERIFY(_valid(fd.vLastSP));
    }
}

bool attachable_hud_item::need_renderable() { return m_parent_hud_item->need_renderable(); }

void attachable_hud_item::render(u32 context_id, IRenderable* root)
{
    ::Render->add_Visual(context_id, root, m_model->dcast_RenderVisual(), m_item_transform);
    debug_draw_firedeps();
    m_parent_hud_item->render_hud_mode(context_id, root);
}

bool attachable_hud_item::render_item_ui_query() { return m_parent_hud_item->render_item_3d_ui_query(); }

void attachable_hud_item::render_item_ui() { m_parent_hud_item->render_item_3d_ui(); }

void hud_item_measures::load(const shared_str& sect_name, IKinematics* K)
{
    bool is_16x9 = UI()->is_widescreen();
    string64 _prefix;
    xr_sprintf(_prefix, "%s", is_16x9 ? "_16x9" : "");
    string128 val_name, val_name2;

    strconcat(sizeof(val_name), val_name, "hands_position", _prefix);
    if (is_16x9 && !pSettings->line_exist(sect_name, val_name))
        xr_strcpy(val_name, "hands_position");
    m_hands_attach[0] = READ_IF_EXISTS(pSettings, r_fvector3, sect_name, val_name, Fvector{});

    strconcat(sizeof(val_name), val_name, "hands_orientation", _prefix);
    if (is_16x9 && !pSettings->line_exist(sect_name, val_name))
        xr_strcpy(val_name, "hands_orientation");
    m_hands_attach[1] = READ_IF_EXISTS(pSettings, r_fvector3, sect_name, val_name, Fvector{});

    strconcat(sizeof(val_name), val_name, "hud_scale", _prefix);
    if (is_16x9 && !pSettings->line_exist(sect_name, val_name))
        xr_strcpy(val_name, "hud_scale");
    m_hud_scale = READ_IF_EXISTS(pSettings, r_float, sect_name, val_name, 1.f);
    clamp(m_hud_scale, 0.001f, 100.f);

    if (!pSettings->line_exist(sect_name, "item_position") && pSettings->line_exist(sect_name, "position"))
        m_item_attach[0] = pSettings->r_fvector3(sect_name, "position");
    else
        m_item_attach[0] = pSettings->r_fvector3(sect_name, "item_position");

    if (!pSettings->line_exist(sect_name, "item_orientation") && pSettings->line_exist(sect_name, "orientation"))
        m_item_attach[1] = pSettings->r_fvector3(sect_name, "orientation");
    else
        m_item_attach[1] = pSettings->r_fvector3(sect_name, "item_orientation");

    shared_str bone_name;
    if (pSettings->line_exist(sect_name, "use_cop_fire_point"))
        useCopFirePoint = !!pSettings->r_bool(sect_name, "use_cop_fire_point");
    else
        useCopFirePoint = !!pSettings->line_exist(sect_name, "item_visual");

    if (!useCopFirePoint) // shoc configs
    {
        m_prop_flags.set(e_fire_point, pSettings->line_exist(sect_name, "fire_bone") && pSettings->line_exist(sect_name, "fire_point"));
        if (m_prop_flags.test(e_fire_point))
        {
            bone_name = pSettings->r_string(sect_name, "fire_bone");
            m_fire_bone = K->LL_BoneID(bone_name);
            ASSERT_FMT(m_fire_bone != BI_NONE, "!![%s] bone [%s] not found in weapon [%s]", __FUNCTION__, bone_name.c_str(), sect_name.c_str());
            m_fire_point_offset = pSettings->r_fvector3(sect_name, "fire_point");
        }
        else
            m_fire_point_offset.set(0.f, 0.f, 0.f);

        m_prop_flags.set(e_fire_point2, pSettings->line_exist(sect_name, "fire_bone") && pSettings->line_exist(sect_name, "fire_point2"));
        if (m_prop_flags.test(e_fire_point2))
        {
            bone_name = pSettings->r_string(sect_name, "fire_bone");
            m_fire_bone2 = K->LL_BoneID(bone_name);
            ASSERT_FMT(m_fire_bone2 != BI_NONE, "!![%s] bone [%s] not found in weapon [%s]", __FUNCTION__, bone_name.c_str(), sect_name.c_str());
            m_fire_point2_offset = pSettings->r_fvector3(sect_name, "fire_point2");
        }
        else if (m_prop_flags.test(e_fire_point))
        {
            m_prop_flags.set(e_fire_point2, true);
            bone_name = pSettings->r_string(sect_name, "fire_bone");
            m_fire_bone2 = K->LL_BoneID(bone_name);
            ASSERT_FMT(m_fire_bone2 != BI_NONE, "!![%s] bone [%s] not found in weapon [%s]", __FUNCTION__, bone_name.c_str(), sect_name.c_str());
            m_fire_point2_offset.set(m_fire_point_offset);
        }
        else
            m_fire_point2_offset.set(0.f, 0.f, 0.f);

        m_prop_flags.set(e_shell_point, pSettings->line_exist(sect_name, "fire_bone") && pSettings->line_exist(sect_name, "shell_point"));
        if (m_prop_flags.test(e_shell_point))
        {
            bone_name = pSettings->r_string(sect_name, "fire_bone");
            m_shell_bone = K->LL_BoneID(bone_name);
            ASSERT_FMT(m_shell_bone != BI_NONE, "!![%s] bone [%s] not found in weapon [%s]", __FUNCTION__, bone_name.c_str(), sect_name.c_str());
            m_shell_point_offset = pSettings->r_fvector3(sect_name, "shell_point");
        }
        else
            m_shell_point_offset.set(0.f, 0.f, 0.f);
    }
    else // cop configs
    {
        m_prop_flags.set(e_fire_point, pSettings->line_exist(sect_name, "fire_bone"));
        if (m_prop_flags.test(e_fire_point))
        {
            bone_name = pSettings->r_string(sect_name, "fire_bone");
            m_fire_bone = K->LL_BoneID(bone_name);
            ASSERT_FMT(m_fire_bone != BI_NONE, "!![%s] bone [%s] not found in weapon [%s]", __FUNCTION__, bone_name.c_str(), sect_name.c_str());
            m_fire_point_offset = pSettings->r_fvector3(sect_name, "fire_point");
            m_shoot_point_offset = READ_IF_EXISTS(pSettings, r_fvector3, sect_name, "shoot_point", (Fvector{m_fire_point_offset.x, m_fire_point_offset.y, -0.5f}));
        }
        else
            m_fire_point_offset.set(0.f, 0.f, 0.f);

        m_prop_flags.set(e_fire_point2, pSettings->line_exist(sect_name, "fire_bone2"));
        if (m_prop_flags.test(e_fire_point2))
        {
            bone_name = pSettings->r_string(sect_name, "fire_bone2");
            m_fire_bone2 = K->LL_BoneID(bone_name);
            ASSERT_FMT(m_fire_bone2 != BI_NONE, "!![%s] bone [%s] not found in weapon [%s]", __FUNCTION__, bone_name.c_str(), sect_name.c_str());
            m_fire_point2_offset = pSettings->r_fvector3(sect_name, "fire_point2");
        }
        else
            m_fire_point2_offset.set(0.f, 0.f, 0.f);

        m_prop_flags.set(e_shell_point, pSettings->line_exist(sect_name, "shell_bone"));
        if (m_prop_flags.test(e_shell_point))
        {
            bone_name = pSettings->r_string(sect_name, "shell_bone");
            m_shell_bone = K->LL_BoneID(bone_name);
            ASSERT_FMT(m_shell_bone != BI_NONE, "!![%s] bone [%s] not found in weapon [%s]", __FUNCTION__, bone_name.c_str(), sect_name.c_str());
            m_shell_point_offset = pSettings->r_fvector3(sect_name, "shell_point");
        }
        else
            m_shell_point_offset.set(0.f, 0.f, 0.f);
    }

    strconcat(sizeof(val_name), val_name, "aim_hud_offset_pos", _prefix);
    if (is_16x9 && !pSettings->line_exist(sect_name, val_name))
        xr_strcpy(val_name, "aim_hud_offset_pos");
    if (!pSettings->line_exist(sect_name, val_name) && pSettings->line_exist(sect_name, "zoom_offset"))
        m_hands_offset[m_hands_offset_pos][m_hands_offset_type_aim] = pSettings->r_fvector3(sect_name, "zoom_offset");
    else
        m_hands_offset[m_hands_offset_pos][m_hands_offset_type_aim] = READ_IF_EXISTS(pSettings, r_fvector3, sect_name, val_name, Fvector{});

    strconcat(sizeof(val_name), val_name, "aim_hud_offset_rot", _prefix);
    if (is_16x9 && !pSettings->line_exist(sect_name, val_name))
        xr_strcpy(val_name, "aim_hud_offset_rot");
    if (!pSettings->line_exist(sect_name, val_name) && pSettings->line_exist(sect_name, "zoom_rotate_x") && pSettings->line_exist(sect_name, "zoom_rotate_y"))
        m_hands_offset[m_hands_offset_rot][m_hands_offset_type_aim] =
            Fvector().set(pSettings->r_float(sect_name, "zoom_rotate_x"), pSettings->r_float(sect_name, "zoom_rotate_y"), 0.f);
    else
        m_hands_offset[m_hands_offset_rot][m_hands_offset_type_aim] = READ_IF_EXISTS(pSettings, r_fvector3, sect_name, val_name, Fvector{});

    strconcat(sizeof(val_name), val_name, "gl_hud_offset_pos", _prefix);
    if (is_16x9 && !pSettings->line_exist(sect_name, val_name))
        xr_strcpy(val_name, "gl_hud_offset_pos");
    if (!pSettings->line_exist(sect_name, val_name) && pSettings->line_exist(sect_name, "grenade_zoom_offset"))
        m_hands_offset[m_hands_offset_pos][m_hands_offset_type_gl] = pSettings->r_fvector3(sect_name, "grenade_zoom_offset");
    else
        m_hands_offset[m_hands_offset_pos][m_hands_offset_type_gl] = READ_IF_EXISTS(pSettings, r_fvector3, sect_name, val_name, Fvector{});

    strconcat(sizeof(val_name), val_name, "gl_hud_offset_rot", _prefix);
    if (is_16x9 && !pSettings->line_exist(sect_name, val_name))
        xr_strcpy(val_name, "gl_hud_offset_rot");
    if (!pSettings->line_exist(sect_name, val_name) && pSettings->line_exist(sect_name, "grenade_zoom_rotate_x") && pSettings->line_exist(sect_name, "grenade_zoom_rotate_y"))
        m_hands_offset[m_hands_offset_rot][m_hands_offset_type_gl] =
            Fvector().set(pSettings->r_float(sect_name, "grenade_zoom_rotate_x"), pSettings->r_float(sect_name, "grenade_zoom_rotate_y"), 0.f);
    else
        m_hands_offset[m_hands_offset_rot][m_hands_offset_type_gl] = READ_IF_EXISTS(pSettings, r_fvector3, sect_name, val_name, Fvector{});

    //ОГСР-специфичные параметры
    xr_strconcat(val_name, "scope_zoom_offset", _prefix);
    if (is_16x9 && !pSettings->line_exist(sect_name, val_name))
        xr_strcpy(val_name, "scope_zoom_offset");
    if (pSettings->line_exist(sect_name, val_name))
        m_hands_offset[m_hands_offset_pos][m_hands_offset_type_aim_scope] = pSettings->r_fvector3(sect_name, val_name);

    xr_strconcat(val_name, "scope_zoom_rotate_x", _prefix);
    xr_strconcat(val_name2, "scope_zoom_rotate_y", _prefix);
    if (is_16x9 && (!pSettings->line_exist(sect_name, val_name) || !pSettings->line_exist(sect_name, val_name2)))
    {
        xr_strcpy(val_name, "scope_zoom_rotate_x");
        xr_strcpy(val_name2, "scope_zoom_rotate_y");
    }
    if (pSettings->line_exist(sect_name, val_name) && pSettings->line_exist(sect_name, val_name2))
        m_hands_offset[m_hands_offset_rot][m_hands_offset_type_aim_scope] = Fvector{pSettings->r_float(sect_name, val_name), pSettings->r_float(sect_name, val_name2)};
    //
    xr_strconcat(val_name, "scope_grenade_zoom_offset", _prefix);
    if (is_16x9 && !pSettings->line_exist(sect_name, val_name))
        xr_strcpy(val_name, "scope_grenade_zoom_offset");
    if (pSettings->line_exist(sect_name, val_name))
        m_hands_offset[m_hands_offset_pos][m_hands_offset_type_gl_scope] = pSettings->r_fvector3(sect_name, val_name);

    xr_strconcat(val_name, "scope_grenade_zoom_rotate_x", _prefix);
    xr_strconcat(val_name2, "scope_grenade_zoom_rotate_y", _prefix);
    if (is_16x9 && (!pSettings->line_exist(sect_name, val_name) || !pSettings->line_exist(sect_name, val_name2)))
    {
        xr_strcpy(val_name, "scope_grenade_zoom_rotate_x");
        xr_strcpy(val_name2, "scope_grenade_zoom_rotate_y");
    }
    if (pSettings->line_exist(sect_name, val_name) && pSettings->line_exist(sect_name, val_name2))
        m_hands_offset[m_hands_offset_rot][m_hands_offset_type_gl_scope] = Fvector{pSettings->r_float(sect_name, val_name), pSettings->r_float(sect_name, val_name2)};
    //
    xr_strconcat(val_name, "grenade_normal_zoom_offset", _prefix);
    if (is_16x9 && !pSettings->line_exist(sect_name, val_name))
        xr_strcpy(val_name, "grenade_normal_zoom_offset");
    if (pSettings->line_exist(sect_name, val_name))
        m_hands_offset[m_hands_offset_pos][m_hands_offset_type_aim_gl_normal] = pSettings->r_fvector3(sect_name, val_name);
    else
        m_hands_offset[m_hands_offset_pos][m_hands_offset_type_aim_gl_normal] = m_hands_offset[m_hands_offset_pos][m_hands_offset_type_aim];

    xr_strconcat(val_name, "grenade_normal_zoom_rotate_x", _prefix);
    xr_strconcat(val_name2, "grenade_normal_zoom_rotate_y", _prefix);
    if (is_16x9 && (!pSettings->line_exist(sect_name, val_name) || !pSettings->line_exist(sect_name, val_name2)))
    {
        xr_strcpy(val_name, "grenade_normal_zoom_rotate_x");
        xr_strcpy(val_name2, "grenade_normal_zoom_rotate_y");
    }
    if (pSettings->line_exist(sect_name, val_name) && pSettings->line_exist(sect_name, val_name2))
        m_hands_offset[m_hands_offset_rot][m_hands_offset_type_aim_gl_normal] = Fvector{pSettings->r_float(sect_name, val_name), pSettings->r_float(sect_name, val_name2)};
    else
        m_hands_offset[m_hands_offset_rot][m_hands_offset_type_aim_gl_normal] = m_hands_offset[m_hands_offset_rot][m_hands_offset_type_aim];
    //
    xr_strconcat(val_name, "scope_grenade_normal_zoom_offset", _prefix);
    if (is_16x9 && !pSettings->line_exist(sect_name, val_name))
        xr_strcpy(val_name, "scope_grenade_normal_zoom_offset");
    if (pSettings->line_exist(sect_name, val_name))
        m_hands_offset[m_hands_offset_pos][m_hands_offset_type_gl_normal_scope] = pSettings->r_fvector3(sect_name, val_name);

    xr_strconcat(val_name, "scope_grenade_normal_zoom_rotate_x", _prefix);
    xr_strconcat(val_name2, "scope_grenade_normal_zoom_rotate_y", _prefix);
    if (is_16x9 && (!pSettings->line_exist(sect_name, val_name) || !pSettings->line_exist(sect_name, val_name2)))
    {
        xr_strcpy(val_name, "scope_grenade_normal_zoom_rotate_x");
        xr_strcpy(val_name2, "scope_grenade_normal_zoom_rotate_y");
    }
    if (pSettings->line_exist(sect_name, val_name) && pSettings->line_exist(sect_name, val_name2))
        m_hands_offset[m_hands_offset_rot][m_hands_offset_type_gl_normal_scope] = Fvector{pSettings->r_float(sect_name, val_name), pSettings->r_float(sect_name, val_name2)};
    //

    if (useCopFirePoint) // cop configs
    {
        R_ASSERT2(pSettings->line_exist(sect_name, "fire_point") == pSettings->line_exist(sect_name, "fire_bone"), sect_name.c_str());
        R_ASSERT2(pSettings->line_exist(sect_name, "fire_point2") == pSettings->line_exist(sect_name, "fire_bone2"), sect_name.c_str());
        R_ASSERT2(pSettings->line_exist(sect_name, "shell_point") == pSettings->line_exist(sect_name, "shell_bone"), sect_name.c_str());
    }

    m_prop_flags.set(e_16x9_mode_now, is_16x9);
}

attachable_hud_item::~attachable_hud_item()
{
    IRenderVisual* v = m_model->dcast_RenderVisual();
    ::Render->model_Delete(v);
    m_model = nullptr;
}

void attachable_hud_item::load(const shared_str& sect_name)
{
    m_sect_name = sect_name;

    // Visual
    if (pSettings->line_exist(sect_name, "visual"))
    {
        m_visual_name = pSettings->r_string(sect_name, "visual");
        m_has_separated_hands = false;
    }
    else
    {
        m_visual_name = pSettings->r_string(sect_name, "item_visual");
        m_has_separated_hands = true;
    }

    ::Render->hud_loading = true;
    m_model = smart_cast<IKinematics*>(::Render->model_Create(m_visual_name.c_str()));
    R_ASSERT3(m_model, "HUD item visual must be skeletal", m_visual_name.c_str());
    m_model->dcast_RenderVisual()->MarkAsHot(false);
    ::Render->hud_loading = false;

    m_attach_place_idx = READ_IF_EXISTS(pSettings, r_u16, sect_name, "attach_place_idx", 0);
    m_measures.load(sect_name, m_model);

    reload_motions();
}

void attachable_hud_item::reload_motions()
{
    IKinematicsAnimated* animatedHudItem = smart_cast<IKinematicsAnimated*>(m_model);
    const bool can_merge_source_skeletons = m_has_separated_hands && animatedHudItem && is_source_hud_skeleton(m_parent->Model()) &&
        is_source_hud_skeleton(m_model);
    m_merge_skeleton = READ_IF_EXISTS(pSettings, r_bool, m_sect_name, "skeleton_merge", can_merge_source_skeletons);

    if (m_merge_skeleton && !can_merge_source_skeletons)
    {
        Msg("! [%s] skeleton_merge requested for incompatible HUD skeletons in section [%s]; using the legacy animation path", __FUNCTION__, m_sect_name.c_str());
        m_merge_skeleton = false;
    }

    m_hand_motions.load(m_has_separated_hands, m_merge_skeleton, m_parent->AnimatedModel(), animatedHudItem, m_sect_name);
}

u32 attachable_hud_item::anim_play(const shared_str& anm_name_b, BOOL bMixIn, const CMotionDef*& md, bool randomAnim, float speed)
{
    R_ASSERT(strstr(anm_name_b.c_str(), "anm_") == anm_name_b.c_str() || strstr(anm_name_b.c_str(), "anim_") == anm_name_b.c_str());
    string256 anim_name_r;
    bool is_16x9 = UI()->is_widescreen();
    xr_sprintf(anim_name_r, "%s%s", anm_name_b.c_str(), ((m_attach_place_idx == 1) && is_16x9) ? "_16x9" : "");

    player_hud_motion* anm = m_hand_motions.find_motion(anim_name_r);
    ASSERT_FMT(anm, "model [%s] has no motion alias defined [%s]", m_visual_name.c_str(), anim_name_r);
    ASSERT_FMT(anm->m_animations.size(), "model [%s] has no motion defined in motion_alias [%s]", m_visual_name.c_str(), anim_name_r);

    u8 rnd_idx = 0;

    if (randomAnim)
        rnd_idx = (u8)Random.randI(anm->m_animations.size());

    const motion_descr& M = anm->m_animations[rnd_idx];

    if (speed == 1.f)
        speed = anm->params.speed_k;

    IKinematicsAnimated* ka = m_model->dcast_PKinematicsAnimated();
    u32 ret = g_player_hud->anim_play(m_attach_place_idx, anm->params, M, bMixIn, md, speed, m_has_separated_hands, m_merge_skeleton, ka);

    if (ka)
    {
        MotionID M2;

        if (anm->m_base_name != anm->m_additional_name)
        {
            u8 rnd_idx2 = 0;

            if (randomAnim)
                rnd_idx2 = (u8)Random.randI(anm->m_additional_animations.size());

            motion_descr& additional = anm->m_additional_animations[rnd_idx2];

            if (bDebug)
                Msg("playing item animation [%s]", additional.name.c_str());

            M2 = ka->ID_Cycle_Safe(additional.name);
        }
        else
        {
            shared_str item_anm_name = M.name;

            if (bDebug)
                Msg("playing item animation [%s]", item_anm_name.c_str());

            M2 = ka->ID_Cycle_Safe(item_anm_name);
        }
        
        if (!M2.valid())
            M2 = ka->ID_Cycle_Safe("idle");
        
        R_ASSERT3(M2.valid(), "model has no motion [idle] ", m_visual_name.c_str());

        if (m_has_separated_hands && !m_merge_skeleton)
        {
            u16 root_id = m_model->LL_GetBoneRoot();
            CBoneInstance& root_binst = m_model->LL_GetBoneInstance(root_id);
            root_binst.set_callback_overwrite(TRUE);
            root_binst.mTransform.identity();
        }

        u16 pc = ka->partitions().count();
        for (u16 pid = 0; pid < pc; ++pid)
        {
            CBlend* B = ka->PlayCycle(pid, M2, bMixIn);
            setup_hud_blend(B, anm->params, speed);
        }

        m_model->CalculateBones_Invalidate();
    }

    R_ASSERT2(m_parent_hud_item, "parent hud item is NULL");
    CPhysicItem& parent_object = m_parent_hud_item->object();

    if (parent_object.H_Parent() == Level().CurrentControlEntity())
    {
        CActor* current_actor = smart_cast<CActor*>(Level().CurrentControlEntity());
        VERIFY(current_actor);

        string_path ce_path, anm_name;
        xr_strconcat(anm_name, "camera_effects\\weapon\\", M.eff_name ? M.eff_name : M.name.c_str(), ".anm");
        if (FS.exist(ce_path, "$game_anims$", anm_name))
        {
            current_actor->Cameras().RemoveCamEffector(eCEWeaponAction);

            CAnimatorCamEffector* e = xr_new<CAnimatorCamEffector>();
            e->SetType(eCEWeaponAction);
            e->SetHudAffect(false);
            e->SetCyclic(false);
            e->Start(anm_name);
            current_actor->Cameras().AddCamEffector(e);
        }
    }
    return ret;
}

player_hud::player_hud()
{
    m_transform.identity();
    m_transform_2.identity();

    script_anim_part = u8(-1);
    script_anim_offset_factor = 0.f;
    m_item_pos.identity();
    script_override_arms = false;
    script_override_item = false;

    if (pSettings->section_exist("hud_movement_layers"))
    {
        m_movement_layers.resize(move_anms_end, nullptr);

        for (int i = 0; i < move_anms_end; i++)
        {
            char temp[20];
            string512 tmp;
            strconcat(sizeof(temp), temp, "movement_layer_", std::to_string(i).c_str());
            if (!pSettings->line_exist("hud_movement_layers", temp))
                continue;

            movement_layer* anm = xr_new<movement_layer>();
            LPCSTR layer_def = pSettings->r_string("hud_movement_layers", temp);
            const int item_count = _GetItemCount(layer_def);
            R_ASSERT2(item_count > 0, make_string("Wrong definition for [hud_movement_layers] %s", temp));

            _GetItem(layer_def, 0, tmp);
            anm->Load(tmp);

            if (item_count > 1)
            {
                _GetItem(layer_def, 1, tmp);
                anm->anm->Speed() = (atof(tmp) ? atof(tmp) : 1.f);
            }
            if (item_count > 2)
            {
                _GetItem(layer_def, 2, tmp);
                anm->m_power = (atof(tmp) ? atof(tmp) : 1.f);
            }
            if (item_count > 3)
            {
                _GetItem(layer_def, 3, tmp);
                anm->m_blend_in = _max(static_cast<float>(atof(tmp)), EPS_S);
            }
            if (item_count > 4)
            {
                _GetItem(layer_def, 4, tmp);
                anm->m_blend_out = _max(static_cast<float>(atof(tmp)), EPS_S);
            }
            m_movement_layers[i] = anm;
        }
    }
}

player_hud::~player_hud()
{
    if (m_model_kinematics)
    {
        IRenderVisual* v = m_model_kinematics->dcast_RenderVisual();
        ::Render->model_Delete(v);
        m_model = nullptr;
        m_model_kinematics = nullptr;
    }
    if (m_model_2_kinematics)
    {
        IRenderVisual* v2 = m_model_2_kinematics->dcast_RenderVisual();
        ::Render->model_Delete(v2);
        m_model_2 = nullptr;
        m_model_2_kinematics = nullptr;
    }

    auto it = m_pool.begin();
    auto it_e = m_pool.end();
    for (; it != it_e; ++it)
    {
        attachable_hud_item* a = *it;
        xr_delete(a);
    }
    m_pool.clear();

    delete_data(m_script_layers);
    delete_data(m_movement_layers);
}

void player_hud::load(const shared_str& player_hud_sect, bool force)
{
    if (!force && player_hud_sect == m_sect_name)
        return;

    if (script_override_arms) return;

    clear_source_skeleton_merge();
    _m_hand_motions.clear();

    const bool b_reload = m_model_kinematics != nullptr || m_model_2_kinematics != nullptr;
    if (m_model_kinematics)
    {
        IRenderVisual* v = m_model_kinematics->dcast_RenderVisual();
        ::Render->model_Delete(v);
        m_model = nullptr;
        m_model_kinematics = nullptr;
    }
    if (m_model_2_kinematics)
    {
        IRenderVisual* v = m_model_2_kinematics->dcast_RenderVisual();
        ::Render->model_Delete(v);
        m_model_2 = nullptr;
        m_model_2_kinematics = nullptr;
    }

    m_source_skeleton_mode = false;

    if (!pSettings->line_exist(player_hud_sect, "visual"))
        return;

    m_sect_name = player_hud_sect;
    const char* model_name = pSettings->r_string(player_hud_sect, "visual");

    ::Render->hud_loading = true;
    IRenderVisual* hands_visual_0 = ::Render->model_Create(model_name);
    m_model_kinematics = smart_cast<IKinematics*>(hands_visual_0);
    m_model = smart_cast<IKinematicsAnimated*>(hands_visual_0);
    const char* model_name_2 = READ_IF_EXISTS(pSettings, r_string, player_hud_sect, "visual_2", model_name);
    IRenderVisual* hands_visual_1 = ::Render->model_Create(model_name_2);
    m_model_2_kinematics = smart_cast<IKinematics*>(hands_visual_1);
    m_model_2 = smart_cast<IKinematicsAnimated*>(hands_visual_1);
    ::Render->hud_loading = false;

    R_ASSERT3(m_model_kinematics, "HUD hands visual must be a skeleton", model_name);
    R_ASSERT3(m_model_2_kinematics, "HUD hands visual_2 must be a skeleton", model_name_2);

    IKinematics* hands_0 = m_model_kinematics;
    IKinematics* hands_1 = m_model_2_kinematics;
    m_source_skeleton_mode = is_source_hud_skeleton(hands_0) && is_source_hud_skeleton(hands_1);
    R_ASSERT3(m_source_skeleton_mode || (m_model && m_model_2), "Legacy HUD hands visuals must be animated skeletons", model_name);

    const LPCSTR l_clavicle_name = m_source_skeleton_mode ? source_l_clavicle : "l_clavicle";
    const LPCSTR r_clavicle_name = m_source_skeleton_mode ? source_r_clavicle : "r_clavicle";
    const u16 l_arm = hud_bone_id(hands_0, l_clavicle_name);
    ASSERT_FMT(l_arm != BI_NONE, "[%s]: bone [%s] not found in sect [%s] visual [%s]", __FUNCTION__, l_clavicle_name, m_sect_name.c_str(), model_name);
    const u16 r_arm = hud_bone_id(hands_1, r_clavicle_name);
    ASSERT_FMT(r_arm != BI_NONE, "[%s]: bone [%s] not found in sect [%s] visual [%s]", __FUNCTION__, r_clavicle_name, m_sect_name.c_str(), model_name_2);

    setup_thumb_callbacks();

    m_model_kinematics->LL_SetBoneVisible(l_arm, FALSE, TRUE);
    m_model_2_kinematics->LL_SetBoneVisible(r_arm, FALSE, TRUE);

    m_ancors.clear();
    for (const auto& [key, _bone] : pSettings->r_section(player_hud_sect).Data)
    {
        if (strstr(key.c_str(), "ancor_") == key.c_str())
        {
            const u16 anchor_id = hud_bone_id(hands_0, _bone.c_str());
            ASSERT_FMT(anchor_id != BI_NONE, "[%s]: anchor bone [%s] not found in sect [%s] visual [%s]", __FUNCTION__, _bone.c_str(),
                m_sect_name.c_str(), model_name);
            m_ancors.push_back(anchor_id);
        }
    }

    //	Msg("hands visual changed to[%s] [%s] [%s]", model_name.c_str(), b_reload?"R":"", m_attached_items[0]?"Y":"");

    // Attached HUD items may outlive a hands-mesh replacement. Re-evaluate
    // their animation source before on_a_hud_attach can start a new motion.
    for (attachable_hud_item* item : m_attached_items)
    {
        if (item)
            item->reload_motions();
    }

    if (!b_reload)
    {
        const shared_str idle_name = m_source_skeleton_mode ? "idle" : "hand_idle_doun";
        if (m_model && m_model->ID_Cycle_Safe(idle_name).valid())
            m_model->PlayCycle(idle_name);
        if (m_model_2 && m_model_2->ID_Cycle_Safe(idle_name).valid())
            m_model_2->PlayCycle(idle_name);
    }
    else
    {
        if (m_attached_items[1])
            m_attached_items[1]->m_parent_hud_item->on_a_hud_attach();

        if (m_attached_items[0])
            m_attached_items[0]->m_parent_hud_item->on_a_hud_attach();
    }
    m_model_kinematics->CalculateBones_Invalidate();
    m_model_kinematics->CalculateBones(TRUE);
    m_model_2_kinematics->CalculateBones_Invalidate();
    m_model_2_kinematics->CalculateBones(TRUE);

    m_model_kinematics->dcast_RenderVisual()->MarkAsHot(true);
    m_model_2_kinematics->dcast_RenderVisual()->MarkAsHot(true);

    refresh_source_skeleton_merge();
}

bool player_hud::render_item_ui_query()
{
    bool res = false;
    if (m_attached_items[0])
        res |= m_attached_items[0]->render_item_ui_query();

    if (m_attached_items[1])
        res |= m_attached_items[1]->render_item_ui_query();

    return res;
}

void player_hud::render_item_ui()
{
    IUIRender::ePointType bk = UI()->m_currentPointType;
    UI()->m_currentPointType = IUIRender::pttLIT;
    UIRender->CacheSetCullMode(IUIRender::cmNONE);

    if (m_attached_items[0])
        m_attached_items[0]->render_item_ui();

    if (m_attached_items[1])
        m_attached_items[1]->render_item_ui();

    UIRender->CacheSetCullMode(IUIRender::cmCCW);
    UI()->m_currentPointType = bk;
}

void player_hud::render_hud(u32 context_id, IRenderable* root)
{
    //if (!m_attached_items[0] && !m_attached_items[1])
    //    return;

	bool b_r0 = ((m_attached_items[0] && m_attached_items[0]->need_renderable()) || script_anim_part == 0 || script_anim_part == 2);
    bool b_r1 = ((m_attached_items[1] && m_attached_items[1]->need_renderable()) || script_anim_part == 1 || script_anim_part == 2);

    if (!b_r0 && !b_r1)
        return;

    bool b_has_hands =
        (m_attached_items[0] && m_attached_items[0]->m_has_separated_hands) || (m_attached_items[1] && m_attached_items[1]->m_has_separated_hands) || script_anim_item_model;

    if (b_has_hands || script_anim_part != u8(-1))
    {
        ::Render->add_Visual(context_id, root, m_model_kinematics->dcast_RenderVisual(), m_transform);

        ::Render->add_Visual(context_id, root, m_model_2_kinematics->dcast_RenderVisual(), m_transform_2);
    }

    if (!script_override_item) // можно скрывать предметы в руках во время скриптовой анимаии, но выглядит кривовато
    {
        if (m_attached_items[0])
            m_attached_items[0]->render(context_id, root);

        if (m_attached_items[1])
            m_attached_items[1]->render(context_id, root);
    }

    if (b_has_hands)
    {
        if (script_anim_item_model)
        {
            ::Render->add_Visual(context_id, root, script_anim_item_model->dcast_RenderVisual(), m_item_pos);
        }
    }
}

#include "../xr_3da/motion.h"

u32 player_hud::motion_length(const shared_str& anim_name, const shared_str& hud_name, const CMotionDef*& md, float speed)
{
    attachable_hud_item* pi = create_hud_item(hud_name);
    player_hud_motion* pm = pi->find_motion(anim_name);
    if (!pm)
        return 100; // ms TEMPORARY
    ASSERT_FMT(pm, "hudItem model [%s] has no motion with alias [%s]", hud_name.c_str(), anim_name.c_str());
    IKinematicsAnimated* motion_model = pi->m_merge_skeleton ? pi->m_model->dcast_PKinematicsAnimated() :
        (pi->m_has_separated_hands ? m_model : smart_cast<IKinematicsAnimated*>(pi->m_model));
    return motion_length(pm->params, pm->m_animations[0], md, motion_model, speed == 1.f ? pm->params.speed_k : speed);
}

u32 player_hud::motion_length(const motion_params& P, const motion_descr& M, const CMotionDef*& md, IKinematicsAnimated* itemModel, float speed)
{
    IKinematicsAnimated* model = itemModel;

    // Msg("~~[%s] model->LL_GetMotionDef [%s] [%s], hasHands = [%u]", __FUNCTION__, M.name.c_str(), model->dcast_RenderVisual()->getDebugName().c_str(), hasHands);

    md = model->LL_GetMotionDef(M.mid);
    VERIFY(md);
    if ((md->flags & esmStopAtEnd) || P.stop_at_end)
    {
        CMotion* motion = model->LL_GetRootMotion(M.mid);

        auto fStartFromTime = CalculateMotionStartSeconds(P.start_k, motion->GetLength());
        if (speed >= 0.0f)
            return iFloor(0.5f + 1000.f * (motion->GetLength() - fStartFromTime) / (md->Speed() * speed) * P.stop_k);
        else
            return iFloor(0.5f + 1000.f * (fStartFromTime) / (md->Speed() * abs(speed)) * P.stop_k);
    }
    return 0;
}

void player_hud::update(const Fmatrix& cam_trans)
{
    //Костыли для правильной работы системы коллизии худа. Это всё плохо и надо будет как-то переделать в будущем. Здесь два апдейта худа подряд делаются для того, чтобы менеджер
    //коллизи мог получить координаты ствола в обычном режиме, из которых уже будет делаться рейтрейс. skip_updated_frame тоже к этому относится.
    static bool need_update_collision{};
    need_update_collision = !need_update_collision;
    bool need_update_collision_local = need_update_collision;
    if (need_update_collision)
        this->update(cam_trans);

    Fmatrix trans = cam_trans;
    Fmatrix trans_b = cam_trans;
    
    auto attach_pos = [this](size_t part) {
        if (m_attached_items[part])
            return m_attached_items[part]->hands_attach_pos();
        else if (m_attached_items[!part])
            return m_attached_items[!part]->hands_attach_pos();
        else
            return Fvector{};
    };
    auto attach_rot = [this](size_t part) {
        if (m_attached_items[part])
            return m_attached_items[part]->hands_attach_rot();
        else if (m_attached_items[!part])
            return m_attached_items[!part]->hands_attach_rot();
        else
            return Fvector{};
    };

    Fvector m1pos = attach_pos(0);
    Fvector m1rot = attach_rot(0);

    Fvector m2pos = attach_pos(1);
    Fvector m2rot = attach_rot(1);

    Fmatrix trans_2 = trans;

    if (m_attached_items[0])
        m_attached_items[0]->m_parent_hud_item->UpdateHudAdditional(trans, need_update_collision_local);

    if (m_attached_items[1])
    {
        m_attached_items[1]->m_parent_hud_item->UpdateHudAdditional(trans_2, need_update_collision_local);
        if (!m_attached_items[0])
            trans = trans_2;
    }
    else
        trans_2 = trans;

    {
        // override hand offset for single hand animation
        if (script_anim_part == 2 || (script_anim_part && !m_attached_items[0] && !m_attached_items[1]))
        {
            m1pos = script_anim_offset[0];
            m2pos = script_anim_offset[0];
            m1rot = script_anim_offset[1];
            m2rot = script_anim_offset[1];

            trans = trans_b;
            trans_2 = trans_b;
        }
        else if (script_anim_offset_factor != 0.f)
        {
            Fvector& hand_pos = script_anim_part == 0 ? m1pos : m2pos;
            Fvector& hand_rot = script_anim_part == 0 ? m1rot : m2rot;

            hand_pos.lerp(script_anim_part == 0 ? m1pos : m2pos, script_anim_offset[0], script_anim_offset_factor);
            hand_rot.lerp(script_anim_part == 0 ? m1rot : m2rot, script_anim_offset[1], script_anim_offset_factor);

            if (script_anim_part == 0)
            {
                trans_b.inertion(trans, script_anim_offset_factor);
                trans = trans_b;
            }
            else
            {
                trans_b.inertion(trans_2, script_anim_offset_factor);
                trans_2 = trans_b;
            }
        }
    }

    // override hand offset for single hand animation
    m1rot.mul(PI / 180.f);
    m_attach_offset.setHPB(m1rot.x, m1rot.y, m1rot.z);
    m_attach_offset.translate_over(m1pos);

    m2rot.mul(PI / 180.f);
    m_attach_offset_2.setHPB(m2rot.x, m2rot.y, m2rot.z);
    m_attach_offset_2.translate_over(m2pos);

    if (need_update_collision_local)
    {
        const bool procedural_source_movement =
            m_source_skeleton_mode && !m_movement_layers.empty() && (m_attached_items[0] || m_attached_items[1]);
        if (!procedural_source_movement && m_attached_items[0] && m_attached_items[0]->m_parent_hud_item->HudBobbingAllowed())
        {
            // m_bobbing привазан к айтему только что б получить zoom factor. Зумится может только основной предмет в руках потому можно считать только по нему
            m_attached_items[0]->m_parent_hud_item->m_bobbing->Update(m_attach_offset, m_attach_offset_2);
        }
    }

    m_transform.mul(trans, m_attach_offset);
    m_transform_2.mul(trans_2, m_attach_offset_2);

    bool hasHands = (m_attached_items[0] && m_attached_items[0]->m_has_separated_hands) || (m_attached_items[1] && m_attached_items[1]->m_has_separated_hands);
    if (hasHands || script_anim_item_model)
    {
        if (m_model)
            m_model->UpdateTracks();
        m_model_kinematics->CalculateBones_Invalidate();
        m_model_kinematics->CalculateBones(TRUE);

        if (m_model_2)
            m_model_2->UpdateTracks();
        m_model_2_kinematics->CalculateBones_Invalidate();
        m_model_2_kinematics->CalculateBones(TRUE);
    }

    for (script_layer* anm : m_script_layers)
    {
        if (!anm || !anm->anm || (!anm->active && anm->blend_amount == 0.f))
            continue;

        if (need_update_collision_local)
        {
            if (anm->active)
                anm->blend_amount += Device.fTimeDelta / .4f;
            else
                anm->blend_amount -= Device.fTimeDelta / .4f;

            clamp(anm->blend_amount, 0.f, 1.f);

            if (anm->blend_amount > 0.f)
            {
                if (anm->anm->bLoop || anm->anm->m_MParam.t_current < anm->anm->m_MParam.max_t)
                    anm->anm->Update(Device.fTimeDelta);
                else
                    anm->Stop(false);
            }
            else
            {
                anm->Stop(true);
                continue;
            }
        }

        Fmatrix blend = anm->XFORM();

        if (anm->m_part == 0 || anm->m_part == 2)
        {
            const IKinematics* K = m_model_kinematics;
            const u16 bone_id = K ? K->LL_BoneID(anm->m_pivot_bone) : static_cast<u16>(-1);
            if (bone_id != static_cast<u16>(-1))
            {
                Fmatrix B = K->LL_GetTransform(bone_id);
                Fmatrix invB;
                invB.invert(B);
                Fmatrix tmp;
                tmp.mul_43(B, blend);
                tmp.mulB_43(invB);
                m_transform.mulB_43(tmp);
            }
            else
                m_transform.mulB_43(blend);
        }

        if (anm->m_part == 1 || anm->m_part == 2)
        {
            const IKinematics* K = m_model_2_kinematics;
            const u16 bone_id = K ? K->LL_BoneID(anm->m_pivot_bone) : static_cast<u16>(-1);
            if (bone_id != static_cast<u16>(-1))
            {
                Fmatrix B = K->LL_GetTransform(bone_id);
                Fmatrix invB;
                invB.invert(B);
                Fmatrix tmp;
                tmp.mul_43(B, blend);
                tmp.mulB_43(invB);
                m_transform_2.mulB_43(tmp);
            }
            else
                m_transform_2.mulB_43(blend);
        }
    }

    bool need_blend[2];
    // Source-style movement layers are procedural HUD overlays. They must be
    // active during the weapon idle state as well, otherwise ordinary walking
    // and running never reach the .anm layer. Keep the old state-dependent
    // behaviour for legacy HUD rigs.
    const bool source_movement = m_source_skeleton_mode && !m_movement_layers.empty() && (m_attached_items[0] || m_attached_items[1]);
    need_blend[0] = source_movement || (script_anim_part == 0 || script_anim_part == 2) ||
        (m_attached_items[0] && m_attached_items[0]->m_parent_hud_item->NeedBlendAnm());
    need_blend[1] = source_movement || (script_anim_part == 1 || script_anim_part == 2) ||
        (m_attached_items[1] && m_attached_items[1]->m_parent_hud_item->NeedBlendAnm());

    // Matrix composition order matters: the original setup applies the idle
    // foundation first and the selected locomotion transform on top of it.
    static constexpr eMovementLayers movement_layer_order[] = {
        eMovementAimIdle, eMovementIdle, eAimWalk, eAimCrouch, eCrouch, eWalk, eRun, eSprint};

    for (const eMovementLayers layer : movement_layer_order)
    {
        const u32 layer_idx = static_cast<u32>(layer);
        movement_layer* anm = layer_idx < m_movement_layers.size() ? m_movement_layers[layer_idx] : nullptr;
        if (!anm || !anm->anm || (!anm->active && anm->blend_amount[0] == 0.f && anm->blend_amount[1] == 0.f))
            continue;

        if (need_update_collision_local)
        {
            if (anm->active && (need_blend[0] || need_blend[1]))
            {
                if (need_blend[0])
                {
                    anm->blend_amount[0] += Device.fTimeDelta / anm->m_blend_in;

                    if (!m_attached_items[1])
                        anm->blend_amount[1] += Device.fTimeDelta / anm->m_blend_in;
                    else if (!need_blend[1])
                        anm->blend_amount[1] -= Device.fTimeDelta / anm->m_blend_out;
                }

                if (need_blend[1])
                {
                    anm->blend_amount[1] += Device.fTimeDelta / anm->m_blend_in;

                    if (!m_attached_items[0])
                        anm->blend_amount[0] += Device.fTimeDelta / anm->m_blend_in;
                    else if (!need_blend[0])
                        anm->blend_amount[0] -= Device.fTimeDelta / anm->m_blend_out;
                }
            }
            else
            {
                anm->blend_amount[0] -= Device.fTimeDelta / anm->m_blend_out;
                anm->blend_amount[1] -= Device.fTimeDelta / anm->m_blend_out;
            }

            clamp(anm->blend_amount[0], 0.f, 1.f);
            clamp(anm->blend_amount[1], 0.f, 1.f);

            if (anm->blend_amount[0] == 0.f && anm->blend_amount[1] == 0.f)
            {
                anm->Stop(true);
                continue;
            }

            anm->anm->Update(Device.fTimeDelta);
        }

        if (anm->blend_amount[0] == anm->blend_amount[1])
        {
            Fmatrix blend = anm->XFORM(0);
            m_transform.mulB_43(blend);
            m_transform_2.mulB_43(blend);
        }
        else
        {
            if (anm->blend_amount[0] > 0.f)
                m_transform.mulB_43(anm->XFORM(0));

            if (anm->blend_amount[1] > 0.f)
                m_transform_2.mulB_43(anm->XFORM(1));
        }
    }

    // Camera recoil controls the real aim direction. A separate, faster spring
    // supplies the mechanical receiver/stock kick without disturbing weapon
    // skeletal animations or the Source hands merge.
    if (CActor* actor = Actor())
    {
        Fmatrix recoil_transform;
        if (actor->weapon_recoil_hud_transform(recoil_transform))
        {
            m_transform.mulB_43(recoil_transform);
            m_transform_2.mulB_43(recoil_transform);
        }
    }

    const attachable_hud_item* scale_source = m_attached_items[0] ? m_attached_items[0] : m_attached_items[1];
    const float hud_scale = scale_source ? scale_source->m_measures.m_hud_scale : 1.f;
    if (!fsimilar(hud_scale, 1.f))
    {
        // Uniformly scale both hands and every item attached to them around
        // the HUD root. Keep the camera-space root position unchanged.
        m_transform.i.mul(hud_scale);
        m_transform.j.mul(hud_scale);
        m_transform.k.mul(hud_scale);
        m_transform_2.i.mul(hud_scale);
        m_transform_2.j.mul(hud_scale);
        m_transform_2.k.mul(hud_scale);
    }

    if (m_attached_items[0])
        m_attached_items[0]->update(true);

    if (m_attached_items[1])
        m_attached_items[1]->update(true);

    // Item animation is calculated first; the replaceable hands mesh then
    // consumes the common Source-bone transforms through merge callbacks.
    if (m_source_skeletons[0])
    {
        m_model_kinematics->CalculateBones_Invalidate();
        m_model_kinematics->CalculateBones(TRUE);
    }
    if (m_source_skeletons[1])
    {
        m_model_2_kinematics->CalculateBones_Invalidate();
        m_model_2_kinematics->CalculateBones(TRUE);
    }

    if (script_anim_item_attached && script_anim_item_model)
        update_script_item();

    {
        // single hand offset smoothing + syncing back to other hand animation on end
        if (script_anim_part != static_cast<u8>(-1))
        {
            if (need_update_collision_local)
                script_anim_offset_factor += Device.fTimeDelta * 2.5f;

            if (m_bStopAtEndAnimIsRunning && Device.dwTimeGlobal >= script_anim_end)
                script_anim_stop();
        }
        else if (need_update_collision_local)
            script_anim_offset_factor -= Device.fTimeDelta * 5.f;

        clamp(script_anim_offset_factor, 0.f, 1.f);
    }
}

u32 player_hud::anim_play(u16 part, const motion_params& P, const motion_descr& M, BOOL bMixIn, const CMotionDef*& md, float speed, bool hasHands,
    bool mergeSkeleton, IKinematicsAnimated* itemModel)
{
    // Msg("~~[%s] model->LL_GetMotionDef [%s] [%s] attached_item(0): [%p], hasHands = [%u]", __FUNCTION__, M.name.c_str(), itemModel ?
    // itemModel->dcast_RenderVisual()->getDebugName().c_str() : "", attached_item(0), hasHands); Msg("~~[%s] model->LL_GetMotionDef [%s] [%s], hasHands = [%u]", __FUNCTION__,
    // M.name.c_str(), itemModel ? itemModel->dcast_RenderVisual()->getDebugName().c_str() : "", hasHands);

    if (hasHands && !mergeSkeleton)
    {
        R_ASSERT2(m_model && m_model_2, "Legacy separated-hands animations require animated hands visuals");

        u16 part_id = u16(-1);
        if (attached_item(0) && attached_item(1))
            part_id = m_model->partitions().part_id((part == 0) ? "right_hand" : "left_hand");

        if (script_anim_part != u8(-1))
        {
            if (script_anim_part != 2)
                part_id = script_anim_part == 0 ? 1 : 0;
            else
                return 0;
        }

        if (part_id == u16(-1))
        {
            for (u8 pid = 0; pid < 3; pid++)
            {
                if (pid == 0 || pid == 2)
                {
                    CBlend* B = m_model->PlayCycle(pid, M.mid, bMixIn);
                    setup_hud_blend(B, P, speed);
                }
                if (pid == 0 || pid == 1)
                {
                    CBlend* B = m_model_2->PlayCycle(pid, M.mid, bMixIn);
                    setup_hud_blend(B, P, speed);
                }
            }

            m_model->dcast_PKinematics()->CalculateBones_Invalidate();
            m_model_2->dcast_PKinematics()->CalculateBones_Invalidate();
        }
        else if (part_id == 0 || part_id == 2)
        {
            for (u8 pid = 0; pid < 3; pid++)
            {
                if (pid != 1)
                {
                    CBlend* B = m_model->PlayCycle(pid, M.mid, bMixIn);
                    setup_hud_blend(B, P, speed);
                }
            }

            m_model->dcast_PKinematics()->CalculateBones_Invalidate();
        }
        else if (part_id == 1)
        {
            for (u8 pid = 0; pid < 3; pid++)
            {
                if (pid != 2)
                {
                    CBlend* B = m_model_2->PlayCycle(pid, M.mid, bMixIn);
                    setup_hud_blend(B, P, speed);
                }
            }

            m_model_2->dcast_PKinematics()->CalculateBones_Invalidate();
        }
    }

    return motion_length(P, M, md, mergeSkeleton ? itemModel : (hasHands ? m_model : itemModel), speed);
}

attachable_hud_item* player_hud::create_hud_item(const shared_str& sect)
{
    auto it = m_pool.begin();
    auto it_e = m_pool.end();
    for (; it != it_e; ++it)
    {
        attachable_hud_item* itm = *it;
        if (itm->m_sect_name == sect)
            return itm;
    }
    attachable_hud_item* res = xr_new<attachable_hud_item>(this);
    res->load(sect);
    m_pool.push_back(res);

    return res;
}

bool player_hud::allow_activation(CHudItem* item)
{
    if (m_attached_items[1])
        return m_attached_items[1]->m_parent_hud_item->CheckCompatibility(item);
    else
        return true;
}

void player_hud::attach_item(CHudItem* item)
{
    attachable_hud_item* pi = create_hud_item(item->HudSection());
    int item_idx = pi->m_attach_place_idx;

    if (m_attached_items[item_idx] != pi || pi->m_parent_hud_item != item)
    {
        if (m_attached_items[item_idx])
            m_attached_items[item_idx]->m_parent_hud_item->on_b_hud_detach();
        m_attached_items[item_idx] = pi;
        pi->m_parent_hud_item = item;

        if (item_idx == 0 && m_attached_items[1])
            m_attached_items[1]->m_parent_hud_item->CheckCompatibility(item);

        pi->reload_motions();
        item->on_a_hud_attach();

        updateMovementLayerState();
    }
    pi->m_parent_hud_item = item;
    refresh_source_skeleton_merge();
}

void player_hud::detach_item_idx(u16 idx)
{
    if (!attached_item(idx))
        return;

    const bool hasHands = attached_item(idx)->m_has_separated_hands;
    const bool mergedSkeleton = attached_item(idx)->m_merge_skeleton;

    m_attached_items[idx]->m_parent_hud_item->on_b_hud_detach();

    m_attached_items[idx]->m_parent_hud_item = nullptr;
    m_attached_items[idx] = nullptr;

    if (hasHands && !mergedSkeleton)
    {
        if (idx == 1)
        {
            if (m_attached_items[0])
                re_sync_anim(2);
            else
            {
                m_model_2->PlayCycle("hand_idle_doun");
            }
        }
        else if (idx == 0)
        {
            if (m_attached_items[1])
            {
                // fix for a rare case where the right hand stays visible on screen after detaching the right hand's attached item
                player_hud_motion* pm = m_attached_items[1]->find_motion("anm_idle");
                if (pm)
                {
                    const motion_descr& M = pm->m_animations[0];
                    m_model->PlayCycle(0, M.mid, false);
                    m_model->PlayCycle(2, M.mid, false);
                }
            }
            else
            {
                m_model->PlayCycle("hand_idle_doun");
            }
        }

        if (!m_attached_items[0] && !m_attached_items[1])
        {
            m_model->PlayCycle("hand_idle_doun");
            m_model_2->PlayCycle("hand_idle_doun");
        }
    }

    refresh_source_skeleton_merge();

    // KRodin: закомментировал этот кусок, не понятно для чего он может быть нужен.
    /*else if (idx == 0 && attached_item(1))
    {
        OnMovementChanged(mcAnyMove);
    }*/
}

void player_hud::detach_all_items()
{
    m_attached_items[0] = nullptr;
    m_attached_items[1] = nullptr;
    refresh_source_skeleton_merge();
}

void player_hud::detach_item(CHudItem* item)
{
    if (nullptr == item->HudItemData())
        return;

    u16 item_idx = item->HudItemData()->m_attach_place_idx;
    if (m_attached_items[item_idx] == item->HudItemData())
        detach_item_idx(item_idx);
}

void player_hud::calc_transform(u16 attach_slot_idx, const Fmatrix& offset, Fmatrix& result, bool merged_skeleton)
{
    bool hasHands = m_attached_items[attach_slot_idx] && m_attached_items[attach_slot_idx]->m_has_separated_hands;

    if (merged_skeleton)
    {
        // Source weapon animations and the hands mesh share model space. The
        // weapon therefore attaches at the HUD root instead of a hand anchor.
        result.set(attach_slot_idx == 0 ? m_transform : m_transform_2);
        result.mulB_43(offset);
    }
    else if (hasHands || script_anim_item_model)
    {
        IKinematics* kin = (attach_slot_idx == 0) ? m_model_kinematics : m_model_2_kinematics;
        Fmatrix ancor_m = kin->LL_GetTransform(m_ancors.at(attach_slot_idx));
        result.mul((attach_slot_idx == 0) ? m_transform : m_transform_2, ancor_m);
        result.mulB_43(offset);
    }
    else
    {
        result.set(m_transform);
        result.mulB_43(offset);
    }
}

void player_hud::OnMovementChanged(ACTOR_DEFS::EMoveCommand cmd)
{
    if (cmd == 0)
    {
        if (m_attached_items[0])
        {
            if (m_attached_items[0]->m_parent_hud_item->GetState() == CHUDState::eIdle)
                m_attached_items[0]->m_parent_hud_item->PlayAnimIdle();
        }
        if (m_attached_items[1])
        {
            if (m_attached_items[1]->m_parent_hud_item->GetState() == CHUDState::eIdle)
                m_attached_items[1]->m_parent_hud_item->PlayAnimIdle();
        }
    }
    else
    {
        if (m_attached_items[0])
            m_attached_items[0]->m_parent_hud_item->OnMovementChanged(cmd);

        if (m_attached_items[1])
            m_attached_items[1]->m_parent_hud_item->OnMovementChanged(cmd);
    }

    updateMovementLayerState();
}

// sync anim of other part to selected part (1 = sync to left hand anim; 2 = sync to right hand anim)
void player_hud::re_sync_anim(u8 part)
{
    if (!m_model || !m_model_2)
        return;

    u32 bc = part == 1 ? m_model_2->LL_PartBlendsCount(part) : m_model->LL_PartBlendsCount(part);
    for (u32 bidx = 0; bidx < bc; ++bidx)
    {
        CBlend* BR = part == 1 ? m_model_2->LL_PartBlend(part, bidx) : m_model->LL_PartBlend(part, bidx);
        if (!BR)
            continue;

        MotionID M = BR->motionID;

        u16 pc = m_model->partitions().count(); // same on both armatures
        for (u16 pid = 0; pid < pc; ++pid)
        {
            if (pid == 0)
            {
                CBlend* B = m_model->PlayCycle(0, M, TRUE);
                B->timeCurrent = BR->timeCurrent;
                B->speed = BR->speed;
                B = m_model_2->PlayCycle(0, M, TRUE);
                B->timeCurrent = BR->timeCurrent;
                B->speed = BR->speed;
            }
            else if (pid != part)
            {
                CBlend* B = part == 1 ? m_model->PlayCycle(pid, M, TRUE) : m_model_2->PlayCycle(pid, M, TRUE);
                B->timeCurrent = BR->timeCurrent;
                B->speed = BR->speed;
            }
        }
    }
}

void player_hud::GetLHandBoneOffsetPosDir(const shared_str& bone_name, Fvector& dest_pos, Fvector& dest_dir, const Fvector& offset)
{
    const u16 bone_id = m_model_2_kinematics->LL_BoneID(bone_name);
    ASSERT_FMT(bone_id != BI_NONE, "!![%s] bone [%s] not found in weapon [%s]", __FUNCTION__, bone_name.c_str(), m_sect_name.c_str());
    Fmatrix& fire_mat = m_model_2_kinematics->LL_GetTransform(bone_id);
    fire_mat.transform_tiny(dest_pos, offset);
    m_transform_2.transform_tiny(dest_pos);
    dest_pos.add(Device.vCameraPosition);
    dest_dir.set(0.f, 0.f, 1.f);
    m_transform_2.transform_dir(dest_dir);
}

void player_hud::setup_thumb_callbacks()
{
    if (!m_model_kinematics)
        return;

    IKinematics* hands = m_model_kinematics;
    const LPCSTR thumb0_name = m_source_skeleton_mode ? source_r_thumb0 : "r_finger0";
    const LPCSTR thumb01_name = m_source_skeleton_mode ? source_r_thumb01 : "r_finger01";
    const LPCSTR thumb02_name = m_source_skeleton_mode ? source_r_thumb02 : "r_finger02";

    const u16 thumb0 = hud_bone_id(hands, thumb0_name);
    const u16 thumb01 = hud_bone_id(hands, thumb01_name);
    const u16 thumb02 = hud_bone_id(hands, thumb02_name);
    ASSERT_FMT(thumb0 != BI_NONE && thumb01 != BI_NONE && thumb02 != BI_NONE, "[%s]: right thumb bones not found in HUD visual [%s]", __FUNCTION__,
        hands->getDebugName().c_str());

    hands->LL_GetBoneInstance(thumb0).set_callback(bctCustom, Thumb0Callback, this);
    hands->LL_GetBoneInstance(thumb01).set_callback(bctCustom, Thumb01Callback, this);
    hands->LL_GetBoneInstance(thumb02).set_callback(bctCustom, Thumb02Callback, this);
}

void player_hud::clear_source_skeleton_merge()
{
    IKinematics* targets[] = {
        m_model_kinematics,
        m_model_2_kinematics,
    };

    for (IKinematics* target : targets)
    {
        if (!target)
            continue;

        for (u16 bone_id = 0; bone_id < target->LL_BoneCount(); ++bone_id)
        {
            CBoneInstance& bone = target->LL_GetBoneInstance(bone_id);
            if (bone.callback_param() == this &&
                (bone.callback() == SourceBoneMergeCallback0 || bone.callback() == SourceBoneMergeCallback1))
            {
                bone.reset_callback();
            }
        }
    }

    m_source_skeletons[0] = nullptr;
    m_source_skeletons[1] = nullptr;
}

void player_hud::refresh_source_skeleton_merge()
{
    clear_source_skeleton_merge();
    setup_thumb_callbacks();

    if (!m_source_skeleton_mode)
        return;

    auto item_skeleton = [this](u16 idx) -> IKinematics* {
        attachable_hud_item* item = m_attached_items[idx];
        return item && item->m_merge_skeleton ? item->m_model : nullptr;
    };

    IKinematics* main_source = item_skeleton(0);
    IKinematics* offhand_source = item_skeleton(1);
    IKinematics* script_source = script_anim_item_model ? script_anim_item_model->dcast_PKinematics() : nullptr;

    // The main item normally drives both hands. If a Source-rigged off-hand
    // item is present, its animation is authoritative for the left hand.
    IKinematics* sources[] = {
        main_source ? main_source : offhand_source,
        offhand_source ? offhand_source : main_source,
    };
    if (is_source_hud_skeleton(script_source))
    {
        if (script_anim_part == 0 || script_anim_part == 2)
            sources[0] = script_source;
        if (script_anim_part == 1 || script_anim_part == 2)
            sources[1] = script_source;
    }
    IKinematics* targets[] = {m_model_kinematics, m_model_2_kinematics};
    const BoneCallback callbacks[] = {SourceBoneMergeCallback0, SourceBoneMergeCallback1};

    for (u16 target_idx = 0; target_idx < 2; ++target_idx)
    {
        IKinematics* source = sources[target_idx];
        IKinematics* target = targets[target_idx];
        if (!source || !is_source_hud_skeleton(source))
            continue;

        u16 merged_bones = 0;
        for (u16 target_bone_id = 0; target_bone_id < target->LL_BoneCount(); ++target_bone_id)
        {
            LPCSTR bone_name = target->LL_BoneName(target_bone_id);
            if (!is_source_arm_bone(bone_name))
                continue;

            const u16 source_bone_id = source->LL_BoneID(bone_name);
            if (source_bone_id == BI_NONE)
                continue; // Optional mesh helper bones can exist only in the hands skeleton.

            CBoneInstance& target_bone = target->LL_GetBoneInstance(target_bone_id);
            target_bone.set_param(0, static_cast<float>(source_bone_id));
            target_bone.set_param(1, 0.f);
            target_bone.set_param(2, static_cast<float>(target_bone_id));
            if (target_idx == 0)
            {
                if (!xr_strcmp(bone_name, source_r_thumb0))
                    target_bone.set_param(1, 1.f);
                else if (!xr_strcmp(bone_name, source_r_thumb01))
                    target_bone.set_param(1, 2.f);
                else if (!xr_strcmp(bone_name, source_r_thumb02))
                    target_bone.set_param(1, 3.f);
            }
            target_bone.set_callback(bctCustom, callbacks[target_idx], this, TRUE);
            ++merged_bones;
        }

        m_source_skeletons[target_idx] = source;
        MsgDbg("HUD Source skeleton merge: [%s] drives [%s], %u bones matched by name", source->getDebugName().c_str(), target->getDebugName().c_str(),
            merged_bones);
    }
}

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

        // Convert the source pose to a parent-local transform first. Retargeting
        // the global skinning matrix directly also transfers the source rig's
        // proportions, which bends/stretches a replacement hands mesh whenever
        // its forearm, wrist or finger lengths differ even slightly.
        Fmatrix source_local;
        if (const u16 source_parent_id = source_data.GetParentID(); source_parent_id != BI_NONE)
        {
            Fmatrix source_parent_inverse;
            source_parent_inverse.invert(source->LL_GetBoneInstance(source_parent_id).mTransform);
            source_local.mul_43(source_parent_inverse, source->LL_GetBoneInstance(source_bone_id).mTransform);
        }
        else
        {
            source_local.set(source->LL_GetBoneInstance(source_bone_id).mTransform);
        }

        // Remove the source bind pose. Source SMD tracks often contain a
        // translation for every joint even when that translation merely stores
        // bone length. For ordinary arm descendants we transfer only the
        // rotational delta and keep the target skeleton's bind translation.
        Fmatrix source_bind_inverse, source_delta;
        source_bind_inverse.invert(source_data.bind_transform);
        source_delta.mul_43(source_bind_inverse, source_local);

        Fquaternion delta_rotation;
        delta_rotation.set(source_delta);
        Fmatrix rotation_delta;
        rotation_delta.rotation(delta_rotation);
        rotation_delta.c.set(0.f, 0.f, 0.f);

        Fmatrix target_local;
        target_local.mul_43(target_data.bind_transform, rotation_delta);
        target_local.c.set(target_data.bind_transform.c);

        // Spine4 is the logical arm root used by the HUD merge. Preserve its
        // authored local translation delta so whole-hand motion is not lost;
        // all descendants retain target bone lengths.
        LPCSTR target_bone_name = target->LL_BoneName(target_bone_id);
        if (target_bone_name && !xr_strcmp(target_bone_name, source_root_bone))
        {
            Fvector translation_delta = source_delta.c;
            target_data.bind_transform.transform_dir(translation_delta);
            target_local.c.add(translation_delta);
        }

        if (const u16 target_parent_id = target_data.GetParentID(); target_parent_id != BI_NONE)
            target_bone->mTransform.mul_43(target->LL_GetBoneInstance(target_parent_id).mTransform, target_local);
        else
            target_bone->mTransform.set(target_local);
    }

    // Preserve the existing procedural right-thumb adjustment after the
    // animation pose has been copied from the Source skeleton.
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

    // Slot zero is the authoritative Source animation for the complete weapon
    // rig. refresh_source_skeleton_merge also falls back to the off-hand item
    // here when no main item is attached.
    IKinematics* source = m_source_skeletons[0];
    if (!m_source_skeleton_mode || !source)
        return false;

    const u16 camera_bone_id = source->LL_BoneID(source_camera_bone);
    if (camera_bone_id == BI_NONE)
        return false;

    const CBoneData& camera_data = source->LL_GetData(camera_bone_id);

    // m2b_transform is inverse(global bind). The product is therefore the
    // animated Camera delta. Rebuild it from a quaternion to remove both the
    // animated translation and any numerical scale/shear.
    Fmatrix camera_delta;
    camera_delta.mul_43(source->LL_GetBoneInstance(camera_bone_id).mTransform, camera_data.m2b_transform);

    Fquaternion camera_rotation;
    camera_rotation.set(camera_delta);
    rotation.rotation(camera_rotation);
    rotation.c.set(0.f, 0.f, 0.f);
    return true;
}

void player_hud::SourceBoneMergeCallback0(CBoneInstance* B)
{
    static_cast<player_hud*>(B->callback_param())->copy_source_bone(0, B);
}

void player_hud::SourceBoneMergeCallback1(CBoneInstance* B)
{
    static_cast<player_hud*>(B->callback_param())->copy_source_bone(1, B);
}

void player_hud::Thumb0Callback(CBoneInstance* B)
{
    player_hud* P = static_cast<player_hud*>(B->callback_param());

    Fvector& target = P->target_thumb0rot;
    Fvector& current = P->thumb0rot;

    if (!target.similar(current))
    {
        Fvector diff[2];
        diff[0] = target;
        diff[0].sub(current);
        diff[0].mul(Device.fTimeDelta / .1f);
        current.add(diff[0]);
    }
    else
        current.set(target);

    Fmatrix rotation;
    rotation.identity();
    rotation.rotateX(current.x);

    Fmatrix rotation_y;
    rotation_y.identity();
    rotation_y.rotateY(current.y);
    rotation.mulA_43(rotation_y);

    rotation_y.identity();
    rotation_y.rotateZ(current.z);
    rotation.mulA_43(rotation_y);

    B->mTransform.mulB_43(rotation);
}

void player_hud::Thumb01Callback(CBoneInstance* B)
{
    player_hud* P = static_cast<player_hud*>(B->callback_param());

    Fvector& target = P->target_thumb01rot;
    Fvector& current = P->thumb01rot;

    if (!target.similar(current))
    {
        Fvector diff[2];
        diff[0] = target;
        diff[0].sub(current);
        diff[0].mul(Device.fTimeDelta / .1f);
        current.add(diff[0]);
    }
    else
        current.set(target);

    Fmatrix rotation;
    rotation.identity();
    rotation.rotateX(current.x);

    Fmatrix rotation_y;
    rotation_y.identity();
    rotation_y.rotateY(current.y);
    rotation.mulA_43(rotation_y);

    rotation_y.identity();
    rotation_y.rotateZ(current.z);
    rotation.mulA_43(rotation_y);

    B->mTransform.mulB_43(rotation);
}

void player_hud::Thumb02Callback(CBoneInstance* B)
{
    player_hud* P = static_cast<player_hud*>(B->callback_param());

    Fvector& target = P->target_thumb02rot;
    Fvector& current = P->thumb02rot;

    if (!target.similar(current))
    {
        Fvector diff[2];
        diff[0] = target;
        diff[0].sub(current);
        diff[0].mul(Device.fTimeDelta / .1f);
        current.add(diff[0]);
    }
    else
        current.set(target);

    Fmatrix rotation;
    rotation.identity();
    rotation.rotateX(current.x);

    Fmatrix rotation_y;
    rotation_y.identity();
    rotation_y.rotateY(current.y);
    rotation.mulA_43(rotation_y);

    rotation_y.identity();
    rotation_y.rotateZ(current.z);
    rotation.mulA_43(rotation_y);

    B->mTransform.mulB_43(rotation);
}

bool player_hud::allow_script_anim()
{
    if (m_attached_items[0] && (m_attached_items[0]->m_parent_hud_item->IsPending() || m_attached_items[0]->m_parent_hud_item->GetState() == CHudItem::EHudStates::eBore))
        return false;
    else if (m_attached_items[1] && (m_attached_items[1]->m_parent_hud_item->IsPending() || m_attached_items[1]->m_parent_hud_item->GetState() == CHudItem::EHudStates::eBore))
        return false;
    else if (script_anim_part != u8(-1))
        return false;

    return true;
}

void player_hud::load_script(LPCSTR section)
{
    script_override_arms = false;
    load(section, true);
    script_override_arms = true;
}

u32 player_hud::script_anim_play(u8 hand, LPCSTR hud_section, LPCSTR anm_name, bool bMixIn, float speed, bool bOverride_item)
{
    if (!pSettings->section_exist(hud_section))
    {
        Msg("! script motion section [%s] does not exist", hud_section);
        m_bStopAtEndAnimIsRunning = true;
        script_anim_end = Device.dwTimeGlobal;

        return 0;
    }

    xr_string pos = "hands_position";
    xr_string rot = "hands_orientation";

    if (UI()->is_widescreen())
    {
        pos.append("_16x9");
        rot.append("_16x9");
    }

    Fvector def = {0.f, 0.f, 0.f};
    Fvector offs = READ_IF_EXISTS(pSettings, r_fvector3, hud_section, pos.c_str(), def);
    Fvector rrot = READ_IF_EXISTS(pSettings, r_fvector3, hud_section, rot.c_str(), def);

    if (pSettings->line_exist(hud_section, "item_visual"))
    {
        ::Render->hud_loading = true;
        script_anim_item_model = ::Render->model_Create(pSettings->r_string(hud_section, "item_visual"))->dcast_PKinematicsAnimated();
        ::Render->hud_loading = false;

        item_pos[0] = READ_IF_EXISTS(pSettings, r_fvector3, hud_section, "item_position", def);
        item_pos[1] = READ_IF_EXISTS(pSettings, r_fvector3, hud_section, "item_orientation", def);
        script_anim_item_attached = READ_IF_EXISTS(pSettings, r_bool, hud_section, "item_attached", true);
        m_attach_idx = READ_IF_EXISTS(pSettings, r_u8, hud_section, "attach_place_idx", 0);

        if (!script_anim_item_attached)
        {
            Fmatrix attach_offs;
            Fvector ypr = item_pos[1];
            ypr.mul(PI / 180.f);
            attach_offs.setHPB(ypr.x, ypr.y, ypr.z);
            attach_offs.translate_over(item_pos[0]);
            m_item_pos = attach_offs;
        }
    }
    else
        script_anim_item_model = nullptr;

    script_anim_offset[0] = offs;
    script_anim_offset[1] = rrot;
    script_anim_part = hand;

    const bool merge_script_skeleton = !m_model && script_anim_item_model && is_source_hud_skeleton(script_anim_item_model->dcast_PKinematics());
    if (!m_model && !merge_script_skeleton)
    {
        Msg("! script motion [%s] requires an animated item_visual when rigid Source hands are active", anm_name);
        m_bStopAtEndAnimIsRunning = true;
        script_anim_end = Device.dwTimeGlobal;
        return 0;
    }

    refresh_source_skeleton_merge();
    player_hud_motion_container* pm = get_hand_motions(hud_section, script_anim_item_model, merge_script_skeleton);
    player_hud_motion* phm = pm->find_motion(anm_name);

    if (!phm)
    {
        Msg("! script motion [%s] not found in section [%s]", anm_name, hud_section);
        m_bStopAtEndAnimIsRunning = true;
        script_anim_end = Device.dwTimeGlobal;

        return 0;
    }

    const motion_descr& M = phm->m_animations[Random.randI(phm->m_animations.size())];

    if (script_anim_item_model)
    {
        MotionID M2;

        if (phm->m_base_name != phm->m_additional_name)
        {
            u8 rnd_idx2 = 0;

            if (false) // randomAnim
                rnd_idx2 = (u8)Random.randI(phm->m_additional_animations.size());
            motion_descr& additional = phm->m_additional_animations[rnd_idx2];

            if (bDebug)
                Msg("playing item animation [%s]", additional.name.c_str());

            M2 = script_anim_item_model->ID_Cycle_Safe(additional.name);
        }
        else
        {
            shared_str item_anm_name = M.name;

            if (bDebug)
                Msg("playing item animation [%s]", item_anm_name.c_str());

            M2 = script_anim_item_model->ID_Cycle_Safe(item_anm_name);
        }

        if (!M2.valid())
            M2 = script_anim_item_model->ID_Cycle_Safe("idle");

        R_ASSERT(M2.valid(), "model %s has no motion [idle] ", pSettings->r_string(hud_section, "item_visual"));

        if (!merge_script_skeleton)
        {
            u16 root_id = script_anim_item_model->dcast_PKinematics()->LL_GetBoneRoot();
            CBoneInstance& root_binst = script_anim_item_model->dcast_PKinematics()->LL_GetBoneInstance(root_id);
            root_binst.set_callback_overwrite(TRUE);
            root_binst.mTransform.identity();
        }

        u16 pc = script_anim_item_model->partitions().count();
        for (u16 pid = 0; pid < pc; ++pid)
        {
            CBlend* B = script_anim_item_model->PlayCycle(pid, M2, bMixIn);
            setup_hud_blend(B, phm->params, speed);
        }

        script_anim_item_model->dcast_PKinematics()->CalculateBones_Invalidate();
    }

    if (!merge_script_skeleton && hand == 0) // right hand
    {
        CBlend* B = m_model->PlayCycle(0, M.mid, bMixIn);
        setup_hud_blend(B, phm->params, speed);
        B = m_model->PlayCycle(2, M.mid, bMixIn);
        setup_hud_blend(B, phm->params, speed);
    }
    else if (!merge_script_skeleton && hand == 1) // left hand
    {
        CBlend* B = m_model_2->PlayCycle(0, M.mid, bMixIn);
        setup_hud_blend(B, phm->params, speed);
        B = m_model_2->PlayCycle(1, M.mid, bMixIn);
        setup_hud_blend(B, phm->params, speed);
    }
    else if (!merge_script_skeleton && hand == 2) // both hands
    {
        CBlend* B = m_model->PlayCycle(0, M.mid, bMixIn);
        setup_hud_blend(B, phm->params, speed);
        B = m_model_2->PlayCycle(0, M.mid, bMixIn);
        setup_hud_blend(B, phm->params, speed);
        B = m_model->PlayCycle(2, M.mid, bMixIn);
        setup_hud_blend(B, phm->params, speed);
        B = m_model_2->PlayCycle(1, M.mid, bMixIn);
        setup_hud_blend(B, phm->params, speed);
    }

    const CMotionDef* md;
    IKinematicsAnimated* motion_model = merge_script_skeleton ? script_anim_item_model : m_model;
    u32 length = motion_length(phm->params, M, md, motion_model, speed);

    if (length > 0)
    {
        script_override_item = bOverride_item;

        m_bStopAtEndAnimIsRunning = true;
        script_anim_end = Device.dwTimeGlobal + length;
    }
    else
        m_bStopAtEndAnimIsRunning = false;

    updateMovementLayerState();

    return length;
}

void player_hud::script_anim_stop()
{
    u8 part = script_anim_part;
    script_anim_part = u8(-1);
    script_anim_item_model = nullptr;
    script_override_item = false;
    refresh_source_skeleton_merge();

    updateMovementLayerState();

    if (part != 2 && !m_attached_items[part])
        re_sync_anim(part + 1);
    else
        OnMovementChanged((ACTOR_DEFS::EMoveCommand)0);
}

u32 player_hud::motion_length_script(LPCSTR hud_section, LPCSTR anm_name, float speed)
{
    if (!pSettings->section_exist(hud_section))
    {
        Msg("! script motion section [%s] does not exist", hud_section);
        return 0;
    }

    IKinematicsAnimated* animatedHudItem = NULL;

    if (pSettings->line_exist(hud_section, "item_visual"))
    {
        ::Render->hud_loading = true;
        animatedHudItem = ::Render->model_Create(pSettings->r_string(hud_section, "item_visual"))->dcast_PKinematicsAnimated();
        ::Render->hud_loading = false;
    }

    const bool merge_skeleton = !m_model && animatedHudItem && is_source_hud_skeleton(animatedHudItem->dcast_PKinematics());
    player_hud_motion_container* pm = get_hand_motions(hud_section, animatedHudItem, merge_skeleton);
    if (!pm)
        return 0;

    player_hud_motion* phm = pm->find_motion(anm_name);
    if (!phm)
    {
        Msg("! script motion [%s] not found in section [%s]", anm_name, hud_section);
        return 0;
    }

    const CMotionDef* md;
    return motion_length(phm->params, phm->m_animations[0], md, merge_skeleton ? animatedHudItem : m_model, speed);
}

void player_hud::update_script_item()
{
    Fvector ypr = item_pos[1];
    ypr.mul(PI / 180.f);
    m_attach_offset.setHPB(ypr.x, ypr.y, ypr.z);
    m_attach_offset.translate_over(item_pos[0]);

    calc_transform(m_attach_idx, m_attach_offset, m_item_pos);

    if (script_anim_item_model)
    {
        script_anim_item_model->UpdateTracks();
        script_anim_item_model->dcast_PKinematics()->CalculateBones_Invalidate();
        script_anim_item_model->dcast_PKinematics()->CalculateBones(TRUE);
    }
}

void player_hud::updateMovementLayerState()
{
    if (!m_movement_layers.size())
        return;

    CActor* pActor = Actor();

    if (!pActor)
        return;

    for (movement_layer* anm : m_movement_layers)
    {
        if (anm)
            anm->Stop(false);
    }

    const bool source_movement = m_source_skeleton_mode && !m_movement_layers.empty() && (m_attached_items[0] || m_attached_items[1]);
    bool need_blend = (source_movement || script_anim_part != u8(-1)
            || (m_attached_items[0] && m_attached_items[0]->m_parent_hud_item->NeedBlendAnm()) 
            || (m_attached_items[1] && m_attached_items[1]->m_parent_hud_item->NeedBlendAnm()));

    const auto play_layer = [this](const eMovementLayers layer)
    {
        const u32 layer_idx = static_cast<u32>(layer);
        if (layer_idx < m_movement_layers.size() && m_movement_layers[layer_idx])
            m_movement_layers[layer_idx]->Play();
    };

    CWeapon* wep = nullptr;
    if (m_attached_items[0] && m_attached_items[0]->m_parent_hud_item->object().cast_weapon())
        wep = m_attached_items[0]->m_parent_hud_item->object().cast_weapon();

    // The original procedural movement setup always plays an idle foundation
    // and adds the current locomotion layer on top of it.
    if (need_blend)
        play_layer(wep && wep->IsZoomed() ? eMovementAimIdle : eMovementIdle);

    if (pActor->AnyMove() && need_blend)
    {
        CEntity::SEntityState state;
        pActor->g_State(state);

        if (wep && wep->IsZoomed())
            play_layer(state.bCrouch ? eAimCrouch : eAimWalk);
        else if (state.bCrouch)
            play_layer(eCrouch);
        else if (state.bSprint)
            play_layer(eSprint);
        else if (!isActorAccelerated(pActor->MovingState(), false))
            play_layer(eWalk);
        else
            play_layer(eRun);
    }
}


float player_hud::PlayBlendAnm(LPCSTR name, u8 part, float speed, float power, bool bLooped, bool no_restart, LPCSTR pivot_bone)
{
    for (script_layer* anm : m_script_layers)
    {
        if (!xr_strcmp(*anm->m_name, name))
        {
            if (!no_restart)
            {
                anm->anm->Stop();
                anm->blend_amount = 0.f;
                anm->blend.identity();
            }

            if (!anm->anm->IsPlaying())
                anm->anm->Play(bLooped);

            anm->anm->bLoop = bLooped;
            anm->m_part = part;
            anm->anm->Speed() = speed;
            anm->m_power = power;
            anm->active = true;
            anm->m_pivot_bone = pivot_bone;
            return (anm->anm->m_MParam.max_t - anm->anm->m_MParam.t_current) / anm->anm->Speed();
        }
    }

    script_layer* anm = xr_new<script_layer>(name, part, speed, power, bLooped, pivot_bone);
    m_script_layers.push_back(anm);
    return (anm->anm->m_MParam.max_t - anm->anm->m_MParam.t_current) / anm->anm->Speed();
}

void player_hud::StopBlendAnm(LPCSTR name, bool bForce)
{
    for (script_layer* anm : m_script_layers)
    {
        if (!xr_strcmp(*anm->m_name, name))
        {
            anm->Stop(bForce);
            return;
        }
    }
}

void player_hud::StopAllBlendAnms(bool bForce)
{
    for (script_layer* anm : m_script_layers)
    {
        anm->Stop(bForce);
    }
}

float player_hud::SetBlendAnmTime(LPCSTR name, float time)
{
    for (script_layer* anm : m_script_layers)
    {
        if (!xr_strcmp(*anm->m_name, name))
        {
            if (!anm->anm->IsPlaying())
                return 0;

            float speed = (anm->anm->m_MParam.max_t - anm->anm->m_MParam.t_current) / time;
            anm->anm->Speed() = speed;
            return speed;
        }
    }

    return 0;
}


player_hud_motion_container* player_hud::get_hand_motions(LPCSTR section, IKinematicsAnimated* animatedHudItem, bool merge_skeleton)
{
    for (hand_motions& phm : _m_hand_motions)
    {
        if (phm.section == section)
            return &phm.pm;
    }

    hand_motions& res = _m_hand_motions.emplace_back();
    res.section = section;
    res.pm.load(true, merge_skeleton, m_model, animatedHudItem, section);

    return &res.pm;
}