////////////////////////////////////////////////////////////////////////////
//	Module 		: embedded_editor_hud.cpp
//	Created 	: 05.05.2021
//  Modified 	: 07.07.2025
//	Author		: Dance Maniac (M.F.S. Team)
//	Description : ImGui Hud Editor
////////////////////////////////////////////////////////////////////////////

#include "stdAfx.h"
#include "embedded_editor_hud.h"
#include "embedded_editor_helper.h"
#include "../../XR_3DA/device.h"
#include "../player_hud.h"
#include "../Weapon.h"
#include "../Inventory.h"
#include "../inventory_item.h"

namespace
{
void RenderBoneAdjustments(u16 item_idx)
{
    static shared_str selected_bone[2];
    xr_vector<shared_str> bones;
    hud_collect_adjustable_bones(item_idx, bones);

    if (bones.empty())
    {
        ImGui::TextDisabled("No skeletal bones available");
        return;
    }

    auto selected_it = std::find_if(bones.begin(), bones.end(), [&](const shared_str& bone) {
        return selected_bone[item_idx].c_str() && !_stricmp(bone.c_str(), selected_bone[item_idx].c_str());
    });
    if (selected_it == bones.end())
        selected_bone[item_idx] = bones.front();

    ImGui::PushID(static_cast<int>(item_idx));
    if (ImGui::BeginCombo("Bone", selected_bone[item_idx].c_str()))
    {
        for (const shared_str& bone : bones)
        {
            const bool selected = !_stricmp(bone.c_str(), selected_bone[item_idx].c_str());
            if (ImGui::Selectable(bone.c_str(), selected))
                selected_bone[item_idx] = bone;
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    Fvector position{}, rotation{};
    shared_str config_section;
    if (hud_get_bone_adjustment(item_idx, selected_bone[item_idx].c_str(), position, rotation, &config_section))
    {
        bool changed = ImGui::DragFloat3("Bone position", (float*)&position, 0.0001f, 0.f, 0.f, "%.6f");
        changed |= ImGui::DragFloat3("Bone rotation (deg)", (float*)&rotation, 0.05f, 0.f, 0.f, "%.3f");

        if (ImGui::Button("Reset bone transform"))
        {
            position.set(0.f, 0.f, 0.f);
            rotation.set(0.f, 0.f, 0.f);
            changed = true;
        }

        if (changed)
            hud_set_bone_adjustment(item_idx, selected_bone[item_idx].c_str(), position, rotation);

        ImGui::TextDisabled("Config section: [%s]", config_section.c_str());
        ImGui::TextDisabled("bone_position_%s", selected_bone[item_idx].c_str());
        ImGui::TextDisabled("bone_rotation_%s", selected_bone[item_idx].c_str());
    }
    ImGui::PopID();
}

void RenderAddonTransforms(CWeapon* weapon, float drag_intensity)
{
    if (!weapon)
        return;

    static int selected_visual = -1;
    static bool hud_mode = true;
    xr_vector<u8> installed;
    for (u8 index = 0; index < CWeapon::AddonVisualCount; ++index)
    {
        shared_str section, slot, parent;
        Fvector position{}, rotation{};
        float scale = 1.f;
        if (weapon->GetAddonEditorTransform(index, hud_mode, section, slot, parent, position, rotation, scale))
            installed.push_back(index);
    }

    if (installed.empty())
    {
        selected_visual = -1;
        ImGui::TextDisabled("No separately rendered addons installed");
        return;
    }
    if (std::find(installed.begin(), installed.end(), static_cast<u8>(selected_visual)) == installed.end())
        selected_visual = installed.front();

    ImGui::Checkbox("HUD render space", &hud_mode);
    shared_str selected_section, selected_slot, selected_parent;
    Fvector position{}, rotation{};
    float scale = 1.f;
    weapon->GetAddonEditorTransform(static_cast<u8>(selected_visual), hud_mode, selected_section, selected_slot, selected_parent,
        position, rotation, scale);

    string256 preview{};
    xr_sprintf(preview, "%s: %s", selected_slot.c_str(), selected_section.c_str());
    if (ImGui::BeginCombo("Addon", preview))
    {
        for (u8 index : installed)
        {
            shared_str section, slot, parent;
            Fvector candidate_position{}, candidate_rotation{};
            float candidate_scale = 1.f;
            weapon->GetAddonEditorTransform(index, hud_mode, section, slot, parent, candidate_position, candidate_rotation, candidate_scale);
            string256 label{};
            xr_sprintf(label, "%s: %s", slot.c_str(), section.c_str());
            const bool selected = index == selected_visual;
            if (ImGui::Selectable(label, selected))
                selected_visual = index;
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // Refresh after a selection made inside the combo.
    weapon->GetAddonEditorTransform(static_cast<u8>(selected_visual), hud_mode, selected_section, selected_slot, selected_parent,
        position, rotation, scale);
    const bool config_hud_mode = weapon->AddonEditorUsesHudConfig(static_cast<u8>(selected_visual), hud_mode);
    bool changed = ImGui::DragFloat3("Attach position", (float*)&position, drag_intensity, 0.f, 0.f, "%.6f");
    changed |= ImGui::DragFloat3("Attach rotation (deg)", (float*)&rotation, 0.05f, 0.f, 0.f, "%.3f");
    changed |= ImGui::DragFloat("Attach scale", &scale, 0.001f, 0.001f, 100.f, "%.6f");
    if (changed)
        weapon->SetAddonEditorTransform(static_cast<u8>(selected_visual), hud_mode, position, rotation, scale);

    if (ImGui::Button("Reset addon transform"))
        weapon->ResetAddonEditorTransform(static_cast<u8>(selected_visual), hud_mode);
    ImGui::SameLine();
    if (ImGui::Button("Copy config lines"))
    {
        string1024 config{};
        LPCSTR prefix = config_hud_mode ? "hud_" : "world_";
        xr_sprintf(config,
            "%sattach_position = %.6f, %.6f, %.6f\n%sattach_rotation = %.3f, %.3f, %.3f\n%sattach_scale = %.6f",
            prefix, position.x, position.y, position.z, prefix, rotation.x, rotation.y, rotation.z, prefix, scale);
        ImGui::SetClipboardText(config);
    }

    ImGui::TextDisabled("Config section: [%s]", selected_section.c_str());
    ImGui::TextDisabled("Config transform: %s", config_hud_mode ? "HUD" : "world");
    ImGui::TextDisabled("Parent: %s", selected_parent.c_str() ? selected_parent.c_str() : "weapon");
}

struct SIKEditorPoint
{
    float time{};
    float weight{};
};

void ParseIKEditorTimeline(LPCSTR value, xr_vector<SIKEditorPoint>& points, float fallback_ik_time)
{
    points.clear();
    string64 item{};
    for (int i = 0, count = value && value[0] ? _GetItemCount(value) : 0; i < count; ++i)
    {
        _GetItem(value, i, item);
        SIKEditorPoint point;
        if (sscanf(item, "%f:%f", &point.time, &point.weight) == 2)
        {
            point.time = clampr(point.time, 0.f, 1.f);
            point.weight = clampr(point.weight, 0.f, 1.f);
            points.push_back(point);
        }
    }
    if (points.empty())
    {
        const float return_time = clampr(fallback_ik_time, 0.f, 1.f);
        points.push_back({0.f, 1.f});
        points.push_back({_min(0.1f, return_time), 0.f});
        points.push_back({return_time, 0.f});
        points.push_back({1.f, 1.f});
    }
}

xr_string BuildIKEditorTimeline(xr_vector<SIKEditorPoint>& points)
{
    std::stable_sort(points.begin(), points.end(), [](const SIKEditorPoint& left, const SIKEditorPoint& right) {
        return left.time < right.time;
    });
    xr_string result;
    string64 item{};
    for (const SIKEditorPoint& point : points)
    {
        xr_sprintf(item, "%s%.4g:%.4g", result.empty() ? "" : ", ", point.time, point.weight);
        result += item;
    }
    return result;
}

void RenderHandPoseIKTransitions(CWeapon* weapon)
{
    if (!weapon)
        return;

    static CWeapon* selected_weapon{};
    static int selected_visual = -1;
    static shared_str selected_motion;
    static bool follow_active_motion = true;
    static bool loop_preview = false;
    if (selected_weapon != weapon)
    {
        selected_weapon = weapon;
        selected_visual = -1;
        selected_motion = nullptr;
        loop_preview = false;
    }

    xr_vector<u8> visuals;
    xr_vector<shared_str> scratch_motions;
    for (u8 visual = 0; visual < CWeapon::AddonVisualCount; ++visual)
    {
        weapon->CollectHandPoseIKEditorMotions(visual, scratch_motions);
        if (!scratch_motions.empty())
            visuals.push_back(visual);
    }
    if (visuals.empty())
    {
        ImGui::TextDisabled("No installed addon with hud_hand_pose");
        return;
    }
    if (std::find(visuals.begin(), visuals.end(), static_cast<u8>(selected_visual)) == visuals.end())
        selected_visual = visuals.front();

    weapon->CollectHandPoseIKEditorMotions(static_cast<u8>(selected_visual), scratch_motions);
    CWeapon::SHandPoseIKEditorState preview_state;
    LPCSTR preview_motion = weapon->GetCurrentHudMotion();
    if ((!preview_motion || !preview_motion[0]) && !scratch_motions.empty())
        preview_motion = scratch_motions.front().c_str();
    weapon->GetHandPoseIKEditorState(static_cast<u8>(selected_visual), preview_motion, preview_state);

    if (ImGui::BeginCombo("IK addon", preview_state.section.c_str() ? preview_state.section.c_str() : "<addon>"))
    {
        for (u8 visual : visuals)
        {
            xr_vector<shared_str> motions;
            weapon->CollectHandPoseIKEditorMotions(visual, motions);
            CWeapon::SHandPoseIKEditorState candidate;
            if (motions.empty() || !weapon->GetHandPoseIKEditorState(visual, motions.front().c_str(), candidate))
                continue;
            const bool selected = visual == selected_visual;
            if (ImGui::Selectable(candidate.section.c_str(), selected))
            {
                selected_visual = visual;
                selected_motion = nullptr;
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    weapon->CollectHandPoseIKEditorMotions(static_cast<u8>(selected_visual), scratch_motions);
    ImGui::Checkbox("Follow active animation", &follow_active_motion);
    if (follow_active_motion)
    {
        LPCSTR active_motion = weapon->GetCurrentHudMotion();
        if (active_motion && active_motion[0])
            selected_motion = active_motion;
        ImGui::Text("Animation: %s", selected_motion.c_str() ? selected_motion.c_str() : "<none>");
    }
    else
    {
        const auto selected_it = std::find_if(scratch_motions.begin(), scratch_motions.end(), [](const shared_str& motion) {
            return selected_motion.c_str() && !_stricmp(motion.c_str(), selected_motion.c_str());
        });
        if (selected_it == scratch_motions.end() && !scratch_motions.empty())
            selected_motion = scratch_motions.front();
        if (ImGui::BeginCombo("Animation", selected_motion.c_str() ? selected_motion.c_str() : "<none>"))
        {
            for (const shared_str& motion : scratch_motions)
            {
                const bool selected = selected_motion.c_str() && !_stricmp(motion.c_str(), selected_motion.c_str());
                if (ImGui::Selectable(motion.c_str(), selected))
                    selected_motion = motion;
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    CWeapon::SHandPoseIKEditorState state;
    if (!selected_motion.c_str() || !weapon->GetHandPoseIKEditorState(static_cast<u8>(selected_visual), selected_motion.c_str(), state))
    {
        ImGui::TextDisabled("No HUD animation selected");
        return;
    }

    const bool selected_is_active = !_stricmp(selected_motion.c_str(), weapon->GetCurrentHudMotion());
    const float progress = selected_is_active ? weapon->GetCurrentHudMotionProgress() : 0.f;
    string64 progress_text{};
    xr_sprintf(progress_text, "%.1f%%", progress * 100.f);
    ImGui::ProgressBar(progress, ImVec2(-1.f, 0.f), progress_text);
    if (ImGui::Button("Preview / restart"))
        weapon->PreviewHandPoseIKEditorMotion(selected_motion.c_str());
    ImGui::SameLine();
    ImGui::Checkbox("Loop preview", &loop_preview);
    if (loop_preview && selected_is_active && progress >= 0.995f)
        weapon->PreviewHandPoseIKEditorMotion(selected_motion.c_str());

    bool changed = false;
    changed |= ImGui::DragFloat("Blend in (sec)", &state.blend_in, 0.005f, 0.f, 5.f, "%.3f");
    changed |= ImGui::DragFloat("Blend out (sec)", &state.blend_out, 0.005f, 0.f, 5.f, "%.3f");
    changed |= ImGui::DragFloat("Fallback IK return time", &state.ik_time, 0.005f, -1.f, 1.f, "%.3f");
    changed |= ImGui::Checkbox("Hold pose between animations", &state.hold_between);
    changed |= ImGui::Checkbox("Override weapon hand animation", &state.override_weapon);

    xr_vector<SIKEditorPoint> points;
    ParseIKEditorTimeline(state.timeline.c_str(), points, state.ik_time);
    ImGui::SeparatorText("Normalized IK timeline");
    int remove_point = -1;
    for (u32 index = 0; index < points.size(); ++index)
    {
        ImGui::PushID(static_cast<int>(index));
        ImGui::SetNextItemWidth(115.f);
        changed |= ImGui::DragFloat("Time", &points[index].time, 0.0025f, 0.f, 1.f, "%.4f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(115.f);
        changed |= ImGui::DragFloat("Weight", &points[index].weight, 0.005f, 0.f, 1.f, "%.3f");
        ImGui::SameLine();
        if (ImGui::SmallButton("X"))
            remove_point = static_cast<int>(index);
        ImGui::PopID();
    }
    if (remove_point >= 0 && points.size() > 1)
    {
        points.erase(points.begin() + remove_point);
        changed = true;
    }
    if (ImGui::Button("Add timeline point"))
    {
        points.push_back({selected_is_active ? progress : 0.5f, 1.f});
        changed = true;
    }

    if (changed)
    {
        const xr_string timeline = BuildIKEditorTimeline(points);
        state.timeline = timeline.c_str();
        weapon->SetHandPoseIKEditorState(static_cast<u8>(selected_visual), state);
    }

    if (ImGui::Button("Reset runtime values"))
        weapon->ResetHandPoseIKEditorState(static_cast<u8>(selected_visual), selected_motion.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Copy IK config"))
    {
        const xr_string timeline = BuildIKEditorTimeline(points);
        string2048 config{};
        xr_sprintf(config,
            "hud_hand_pose_blend_in = %.3f\nhud_hand_pose_blend_out = %.3f\nhud_hand_pose_hold_between_animations = %s\n"
            "hand_pose_ik_time_%s = %.3f\nhand_pose_ik_timeline_%s = %s",
            state.blend_in, state.blend_out, state.hold_between ? "true" : "false", state.motion.c_str(), state.ik_time,
            state.motion.c_str(), timeline.c_str());
        ImGui::SetClipboardText(config);
    }
    ImGui::TextDisabled("Runtime override: %s", state.runtime_override ? "yes" : "no");
    ImGui::TextDisabled("Timeline uses normalized animation time (0..1) and IK weight (0..1)");
}

void ApplyInventoryIconValues(CInventory& inventory, const CInventoryItem& source)
{
    const shared_str section = source.object().cNameSect();
    for (PIItem item : inventory.m_all)
    {
        if (!item || xr_strcmp(item->object().cNameSect(), section))
            continue;

        item->m_icon_3d_enabled = source.m_icon_3d_enabled;
        item->m_icon_3d_rotation = source.m_icon_3d_rotation;
        item->m_icon_3d_offset = source.m_icon_3d_offset;
        item->m_icon_3d_scale = source.m_icon_3d_scale;
        item->m_icon_3d_rotation_speed = source.m_icon_3d_rotation_speed;
    }
}

void ResetInventoryIconValues(CInventoryItem& item)
{
    LPCSTR section = item.object().cNameSect().c_str();
    item.m_icon_3d_enabled = READ_IF_EXISTS(pSettings, r_bool, "dragdrop", "use_3d_icons", true);
    item.m_icon_3d_rotation.set(15.f, 110.f, -5.f);
    item.m_icon_3d_offset.set(0.f, 0.f, 0.f);
    item.m_icon_3d_scale = READ_IF_EXISTS(pSettings, r_float, "dragdrop", "icon_3d_scale", 1.f);
    item.m_icon_3d_rotation_speed = READ_IF_EXISTS(pSettings, r_float, "dragdrop", "icon_3d_rotation_speed", 0.f);

    if (pSettings->line_exist("dragdrop", "icon_3d_rotation"))
        item.m_icon_3d_rotation = pSettings->r_fvector3("dragdrop", "icon_3d_rotation");
    if (pSettings->line_exist("dragdrop", "icon_3d_offset"))
        item.m_icon_3d_offset = pSettings->r_fvector3("dragdrop", "icon_3d_offset");

    item.m_icon_3d_enabled = READ_IF_EXISTS(pSettings, r_bool, section, "inv_icon_3d", item.m_icon_3d_enabled);
    item.m_icon_3d_scale = READ_IF_EXISTS(pSettings, r_float, section, "inv_icon_3d_scale", item.m_icon_3d_scale);
    item.m_icon_3d_rotation_speed =
        READ_IF_EXISTS(pSettings, r_float, section, "inv_icon_3d_rotation_speed", item.m_icon_3d_rotation_speed);
    if (pSettings->line_exist(section, "inv_icon_3d_rotation"))
        item.m_icon_3d_rotation = pSettings->r_fvector3(section, "inv_icon_3d_rotation");
    if (pSettings->line_exist(section, "inv_icon_3d_offset"))
        item.m_icon_3d_offset = pSettings->r_fvector3(section, "inv_icon_3d_offset");
}

void RenderInventoryIconEditor(CInventory& inventory)
{
    static PIItem selected_item{};
    xr_vector<PIItem> sections;
    for (PIItem item : inventory.m_all)
    {
        if (!item)
            continue;

        const auto duplicate = std::find_if(sections.begin(), sections.end(), [item](PIItem candidate) {
            return !xr_strcmp(candidate->object().cNameSect(), item->object().cNameSect());
        });
        if (duplicate == sections.end())
            sections.push_back(item);
    }

    const auto selected = std::find_if(sections.begin(), sections.end(), [](PIItem item) {
        return item == selected_item;
    });
    if (selected == sections.end())
    {
        PIItem active_item = inventory.ActiveItem();
        const auto active = std::find_if(sections.begin(), sections.end(), [active_item](PIItem item) {
            return active_item && !xr_strcmp(item->object().cNameSect(), active_item->object().cNameSect());
        });
        selected_item = active != sections.end() ? *active : (sections.empty() ? nullptr : sections.front());
    }

    if (!selected_item)
    {
        ImGui::TextDisabled("Inventory is empty");
        return;
    }

    LPCSTR selected_section = selected_item->object().cNameSect().c_str();
    if (ImGui::BeginCombo("Inventory item", selected_section))
    {
        for (PIItem item : sections)
        {
            LPCSTR section = item->object().cNameSect().c_str();
            const bool is_selected = item == selected_item;
            if (ImGui::Selectable(section, is_selected))
                selected_item = item;
            if (is_selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    bool changed = ImGui::Checkbox("Use 3D icon", &selected_item->m_icon_3d_enabled);
    changed |= ImGui::DragFloat3("Position", (float*)&selected_item->m_icon_3d_offset, 0.001f, 0.f, 0.f, "%.5f");
    changed |= ImGui::DragFloat3("Rotation (deg)", (float*)&selected_item->m_icon_3d_rotation, 0.1f, 0.f, 0.f, "%.2f");
    changed |= ImGui::DragFloat("Size / scale", &selected_item->m_icon_3d_scale, 0.005f, 0.05f, 20.f, "%.4f");
    changed |= ImGui::DragFloat(
        "Rotation speed (deg/s)", &selected_item->m_icon_3d_rotation_speed, 0.1f, -720.f, 720.f, "%.2f");
    selected_item->m_icon_3d_scale = _max(selected_item->m_icon_3d_scale, 0.05f);

    if (changed)
        ApplyInventoryIconValues(inventory, *selected_item);

    if (ImGui::Button("Reset from config"))
    {
        ResetInventoryIconValues(*selected_item);
        ApplyInventoryIconValues(inventory, *selected_item);
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy config lines"))
    {
        string1024 config{};
        xr_sprintf(config,
            "inv_icon_3d = %s\ninv_icon_3d_offset = %.5f, %.5f, %.5f\n"
            "inv_icon_3d_rotation = %.2f, %.2f, %.2f\ninv_icon_3d_scale = %.4f\ninv_icon_3d_rotation_speed = %.2f",
            selected_item->m_icon_3d_enabled ? "true" : "false", selected_item->m_icon_3d_offset.x,
            selected_item->m_icon_3d_offset.y, selected_item->m_icon_3d_offset.z, selected_item->m_icon_3d_rotation.x,
            selected_item->m_icon_3d_rotation.y, selected_item->m_icon_3d_rotation.z, selected_item->m_icon_3d_scale,
            selected_item->m_icon_3d_rotation_speed);
        ImGui::SetClipboardText(config);
    }

    ImGui::TextDisabled("Section: [%s]", selected_item->object().cNameSect().c_str());
    ImGui::TextDisabled("Open the inventory for a live preview; changes affect every item of this section.");
}
} // namespace


void CImGuiHudEditorWnd::Render()
{
    if (!g_player_hud)
        return;

    if (!RenderBegin())
    {
        RenderEnd();
        return;
    }

    CActor* actor = Actor();
    if (!actor)
    {
        RenderEnd();
        return;
    }

    bool showSeparator = true;
    auto item = g_player_hud->attached_item(0);
    auto Wpn = smart_cast<CWeapon*>(actor->inventory().ActiveItem());

    if (ImGui::CollapsingHeader("3D inventory icons", ImGuiTreeNodeFlags_DefaultOpen))
        RenderInventoryIconEditor(actor->inventory());

    static float drag_intensity = 0.0001f;

    ImGui::DragFloat("Drag Intensity", &drag_intensity, 0.000001f, 0.000001f, 1.0f, "%.6f");

    if (item)
    {
        if (showSeparator)
            ImGui::Separator();

        ImGui::Text("Item 0");
        ImGui::DragFloat3("hands_position 0",				(float*)&item->m_measures.m_hands_attach[0],		drag_intensity, NULL, NULL, "%.6f");
		ImGui::DragFloat3("hands_orientation 0",			(float*)&item->m_measures.m_hands_attach[1],		drag_intensity, NULL, NULL, "%.6f");
		ImGui::DragFloat3("item_position 0",				(float*)&item->m_measures.m_item_attach[0],			drag_intensity, NULL, NULL, "%.6f");
		ImGui::DragFloat3("item_orientation 0",				(float*)&item->m_measures.m_item_attach[1],			drag_intensity, NULL, NULL, "%.6f");
		ImGui::DragFloat3("aim_hud_offset_pos 0",			(float*)&item->m_measures.m_hands_offset[0][1],		drag_intensity, NULL, NULL, "%.6f");
		ImGui::DragFloat3("aim_hud_offset_rot 0",			(float*)&item->m_measures.m_hands_offset[1][1],		drag_intensity, NULL, NULL, "%.6f");
		ImGui::DragFloat3("gl_hud_offset_pos 0",			(float*)&item->m_measures.m_hands_offset[0][2],		drag_intensity, NULL, NULL, "%.6f");
		ImGui::DragFloat3("gl_hud_offset_rot 0",			(float*)&item->m_measures.m_hands_offset[1][2],		drag_intensity, NULL, NULL, "%.6f");
		ImGui::DragFloat3("sprint_hud_offset_pos 0",        (float*)&item->m_measures.m_sprint_offset[0],       drag_intensity, NULL, NULL, "%.6f");
		ImGui::DragFloat3("sprint_hud_offset_rot 0",        (float*)&item->m_measures.m_sprint_offset[1],       0.05f, NULL, NULL, "%.3f");
        bool sprint_offset_enabled = item->m_measures.m_sprint_offset[2].x > 0.f;
        if (ImGui::Checkbox("sprint_hud_offset_enabled 0", &sprint_offset_enabled))
            item->m_measures.m_sprint_offset[2].x = sprint_offset_enabled ? 1.f : 0.f;
        ImGui::DragFloat("sprint_hud_offset_time 0", &item->m_measures.m_sprint_offset[2].y, 0.01f, 0.01f, 5.f, "%.2f");
		ImGui::DragFloat3("fire_point 0",					(float*)&item->m_measures.m_fire_point_offset[0],	drag_intensity, NULL, NULL, "%.6f");
		ImGui::DragFloat3("fire_point2 0",					(float*)&item->m_measures.m_fire_point2_offset[0],	drag_intensity, NULL, NULL, "%.6f");
		ImGui::DragFloat3("shell_point 0",					(float*)&item->m_measures.m_shell_point_offset[0],	drag_intensity, NULL, NULL, "%.6f");

        if (ImGui::CollapsingHeader("Bone transforms 0"))
            RenderBoneAdjustments(0);

        if (Wpn && ImGui::CollapsingHeader("Installed addon transforms", ImGuiTreeNodeFlags_DefaultOpen))
            RenderAddonTransforms(Wpn, drag_intensity);

        if (Wpn && ImGui::CollapsingHeader("Addon hand-pose IK transitions"))
            RenderHandPoseIKTransitions(Wpn);

        if (Wpn)
        {
            // Laser light offsets
            if (pSettings->line_exist(Wpn->cNameSect(), "laser_light_section"))
            {
                ImGui::DragFloat3("laserdot_attach_offset 0", (float*)&Wpn->laserdot_attach_offset, drag_intensity, NULL, NULL, "%.6f");
            }

            // Torch light offsets
            if (pSettings->line_exist(Wpn->cNameSect(), "flashlight_section"))
            {
                ImGui::DragFloat3("torch_attach_offset 0", (float*)&Wpn->flashlight_attach_offset, drag_intensity, NULL, NULL, "%.6f");
                ImGui::DragFloat3("torch_omni_attach_offset 0", (float*)&Wpn->flashlight_omni_attach_offset, drag_intensity, NULL, NULL, "%.6f");
            }
        }
    }

    item = g_player_hud->attached_item(1);

    if (item)
    {
        if (showSeparator)
            ImGui::Separator();

        ImGui::Text("Item 1");
        ImGui::DragFloat3("hands_position 1",		(float*)&item->m_measures.m_hands_attach[0][0],		drag_intensity, NULL, NULL, "%.6f");
		ImGui::DragFloat3("hands_orientation 1",	(float*)&item->m_measures.m_hands_attach[1][0],		drag_intensity, NULL, NULL, "%.6f");
		ImGui::DragFloat3("item_position 1",		(float*)&item->m_measures.m_item_attach[0],			drag_intensity, NULL, NULL, "%.6f");
		ImGui::DragFloat3("item_orientation 1",		(float*)&item->m_measures.m_item_attach[1],			drag_intensity, NULL, NULL, "%.6f");
		ImGui::DragFloat3("fire_point 1",			(float*)&item->m_measures.m_fire_point_offset[0],	drag_intensity, NULL, NULL, "%.6f");
		ImGui::DragFloat3("fire_point2 1",			(float*)&item->m_measures.m_fire_point2_offset[0],	drag_intensity, NULL, NULL, "%.6f");
		ImGui::DragFloat3("shell_point 1",			(float*)&item->m_measures.m_shell_point_offset[0],	drag_intensity, NULL, NULL, "%.6f");

        if (ImGui::CollapsingHeader("Bone transforms 1"))
            RenderBoneAdjustments(1);
    }

    if (ImGui::Button("Save"))
    {
        // TODO ImGui fix

        // g_player_hud->SaveCfg(0);
        // g_player_hud->SaveCfg(1);
    }

    RenderEnd();
}
