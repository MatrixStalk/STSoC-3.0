#include "stdafx.h"
#include "imgui.h"
#include "embedded_editor_sound_env.h"
#include "../HudSound.h"
#include "../Actor.h"
#include "../Inventory.h"
#include "../Weapon.h"

#include <mmeapi.h>
#include <../eax/Include/eax.h>

namespace
{
struct TimelineLayerEditor
{
    string64 name{"layer"};
    string_path sound{};
    float volume{1.f};
    float time{};
    float pitch{1.f};
};

xr_vector<xr_string> timeline_sound_files;
xr_vector<TimelineLayerEditor> timeline_layers;
string128 timeline_section{"weapon_animation_timeline"};
string_path timeline_output{"sound_timelines.ltx"};
u32 timeline_preview_started{};
float timeline_preview_duration{};
float timeline_preview_animation_duration{};
bool timeline_preview_with_animation{};
CWeapon* timeline_animation_weapon{};
shared_str timeline_animation;

void finish_timeline_preview()
{
    timeline_preview_started = 0;
    timeline_preview_duration = 0.f;
    timeline_preview_animation_duration = 0.f;
    timeline_preview_with_animation = false;
}

CWeapon* active_hud_weapon()
{
    CActor* actor = Actor();
    return actor ? smart_cast<CWeapon*>(actor->inventory().ActiveItem()) : nullptr;
}

bool start_timeline_preview(bool with_animation)
{
    finish_timeline_preview();

    CWeapon* preview_weapon = nullptr;
    if (with_animation)
    {
        preview_weapon = active_hud_weapon();
        if (!preview_weapon || !timeline_animation.c_str())
            return false;
    }

    HUD_SOUND preview_sound;
    for (const TimelineLayerEditor& source : timeline_layers)
    {
        if (!source.sound[0])
            continue;
        auto& layer = preview_sound.timeline_layers.emplace_back();
        auto& sound = layer.emplace_back();
        sound.snd.create(source.sound, st_Effect, sg_Interface);
        sound.volume = source.volume;
        sound.delay = source.time;
        sound.freq = source.pitch;
        const float sound_duration = sound.snd.get_length_sec() / _max(source.pitch, EPS_S);
        timeline_preview_duration = _max(timeline_preview_duration, source.time + sound_duration);
    }

    if (preview_sound.timeline_layers.empty())
        return false;

    // Load/decode the draft first so asset preparation cannot shift the mix
    // away from frame zero of the animation preview.
    if (with_animation)
    {
        if (!preview_weapon->PreviewSoundEditorMotion(timeline_animation.c_str()))
        {
            HUD_SOUND::DestroySound(preview_sound);
            return false;
        }
        timeline_preview_animation_duration = preview_weapon->GetCurrentHudMotionDuration();
    }

    Fvector position{};
    // overlap=true uses no-feedback emitters. All layers retain their delay,
    // volume and pitch while the temporary source references can be released
    // immediately and cannot outlive the sound subsystem during shutdown.
    HUD_SOUND::PlaySound(preview_sound, position, nullptr, true, false, true);
    HUD_SOUND::DestroySound(preview_sound);
    timeline_preview_started = Device.dwTimeGlobal;
    timeline_preview_with_animation = with_animation;
    return true;
}

void refresh_sound_files()
{
    timeline_sound_files.clear();
    constexpr LPCSTR masks[]{"*.ogg", "*.wav", "*.mp3", "*.flac"};
    for (LPCSTR mask : masks)
    {
        FS_FileSet files;
        FS.file_list(files, fsgame::game_sounds, FS_ListFiles, mask);
        for (const auto& file : files)
        {
            string_path name;
            xr_strcpy(name, file.name.c_str());
            if (LPSTR extension = strext(name))
                *extension = 0;
            if (std::find(timeline_sound_files.begin(), timeline_sound_files.end(), name) == timeline_sound_files.end())
                timeline_sound_files.emplace_back(name);
        }
    }
    std::sort(timeline_sound_files.begin(), timeline_sound_files.end());
}

void render_equalizer_editor()
{
    if (!ImGui::CollapsingHeader("Level equalizer", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    SSoundEqualizer* eq = ::Sound->DbgEqualizer();
    bool changed = false;
    bool enabled = !!eq->enabled;
    if (ImGui::Checkbox("Enabled (level sounds only)", &enabled))
    {
        eq->enabled = enabled;
        changed = true;
    }
    changed |= ImGui::SliderFloat("Preamp", &eq->preamp_db, -18.f, 12.f, "%.1f dB");
    constexpr LPCSTR names[4]{"Low shelf", "Low mid", "High mid", "High shelf"};
    for (u32 i = 0; i < 4; ++i)
    {
        ImGui::PushID(i);
        ImGui::TextUnformatted(names[i]);
        changed |= ImGui::SliderFloat("Frequency", &eq->bands[i].frequency, 20.f, 20000.f, "%.0f Hz", ImGuiSliderFlags_Logarithmic);
        changed |= ImGui::SliderFloat("Gain", &eq->bands[i].gain_db, -18.f, 18.f, "%.1f dB");
        changed |= ImGui::SliderFloat("Q", &eq->bands[i].q, 0.1f, 10.f, "%.2f", ImGuiSliderFlags_Logarithmic);
        ImGui::Separator();
        ImGui::PopID();
    }
    if (changed)
        ++eq->revision;
    if (ImGui::Button("Save equalizer"))
        ::Sound->DbgEqualizerSave();
    ImGui::SameLine();
    if (ImGui::Button("Reload equalizer"))
        ::Sound->DbgEqualizerReload();
}

void render_timeline_editor()
{
    if (!ImGui::CollapsingHeader("Animation sound timeline", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    ImGui::InputText("Section", timeline_section, std::size(timeline_section));
    ImGui::InputText("Output LTX", timeline_output, std::size(timeline_output));

    CWeapon* weapon = active_hud_weapon();
    if (timeline_animation_weapon != weapon)
    {
        timeline_animation_weapon = weapon;
        timeline_animation = nullptr;
    }
    xr_vector<shared_str> animations;
    if (weapon)
        weapon->CollectSoundEditorMotions(animations);
    const auto selected_animation = std::find_if(animations.begin(), animations.end(), [](const shared_str& candidate) {
        return timeline_animation.c_str() && !_stricmp(candidate.c_str(), timeline_animation.c_str());
    });
    if (selected_animation == animations.end() && !animations.empty())
        timeline_animation = animations.front();

    if (ImGui::BeginCombo("Test animation", timeline_animation.c_str() ? timeline_animation.c_str() : "<no HUD animation>"))
    {
        for (const shared_str& animation : animations)
        {
            const bool selected = timeline_animation.c_str() && !_stricmp(animation.c_str(), timeline_animation.c_str());
            if (ImGui::Selectable(animation.c_str(), selected))
                timeline_animation = animation;
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    const bool has_mix = std::any_of(timeline_layers.begin(), timeline_layers.end(), [](const TimelineLayerEditor& layer) {
        return layer.sound[0] != 0;
    });
    ImGui::BeginDisabled(!has_mix || timeline_preview_started);
    if (ImGui::Button("Preview final mix"))
        start_timeline_preview(false);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!has_mix || !weapon || !timeline_animation.c_str() || timeline_preview_started);
    if (ImGui::Button("Preview mix with animation"))
        start_timeline_preview(true);
    ImGui::EndDisabled();
    if (timeline_preview_started)
    {
        const float elapsed = float(Device.dwTimeGlobal - timeline_preview_started) / 1000.f;
        const float total = _max(timeline_preview_duration, timeline_preview_animation_duration);
        const float progress = total > EPS_S ? clampr(elapsed / total, 0.f, 1.f) : 1.f;
        string64 progress_text;
        xr_sprintf(progress_text, "%.2f / %.2f s", _min(elapsed, total), total);
        ImGui::ProgressBar(progress, ImVec2(-1.f, 0.f), progress_text);
        if (timeline_preview_with_animation && timeline_preview_duration > timeline_preview_animation_duration + EPS_S)
            ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f), "Mix tail exceeds animation by %.3f s",
                timeline_preview_duration - timeline_preview_animation_duration);
        if (elapsed >= total)
            finish_timeline_preview();
    }

    if (timeline_sound_files.empty() && ImGui::Button("Scan game sounds"))
        refresh_sound_files();
    else if (!timeline_sound_files.empty() && ImGui::Button("Rescan game sounds"))
        refresh_sound_files();
    ImGui::SameLine();
    if (ImGui::Button("Add layer"))
    {
        TimelineLayerEditor& layer = timeline_layers.emplace_back();
        xr_sprintf(layer.name, "layer_%u", timeline_layers.size());
    }

    for (u32 i = 0; i < timeline_layers.size();)
    {
        TimelineLayerEditor& layer = timeline_layers[i];
        ImGui::PushID(i);
        ImGui::InputText("Layer name", layer.name, std::size(layer.name));
        const char* preview = layer.sound[0] ? layer.sound : "<select sound>";
        if (ImGui::BeginCombo("Sound", preview))
        {
            static ImGuiTextFilter filter;
            filter.Draw("Filter");
            for (const xr_string& sound : timeline_sound_files)
            {
                if (!filter.PassFilter(sound.c_str()))
                    continue;
                if (ImGui::Selectable(sound.c_str(), !_stricmp(layer.sound, sound.c_str())))
                    xr_strcpy(layer.sound, sound.c_str());
            }
            ImGui::EndCombo();
        }
        ImGui::DragFloat("Timeline (seconds)", &layer.time, 0.01f, 0.f, 60.f, "%.3f s");
        ImGui::SliderFloat("Volume", &layer.volume, 0.f, 2.f, "%.2f");
        ImGui::SliderFloat("Pitch", &layer.pitch, 0.25f, 4.f, "%.2f");
        if (ImGui::Button("Preview") && layer.sound[0])
        {
            ref_sound preview_sound;
            preview_sound.create(layer.sound, st_Effect, sg_Interface);
            Fvector position{};
            float volume = layer.volume;
            float pitch = layer.pitch;
            preview_sound.play_no_feedback(nullptr, sm_2D, 0.f, &position, &volume, &pitch);
            preview_sound.destroy();
        }
        ImGui::SameLine();
        bool remove = ImGui::Button("Remove layer");
        ImGui::Separator();
        ImGui::PopID();
        if (remove)
            timeline_layers.erase(timeline_layers.begin() + i);
        else
            ++i;
    }

    if (ImGui::Button("Save timeline LTX") && timeline_section[0] && timeline_output[0])
    {
        string_path path;
        FS.update_path(path, fsgame::game_configs, timeline_output);
        CInifile ini(path, FALSE, FS.exist(path) != nullptr, TRUE);
        xr_string names;
        for (const TimelineLayerEditor& layer : timeline_layers)
        {
            if (!layer.name[0] || !layer.sound[0])
                continue;
            if (!names.empty())
                names += ", ";
            names += layer.name;
            string128 key;
            string512 value;
            xr_sprintf(key, "layer_%s", layer.name);
            xr_sprintf(value, "%s, %.4f, %.4f, %.4f", layer.sound, layer.volume, layer.time, layer.pitch);
            ini.w_string(timeline_section, key, value);
        }
        ini.w_string(timeline_section, "layers", names.c_str());
        ini.save_as();
        HUD_SOUND::ReloadTimelineConfig();
        Msg("--Sound timeline [%s] saved to [%s]", timeline_section, path);
    }
    ImGui::TextWrapped("Use from a weapon/addon as: snd_anm_<animation> = %s", timeline_section);
}
}

/*
constexpr const char* env_names[]{"GENERIC",    "PADDEDCELL",      "ROOM",       "BATHROOM",      "LIVINGROOM", "STONEROOM", "AUDITORIUM", "CONCERTHALL", "CAVE",   "ARENA",
                                  "HANGAR",     "CARPETEDHALLWAY", "HALLWAY",    "STONECORRIDOR", "ALLEY",      "FOREST",    "CITY",       "MOUNTAINS",   "QUARRY", "PLAIN",
                                  "PARKINGLOT", "SEWERPIPE",       "UNDERWATER", "DRUGGED",       "DIZZY",      "PSYCHOTIC"};
static_assert(std::size(env_names) == EAX_ENVIRONMENT_COUNT);
*/

void CImGuiSoundEnvWnd::Render()
{
    if (!RenderBegin())
    {
        RenderEnd();
        return;
    }

    ::Sound->DbgCurrentEnvPaused(true);

    CSound_environment* env = ::Sound->DbgCurrentEnv();

    static string256 env_name;

    strcpy_s(env_name, env->name.c_str());

    ImGui::InputText("Current Zone Name", env_name, std::size(env_name), ImGuiInputTextFlags_ReadOnly);

    ImGui::Separator();

    render_equalizer_editor();
    ImGui::Separator();
    render_timeline_editor();
    ImGui::Separator();

    ImGui::SliderFloat("Room", &env->Room, EAXLISTENER_MINROOM, EAXLISTENER_MAXROOM);
    ImGui::SliderFloat("RoomHF", &env->RoomHF, EAXLISTENER_MINROOMHF, EAXLISTENER_MAXROOMHF);
    ImGui::SliderFloat("RoomRolloffFactor", &env->RoomRolloffFactor, EAXLISTENER_MINROOMROLLOFFFACTOR, EAXLISTENER_MAXROOMROLLOFFFACTOR);
    ImGui::SliderFloat("DecayTime", &env->DecayTime, EAXLISTENER_MINDECAYTIME, EAXLISTENER_MAXDECAYTIME);
    ImGui::SliderFloat("DecayHFRatio", &env->DecayHFRatio, EAXLISTENER_MINDECAYHFRATIO, EAXLISTENER_MAXDECAYHFRATIO);
    ImGui::SliderFloat("Reflections", &env->Reflections, EAXLISTENER_MINREFLECTIONS, EAXLISTENER_MAXREFLECTIONS);
    ImGui::SliderFloat("ReflectionsDelay", &env->ReflectionsDelay, EAXLISTENER_MINREFLECTIONSDELAY, EAXLISTENER_MAXREFLECTIONSDELAY);
    ImGui::SliderFloat("Reverb", &env->Reverb, EAXLISTENER_MINREVERB, EAXLISTENER_MAXREVERB);
    ImGui::SliderFloat("ReverbDelay", &env->ReverbDelay, EAXLISTENER_MINREVERBDELAY, EAXLISTENER_MAXREVERBDELAY);
    ImGui::SliderFloat("EnvironmentSize", &env->EnvironmentSize, EAXLISTENER_MINENVIRONMENTSIZE, EAXLISTENER_MAXENVIRONMENTSIZE);
    ImGui::SliderFloat("EnvironmentDiffusion", &env->EnvironmentDiffusion, EAXLISTENER_MINENVIRONMENTDIFFUSION, EAXLISTENER_MAXENVIRONMENTDIFFUSION);
    ImGui::SliderFloat("AirAbsorptionHF", &env->AirAbsorptionHF, EAXLISTENER_MINAIRABSORPTIONHF, EAXLISTENER_MAXAIRABSORPTIONHF);

/* // Закомментировано, т.к. у нас вот это Environment нигде не используется, его менять смысла нет.
    ImGui::SliderInt("Current: ", reinterpret_cast<int*>(&env->Environment), 0, EAX_ENVIRONMENT_COUNT - 1);
    ImGui::SameLine();
    ImGui::Text(env_names[env->Environment]);
*/

    if (ImGui::Button("Save"))
    {
        ::Sound->DbgCurrentEnvSave();

        Msg("--SoundEnvEditor saved!");
    }

    RenderEnd();
}
