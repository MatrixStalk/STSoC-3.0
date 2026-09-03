//////////////////////////////////////////////////////////////////////
// HudSound.cpp:	структура для работы со звуками применяемыми в
//					HUD-объектах (обычные звуки, но с доп. параметрами)
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"

#include "HudSound.h"
#include "../xr_3da/x_ray.h"
#include <memory>

namespace
{
std::unique_ptr<CInifile> sound_timeline_config;
u32 sound_timeline_revision{};

CInifile* GetSoundTimelineConfig()
{
    if (!sound_timeline_config)
    {
        string_path path;
        FS.update_path(path, "$game_config$", "sound_timelines.ltx");
        if (FS.exist(path))
            sound_timeline_config = std::make_unique<CInifile>(path, TRUE, TRUE, FALSE);
    }
    return sound_timeline_config.get();
}

void LoadSoundVariants(CInifile* ini, LPCSTR section, LPCSTR line, xr_vector<HUD_SOUND::SSnd>& sounds, int type)
{
    string256 sound_line;
    strcpy_s(sound_line, line);
    int k = 0;
    while (ini->line_exist(section, sound_line))
    {
        HUD_SOUND::SSnd& s = sounds.emplace_back();
        LPCSTR str = ini->r_string(section, sound_line);
        string256 value;
        const int count = _GetItemCount(str);
        R_ASSERT(count);
        _GetItem(str, 0, value);
        s.snd.create(value, st_Effect, type);
        s.volume = count > 1 && xr_strlen(_GetItem(str, 1, value)) ? (float)atof(value) : 1.f;
        s.delay = count > 2 && xr_strlen(_GetItem(str, 2, value)) ? (float)atof(value) : 0.f;
        s.freq = count > 3 && xr_strlen(_GetItem(str, 3, value)) ? (float)atof(value) : 1.f;
        sprintf_s(sound_line, "%s%d", line, ++k);
    }
}
}

void HUD_SOUND::ReloadTimelineConfig()
{
    sound_timeline_config.reset();
    GetSoundTimelineConfig();
    ++sound_timeline_revision;
}

u32 HUD_SOUND::TimelineConfigRevision() { return sound_timeline_revision; }

void HUD_SOUND::LoadSound(LPCSTR section, LPCSTR line, HUD_SOUND& hud_snd, int type)
{
    hud_snd.m_activeSnd = NULL;
    hud_snd.sounds.clear();
    hud_snd.timeline_layers.clear();
    hud_snd.m_config_section = section;
    hud_snd.m_config_line = line;
    hud_snd.m_config_type = type;

    LPCSTR definition = pSettings->r_string(section, line);
    string256 first_item;
    _GetItem(definition, 0, first_item);
    CInifile* timeline_ini = pSettings;
    if (!pSettings->section_exist(first_item))
    {
        CInifile* external = GetSoundTimelineConfig();
        if (external && external->section_exist(first_item))
            timeline_ini = external;
    }
    if (timeline_ini->section_exist(first_item))
    {
        LPCSTR timeline_section = first_item;
        string256 layer_line;
        for (u32 layer = 1;; ++layer)
        {
            xr_sprintf(layer_line, "snd_%u_layer", layer);
            if (!timeline_ini->line_exist(timeline_section, layer_line))
                break;
            auto& variants = hud_snd.timeline_layers.emplace_back();
            LoadSoundVariants(timeline_ini, timeline_section, layer_line, variants, type);
        }

        // Named layers make attachment overrides readable. An inherited LTX
        // section can replace only layer_mag_in while retaining other layers.
        if (timeline_ini->line_exist(timeline_section, "layers"))
        {
            LPCSTR names = timeline_ini->r_string(timeline_section, "layers");
            for (int i = 0, count = _GetItemCount(names); i < count; ++i)
            {
                string128 name;
                string256 named_line;
                _GetItem(names, i, name);
                xr_sprintf(named_line, "layer_%s", name);
                if (!timeline_ini->line_exist(timeline_section, named_line))
                    continue;
                auto& variants = hud_snd.timeline_layers.emplace_back();
                LoadSoundVariants(timeline_ini, timeline_section, named_line, variants, type);
            }
        }

        ASSERT_FMT(!hud_snd.timeline_layers.empty(), "there are no timeline layers in [%s] for [%s]", timeline_section, line);
        return;
    }

    LoadSoundVariants(pSettings, section, line, hud_snd.sounds, type);

    ASSERT_FMT(!hud_snd.sounds.empty(), "there is no sounds [%s] for [%s]", line, section);
}

void HUD_SOUND::LoadSound(LPCSTR section, LPCSTR line, ref_sound& snd, int type, float* volume, float* delay, float* freq)
{
    LPCSTR str = pSettings->r_string(section, line);
    string256 buf_str;

    int count = _GetItemCount(str);
    R_ASSERT(count);

    _GetItem(str, 0, buf_str);
    snd.create(buf_str, st_Effect, type);

    if (volume != NULL)
    {
        *volume = 1.f;
        if (count > 1)
        {
            _GetItem(str, 1, buf_str);
            if (xr_strlen(buf_str) > 0)
                *volume = (float)atof(buf_str);
        }
    }

    if (delay != NULL)
    {
        *delay = 0;
        if (count > 2)
        {
            _GetItem(str, 2, buf_str);
            if (xr_strlen(buf_str) > 0)
                *delay = (float)atof(buf_str);
        }
    }

    if (freq != NULL)
    {
        *freq = 1.f;
        if (count > 3)
        {
            _GetItem(str, 3, buf_str);
            if (xr_strlen(buf_str) > 0)
                *freq = (float)atof(buf_str);
        }
    }
}

void HUD_SOUND::DestroySound(HUD_SOUND& hud_snd)
{
    for (auto& layer : hud_snd.timeline_layers)
        for (auto& sound : layer)
            sound.snd.destroy();
    hud_snd.timeline_layers.clear();
    xr_vector<SSnd>::iterator it = hud_snd.sounds.begin();
    for (; it != hud_snd.sounds.end(); ++it)
        (*it).snd.destroy();
    hud_snd.sounds.clear();

    hud_snd.m_activeSnd = NULL;
}

void HUD_SOUND::PlaySound(HUD_SOUND& hud_snd, const Fvector& position, const CObject* parent, bool b_hud_mode, bool looped, bool overlap,
    float timeline_scale, u8 index)
{
    if (!hud_snd.timeline_layers.empty())
    {
        if (!overlap)
            StopSound(hud_snd);
        const u32 flags = (b_hud_mode ? sm_2D : 0) | (looped ? sm_Looped : 0);
        Fvector pos = (flags & sm_2D) ? Fvector{} : position;
        static const float hud_vol = READ_IF_EXISTS(pSettings, r_float, "hud_sound", "hud_sound_vol_k", 1.0f);
        for (auto& layer : hud_snd.timeline_layers)
        {
            if (layer.empty())
                continue;
            const u32 selected_index = index != u8(-1) && index < layer.size() ? index : Random.randI(layer.size());
            HUD_SOUND::SSnd& selected = layer[selected_index];
            float vol = selected.volume * (b_hud_mode ? hud_vol : 1.f);
            float freq = selected.freq;
            if (overlap)
                selected.snd.play_no_feedback(const_cast<CObject*>(parent), flags, selected.delay * timeline_scale, &pos, &vol, &freq);
            else
            {
                selected.snd.play_at_pos(const_cast<CObject*>(parent), pos, flags, selected.delay * timeline_scale);
                selected.snd.set_volume(vol);
                selected.snd.set_frequency(freq);
            }
        }
        return;
    }

    if (hud_snd.sounds.empty())
        return;

    if (!overlap)
        StopSound(hud_snd);

    u32 flags = b_hud_mode ? sm_2D : 0;
    if (looped)
        flags |= sm_Looped;

    const u32 selected_index = index != u8(-1) && index < hud_snd.sounds.size() ? index : Random.randI(hud_snd.sounds.size());
    hud_snd.m_activeSnd = &hud_snd.sounds[selected_index];
    float freq = hud_snd.m_activeSnd->freq;
    Fvector pos = (flags & sm_2D) ? Fvector{} : position;

    static const float hud_vol = READ_IF_EXISTS(pSettings, r_float, "hud_sound", "hud_sound_vol_k", 1.0f);
    float vol = hud_snd.m_activeSnd->volume * (b_hud_mode ? hud_vol : 1.0f);

    if (overlap)
    {
        hud_snd.m_activeSnd->snd.play_no_feedback(const_cast<CObject*>(parent), flags, hud_snd.m_activeSnd->delay, &pos, &vol , &freq);
    }
    else
    {
        hud_snd.m_activeSnd->snd.play_at_pos(const_cast<CObject*>(parent), pos, flags, hud_snd.m_activeSnd->delay);
        hud_snd.m_activeSnd->snd.set_volume(vol);
        hud_snd.m_activeSnd->snd.set_frequency(freq);
    }
}

void HUD_SOUND::StopSound(HUD_SOUND& hud_snd)
{
    for (auto& layer : hud_snd.timeline_layers)
        for (auto& sound : layer)
            sound.snd.stop();
    for (auto& sound : hud_snd.sounds)
        sound.snd.stop();

    hud_snd.m_activeSnd = nullptr;
}


//--------------------------------------------------------------------------------------------
//----------------------------------LAYERED  SOUND--------------------------------------------
//--------------------------------------------------------------------------------------------
HUD_SOUND_COLLECTION::~HUD_SOUND_COLLECTION()
{
    for (auto& sound_item : m_sound_items)
    {
        HUD_SOUND::StopSound(sound_item);
        HUD_SOUND::DestroySound(sound_item);
    }

    m_sound_items.clear();
}

HUD_SOUND* HUD_SOUND_COLLECTION::FindSoundItem(LPCSTR alias, bool b_assert)
{
    xr_vector<HUD_SOUND>::iterator it = std::find(m_sound_items.begin(), m_sound_items.end(), alias);

    if (it != m_sound_items.end())
        return &*it;

    R_ASSERT3(!b_assert, "sound item not found in collection", alias);
    return nullptr;
}

void HUD_SOUND_COLLECTION::PlaySound(LPCSTR alias, const Fvector& position, const CObject* parent, bool hud_mode, bool looped, u8 index)
{
    for (auto& sound_item : m_sound_items)
        if (sound_item.m_b_exclusive)
            HUD_SOUND::StopSound(sound_item);

    HUD_SOUND* snd_item = FindSoundItem(alias, true);
    HUD_SOUND::PlaySound(*snd_item, position, parent, hud_mode, looped, false, 1.f, index);
}

void HUD_SOUND_COLLECTION::StopSound(LPCSTR alias)
{
    HUD_SOUND* snd_item = FindSoundItem(alias, true);
    HUD_SOUND::StopSound(*snd_item);
}

void HUD_SOUND_COLLECTION::SetPosition(LPCSTR alias, const Fvector& pos)
{
    HUD_SOUND* snd_item = FindSoundItem(alias, true);
    if (snd_item->playing())
        snd_item->set_position(pos);
}

void HUD_SOUND_COLLECTION::StopAllSounds()
{
    for (auto& sound_item : m_sound_items)
        HUD_SOUND::StopSound(sound_item);
}

void HUD_SOUND_COLLECTION::LoadSound(LPCSTR section, LPCSTR line, LPCSTR alias, bool exclusive, int type)
{
    R_ASSERT(NULL == FindSoundItem(alias, false));
    m_sound_items.resize(m_sound_items.size() + 1);
    HUD_SOUND& snd_item = m_sound_items.back();
    HUD_SOUND::LoadSound(section, line, snd_item, type);
    snd_item.m_alias = alias;
    snd_item.m_b_exclusive = exclusive;
}

// Alundaio:
/*
    It's usage is to play a group of sounds HUD_SOUND_ITEMs as if they were a single layered entity. This is a achieved by
    wrapping the class around HUD_SOUND_COLLECTION and tagging them with the same alias. This way, when one for example
    sndShot is played, it will play all the sound items with the same alias.
*/
//----------------------------------------------------------
void HUD_SOUND_COLLECTION_LAYERED::StopAllSounds()
{
    for (auto& sound_item : m_sound_layered_items)
        sound_item.StopAllSounds();
}

void HUD_SOUND_COLLECTION_LAYERED::Clear()
{
    StopAllSounds();
    m_sound_layered_items.clear();
}

void HUD_SOUND_COLLECTION_LAYERED::ClearSound(pcstr alias)
{
    for (auto it = m_sound_layered_items.begin(); it != m_sound_layered_items.end();)
    {
        if (it->m_alias == alias)
            it = m_sound_layered_items.erase(it);
        else
            ++it;
    }
}

void HUD_SOUND_COLLECTION_LAYERED::StopSound(pcstr alias)
{
    for (auto& sound_item : m_sound_layered_items)
        if (sound_item.m_alias == alias)
            sound_item.StopSound(alias);
}

void HUD_SOUND_COLLECTION_LAYERED::SetPosition(pcstr alias, const Fvector& pos)
{
    // xr_vector<HUD_SOUND> m_sound_items;
    for (auto& sound_item : m_sound_layered_items)
        if (sound_item.m_alias == alias)
            sound_item.SetPosition(alias, pos);
}

void HUD_SOUND_COLLECTION_LAYERED::PlaySound(pcstr alias, const Fvector& position, const CObject* parent, bool hud_mode, bool looped, u8 index)
{
    for (auto& sound_item : m_sound_layered_items)
        if (sound_item.m_alias == alias)
            sound_item.PlaySound(alias, position, parent, hud_mode, looped, index);
}

HUD_SOUND* HUD_SOUND_COLLECTION_LAYERED::FindSoundItem(pcstr alias, bool b_assert)
{
    for (auto& sound_item : m_sound_layered_items)
        if (sound_item.m_alias == alias)
            return sound_item.FindSoundItem(alias, b_assert);

    return nullptr;
}

void HUD_SOUND_COLLECTION_LAYERED::LoadSound(pcstr section, pcstr line, pcstr alias, bool exclusive, int type)
{
    pcstr str = pSettings->r_string(section, line);
    string256 buf_str;

    int count = _GetItemCount(str);
    R_ASSERT(count);

    _GetItem(str, 0, buf_str);

    if (pSettings->section_exist(buf_str))
    {
        string256 sound_line;
        strcpy_s(sound_line, "snd_1_layer");
        int k = 1;
        while (pSettings->line_exist(buf_str, sound_line))
        {
            m_sound_layered_items.resize(m_sound_layered_items.size() + 1);
            HUD_SOUND_COLLECTION& snd_item = m_sound_layered_items.back();
            snd_item.LoadSound(buf_str, sound_line, alias, exclusive, type);
            snd_item.m_alias = alias;
            sprintf_s(sound_line, "snd_%d_layer", ++k);
        }
    }
    else // For compatibility with normal HUD_SOUND_COLLECTION sounds
    {
        m_sound_layered_items.resize(m_sound_layered_items.size() + 1);
        HUD_SOUND_COLLECTION& snd_item = m_sound_layered_items.back();
        snd_item.LoadSound(section, line, alias, exclusive, type);
        snd_item.m_alias = alias;
    }
}

void HUD_SOUND_COLLECTION_LAYERED::LoadSound(CInifile* ini, pcstr section, pcstr line, pcstr alias, bool exclusive, int type)
{
    pcstr str = ini->r_string(section, line);
    string256 buf_str;

    int count = _GetItemCount(str);
    R_ASSERT(count);

    _GetItem(str, 0, buf_str);

    if (ini->section_exist(buf_str))
    {
        string256 sound_line;
        strcpy_s(sound_line, "snd_1_layer");
        int k = 1;
        while (ini->line_exist(buf_str, sound_line))
        {
            m_sound_layered_items.resize(m_sound_layered_items.size() + 1);
            HUD_SOUND_COLLECTION& snd_item = m_sound_layered_items.back();
            // HUD_SOUND_COLLECTION currently reads from pSettings. External
            // ini support is retained for the section lookup; game weapon
            // definitions use the global settings file.
            snd_item.LoadSound(buf_str, sound_line, alias, exclusive, type);
            snd_item.m_alias = alias;
            sprintf_s(sound_line, "snd_%d_layer", ++k);
        }
    }
    else // For compatibility with normal HUD_SOUND_COLLECTION sounds
    {
        m_sound_layered_items.resize(m_sound_layered_items.size() + 1);
        HUD_SOUND_COLLECTION& snd_item = m_sound_layered_items.back();
        snd_item.LoadSound(section, line, alias, exclusive, type);
        snd_item.m_alias = alias;
    }
}
//-Alundaio
