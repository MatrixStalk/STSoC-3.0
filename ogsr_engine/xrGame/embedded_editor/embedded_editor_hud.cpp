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

    ImGui::Checkbox("HUD transform", &hud_mode);
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
        LPCSTR prefix = hud_mode ? "hud_" : "world_";
        xr_sprintf(config,
            "%sattach_position = %.6f, %.6f, %.6f\n%sattach_rotation = %.3f, %.3f, %.3f\n%sattach_scale = %.6f",
            prefix, position.x, position.y, position.z, prefix, rotation.x, rotation.y, rotation.z, prefix, scale);
        ImGui::SetClipboardText(config);
    }

    ImGui::TextDisabled("Config section: [%s]", selected_section.c_str());
    ImGui::TextDisabled("Parent: %s", selected_parent.c_str() ? selected_parent.c_str() : "weapon");
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

    bool showSeparator = true;
    auto item = g_player_hud->attached_item(0);
    auto Wpn = smart_cast<CWeapon*>(Actor()->inventory().ActiveItem());

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

        if (Wpn && ImGui::CollapsingHeader("Addon transforms"))
            RenderAddonTransforms(Wpn, drag_intensity);

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
