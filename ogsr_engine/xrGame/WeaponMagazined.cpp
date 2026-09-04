#include "stdafx.h"
#include "hudmanager.h"
#include "WeaponMagazined.h"
#include "player_hud.h"
#include "../Include/xrRender/Kinematics.h"
#include "weaponBM16.h"
#include "entity.h"
#include "actor.h"
#include "torch.h"
#include "ParticlesObject.h"
#include "scope.h"
#include "silencer.h"
#include "GrenadeLauncher.h"
#include "inventory.h"
#include "xrserver_objects_alife_items.h"
#include "ActorEffector.h"
#include "EffectorZoomInertion.h"
#include "xr_level_controller.h"
#include "level.h"
#include "object_broker.h"
#include "string_table.h"
#include "WeaponBinoculars.h"
#include "WeaponBinocularsVision.h"
#include "ai_object_location.h"
#include "../xr_3da/gamemtllib.h"

#include "game_object_space.h"
#include "script_callback_ex.h"
#include "script_game_object.h"
#include <regex>
#include <limits>
#include "../xr_3da/x_ray.h"

#include "HudSound.h"
#include "UIGameCustom.h"
#include "ui/UIStatic.h"

CUIXml* g_wpnScopeXml = NULL;

CWeaponMagazined::CWeaponMagazined(LPCSTR name, ESoundTypes eSoundType) : CWeapon(name)
{
    m_eSoundShow = ESoundTypes(SOUND_TYPE_ITEM_TAKING | eSoundType);
    m_eSoundHide = ESoundTypes(SOUND_TYPE_ITEM_HIDING | eSoundType);
    m_eSoundShot = ESoundTypes(SOUND_TYPE_WEAPON_SHOOTING | eSoundType);
    m_eSoundEmptyClick = ESoundTypes(SOUND_TYPE_WEAPON_EMPTY_CLICKING | eSoundType);
    m_eSoundReload = ESoundTypes(SOUND_TYPE_WEAPON_RECHARGING | eSoundType);

    m_sSndShotCurrent = NULL;
    m_sSilencerFlameParticles = m_sSilencerSmokeParticles = NULL;

    m_bFireSingleShot = false;
    m_iShotNum = 0;
    m_iQueueSize = WEAPON_ININITE_QUEUE;
    m_bLockType = false;

    m_bHasDifferentFireModes = false;
    m_iCurFireMode = 0;
    m_iPrefferedFireMode = -1;

    m_binoc_vision = nullptr;
    m_bVision = false;
}

CWeaponMagazined::~CWeaponMagazined()
{
    ClearResolvedSounds();
    // sounds
    HUD_SOUND::DestroySound(sndShow);
    HUD_SOUND::DestroySound(sndHide);
    HUD_SOUND::DestroySound(sndShot);
    HUD_SOUND::DestroySound(sndSilencerShot);
    HUD_SOUND::DestroySound(sndEmptyClick);
    HUD_SOUND::DestroySound(sndReload);
    HUD_SOUND::DestroySound(sndReloadPartly);
    HUD_SOUND::DestroySound(sndReloadJammed);
    HUD_SOUND::DestroySound(sndReloadJammedLast);
    HUD_SOUND::DestroySound(sndFireModes);
    HUD_SOUND::DestroySound(sndZoomChange);
    HUD_SOUND::DestroySound(sndTactItemOn);
    HUD_SOUND::DestroySound(sndItemOn);
    HUD_SOUND::DestroySound(sndAimStart);
    HUD_SOUND::DestroySound(sndAimEnd);
	HUD_SOUND::DestroySound(sndBore);
	HUD_SOUND::DestroySound(sndBoreEmpty);
    HUD_SOUND::DestroySound(sndLook);
    HUD_SOUND::DestroySound(sndMagCheck);
    HUD_SOUND::DestroySound(sndMuzzleCheck);
    HUD_SOUND::DestroySound(sndReady);
    HUD_SOUND::DestroySound(sndLoadSingle);
    HUD_SOUND::DestroySound(sndMagazineReload);
    HUD_SOUND::DestroySound(sndMagazineReloadTactical);
    HUD_SOUND::DestroySound(sndMagazineReloadEmpty);
    HUD_SOUND::DestroySound(sndMagazineCheckVariant);
    if (m_binoc_vision)
        xr_delete(m_binoc_vision);
}

void CWeaponMagazined::ClearResolvedSounds()
{
    for (auto& [key, sound] : m_resolved_sounds)
    {
        if (sound)
        {
            HUD_SOUND::DestroySound(*sound);
            xr_delete(sound);
        }
    }
    m_resolved_sounds.clear();
}

shared_str CWeaponMagazined::ResolveSoundProvider(LPCSTR line) const
{
    shared_str result;
    int best_priority = std::numeric_limits<int>::min();
    auto consider = [&](LPCSTR candidate, bool base) {
        if (!candidate || !candidate[0] || !pSettings->section_exist(candidate) || !pSettings->line_exist(candidate, line))
            return;
        const int priority = base ? std::numeric_limits<int>::min() + 1 :
            READ_IF_EXISTS(pSettings, r_s32, candidate, "sound_override_priority", 0);
        if (!result.c_str() || priority >= best_priority)
        {
            result = candidate;
            best_priority = priority;
        }
    };

    consider(cNameSect().c_str(), true);
    if (IsScopeAttached())
        consider(m_sScopeName.c_str(), false);
    if (IsSilencerAttached())
        consider(m_sSilencerName.c_str(), false);
    if (IsGrenadeLauncherAttached())
        consider(m_sGrenadeLauncherName.c_str(), false);
    for (u8 slot = 0; slot < eCustomAddonCount; ++slot)
        consider(GetCustomAddonSection(static_cast<ECustomAddonSlot>(slot)).c_str(), false);
    return result;
}

HUD_SOUND* CWeaponMagazined::ResolveSoundLine(LPCSTR line, int type)
{
    const u32 timeline_revision = HUD_SOUND::TimelineConfigRevision();
    if (m_sound_timeline_revision != timeline_revision)
    {
        ClearResolvedSounds();
        m_sound_timeline_revision = timeline_revision;
    }

    const shared_str provider = ResolveSoundProvider(line);
    if (!provider.c_str())
        return nullptr;

    string512 cache_key;
    xr_sprintf(cache_key, "%s|%s", provider.c_str(), line);
    const shared_str cache_id = cache_key;
    const auto found = m_resolved_sounds.find(cache_id);
    if (found != m_resolved_sounds.end())
        return found->second;

    LPCSTR definition = pSettings->r_string(provider, line);
    string256 first_item;
    _GetItem(definition, 0, first_item);
    if (!_stricmp(first_item, "none") || !_stricmp(first_item, "false"))
    {
        m_resolved_sounds[cache_id] = nullptr;
        return nullptr;
    }

    HUD_SOUND* sound = xr_new<HUD_SOUND>();
    HUD_SOUND::LoadSound(provider.c_str(), line, *sound, type);
    m_resolved_sounds[cache_id] = sound;
    return sound;
}

HUD_SOUND* CWeaponMagazined::ResolveSound(HUD_SOUND& fallback)
{
    if (!fallback.m_config_line.c_str())
        return &fallback;
    HUD_SOUND* resolved = ResolveSoundLine(fallback.m_config_line.c_str(), fallback.m_config_type);
    return resolved ? resolved : (ResolveSoundProvider(fallback.m_config_line.c_str()).c_str() ? nullptr : &fallback);
}

void CWeaponMagazined::PlaySound(HUD_SOUND& sound, const Fvector& position, bool overlap)
{
    if (m_animation_sounds_replace_legacy)
        return;
    if (HUD_SOUND* resolved = ResolveSound(sound))
        HUD_SOUND::PlaySound(*resolved, position, CHudItem::object().H_Root(), !!GetHUDmode(), false, overlap);
}

void CWeaponMagazined::OnHudMotionStart(LPCSTR motion, float speed)
{
    if (!motion || !motion[0])
        return;
    for (auto& [key, cached] : m_resolved_sounds)
        if (cached && cached->m_config_line.c_str() && !_strnicmp(cached->m_config_line.c_str(), "snd_anm_", 8))
            HUD_SOUND::StopSound(*cached);
    if (SoundEditorSuppressesMotionSound())
        return;
    string256 line;
    xr_sprintf(line, "snd_anm_%s", motion);
    if (HUD_SOUND* sound = ResolveSoundLine(line, m_eSoundReload))
        HUD_SOUND::PlaySound(*sound, get_LastFP(), CHudItem::object().H_Root(), !!GetHUDmode(), false, false, speed > EPS_S ? 1.f / speed : 1.f);
}

void CWeaponMagazined::RegisterShotSound(LPCSTR section, LPCSTR line, LPCSTR alias)
{
    (void)section; // kept in the signature for config-loading call-site clarity
    m_shot_alias_lines[alias] = line;
}

bool CWeaponMagazined::HasShotSound(LPCSTR alias) const
{
    const shared_str alias_id = alias;
    const auto found = m_shot_alias_lines.find(alias_id);
    return found != m_shot_alias_lines.end() && ResolveSoundProvider(found->second.c_str()).c_str();
}

void CWeaponMagazined::PlayShotSound(LPCSTR alias)
{
    const shared_str alias_id = alias;
    const auto found = m_shot_alias_lines.find(alias_id);
    if (found == m_shot_alias_lines.end())
        return;
    if (HUD_SOUND* sound = ResolveSoundLine(found->second.c_str(), m_eSoundShot))
        HUD_SOUND::PlaySound(*sound, get_LastFP(), H_Root(), !!GetHUDmode(), false, true);
}

bool CWeaponMagazined::IsShotIndoor()
{
    if (!g_pGameLevel || m_indoor_sound_check_distance <= 0.f)
        return false;

    Fvector up;
    up.set(0.f, 1.f, 0.f);
    const collide::ray_defs query(
        get_LastFP(), up, m_indoor_sound_check_distance, CDB::OPT_CULL, collide::rqtStatic);
    collide::rq_results results;
    bool indoor = false;
    Level().ObjectSpace.RayQuery(
        results, query,
        [](collide::rq_result& result, LPVOID params) -> BOOL {
            const CDB::TRI* triangle = Level().ObjectSpace.GetStaticTris() + result.element;
            const SGameMtl* material = GMLib.GetMaterialByIdx(triangle->material);
            LPCSTR material_name = material->m_Name.c_str();
            constexpr LPCSTR foliage_prefix = "materials\\bush";
            const bool foliage = material_name &&
                !_strnicmp(material_name, foliage_prefix, xr_strlen(foliage_prefix));
            const bool non_occluding = foliage || material->Flags.is(SGameMtl::flPassable) ||
                (fsimilar(material->fVisTransparencyFactor, 1.f, EPS) &&
                    material->Flags.is(SGameMtl::flSuppressWallmarks));
            if (non_occluding)
                return TRUE;

            *static_cast<bool*>(params) = true;
            return FALSE;
        },
        &indoor, nullptr, this);
    return indoor;
}

void CWeaponMagazined::StopHUDSounds()
{
    for (auto& [key, sound] : m_resolved_sounds)
        if (sound)
            HUD_SOUND::StopSound(*sound);

    HUD_SOUND::StopSound(sndShow);
    HUD_SOUND::StopSound(sndHide);

    HUD_SOUND::StopSound(sndEmptyClick);
    HUD_SOUND::StopSound(sndReload);
    HUD_SOUND::StopSound(sndReloadPartly);
    HUD_SOUND::StopSound(sndReloadJammed);
    HUD_SOUND::StopSound(sndReloadJammedLast);
    HUD_SOUND::StopSound(sndFireModes);
    HUD_SOUND::StopSound(sndZoomChange);
    HUD_SOUND::StopSound(sndTactItemOn);
    HUD_SOUND::StopSound(sndItemOn);
    HUD_SOUND::StopSound(sndAimStart);
    HUD_SOUND::StopSound(sndAimEnd);
	HUD_SOUND::StopSound(sndBore);
    HUD_SOUND::StopSound(sndLook);
    HUD_SOUND::StopSound(sndMagCheck);
    HUD_SOUND::StopSound(sndMuzzleCheck);
    HUD_SOUND::StopSound(sndReady);
    HUD_SOUND::StopSound(sndLoadSingle);
    HUD_SOUND::StopSound(sndMagazineReload);
    HUD_SOUND::StopSound(sndMagazineReloadTactical);
    HUD_SOUND::StopSound(sndMagazineReloadEmpty);
    HUD_SOUND::StopSound(sndMagazineCheckVariant);

    HUD_SOUND::StopSound(sndShot);
    HUD_SOUND::StopSound(sndSilencerShot);

    inherited::StopHUDSounds();
}

void CWeaponMagazined::net_Destroy()
{
    m_world_firemode_pose_model = nullptr;
    m_hud_firemode_pose_model = nullptr;
    m_applied_world_firemode_pose = -1;
    m_applied_hud_firemode_pose = -1;
    m_world_firemode_transition_end = 0;
    inherited::net_Destroy();
    if (m_binoc_vision)
        xr_delete(m_binoc_vision);
}

BOOL CWeaponMagazined::net_Spawn(CSE_Abstract* DC)
{
    BOOL bRes = inherited::net_Spawn(DC);
    const auto wpn = smart_cast<CSE_ALifeItemWeaponMagazined*>(DC);
    m_iCurFireMode = wpn->m_u8CurFireMode;
    if (HasFireModes() && m_iCurFireMode >= m_aFireModes.size())
    {
        Msg("! [%s]: %s: wrong m_iCurFireMode[%u/%u]", __FUNCTION__, cName().c_str(), m_iCurFireMode, m_aFireModes.size() - 1);
        m_iCurFireMode = m_aFireModes.size() - 1;
        auto se_obj = alife_object();
        if (se_obj)
        {
            auto W = smart_cast<CSE_ALifeItemWeaponMagazined*>(se_obj);
            W->m_u8CurFireMode = m_iCurFireMode;
        }
    }
    SetQueueSize(GetCurrentFireMode());
    m_previous_firemode_animation_index = CurrentFireModeAnimationIndex();
    m_applied_world_firemode_pose = -1;
    m_applied_hud_firemode_pose = -1;
    return bRes;
}

void CWeaponMagazined::Load(LPCSTR section)
{
    inherited::Load(section);

    // Sounds
    HUD_SOUND::LoadSound(section, "snd_draw", sndShow, m_eSoundShow);
    HUD_SOUND::LoadSound(section, "snd_holster", sndHide, m_eSoundHide);

    // Alundaio: LAYERED_SND_SHOOT
    RegisterShotSound(section, "snd_shoot", "sndShot");

    RegisterShotSound(section, "snd_shoot_actor", "sndShotActor");
    RegisterShotSound(section, "snd_shoot_indoor", "sndShotIndoor");
    RegisterShotSound(section, "snd_shoot_actor_indoor", "sndShotIndoorActor");
    RegisterShotSound(section, "snd_shoot_misfire", "sndShotMisfire");
    RegisterShotSound(section, "snd_shoot_misfire_actor", "sndShotMisfireActor");
    RegisterShotSound(section, "snd_shoot_misfire_indoor", "sndShotIndoorMisfire");
    RegisterShotSound(section, "snd_shoot_misfire_actor_indoor", "sndShotIndoorMisfireActor");

    HUD_SOUND::LoadSound(section, "snd_empty", sndEmptyClick, m_eSoundEmptyClick);

    if (pSettings->line_exist(section, "snd_reload_empty"))
        HUD_SOUND::LoadSound(section, "snd_reload_empty", sndReload, m_eSoundReload);
    else
        HUD_SOUND::LoadSound(section, "snd_reload", sndReload, m_eSoundReload);

    if (pSettings->line_exist(section, "snd_reload_jammed"))
        HUD_SOUND::LoadSound(section, "snd_reload_jammed", sndReloadJammed, m_eSoundReload);

    if (pSettings->line_exist(section, "snd_reload_jammed_last"))
        HUD_SOUND::LoadSound(section, "snd_reload_jammed_last", sndReloadJammedLast, m_eSoundReload);

    if (pSettings->line_exist(section, "snd_reload_empty")) // OpenXRay-style неполная перезарядка
        HUD_SOUND::LoadSound(section, "snd_reload", sndReloadPartly, m_eSoundReload);
    else if (pSettings->line_exist(section, "snd_reload_partly")) // OGSR-style неполная перезарядка
        HUD_SOUND::LoadSound(section, "snd_reload_partly", sndReloadPartly, m_eSoundReload);

    if (pSettings->line_exist(section, "snd_fire_modes"))
        HUD_SOUND::LoadSound(section, "snd_fire_modes", sndFireModes, m_eSoundEmptyClick);
	else if (pSettings->line_exist(section, "snd_changefiremode")) //STCoP or IWP firemode sound
        HUD_SOUND::LoadSound(section, "snd_changefiremode", sndFireModes, m_eSoundEmptyClick);
		
    if (pSettings->line_exist(section, "snd_zoom_change"))
        HUD_SOUND::LoadSound(section, "snd_zoom_change", sndZoomChange, m_eSoundEmptyClick);
    if (pSettings->line_exist(section, "snd_tact_item_on"))
        HUD_SOUND::LoadSound(section, "snd_tact_item_on", sndTactItemOn, m_eSoundEmptyClick);
    if (pSettings->line_exist(section, "snd_item_on"))
        HUD_SOUND::LoadSound(section, "snd_item_on", sndItemOn, m_eSoundEmptyClick);

    if (pSettings->line_exist(section, "snd_aim_start"))
        HUD_SOUND::LoadSound(section, "snd_aim_start", sndAimStart, m_eSoundShow);
    if (pSettings->line_exist(section, "snd_aim_end"))
        HUD_SOUND::LoadSound(section, "snd_aim_end", sndAimEnd, m_eSoundHide);
	
	if (pSettings->line_exist(section, "snd_bore"))
        HUD_SOUND::LoadSound(section, "snd_bore", sndBore, m_eSoundEmptyClick);
	
	if (pSettings->line_exist(section, "snd_bore_empty"))
        HUD_SOUND::LoadSound(section, "snd_bore_empty", sndBoreEmpty, m_eSoundEmptyClick);

    if (pSettings->line_exist(section, "snd_look"))
        HUD_SOUND::LoadSound(section, "snd_look", sndLook, m_eSoundEmptyClick);
    if (pSettings->line_exist(section, "snd_magcheck"))
        HUD_SOUND::LoadSound(section, "snd_magcheck", sndMagCheck, m_eSoundEmptyClick);
    if (pSettings->line_exist(section, "snd_muzzle_check"))
        HUD_SOUND::LoadSound(section, "snd_muzzle_check", sndMuzzleCheck, m_eSoundEmptyClick);
    if (pSettings->line_exist(section, "snd_ready"))
        HUD_SOUND::LoadSound(section, "snd_ready", sndReady, m_eSoundShow);
    if (pSettings->line_exist(section, "snd_load_single"))
        HUD_SOUND::LoadSound(section, "snd_load_single", sndLoadSingle, m_eSoundReload);

    m_bore_idle_time_min = READ_IF_EXISTS(pSettings, r_float, section, "bore_idle_time_min", 30.f);
    m_bore_idle_time_max = READ_IF_EXISTS(pSettings, r_float, section, "bore_idle_time_max", 60.f);
    if (m_bore_idle_time_max < m_bore_idle_time_min)
        std::swap(m_bore_idle_time_min, m_bore_idle_time_max);
	

    m_sSndShotCurrent = "sndShot";
    m_animation_sounds_replace_legacy = READ_IF_EXISTS(pSettings, r_bool, section, "animation_sounds_replace_legacy", false);
    m_indoor_sound_check_distance = READ_IF_EXISTS(pSettings, r_float, section, "indoor_sound_check_distance", 30.f);

    // Silencer sounds are shared by legacy and class-based muzzle addons.
    if (m_eSilencerStatus == ALife::eAddonAttachable || pSettings->line_exist(section, "snd_silncer_shot"))
    {
        if (pSettings->line_exist(section, "silencer_flame_particles"))
            m_sSilencerFlameParticles = pSettings->r_string(section, "silencer_flame_particles");
        if (pSettings->line_exist(section, "silencer_smoke_particles"))
            m_sSilencerSmokeParticles = pSettings->r_string(section, "silencer_smoke_particles");
        // Alundaio: LAYERED_SND_SHOOT Silencer
        //-Alundaio
    }
    // Register aliases even when the base weapon has no suppressor sound: a
    // class-based muzzle attachment may provide the complete profile itself.
    RegisterShotSound(section, "snd_silncer_shot", "sndSilencerShot");
    RegisterShotSound(section, "snd_silncer_shot_actor", "sndSilencerShotActor");
    RegisterShotSound(section, "snd_silncer_shot_indoor", "sndSilencerShotIndoor");
    RegisterShotSound(section, "snd_silncer_shot_actor_indoor", "sndSilencerShotIndoorActor");
    RegisterShotSound(section, "snd_silncer_shot_misfire", "sndSilencerShotMisfire");
    RegisterShotSound(section, "snd_silncer_shot_misfire_actor", "sndSilencerShotMisfireActor");
    RegisterShotSound(section, "snd_silncer_shot_misfire_indoor", "sndSilencerShotIndoorMisfire");
    RegisterShotSound(section, "snd_silncer_shot_misfire_actor_indoor", "sndSilencerShotIndoorMisfireActor");
    //  [7/20/2005]
    if (pSettings->line_exist(section, "dispersion_start"))
        m_iShootEffectorStart = pSettings->r_u8(section, "dispersion_start");
    else
        m_iShootEffectorStart = 0;
    //  [7/20/2005]
    //  [7/21/2005]
    if (pSettings->line_exist(section, "fire_modes"))
    {
        m_bHasDifferentFireModes = true;
        shared_str FireModesList = pSettings->r_string(section, "fire_modes");
        int ModesCount = _GetItemCount(FireModesList.c_str());
        m_aFireModes.clear();
        for (int i = 0; i < ModesCount; i++)
        {
            string16 sItem;
            _GetItem(FireModesList.c_str(), i, sItem);
            int FireMode = atoi(sItem);
            m_aFireModes.push_back(FireMode);
        }

        m_firemode_animation_indices.clear();
        if (pSettings->line_exist(section, "firemode_animation_indices"))
        {
            LPCSTR indices = pSettings->r_string(section, "firemode_animation_indices");
            const int index_count = _GetItemCount(indices);
            if (index_count == ModesCount)
            {
                for (int i = 0; i < index_count; ++i)
                {
                    string16 item;
                    _GetItem(indices, i, item);
                    m_firemode_animation_indices.push_back(atoi(item));
                }
            }
            else
                Msg("! [%s]: firemode_animation_indices must contain %d values, got %d", section, ModesCount, index_count);
        }
        m_iCurFireMode = ModesCount - 1;
        m_iPrefferedFireMode = READ_IF_EXISTS(pSettings, r_s16, section, "preffered_fire_mode", -1);
        m_previous_firemode_animation_index = CurrentFireModeAnimationIndex();
    }
    else
    {
        m_bHasDifferentFireModes = false;
        m_firemode_animation_indices.clear();
    }

    m_bVision = !!READ_IF_EXISTS(pSettings, r_bool, section, "vision_present", false);
    m_fire_zoomout_time = READ_IF_EXISTS(pSettings, r_u32, section, "fire_zoomout_time", u32(-1));

    m_str_count_tmpl = READ_IF_EXISTS(pSettings, r_string, "features", "wpn_magazined_str_count_tmpl", "{AE}/{AC}");

    CartridgeInTheChamberEnabled = READ_IF_EXISTS(pSettings, r_bool, section, "CartridgeInTheChamberEnabled", false);

    if (pSettings->line_exist(section, "bullet_bones"))
    {
        bHasBulletsToHide = true;
        LPCSTR str = pSettings->r_string(section, "bullet_bones");
        for (int i = 0, count = _GetItemCount(str); i < count; ++i)
        {
            string128 bullet_bone_name;
            _GetItem(str, i, bullet_bone_name);
            bullets_bones.push_back(bullet_bone_name);
            bullet_cnt++;
        }
    }

    m_bFlameParticlesHideInZoom = READ_IF_EXISTS(pSettings, r_bool, section, "flame_particles_hide_in_zoom", false);
}

void CWeaponMagazined::PlaySoundShot()
{
    if (m_animation_sounds_replace_legacy)
        return;
    string128 environment_alias;
    xr_strcpy(environment_alias, m_sSndShotCurrent.c_str());
    if (IsShotIndoor())
    {
        string128 indoor_alias;
        xr_strconcat(indoor_alias, environment_alias, "Indoor");
        if (HasShotSound(indoor_alias))
            xr_strcpy(environment_alias, indoor_alias);
    }

    if (ParentIsActor())
    {
        if (eMisfire)
        {
            string128 sndNameMisfire;
            strconcat(sizeof(sndNameMisfire), sndNameMisfire, environment_alias, "MisfireActor");
            if (HasShotSound(sndNameMisfire))
            {
                PlayShotSound(sndNameMisfire);
                return;
            }
        }

        string128 sndName;
        strconcat(sizeof(sndName), sndName, environment_alias, "Actor");
        if (HasShotSound(sndName))
        {
            PlayShotSound(sndName);
            return;
        }
    }

    if (eMisfire)
    {
        string128 sndNameMisfire;
        strconcat(sizeof(sndNameMisfire), sndNameMisfire, environment_alias, "Misfire");
        if (HasShotSound(sndNameMisfire))
        {
            PlayShotSound(sndNameMisfire);
            return;
        }
    }

    PlayShotSound(environment_alias);
}

void CWeaponMagazined::FireStart()
{
    if (IsValid() && (!IsMisfire() || IsGrenadeMode()))
    {
        if (!IsWorking() || AllowFireWhileWorking())
        {
            if (GetState() == eReload)
                return;
            if (GetState() == eShowing)
                return;
            if (GetState() == eHiding)
                return;
            if (GetState() == eMisfire)
                return;
			if (GetState() == eBore)
                return;
			if (GetState() == eLook || GetState() == eMagCheck || GetState() == eMuzzleCheck)
                return;
			if (GetState() == eFiremode)
                return;

            inherited::FireStart();

            if (iAmmoElapsed == 0)
                OnMagazineEmpty();
            else
                SwitchState(eFire);
        }
    }
    else if (IsMisfire() && !IsGrenadeMode())
    {
        if (smart_cast<CActor*>(H_Parent()))
        {
            HUD().GetUI()->AddInfoMessage("gun_jammed");
            Misfire();
        }
    }
    else if (eReload != GetState() && eMisfire != GetState())
        OnMagazineEmpty();
}

void CWeaponMagazined::FireEnd()
{
    inherited::FireEnd();

    if (Core.Features.test(xrCore::Feature::autoreload_wpn))
    {
        auto actor = smart_cast<CActor*>(H_Parent());
        if (!iAmmoElapsed && actor && GetState() != eReload)
            Reload();
    }
}

int CWeaponMagazined::CheckAmmoBeforeReload(u32& v_ammoType)
{
    if (m_set_next_ammoType_on_reload != u32(-1))
        v_ammoType = m_set_next_ammoType_on_reload;

    // Msg("Ammo type in next reload : %d", m_set_next_ammoType_on_reload);

    if (m_ammoTypes.size() <= v_ammoType)
    {
        // Msg("Ammo type is wrong : %d", v_ammoType);
        return 0;
    }

    LPCSTR tmp_sect_name = m_ammoTypes[v_ammoType].c_str();

    if (!tmp_sect_name)
    {
        // Msg("Sect name is wrong");
        return 0;
    }

    CWeaponAmmo* ammo = smart_cast<CWeaponAmmo*>(m_pCurrentInventory->GetAny(tmp_sect_name));

    if (!ammo && !m_bLockType)
    {
        for (u8 i = 0; i < u8(m_ammoTypes.size()); ++i)
        {
            //проверить патроны всех подходящих типов
            ammo = smart_cast<CWeaponAmmo*>(m_pCurrentInventory->GetAny(m_ammoTypes[i].c_str()));
            if (ammo)
            {
                v_ammoType = i;
                break;
            }
        }
    }

    // Msg("Ammo type %d", v_ammoType);

    return GetAmmoCount(v_ammoType);
}

void CWeaponMagazined::Reload()
{
    inherited::Reload();

    TryReload();
}

bool CWeaponMagazined::TryReload()
{
    if (m_pCurrentInventory)
    {
        bool forActor = ParentIsActor();

        m_pAmmo = smart_cast<CWeaponAmmo*>(m_pCurrentInventory->GetAmmo(*m_ammoTypes[m_ammoType], forActor));

        if ((m_pAmmo || m_set_next_ammoType_on_reload != u32(-1)) || unlimited_ammo() || (IsMisfire() && iAmmoElapsed))
        {
            SetPending(TRUE);
            SwitchState(eReload);
            return true;
        }
        else
            for (u32 i = 0; i < m_ammoTypes.size(); ++i)
            {
                m_pAmmo = smart_cast<CWeaponAmmo*>(m_pCurrentInventory->GetAmmo(*m_ammoTypes[i], forActor));
                if (m_pAmmo)
                {
                    m_set_next_ammoType_on_reload = i; // https://github.com/revolucas/CoC-Xray/pull/5/commits/3c45cad1edb388664efbe3bb20a29f92e2d827ca
                    SetPending(TRUE);
                    SwitchState(eReload);
                    return true;
                }
            }
    }

    return false;
}

void CWeaponMagazined::OnMagazineEmpty()
{
    //попытка стрелять когда нет патронов
    if (GetState() == eIdle)
    {
        OnEmptyClick();
        return;
    }

    if (GetNextState() != eMagEmpty && GetNextState() != eReload)
    {
        SwitchState(eMagEmpty);
    }

    inherited::OnMagazineEmpty();
}

void CWeaponMagazined::UnloadMagazine(bool spawn_ammo)
{
    last_hide_bullet = -1;

    xr_map<LPCSTR, u16> l_ammo;

    while (!m_magazine.empty())
    {
        CCartridge& l_cartridge = m_magazine.back();
        xr_map<LPCSTR, u16>::iterator l_it;
        for (l_it = l_ammo.begin(); l_ammo.end() != l_it; ++l_it)
        {
            if (!xr_strcmp(*l_cartridge.m_ammoSect, l_it->first))
            {
                ++(l_it->second);
                break;
            }
        }

        if (l_it == l_ammo.end())
            l_ammo[*l_cartridge.m_ammoSect] = 1;
        m_magazine.pop_back();
        --iAmmoElapsed;
    }

    VERIFY((u32)iAmmoElapsed == m_magazine.size());

    UpdateEmptyBonesVisibility();

    if (!spawn_ammo)
        return;

    bool forActor = ParentIsActor();

    xr_map<LPCSTR, u16>::iterator l_it;
    for (l_it = l_ammo.begin(); l_ammo.end() != l_it; ++l_it)
    {
        if (Core.Features.test(xrCore::Feature::hard_ammo_reload) ? (!forActor && m_pCurrentInventory) : !!m_pCurrentInventory)
        {
            CWeaponAmmo* l_pA = smart_cast<CWeaponAmmo*>(m_pCurrentInventory->GetAmmo(l_it->first, forActor));
            if (l_pA)
            {
                u16 l_free = l_pA->m_boxSize - l_pA->m_boxCurr;
                l_pA->m_boxCurr = l_pA->m_boxCurr + (l_free < l_it->second ? l_free : l_it->second);
                l_it->second = l_it->second - (l_free < l_it->second ? l_free : l_it->second);
            }
        }
        if (l_it->second && !unlimited_ammo())
            SpawnAmmo(l_it->second, l_it->first);
    }
}

void CWeaponMagazined::ReloadMagazine()
{
    m_dwAmmoCurrentCalcFrame = 0;

    //устранить осечку при перезарядке
    if (IsMisfire() && !IsGrenadeMode())
    {
        SwitchMisfire(false);
        return;
    }

    //переменная блокирует использование
    //только разных типов патронов
    if (!m_bLockType)
    {
        m_pAmmo = NULL;
    }

    if (!m_pCurrentInventory)
        return;

    if (m_set_next_ammoType_on_reload != u32(-1))
    {
        m_ammoType = m_set_next_ammoType_on_reload;
        m_set_next_ammoType_on_reload = u32(-1);
    }

    if (!unlimited_ammo())
    {
        bool forActor = ParentIsActor();

        //попытаться найти в инвентаре патроны текущего типа
        if (Core.Features.test(xrCore::Feature::hard_ammo_reload) && forActor)
            m_pAmmo = smart_cast<CWeaponAmmo*>(m_pCurrentInventory->GetAmmoMaxCurr(*m_ammoTypes[m_ammoType], forActor));
        else
            m_pAmmo = smart_cast<CWeaponAmmo*>(m_pCurrentInventory->GetAmmo(*m_ammoTypes[m_ammoType], forActor));

        if (!m_pAmmo && !m_bLockType)
        {
            for (u32 i = 0; i < m_ammoTypes.size(); ++i)
            {
                //проверить патроны всех подходящих типов
                if (Core.Features.test(xrCore::Feature::hard_ammo_reload) && forActor)
                    m_pAmmo = smart_cast<CWeaponAmmo*>(m_pCurrentInventory->GetAmmoMaxCurr(*m_ammoTypes[i], forActor));
                else
                    m_pAmmo = smart_cast<CWeaponAmmo*>(m_pCurrentInventory->GetAmmo(*m_ammoTypes[i], forActor));

                if (m_pAmmo)
                {
                    m_ammoType = i;
                    break;
                }
            }
        }
    }

    //нет патронов для перезарядки
    if (!m_pAmmo && !unlimited_ammo())
        return;

    //разрядить магазин, если загружаем патронами другого типа
    if (Core.Features.test(xrCore::Feature::hard_ammo_reload))
    {
        if (!m_bLockType && !m_magazine.empty())
            if ((ParentIsActor() && !unlimited_ammo()) || (!m_pAmmo || xr_strcmp(m_pAmmo->cNameSect(), *m_magazine.back().m_ammoSect)))
                UnloadMagazine();
    }
    else
    {
        if (!m_bLockType && !m_magazine.empty() && (!m_pAmmo || xr_strcmp(m_pAmmo->cNameSect(), *m_magazine.back().m_ammoSect)))
            UnloadMagazine();
    }

    VERIFY((u32)iAmmoElapsed == m_magazine.size());

    if (m_DefaultCartridge.m_LocalAmmoType != m_ammoType)
        m_DefaultCartridge.Load(*m_ammoTypes[m_ammoType], u8(m_ammoType));
    CCartridge l_cartridge = m_DefaultCartridge;
    while (iAmmoElapsed < ReloadTargetCapacity())
    {
        if (!unlimited_ammo())
        {
            if (!m_pAmmo->Get(l_cartridge))
                break; //-V595
        }
        ++iAmmoElapsed;
        l_cartridge.m_LocalAmmoType = u8(m_ammoType);
        m_magazine.push_back(l_cartridge);
    }

    VERIFY((u32)iAmmoElapsed == m_magazine.size());

    UpdateEmptyBonesVisibility();

    //выкинуть коробку патронов, если она пустая
    if (m_pAmmo && !m_pAmmo->m_boxCurr)
        m_pAmmo->SetDropManual(TRUE);

    if (Core.Features.test(xrCore::Feature::hard_ammo_reload) && ParentIsActor() && m_pAmmo)
    {
        int box_size = m_pAmmo->m_boxSize;
        if (!m_bLockType && iMagazineSize > iAmmoElapsed && iMagazineSize > box_size)
        {
            m_bLockType = true;
            int need_ammo = iMagazineSize - box_size;
            while (need_ammo > 0)
            {
                ReloadMagazine();
                if (need_ammo < box_size)
                    break;
                need_ammo -= box_size;
            }
            m_bLockType = false;
        }
    }
    else if (iMagazineSize > iAmmoElapsed)
    {
        m_bLockType = true;
        ReloadMagazine();
        m_bLockType = false;
    }

    VERIFY((u32)iAmmoElapsed == m_magazine.size());
}

void CWeaponMagazined::Misfire()
{
    inherited::Misfire();

    if (IsZoomed() && !IsRotatingToZoom())
    {
        OnEmptyClick();
    }
    else
    {
        SetPending(TRUE);
        SwitchState(eMisfire);
    }
}

void CWeaponMagazined::DeviceSwitch()
{
    inherited::DeviceSwitch();

    SetPending(TRUE);
    SwitchState(eDeviceSwitch);
}

void CWeaponMagazined::OnStateSwitch(u32 S, u32 oldState)
{
    inherited::OnStateSwitch(S, oldState);
    if (S == eIdle)
        ScheduleNextBore();
    else
        m_next_bore_time = 0;

    switch (S)
    {
    case eIdle: switch2_Idle(); break;
    case eFire: switch2_Fire(); break;
    case eFire2: switch2_Fire2(); break;
    case eMisfire: {
        PlayAnimCheckMisfire();
    }
    break;
    case eMagEmpty: {
        const bool need_play_empty_click = (oldState != eFire && oldState != eFire2) || !dont_interrupt_shot_anm;
        switch2_Empty(need_play_empty_click);

        if (auto parent = smart_cast<CActor*>(H_Parent()))
        {
            parent->callback(GameObject::eOnActorWeaponEmpty)(lua_game_object());
        }

        if (GetNextState() != eReload && need_play_empty_click)
        {
            SwitchState(eIdle);
        }
        break;
    }
    case eReload: {
        switch2_Reload();

        if (auto parent = smart_cast<CActor*>(H_Parent()))
        {
            parent->callback(GameObject::eOnActorWeaponReload)(lua_game_object());
        }
        break;
    }
    case eShowing: switch2_Showing(); break;
    case eHiding: switch2_Hiding(); break;
    case eHidden: switch2_Hidden(); break;
    case eDeviceSwitch:
        PlayAnimDeviceSwitch();
        break;
	case eBore: PlayAnimBore(); break;
	case eLook: PlayAnimLook(); break;
    case eMagCheck: PlayAnimMagCheck(); break;
    case eMuzzleCheck: PlayAnimMuzzleCheck(); break;
	case eFiremode: PlayAnimFiremode(); break;
    }
}

void CWeaponMagazined::DeviceUpdate()
{
    if (auto pA = smart_cast<CActor*>(H_Parent()))
    {
        if (LaserSwitch)
        {
            SwitchLaser(!IsLaserOn());
            LaserSwitch = false;
        }
        else if (TorchSwitch)
        {
            SwitchFlashlight(!IsFlashlightOn());
            TorchSwitch = false;
        }
        else if (HeadLampSwitch)
        {
            auto pActorTorch = smart_cast<CTorch*>(pA->inventory().ItemFromSlot(TORCH_SLOT));
            pActorTorch->Switch();
            HeadLampSwitch = false;
        }
        else if (NightVisionSwitch)
        {
            if (auto pActorTorch = smart_cast<CTorch*>(pA->inventory().ItemFromSlot(TORCH_SLOT)))
                pActorTorch->SwitchNightVision();
            NightVisionSwitch = false;
        }
    }
}

void CWeaponMagazined::UpdateCL()
{
    inherited::UpdateCL();
    float dt = Device.fTimeDelta;

    //когда происходит апдейт состояния оружия
    //ничего другого не делать
    if (GetNextState() == GetState())
    {
        switch (GetState())
        {
        case eShowing:
        case eHiding:
        case eReload:
        case eMisfire:
        case eDeviceSwitch:
        case eSprintStart:
        case eSprintEnd:
		case eBore:
		case eLook:
        case eMagCheck:
        case eMuzzleCheck:
		case eFiremode:
        case eIdle:
            fTime -= dt;
            if (fTime < 0)
                fTime = 0;
            break;
        case eFire:
            if ((!IsMisfire() || IsGrenadeMode()) && GetAmmoElapsed() > 0)
                state_Fire(dt);

            if (fTime <= 0)
            {
                if ((!IsMisfire() || IsGrenadeMode()) && GetAmmoElapsed() == 0)
                    OnMagazineEmpty();
                StopShooting();
            }
            else
            {
                fTime -= dt;
            }

            if (m_fire_zoomout_time != u32(-1) && IsZoomed() && m_dwStateTime > m_fire_zoomout_time)
                OnZoomOut();

            break;
        case eMagEmpty:
        case eHidden: break;
        }
    }

    if (H_Parent() && IsZoomed() && !IsRotatingToZoom() && m_binoc_vision)
        m_binoc_vision->Update();

    const bool detachable_enabled = DetachableMagazineSystemActive();
    const shared_str& installed_magazine = InstalledMagazineSection();
    const shared_str desired_section = detachable_enabled ? installed_magazine : shared_str();
    if (detachable_enabled != m_applied_detachable_magazines || desired_section != m_applied_magazine_section)
        ApplyMagazineAddonConfiguration(true);

    if (GetState() == eIdle && GetNextState() == eIdle && !IsPending() && ParentIsActor())
    {
        CActor* actor = smart_cast<CActor*>(H_Parent());
        const bool really_idle = actor && !(actor->get_state() & mcAnyMove) && !IsZoomed();
        if (!really_idle)
            ScheduleNextBore();
        else if (m_next_bore_time && Device.dwTimeGlobal >= m_next_bore_time)
            SwitchState(eBore);
    }

    UpdateFireModePoses();

    UpdateSounds();
}

void CWeaponMagazined::UpdateSounds()
{
    if (Device.dwFrame == dwUpdateSounds_Frame)
        return;

    dwUpdateSounds_Frame = Device.dwFrame;

    // ref_sound positions
    if (sndShow.playing())
        sndShow.set_position(get_LastFP());
    if (sndHide.playing())
        sndHide.set_position(get_LastFP());
    if (sndShot.playing())
        sndShot.set_position(get_LastFP());
    if (sndSilencerShot.playing())
        sndSilencerShot.set_position(get_LastFP());
    if (sndReload.playing())
        sndReload.set_position(get_LastFP());
    if (sndReloadPartly.playing())
        sndReloadPartly.set_position(get_LastFP());
    if (sndReloadJammed.playing())
        sndReloadJammed.set_position(get_LastFP());
    if (sndReloadJammedLast.playing())
        sndReloadJammedLast.set_position(get_LastFP());
    if (sndEmptyClick.playing())
        sndEmptyClick.set_position(get_LastFP());
    if (sndFireModes.playing())
        sndFireModes.set_position(get_LastFP());
    if (sndZoomChange.playing())
        sndZoomChange.set_position(get_LastFP());
    if (sndTactItemOn.playing())
        sndTactItemOn.set_position(get_LastFP());
    if (sndItemOn.playing())
        sndItemOn.set_position(get_LastFP());
    if (sndAimStart.playing())
        sndAimStart.set_position(get_LastFP());
    if (sndAimEnd.playing())
        sndAimEnd.set_position(get_LastFP());
	if (sndBore.playing())
        sndBore.set_position(get_LastFP());
    if (sndLook.playing())
        sndLook.set_position(get_LastFP());
    if (sndMagCheck.playing())
        sndMagCheck.set_position(get_LastFP());
    if (sndMuzzleCheck.playing())
        sndMuzzleCheck.set_position(get_LastFP());
    if (sndReady.playing())
        sndReady.set_position(get_LastFP());
    if (sndLoadSingle.playing())
        sndLoadSingle.set_position(get_LastFP());
    if (sndMagazineReload.playing())
        sndMagazineReload.set_position(get_LastFP());
    if (sndMagazineReloadTactical.playing())
        sndMagazineReloadTactical.set_position(get_LastFP());
    if (sndMagazineReloadEmpty.playing())
        sndMagazineReloadEmpty.set_position(get_LastFP());
    if (sndMagazineCheckVariant.playing())
        sndMagazineCheckVariant.set_position(get_LastFP());
}

void CWeaponMagazined::state_Fire(float dt)
{
    VERIFY(fTimeToFire > 0.f);

    Fvector p1, d;
    p1.set(get_LastFP());
    d.set(get_LastFD());

    auto Parent = H_Parent();
    if (!Parent)
        return;

    auto ParentEnt = smart_cast<CEntity*>(Parent);
    if (!ParentEnt)
        return; //Такое иногда бывает. Не понятно почему, но бывает. Например был случай когда пыталось стрелять оружие лежащее в ящике.

#ifdef DEBUG
    CInventoryOwner* io = smart_cast<CInventoryOwner*>(H_Parent());
    if (!io->inventory().ActiveItem())
    {
        Log("current_state", GetState());
        Log("next_state", GetNextState());
        Log("state_time", m_dwStateTime);
        Log("item_sect", cNameSect().c_str());
        Log("H_Parent", H_Parent()->cNameSect().c_str());
    }
#endif

    ParentEnt->g_fireParams(this, p1, d);

    if (m_iShotNum == 0)
    {
        m_vStartPos = p1;
        m_vStartDir = d;
    }

    VERIFY(!m_magazine.empty());
    //	Msg("%d && %d && (%d || %d) && (%d || %d)", !m_magazine.empty(), fTime<=0, IsWorking(), m_bFireSingleShot, m_iQueueSize < 0, m_iShotNum < m_iQueueSize);
    while (!m_magazine.empty() && fTime <= 0 && (IsWorking() || m_bFireSingleShot) && (m_iQueueSize < 0 || m_iShotNum < m_iQueueSize))
    {
        m_bFireSingleShot = false;

        VERIFY(fTimeToFire > 0.f);
        // Alundaio: Use fModeShotTime instead of fOneShotTime if current fire mode is 2-shot burst
        // Alundaio: Cycle down RPM after two shots; used for Abakan/AN-94
        if (GetCurrentFireMode() == 2 || (bCycleDown == true && m_iShotNum < 1))
        {
            fTime += fTimeToFire2;
        }
        else
            fTime += fTimeToFire;
        // Alundaio: END

        ++m_iShotNum;

        if (!IsGrenadeMode())
            CheckForMisfire();

        OnShot();

        if (smart_cast<CWeaponBM16*>(this) && IsMisfire())
            return;

        if (auto parent = smart_cast<CActor*>(H_Parent()))
        {
            parent->callback(GameObject::eOnActorWeaponFire)(lua_game_object());
        }

        if (m_iShotNum > m_iShootEffectorStart)
            FireTrace(p1, d);
        else
            FireTrace(m_vStartPos, m_vStartDir);
    }

    if (m_iShotNum == m_iQueueSize)
        m_bStopedAfterQueueFired = true;

    UpdateSounds();
}

void CWeaponMagazined::SetDefaults() { CWeapon::SetDefaults(); }

void CWeaponMagazined::OnShot()
{
    // Если актор бежит - останавливаем его
    if (ParentIsActor())
        Actor()->set_state_wishful(Actor()->get_state_wishful() & (~mcSprint));

    // Sound
    PlaySoundShot();

    // Camera
    AddShotEffector();

    // Animation
    PlayAnimShoot();

    // Shell Drop
    Fvector vel;
    PHGetLinearVell(vel);
    OnShellDrop(get_LastSP(), vel);

    // Огонь из ствола
    if (ShouldPlayFlameParticles())
    {
        StartFlameParticles();
        ForceUpdateFireParticles();
    }

    //дым из ствола
    StartSmokeParticles(get_LastFP(), vel);

    update_visual_bullet_textures();
	
	AddHudShootingEffect();
}

void CWeaponMagazined::OnEmptyClick()
{
    PlayAnimFakeShoot();
    PlaySound(sndEmptyClick, get_LastFP());
}

void CWeaponMagazined::OnAnimationEnd(u32 state)
{
    switch (state)
    {
    case eReload:
        ReloadMagazine();
        HUD_SOUND::StopSound(sndReload);
        HUD_SOUND::StopSound(sndReloadPartly);
        HUD_SOUND::StopSound(sndReloadJammed);
        HUD_SOUND::StopSound(sndReloadJammedLast);
        HUD_SOUND::StopSound(sndLoadSingle);
        HUD_SOUND::StopSound(sndMagazineReload);
        HUD_SOUND::StopSound(sndMagazineReloadTactical);
        HUD_SOUND::StopSound(sndMagazineReloadEmpty);
        bullet_update = true;
        SwitchState(eIdle);
        break; // End of reload animation
    case eHiding: SwitchState(eHidden); break; // End of Hide
    case eIdle: switch2_Idle(); break; // Keep showing idle
    case eShowing: {
        update_visual_bullet_textures(true);
        SwitchState(eIdle);
        break;
    }
    case eMisfire:
    case eDeviceSwitch:
    case eFire:
    case eFire2: SwitchState(eIdle); break;
	case eBore: SwitchState(eIdle); break;
	case eLook: SwitchState(eIdle); break;
    case eMagCheck:
        ShowMagazineAmmoCount();
        SwitchState(eIdle);
        break;
    case eMuzzleCheck: SwitchState(eIdle); break;
	case eFiremode:
        m_previous_firemode_animation_index = CurrentFireModeAnimationIndex();
        SwitchState(eIdle);
        UpdateFireModePoses(true);
        break;
    default: inherited::OnAnimationEnd(state);
    }
}

void CWeaponMagazined::switch2_Idle()
{
    SetPending(FALSE);
    PlayAnimIdle();
}

#ifdef DEBUG
#include "ai\stalker\ai_stalker.h"
#include "object_handler_planner.h"
#endif
#include <ui/UIXmlInit.h>

void CWeaponMagazined::switch2_Fire()
{
    CInventoryOwner* io = smart_cast<CInventoryOwner*>(H_Parent());
#ifdef DEBUG
    CInventoryItem* ii = smart_cast<CInventoryItem*>(this);
    VERIFY2(io, make_string("no inventory owner, item %s", *cName()));

    if (ii != io->inventory().ActiveItem())
        Msg("! not an active item, item %s, owner %s, active item %s", *cName(), *H_Parent()->cName(),
            io->inventory().ActiveItem() ? *io->inventory().ActiveItem()->object().cName() : "no_active_item");

    if (!(io && (ii == io->inventory().ActiveItem())))
    {
        CAI_Stalker* stalker = smart_cast<CAI_Stalker*>(H_Parent());
        if (stalker)
        {
            stalker->planner().show();
            stalker->planner().show_current_world_state();
            stalker->planner().show_target_world_state();
        }
    }
#else
    if (!io)
        return;
#endif // DEBUG

    //
    //	VERIFY2(
    //		io && (ii == io->inventory().ActiveItem()),
    //		make_string(
    //			"item[%s], parent[%s]",
    //			*cName(),
    //			H_Parent() ? *H_Parent()->cName() : "no_parent"
    //		)
    //	);

    m_bStopedAfterQueueFired = false;
    m_bFireSingleShot = true;
    m_iShotNum = 0;

    /*	if(SingleShotMode())
        {
            m_bFireSingleShot = true;
            bWorking = false;
        }*/
}
void CWeaponMagazined::switch2_Empty(const bool empty_click_anim_play)
{
    if (!Core.Features.test(xrCore::Feature::autoreload_wpn) && smart_cast<CActor*>(H_Parent()))
    {
        if (empty_click_anim_play)
            OnEmptyClick();
        return;
    }

    if (!TryReload())
    {
        if (empty_click_anim_play)
            OnEmptyClick();
    }
    else
    {
        inherited::FireEnd();
    }
}
void CWeaponMagazined::PlayReloadSound()
{
    if (DetachableMagazineSystemActive() && !InstalledMagazineSection().c_str())
    {
        PlaySound(sndLoadSingle.empty() ? sndReload : sndLoadSingle, get_LastFP());
        return;
    }

    if (m_reload_animation_variant.c_str() && m_reload_animation_variant.size())
    {
        HUD_SOUND* variant = iAmmoElapsed == 0 ? &sndMagazineReloadEmpty :
            (IsPartlyReloading() ? &sndMagazineReloadTactical : &sndMagazineReload);
        if (!variant->empty())
        {
            PlaySound(*variant, get_LastFP());
            return;
        }
    }

    if ((IsMisfire() && iAmmoElapsed == 1) && !sndReloadJammedLast.empty())
        PlaySound(sndReloadJammedLast, get_LastFP());
    else if (IsMisfire() && !sndReloadJammed.empty())
        PlaySound(sndReloadJammed, get_LastFP());
    else if (IsPartlyReloading() && !sndReloadPartly.empty())
        PlaySound(sndReloadPartly, get_LastFP());
    else
        PlaySound(sndReload, get_LastFP());
}

void CWeaponMagazined::switch2_Reload()
{
    CWeapon::FireEnd();

    if (iAmmoElapsed > 0 && CartridgeInTheChamberEnabled)
        CartridgeInTheChamber = 1;
    else
        CartridgeInTheChamber = 0;

    PlayReloadSound();
    PlayAnimReload();
    SetPending(TRUE);
    bullet_update = false;
}

void CWeaponMagazined::switch2_Hiding()
{
    CWeapon::FireEnd();

    StopHUDSounds();
    PlaySound(sndHide, get_LastFP());

    PlayAnimHide();
    SetPending(TRUE);
}

void CWeaponMagazined::switch2_Hidden()
{
    CWeapon::FireEnd();

    HUD_SOUND::StopSound(sndReload);
    HUD_SOUND::StopSound(sndReloadPartly);
    HUD_SOUND::StopSound(sndReloadJammed);
    HUD_SOUND::StopSound(sndReloadJammedLast);
    HUD_SOUND::StopSound(sndLoadSingle);
    HUD_SOUND::StopSound(sndMagazineReload);
    HUD_SOUND::StopSound(sndMagazineReloadTactical);
    HUD_SOUND::StopSound(sndMagazineReloadEmpty);
    StopCurrentAnimWithoutCallback();

    signal_HideComplete();
    RemoveShotEffector();
}
void CWeaponMagazined::switch2_Showing()
{
    if (ParentIsActor() && !m_first_ready_played)
    {
        m_first_ready_played = true;
        if (AnimationExist("anm_ready"))
        {
            PlayAnimReady();
            SetPending(TRUE);
            return;
        }
    }

    PlaySound(sndShow, get_LastFP());

    SetPending(TRUE);
    PlayAnimShow();
}

bool CWeaponMagazined::Action(s32 cmd, u32 flags)
{
    if (inherited::Action(cmd, flags))
        return true;

    //если оружие чем-то занято, то ничего не делать
    if (IsPending() && cmd != kWPN_FIREMODE_PREV && cmd != kWPN_FIREMODE_NEXT)
        return false;

    switch (cmd)
    {
    case kWPN_RELOAD: {
        if (!psActorFlags.test(AF_LOCK_RELOAD) || (!ParentIsActor() || !(g_actor->get_state() & mcSprint)))
            if (flags & CMD_START)
            {
                if (ParentIsActor() && Actor()->is_safemode())
                    Actor()->set_safemode(false);
                if (iAmmoElapsed < iMagazineSize || (IsMisfire() && !IsGrenadeMode()))
                    Reload();
            }
    }
        return true;
    case kWPN_FIREMODE_PREV: {
        if ((flags & CMD_START) && GetState() == eIdle)
        {
            OnPrevFireMode(flags & CMD_OPT);
            if (m_firemode_changed)
			    SwitchState(eFiremode);
            return true;
        }
    }
    break;
    case kWPN_FIREMODE_NEXT: {
        if ((flags & CMD_START) && GetState() == eIdle)
        {
            OnNextFireMode(flags & CMD_OPT);
            if (m_firemode_changed)
			    SwitchState(eFiremode);
            return true;
        }
    }
    break;
    case kLASER_ON: {
        if ((flags & CMD_START) && has_laser && GetState() == eIdle)
        {
            LaserSwitch = true;
            DeviceSwitch();
            return true;
        }
    }
    break;
    case kFLASHLIGHT: {
        if ((flags & CMD_START) && has_flashlight && GetState() == eIdle)
        {
            TorchSwitch = true;
            DeviceSwitch();
            return true;
        }
    }
    break;
    case kTORCH: {
        auto pActorTorch = smart_cast<CActor*>(H_Parent())->inventory().ItemFromSlot(TORCH_SLOT);
        if ((flags & CMD_START) && pActorTorch && GetState() == eIdle)
        {
            HeadLampSwitch = true;
            DeviceSwitch();
            return true;
        }
    }
    break;
    case kNIGHT_VISION: {
        auto pActor = smart_cast<CActor*>(H_Parent());
        auto pActorNv = pActor->inventory().ItemFromSlot(IS_OGSR_GA ? NIGHT_VISION_SLOT : TORCH_SLOT);
        if ((flags & CMD_START) && pActorNv && GetState() == eIdle)
        {
            NightVisionSwitch = true;
            DeviceSwitch();
            return true;
        }
    }
    break;
	case kANIM_BORE: {
        if ((flags & CMD_START) && GetState() == eIdle)
        {
            SwitchState(eLook);
            return true;
        }
    }
    break;
    case kANIM_MAGCHECK: {
        const bool has_magazine = !DetachableMagazineSystemActive() || InstalledMagazineSection().c_str();
        if ((flags & CMD_START) && GetState() == eIdle && has_magazine)
        {
            SwitchState(eMagCheck);
            return true;
        }
    }
    break;
    case kANIM_MUZZLE_CHECK: {
        if ((flags & CMD_START) && GetState() == eIdle)
        {
            SwitchState(eMuzzleCheck);
            return true;
        }
    }
    break;
    }
    return false;
}

bool CWeaponMagazined::CanAttach(PIItem pIItem)
{
    CScope* pScope = smart_cast<CScope*>(pIItem);
    CSilencer* pSilencer = smart_cast<CSilencer*>(pIItem);
    CGrenadeLauncher* pGrenadeLauncher = smart_cast<CGrenadeLauncher*>(pIItem);

    if (pScope && AddonRequirementsSatisfied(pIItem->object().cNameSect().c_str()) && m_eScopeStatus == ALife::eAddonAttachable && (m_flagsAddOnState & CSE_ALifeItemWeapon::eWeaponAddonScope) == 0 &&
        std::find(m_allScopeNames.begin(), m_allScopeNames.end(), pIItem->object().cNameSect()) != m_allScopeNames.end())
        return true;
    else if (pSilencer && AddonRequirementsSatisfied(pIItem->object().cNameSect().c_str()) && m_eSilencerStatus == ALife::eAddonAttachable && (m_flagsAddOnState & CSE_ALifeItemWeapon::eWeaponAddonSilencer) == 0 &&
             (m_sSilencerName == pIItem->object().cNameSect()))
        return true;
    else if (pGrenadeLauncher && AddonRequirementsSatisfied(pIItem->object().cNameSect().c_str()) && m_eGrenadeLauncherStatus == ALife::eAddonAttachable && (m_flagsAddOnState & CSE_ALifeItemWeapon::eWeaponAddonGrenadeLauncher) == 0 &&
             (m_sGrenadeLauncherName == pIItem->object().cNameSect()))
        return true;
    else if (CanAttachCustomAddon(pIItem))
        return true;
    else
        return inherited::CanAttach(pIItem);
}

bool CWeaponMagazined::CanDetach(const char* item_section_name)
{
    if (CanDetachAddonSection(item_section_name) && m_eScopeStatus == CSE_ALifeItemWeapon::eAddonAttachable && 0 != (m_flagsAddOnState & CSE_ALifeItemWeapon::eWeaponAddonScope) && (m_sScopeName == item_section_name))
        return true;
    else if (CanDetachAddonSection(item_section_name) && m_eSilencerStatus == CSE_ALifeItemWeapon::eAddonAttachable && 0 != (m_flagsAddOnState & CSE_ALifeItemWeapon::eWeaponAddonSilencer) &&
             (m_sSilencerName == item_section_name))
        return true;
    else if (CanDetachAddonSection(item_section_name) && m_eGrenadeLauncherStatus == CSE_ALifeItemWeapon::eAddonAttachable && 0 != (m_flagsAddOnState & CSE_ALifeItemWeapon::eWeaponAddonGrenadeLauncher) &&
             (m_sGrenadeLauncherName == item_section_name))
        return true;
    else if (CanDetachCustomAddon(item_section_name))
        return true;
    else
        return inherited::CanDetach(item_section_name);
}

bool CWeaponMagazined::Attach(PIItem pIItem, bool b_send_event)
{
    bool result = false;

    CScope* pScope = smart_cast<CScope*>(pIItem);
    CSilencer* pSilencer = smart_cast<CSilencer*>(pIItem);
    CGrenadeLauncher* pGrenadeLauncher = smart_cast<CGrenadeLauncher*>(pIItem);

    if ((pScope || pSilencer || pGrenadeLauncher) && !AddonRequirementsSatisfied(pIItem->object().cNameSect().c_str()))
        return false;

    if (pScope && m_eScopeStatus == CSE_ALifeItemWeapon::eAddonAttachable && (m_flagsAddOnState & CSE_ALifeItemWeapon::eWeaponAddonScope) == 0 &&
        std::find(m_allScopeNames.begin(), m_allScopeNames.end(), pIItem->object().cNameSect()) != m_allScopeNames.end())
    {
        m_flagsAddOnState |= CSE_ALifeItemWeapon::eWeaponAddonScope;
        result = true;
    }
    else if (pSilencer && m_eSilencerStatus == CSE_ALifeItemWeapon::eAddonAttachable && (m_flagsAddOnState & CSE_ALifeItemWeapon::eWeaponAddonSilencer) == 0 &&
             (m_sSilencerName == pIItem->object().cNameSect()))
    {
        m_flagsAddOnState |= CSE_ALifeItemWeapon::eWeaponAddonSilencer;
        result = true;
    }
    else if (pGrenadeLauncher && m_eGrenadeLauncherStatus == CSE_ALifeItemWeapon::eAddonAttachable && (m_flagsAddOnState & CSE_ALifeItemWeapon::eWeaponAddonGrenadeLauncher) == 0 &&
             (m_sGrenadeLauncherName == pIItem->object().cNameSect()))
    {
        m_flagsAddOnState |= CSE_ALifeItemWeapon::eWeaponAddonGrenadeLauncher;
        result = true;
    }
    else if (AttachCustomAddon(pIItem))
        result = true;

    if (result)
    {
        if (b_send_event)
        {
            //уничтожить подсоединенную вещь из инвентаря
            //.			pIItem->Drop					();
            pIItem->object().DestroyObject();
        };

        if (!ScopeRespawn(pIItem))
        {
            UpdateAddonsVisibility();
            InitAddons();
        }

        return true;
    }
    else
        return inherited::Attach(pIItem, b_send_event);
}

bool CWeaponMagazined::Detach(const char* item_section_name, bool b_spawn_item)
{
    if (IsAddonSectionInstalled(item_section_name) && !CanDetachAddonSection(item_section_name))
        return false;

    if (CanDetachAddonSection(item_section_name) && m_eScopeStatus == CSE_ALifeItemWeapon::eAddonAttachable && 0 != (m_flagsAddOnState & CSE_ALifeItemWeapon::eWeaponAddonScope) && (m_sScopeName == item_section_name))
    {
        m_flagsAddOnState &= ~CSE_ALifeItemWeapon::eWeaponAddonScope;

        if (!ScopeRespawn(nullptr))
        {
            UpdateAddonsVisibility();
            InitAddons();
        }

        return CInventoryItemObject::Detach(item_section_name, b_spawn_item);
    }
    else if (CanDetachAddonSection(item_section_name) && m_eSilencerStatus == CSE_ALifeItemWeapon::eAddonAttachable && 0 != (m_flagsAddOnState & CSE_ALifeItemWeapon::eWeaponAddonSilencer) &&
             (m_sSilencerName == item_section_name))
    {
        m_flagsAddOnState &= ~CSE_ALifeItemWeapon::eWeaponAddonSilencer;

        UpdateAddonsVisibility();
        InitAddons();
        return CInventoryItemObject::Detach(item_section_name, b_spawn_item);
    }
    else if (CanDetachAddonSection(item_section_name) && m_eGrenadeLauncherStatus == CSE_ALifeItemWeapon::eAddonAttachable && 0 != (m_flagsAddOnState & CSE_ALifeItemWeapon::eWeaponAddonGrenadeLauncher) &&
             (m_sGrenadeLauncherName == item_section_name))
    {
        m_flagsAddOnState &= ~CSE_ALifeItemWeapon::eWeaponAddonGrenadeLauncher;

        UpdateAddonsVisibility();
        InitAddons();
        return CInventoryItemObject::Detach(item_section_name, b_spawn_item);
    }
    else if (DetachCustomAddon(item_section_name))
    {
        ApplyMagazineAddonConfiguration(true);
        UpdateAddonsVisibility();
        InitAddons();
        return CInventoryItemObject::Detach(item_section_name, b_spawn_item);
    }
    else
        return inherited::Detach(item_section_name, b_spawn_item);
}

const shared_str& CWeaponMagazined::InstalledMagazineSection() const
{
    return GetCustomAddonSection(eCustomAddonMagazine);
}

bool CWeaponMagazined::DetachableMagazineSystemActive() const
{
    return psActorFlags.test(AF_DETACHABLE_MAGAZINES) && !GetCustomAddonAllowed(eCustomAddonMagazine).empty();
}

int CWeaponMagazined::ReloadTargetCapacity() const
{
    if (DetachableMagazineSystemActive())
        return iMagazineSize;
    return iMagazineSize + static_cast<int>(CartridgeInTheChamber);
}

void CWeaponMagazined::TrimMagazineToCapacity(bool return_ammo)
{
    xr_map<shared_str, u32> removed;
    while (iAmmoElapsed > iMagazineSize && !m_magazine.empty())
    {
        const CCartridge& cartridge = m_magazine.back();
        ++removed[cartridge.m_ammoSect];
        m_magazine.pop_back();
        --iAmmoElapsed;
    }

    if (return_ammo && !unlimited_ammo())
    {
        for (const auto& [section, count] : removed)
            SpawnAmmo(count, section.c_str());
    }
    m_dwAmmoCurrentCalcFrame = 0;
    UpdateEmptyBonesVisibility();
}

void CWeaponMagazined::LoadMagazineVariantSounds()
{
    HUD_SOUND::DestroySound(sndMagazineReload);
    HUD_SOUND::DestroySound(sndMagazineReloadTactical);
    HUD_SOUND::DestroySound(sndMagazineReloadEmpty);
    HUD_SOUND::DestroySound(sndMagazineCheckVariant);

    if (!m_reload_animation_variant.c_str() || !m_reload_animation_variant.size())
        return;

    const shared_str& magazine = InstalledMagazineSection();
    auto load_variant = [&](HUD_SOUND& sound, LPCSTR prefix, LPCSTR suffix, ESoundTypes type) {
        string128 key{};
        xr_sprintf(key, "%s_%s%s", prefix, m_reload_animation_variant.c_str(), suffix);
        if (pSettings->line_exist(cNameSect(), key))
            HUD_SOUND::LoadSound(cNameSect().c_str(), key, sound, type);
        else if (magazine.c_str() && pSettings->line_exist(magazine, key))
            HUD_SOUND::LoadSound(magazine.c_str(), key, sound, type);
    };

    load_variant(sndMagazineReload, "snd_reload", "", m_eSoundReload);
    load_variant(sndMagazineReloadTactical, "snd_reload", "_t", m_eSoundReload);
    load_variant(sndMagazineReloadEmpty, "snd_reload", "_empty", m_eSoundReload);

    string128 magcheck_key{};
    xr_sprintf(magcheck_key, "snd_magcheck_%s", m_magcheck_animation_variant.c_str());
    if (pSettings->line_exist(cNameSect(), magcheck_key))
        HUD_SOUND::LoadSound(cNameSect().c_str(), magcheck_key, sndMagazineCheckVariant, m_eSoundEmptyClick);
    else if (magazine.c_str() && pSettings->line_exist(magazine, magcheck_key))
        HUD_SOUND::LoadSound(magazine.c_str(), magcheck_key, sndMagazineCheckVariant, m_eSoundEmptyClick);
}

void CWeaponMagazined::ApplyMagazineAddonConfiguration(bool trim_ammo)
{
    const bool enabled = DetachableMagazineSystemActive();
    const shared_str& magazine = InstalledMagazineSection();
    const shared_str applied_section = enabled ? magazine : shared_str();

    int capacity = m_configuredMagazineSize;
    m_reload_animation_variant = nullptr;
    m_magcheck_animation_variant = nullptr;
    if (enabled)
    {
        capacity = 1;
        if (magazine.c_str())
        {
            capacity = READ_IF_EXISTS(pSettings, r_s32, magazine, "magazine_capacity", m_configuredMagazineSize);
            if (pSettings->line_exist(magazine, "use_reload_animations"))
                m_reload_animation_variant = pSettings->r_string(magazine, "use_reload_animations");
            if (pSettings->line_exist(magazine, "use_magcheck_animations"))
                m_magcheck_animation_variant = pSettings->r_string(magazine, "use_magcheck_animations");
            else
                m_magcheck_animation_variant = m_reload_animation_variant;
        }
    }

    iMagazineSize = _max(1, capacity);
    m_applied_detachable_magazines = enabled;
    m_applied_magazine_section = applied_section;
    LoadMagazineVariantSounds();
    TrimMagazineToCapacity(trim_ammo);
}

void CWeaponMagazined::InitZoomParams(LPCSTR section, bool useTexture)
{
    m_fMinZoomK = def_min_zoom_k;
    m_fZoomStepCount = def_zoom_step_count;

    LPCSTR dynamicZoomParams = READ_IF_EXISTS(pSettings, r_string, section, "scope_dynamic_zoom", NULL);
    if (dynamicZoomParams)
    {
        int num_zoom_param = _GetItemCount(dynamicZoomParams);

        ASSERT_FMT(num_zoom_param >= 1, "!![%s] : Invalid scope_dynamic_zoom parameter in section [%s]", __FUNCTION__, section);

        string128 tmp;
        m_bScopeDynamicZoom = CInifile::IsBOOL(_GetItem(dynamicZoomParams, 0, tmp));

        if (num_zoom_param > 1)
            m_fZoomStepCount = atof(_GetItem(dynamicZoomParams, 1, tmp));

        if (num_zoom_param > 2)
            m_fMinZoomK = atof(_GetItem(dynamicZoomParams, 2, tmp));
    }
    else
        m_bScopeDynamicZoom = false;

    m_fScopeInertionFactor = READ_IF_EXISTS(pSettings, r_float, section, "scope_inertion_factor", m_fControlInertionFactor);
    clamp(m_fScopeInertionFactor, m_fControlInertionFactor, m_fScopeInertionFactor);

    m_fScopeZoomFactor = pSettings->r_float(section, "scope_zoom_factor");

    m_fZoomHudFov = READ_IF_EXISTS(pSettings, r_float, section, "scope_zoom_hud_fov", 0.0f);
    m_f3dssHudFov = READ_IF_EXISTS(pSettings, r_float, section, "scope_lense_hud_fov", 0.0f);

    if (m_UIScope)
        xr_delete(m_UIScope);

    if (useTexture)
    {
        shared_str scope_tex_name = READ_IF_EXISTS(pSettings, r_string, section, "scope_texture", "");
        const bool scope_tex_autoresize = READ_IF_EXISTS(pSettings, r_bool, section, "scope_texture_autoresize", true);

        if (scope_tex_name.size() > 0 && !scope_tex_name.equal("none"))
        {
            m_UIScope = xr_new<CUIWindow>();

            bool was_set = false;

            if (Core.Features.test(xrCore::Feature::cop_style_scope_texture))
            {
                if (!g_wpnScopeXml)
                {
                    g_wpnScopeXml = xr_new<CUIXml>();
                    g_wpnScopeXml->Init(CONFIG_PATH, UI_PATH, "scopes.xml");
                }

                if (g_wpnScopeXml->NavigateToNode(scope_tex_name.c_str()))
                {
                    CUIXmlInit::InitWindow(*g_wpnScopeXml, scope_tex_name.c_str(), 0, m_UIScope);

                    was_set = true;
                }
                else if (g_wpnScopeXml->NavigateToNode("wpn_crosshair_fallback") && READ_IF_EXISTS(pSettings, r_bool, section, "wpn_crosshair_fallback", true))
                {
                    CUIXmlInit::InitWindow(*g_wpnScopeXml, "wpn_crosshair_fallback", 0, m_UIScope);

                    was_set = true;

                    CUIWindow* scope_wnd = m_UIScope->FindChild("scope_texture");
                    if (scope_wnd && smart_cast<CUIStatic*>(scope_wnd))
                    {
                        smart_cast<CUIStatic*>(scope_wnd)->InitTexture(scope_tex_name.c_str());
                    }
                }
            }
            
            if (!was_set)
            {
                m_UIScope->SetWndRect(0, 0, UI_BASE_WIDTH, UI_BASE_HEIGHT);

                // legacy mode
                CUIStatic* inner = xr_new<CUIStatic>();
                inner->SetAutoDelete(true);
                inner->SetWndRect(0, 0, UI_BASE_WIDTH, UI_BASE_HEIGHT);
                inner->SetStretchTexture(true);
                inner->GetStaticItem()->Init(scope_tex_name.c_str(), (Core.Features.test(xrCore::Feature::scope_textures_autoresize) && scope_tex_autoresize) ? "hud\\scope" : "hud\\default", 0, 0, alNone);
                m_UIScope->AttachChild(inner);
            }
        }
    }
}

void CWeaponMagazined::InitAddons()
{
    ClearResolvedSounds();
    ApplyMagazineAddonConfiguration(false);
    //////////////////////////////////////////////////////////////////////////
    // Прицел
    m_fIronSightZoomFactor = READ_IF_EXISTS(pSettings, r_float, cNameSect(), "ironsight_zoom_factor", 50.0f);

    const shared_str scope_section = GetInstalledAddonByClass("scope");
    if (scope_section.c_str())
    {
        if (!IsScopeAttached())
        {
            InitZoomParams(scope_section.c_str(), !m_bIgnoreScopeTexture);
            m_fZoomHudFov = READ_IF_EXISTS(pSettings, r_float, scope_section, "scope_zoom_hud_fov",
                READ_IF_EXISTS(pSettings, r_float, cNameSect(), "scope_zoom_hud_fov", m_fZoomHudFov));
            m_f3dssHudFov = READ_IF_EXISTS(pSettings, r_float, scope_section, "scope_lense_hud_fov",
                READ_IF_EXISTS(pSettings, r_float, cNameSect(), "scope_lense_hud_fov", m_f3dssHudFov));
        }
        else if (m_eScopeStatus == ALife::eAddonAttachable)
        {
            m_sScopeName = pSettings->r_string(cNameSect(), "scope_name");
            m_iScopeX = pSettings->r_s32(cNameSect(), "scope_x");
            m_iScopeY = pSettings->r_s32(cNameSect(), "scope_y");

            InitZoomParams(*m_sScopeName, !m_bIgnoreScopeTexture);

            m_fZoomHudFov = READ_IF_EXISTS(pSettings, r_float, cNameSect().c_str(), "scope_zoom_hud_fov", m_fZoomHudFov);
            m_f3dssHudFov = READ_IF_EXISTS(pSettings, r_float, cNameSect().c_str(), "scope_lense_hud_fov", m_f3dssHudFov);
        }
        else if (m_eScopeStatus == ALife::eAddonPermanent)
        {
            InitZoomParams(cNameSect().c_str(), !m_bIgnoreScopeTexture);

            // CWeaponBinoculars always use dynamic zoom
            m_bScopeDynamicZoom = m_bScopeDynamicZoom || !!smart_cast<CWeaponBinoculars*>(this);
        }
    }
    else
    {
        m_bScopeDynamicZoom = false;

        if (IsZoomEnabled())
        {
            InitZoomParams(cNameSect().c_str(), !!READ_IF_EXISTS(pSettings, r_bool, cNameSect(), "force_scope_texture", false));

            // for weapon without any scope - scope_zoom_factor will overrider ironsight_zoom_factor
            m_fIronSightZoomFactor = m_fScopeZoomFactor;
        }
        else
        {
            m_fZoomHudFov = 0.0f;
            m_f3dssHudFov = 0.0f;
            m_fScopeInertionFactor = m_fControlInertionFactor;
        }
    }

    if (m_bScopeDynamicZoom)
    {
        {
            if (Core.Features.test(xrCore::Feature::ogse_wpn_zoom_system))
            {
                float delta, min_zoom_factor;
                GetZoomData(m_fScopeZoomFactor, delta, min_zoom_factor);

                m_fRTZoomFactor = min_zoom_factor; // set minimal zoom by default for ogse mode
            }
            else
            {
                m_fRTZoomFactor = m_fScopeZoomFactor;
            }
        }
    }

    const shared_str silencer_section = GetInstalledSilencerSection();
    if (silencer_section.c_str())
    {
        m_sFlameParticlesCurrent = READ_IF_EXISTS(
            pSettings, r_string, silencer_section, "silencer_flame_particles", m_sSilencerFlameParticles);
        m_sSmokeParticlesCurrent = READ_IF_EXISTS(
            pSettings, r_string, silencer_section, "silencer_smoke_particles", m_sSilencerSmokeParticles);
        m_sSndShotCurrent = HasShotSound("sndSilencerShot") || pSettings->line_exist(silencer_section, "snd_silncer_shot") ? "sndSilencerShot" : "sndShot";

        //сила выстрела
        LoadFireParams(*cNameSect(), "");

        //подсветка от выстрела
        LoadLights(*cNameSect(), "silencer_");

        ApplySilencerKoeffs(silencer_section.c_str());
    }
    else
    {
        m_sFlameParticlesCurrent = m_sFlameParticles;
        m_sSmokeParticlesCurrent = m_sSmokeParticles;
        m_sSndShotCurrent = "sndShot"; 

        //сила выстрела
        LoadFireParams(*cNameSect(), "");

        //подсветка от выстрела
        LoadLights(*cNameSect(), "");
    }

    inherited::InitAddons();
    callback(GameObject::eOnAddonInit)(1);

    m_fZoomFactor = CurrentZoomFactor();
}

void CWeaponMagazined::ApplySilencerKoeffs(LPCSTR silencer_section)
{
    float BHPk = 1.0f, BSk = 1.0f;
    float FDB_k = 1.0f, CD_k = 1.0f;

    if (pSettings->line_exist(silencer_section, "bullet_hit_power_k"))
    {
        BHPk = pSettings->r_float(silencer_section, "bullet_hit_power_k");
        clamp(BHPk, 0.0f, 1.0f);
    };
    if (pSettings->line_exist(silencer_section, "bullet_speed_k"))
    {
        BSk = pSettings->r_float(silencer_section, "bullet_speed_k");
        clamp(BSk, 0.0f, 1.0f);
    };
    if (pSettings->line_exist(silencer_section, "fire_dispersion_base_k"))
    {
        FDB_k = pSettings->r_float(silencer_section, "fire_dispersion_base_k");
        //		clamp(FDB_k, 0.0f, 1.0f);
    };
    if (pSettings->line_exist(silencer_section, "cam_dispersion_k"))
    {
        CD_k = pSettings->r_float(silencer_section, "cam_dispersion_k");
        clamp(CD_k, 0.0f, 1.0f);
    };

    // fHitPower			= fHitPower*BHPk;
    fvHitPower.mul(BHPk);
    fHitImpulse *= BSk;
    m_fStartBulletSpeed *= BSk;
    fireDispersionBase *= FDB_k;
    camDispersion *= CD_k;
    camDispersionInc *= CD_k;
}

//виртуальные функции для проигрывания анимации HUD
void CWeaponMagazined::PlayAnimShow()
{
    PlayHUDMotion({IsMisfire() ? "anm_show_jammed" : (iAmmoElapsed == 0 ? "anm_show_empty" : "nullptr"), "anim_draw", "anm_show"}, false, GetState());
}

void CWeaponMagazined::PlayAnimHide()
{
    PlayHUDMotion({IsMisfire() ? "anm_hide_jammed" : (iAmmoElapsed == 0 ? "anm_hide_empty" : "nullptr"), "anim_holster", "anm_hide"}, true, GetState());
}

void CWeaponMagazined::PlayAnimReload()
{
    if (DetachableMagazineSystemActive() && !InstalledMagazineSection().c_str())
    {
        PlayHUDMotion({"anm_load_single", "anm_reload_empty", "anim_reload", "anm_reload"}, true, GetState());
        return;
    }

    if (m_reload_animation_variant.c_str() && m_reload_animation_variant.size())
    {
        string128 selected{}, generic{}, tactical{}, empty{};
        xr_sprintf(generic, "anm_reload_%s", m_reload_animation_variant.c_str());
        xr_sprintf(tactical, "anm_reload_%s_t", m_reload_animation_variant.c_str());
        xr_sprintf(empty, "anm_reload_%s_empty", m_reload_animation_variant.c_str());
        xr_strcpy(selected, iAmmoElapsed == 0 ? empty : (IsPartlyReloading() ? tactical : generic));
        PlayHUDMotion({selected, generic, "anm_reload_empty", "anim_reload", "anm_reload"}, true, GetState());
        return;
    }

    if (IsMisfire())
        PlayHUDMotion({iAmmoElapsed == 1 ? "anm_reload_jammed_last" : "anm_reload_jammed", "anm_reload_jammed", "anm_reload_empty", "anim_reload", "anm_reload"}, true, GetState());
    else if (IsPartlyReloading())
        PlayHUDMotion({"anim_reload_partly", "anm_reload_partly", "anim_reload", "anm_reload"}, true, GetState());
    else
        PlayHUDMotion({"anm_reload_empty", "anim_reload", "anm_reload"}, true, GetState());
}

const char* CWeaponMagazined::GetAnimAimName()
{
    if (auto pActor = smart_cast<const CActor*>(H_Parent()))
    {
        if (AnmIdleMovingAllowed())
        {
            if (const u32 state = pActor->get_state(); state & mcAnyMove)
            {
                if (IsScopeFunctional())
                    return xr_strconcat(guns_aim_anm, "anm_idle_aim_scope_moving", IsMisfire() ? "_jammed" : (iAmmoElapsed == 0 ? "_empty" : ""));
                else
                    return xr_strconcat(guns_aim_anm, "anm_idle_aim_moving", (state & mcFwd) ? "_forward" : ((state & mcBack) ? "_back" : ""),
                                        (state & mcLStrafe) ? "_left" : ((state & mcRStrafe) ? "_right" : ""), IsMisfire() ? "_jammed" : (iAmmoElapsed == 0 ? "_empty" : ""));
            }
        }
    }
    return nullptr;
}

void CWeaponMagazined::PlayAnimAim()
{
    if (IsRotatingToZoom() && !IsRotatingFromZoom())
    {
        string128 guns_aim_start_anm;
        xr_strconcat(guns_aim_start_anm, "anm_idle_aim_start", IsMisfire() ? "_jammed" : (iAmmoElapsed == 0 ? "_empty" : ""));
        if (AnimationExist(guns_aim_start_anm))
        {
            PlayHUDMotion(guns_aim_start_anm, true, GetState());
            PlaySound(sndAimStart, get_LastFP());
            return;
        }
    }

    if (const char* guns_aim_anm = GetAnimAimName())
    {
        if (AnimationExist(guns_aim_anm))
        {
            PlayHUDMotion(guns_aim_anm, true, GetState());
            return;
        }
    }

    PlayHUDMotion({IsMisfire() ? "anm_idle_aim_jammed" : (iAmmoElapsed == 0 ? "anm_idle_aim_empty" : "nullptr"), "anim_idle_aim", "anm_idle_aim"}, true, GetState());
}

void CWeaponMagazined::PlayAnimIdle()
{
    if (GetState() != eIdle)
        return;

    if (IsZoomed())
        PlayAnimAim();
    else
    {
        if (IsRotatingFromZoom() && !IsRotatingToZoom())
        {
            string128 guns_aim_end_anm;
            xr_strconcat(guns_aim_end_anm, "anm_idle_aim_end", IsMisfire() ? "_jammed" : (iAmmoElapsed == 0 ? "_empty" : ""));
            if (AnimationExist(guns_aim_end_anm))
            {
                PlayHUDMotion(guns_aim_end_anm, true, GetState());
                PlaySound(sndAimEnd, get_LastFP());
                return;
            }
        }

        inherited::PlayAnimIdle();
    }
}

void CWeaponMagazined::PlayAnimShoot()
{
    string128 guns_shoot_anm;
    xr_strconcat(guns_shoot_anm, "anm_shoot", (IsZoomed() && !IsRotatingToZoom()) ? (IsScopeFunctional() ? "_aim_scope" : "_aim") : "",
                 IsMisfire() ? "_jammed" : (GetAmmoElapsed() == 1 ? "_last" : ""),
                 IsSilencerFunctional() ? "_sil" : "");

    PlayHUDMotion({guns_shoot_anm, "anim_shoot", "anm_shots"}, false, GetState());
}

void CWeaponMagazined::PlayAnimFakeShoot()
{
    auto wpn = smart_cast<CWeapon*>(this);
    string128 guns_fakeshoot_anm;
    xr_strconcat(guns_fakeshoot_anm, "anm_fakeshoot",
                 (IsZoomed() && !IsRotatingToZoom()) ? (IsMisfire() ? "_aim_jammed" : "_aim") : ((IsGrenadeMode() && IsMisfire()) ? "_jammed" : ""),
                 ((iAmmoElapsed == 0 && !IsGrenadeMode()) || (wpn && wpn->GetAmmoElapsed2() == 0 && IsGrenadeMode())) ? "_empty" : "",
                 IsGrenadeLauncherFunctional() ? (!IsGrenadeMode() ? "_w_gl" : "_g") : "");
    if (AnimationExist(guns_fakeshoot_anm))
        PlayHUDMotion(guns_fakeshoot_anm, true, GetState());
}

void CWeaponMagazined::PlayAnimCheckMisfire()
{
    string128 guns_fakeshoot_anm;
    xr_strconcat(guns_fakeshoot_anm, "anm_fakeshoot", IsMisfire() ? "_jammed" : "", IsGrenadeLauncherFunctional() ? (!IsGrenadeMode() ? "_w_gl" : "_g") : "");
    if (AnimationExist(guns_fakeshoot_anm))
    {
        PlayHUDMotion(guns_fakeshoot_anm, true, GetState());
        
        SetPending(TRUE);
    }
    else
    {
        SwitchState(eIdle);
    }
}

void CWeaponMagazined::PlayAnimDeviceSwitch()
{
    PlaySound((HeadLampSwitch || NightVisionSwitch) ? sndItemOn : sndTactItemOn, get_LastFP());

    auto wpn = smart_cast<CWeapon*>(this);
    string128 guns_device_anm;
    xr_strconcat(guns_device_anm, LaserSwitch ? "anm_laser_on" : (TorchSwitch ? "anm_torch_on" : ((HeadLampSwitch || NightVisionSwitch) ? "anm_headlamp_on" : "")),
                 IsMisfire()                                                                                              ? "_jammed" :
                     ((iAmmoElapsed == 0 && !IsGrenadeMode()) || (wpn && wpn->GetAmmoElapsed2() == 0 && IsGrenadeMode())) ? "_empty" :
                                                                                                                            "",
                 IsGrenadeLauncherFunctional() ? (!IsGrenadeMode() ? "_w_gl" : "_g") : "");
    if (AnimationExist(guns_device_anm))
    {
        PlayHUDMotion(guns_device_anm, true, GetState());

        SetPending(TRUE);
    }
    else
    {
        DeviceUpdate();
        SwitchState(eIdle);
    }
}

void CWeaponMagazined::OnMotionMark(u32 state, const motion_marks& M)
{
    inherited::OnMotionMark(state, M);

    if (state == eReload)
    {
        if (bHasBulletsToHide && xr_strcmp(M.name.c_str(), "lmg_reload") == 0)
        {
            auto ammo_type = m_ammoType;
            int ae = CheckAmmoBeforeReload(ammo_type);

            if (ammo_type == m_ammoType)
            {
                ae += iAmmoElapsed;
            }

            last_hide_bullet = (ae >= bullet_cnt || unlimited_ammo()) ? bullet_cnt : bullet_cnt - ae - 1;
            HUD_VisualBulletUpdate();
        }
        else
        {
            ReloadMagazine();
        }
    }
}

void CWeaponMagazined::OnZoomIn()
{
    inherited::OnZoomIn();

    if (GetState() == eIdle)
        PlayAnimIdle();

    CActor* pActor = smart_cast<CActor*>(H_Parent());
    if (pActor)
    {
        CEffectorCam* ec = pActor->Cameras().GetCamEffector(eCEActorMoving);
        if (ec)
            pActor->Cameras().RemoveCamEffector(eCEActorMoving);

        CEffectorZoomInertion* S = smart_cast<CEffectorZoomInertion*>(pActor->Cameras().GetCamEffector(eCEZoom));
        if (!S)
        {
            S = (CEffectorZoomInertion*)pActor->Cameras().AddCamEffector(xr_new<CEffectorZoomInertion>());
            S->Init(this);
        }
        R_ASSERT(S);

        if (m_bVision && !m_binoc_vision)
            m_binoc_vision = xr_new<CBinocularsVision>(this);
    }
}
void CWeaponMagazined::OnZoomOut()
{
    if (!m_bZoomMode)
        return;

    inherited::OnZoomOut();

    if (GetState() == eIdle)
        PlayAnimIdle();

    CActor* pActor = smart_cast<CActor*>(H_Parent());
    if (pActor)
    {
        pActor->Cameras().RemoveCamEffector(eCEZoom);
        if (m_bVision)
        {
            VERIFY(m_binoc_vision);
            xr_delete(m_binoc_vision);
        }
    }
}

void CWeaponMagazined::OnZoomChanged() { PlaySound(sndZoomChange, get_LastFP()); }

//переключение режимов стрельбы одиночными и очередями
bool CWeaponMagazined::SwitchMode()
{
    if (eIdle != GetState() || IsPending())
        return false;

    if (SingleShotMode())
        m_iQueueSize = WEAPON_ININITE_QUEUE;
    else
        m_iQueueSize = 1;

    PlaySound(sndEmptyClick, get_LastFP());

    return true;
}

void CWeaponMagazined::OnNextFireMode(bool opt)
{
    m_firemode_changed = false;
    if (m_aFireModes.size() < 2)
        return;
    if (opt && m_iCurFireMode + 1 == m_aFireModes.size())
        return;
    m_previous_firemode_animation_index = CurrentFireModeAnimationIndex();
    m_iCurFireMode = (m_iCurFireMode + 1 + m_aFireModes.size()) % m_aFireModes.size();
    SetQueueSize(GetCurrentFireMode());
    m_firemode_changed = true;
    m_applied_world_firemode_pose = -1;
    m_applied_hud_firemode_pose = -1;
    PlaySound(sndFireModes, get_LastFP());
}

void CWeaponMagazined::OnPrevFireMode(bool opt)
{
    m_firemode_changed = false;
    if (m_aFireModes.size() < 2)
        return;
    if (opt && m_iCurFireMode == 0)
        return;
    m_previous_firemode_animation_index = CurrentFireModeAnimationIndex();
    m_iCurFireMode = (m_iCurFireMode - 1 + m_aFireModes.size()) % m_aFireModes.size();
    SetQueueSize(GetCurrentFireMode());
    m_firemode_changed = true;
    m_applied_world_firemode_pose = -1;
    m_applied_hud_firemode_pose = -1;
    PlaySound(sndFireModes, get_LastFP());
}

void CWeaponMagazined::OnH_A_Chield()
{
    if (m_bHasDifferentFireModes)
    {
        CActor* actor = smart_cast<CActor*>(H_Parent());
        if (!actor)
            SetQueueSize(-1);
        else
            SetQueueSize(GetCurrentFireMode());
    };
    inherited::OnH_A_Chield();
};

void CWeaponMagazined::SetQueueSize(int size)
{
    m_iQueueSize = size;
    if (m_iQueueSize == -1)
        strcpy_s(m_sCurFireMode, " (A)");
    else
        sprintf_s(m_sCurFireMode, " (%d)", m_iQueueSize);
};

float CWeaponMagazined::GetWeaponDeterioration()
{
    if (!m_bHasDifferentFireModes || m_iPrefferedFireMode == -1 || u32(GetCurrentFireMode()) <= u32(m_iPrefferedFireMode))
    {
        if (IsSilencerFunctional())
            return conditionDecreasePerShotSilencer;
        else
            return inherited::GetWeaponDeterioration();
    }
    if (IsSilencerFunctional())
        return m_iShotNum * conditionDecreasePerShotSilencer;
    return m_iShotNum * conditionDecreasePerShot;
}

void CWeaponMagazined::save(NET_Packet& output_packet)
{
    inherited::save(output_packet);
    save_data(m_iQueueSize, output_packet);
    save_data(m_iShotNum, output_packet);
    save_data(m_iCurFireMode, output_packet);
}

void CWeaponMagazined::load(IReader& input_packet)
{
    inherited::load(input_packet);
    // A restored weapon already existed before this session; never replay
    // the first-acquisition ready animation after loading a save.
    m_first_ready_played = true;
    load_data(m_iQueueSize, input_packet);
    SetQueueSize(m_iQueueSize);
    load_data(m_iShotNum, input_packet);
    load_data(m_iCurFireMode, input_packet);
    if (m_bHasDifferentFireModes && !m_aFireModes.empty())
    {
        if (m_iCurFireMode < 0 || m_iCurFireMode >= static_cast<int>(m_aFireModes.size()))
            m_iCurFireMode = static_cast<int>(m_aFireModes.size()) - 1;
        SetQueueSize(GetCurrentFireMode());
        m_previous_firemode_animation_index = CurrentFireModeAnimationIndex();
        m_applied_world_firemode_pose = -1;
        m_applied_hud_firemode_pose = -1;
    }
}

void CWeaponMagazined::net_Export(CSE_Abstract* E)
{
    inherited::net_Export(E);
    CSE_ALifeItemWeaponMagazined* wpn = smart_cast<CSE_ALifeItemWeaponMagazined*>(E);
    wpn->m_u8CurFireMode = u8(m_iCurFireMode & 0x00ff);
}

void CWeaponMagazined::GetBriefInfo(xr_string& str_name, xr_string& icon_sect_name, xr_string& str_count)
{
    const int AE = GetAmmoElapsed(), AC = GetAmmoCurrent();

    if (AE == 0 || m_magazine.empty())
        icon_sect_name = m_ammoTypes[m_ammoType].c_str();
    else
        icon_sect_name = m_ammoTypes[m_magazine.back().m_LocalAmmoType].c_str();

    string256 sItemName;
    strcpy_s(sItemName, CStringTable().translate(pSettings->r_string(icon_sect_name.c_str(), "inv_name_short")).c_str());

    if (HasFireModes())
        strcat_s(sItemName, GetCurrentFireModeStr());

    str_name = sItemName;

    static const std::regex ae_re{R"(\{AE\})"}, ac_re{R"(\{AC\})"};
    str_count = m_str_count_tmpl;
    str_count = std::regex_replace(str_count, ae_re, std::to_string(AE));
    str_count = std::regex_replace(str_count, ac_re, unlimited_ammo() ? "--" : std::to_string(AC - AE));
}

void CWeaponMagazined::OnDrawUI()
{
    if (H_Parent() && IsZoomed() && !IsRotatingToZoom() && m_binoc_vision)
        m_binoc_vision->Draw();
    inherited::OnDrawUI();
}
void CWeaponMagazined::net_Relcase(CObject* object)
{
    if (!m_binoc_vision)
        return;

    m_binoc_vision->remove_links(object);
}

bool CWeaponMagazined::ScopeRespawn(PIItem pIItem)
{
    xr_string scope_respawn = "scope_respawn";
    if (ScopeAttachable() && IsScopeAttached())
    {
        scope_respawn += "_";
        if (smart_cast<CScope*>(pIItem))
            scope_respawn += pIItem->object().cNameSect().c_str();
        else
            scope_respawn += m_sScopeName.c_str();
    }

    if (pSettings->line_exist(cNameSect(), scope_respawn.c_str()))
    {
        LPCSTR S = pSettings->r_string(cNameSect(), scope_respawn.c_str());
        if (xr_strcmp(cName().c_str(), S) != 0)
        {
            CSE_Abstract* _abstract = Level().spawn_item(S, Position(), ai_location().level_vertex_id(), H_Parent()->ID(), true);
            CSE_ALifeDynamicObject* sobj1 = alife_object();
            CSE_ALifeDynamicObject* sobj2 = smart_cast<CSE_ALifeDynamicObject*>(_abstract);

            NET_Packet P;
            P.w_begin(M_UPDATE);
            u32 position = P.w_tell();
            P.w_u16(0);
            sobj1->STATE_Write(P);
            u16 size = u16(P.w_tell() - position);
            P.w_seek(position, &size, sizeof(u16));
            u16 id;
            P.r_begin(id);
            P.r_u16(size);
            sobj2->STATE_Read(P, size);

            net_Export(_abstract);

            auto io = smart_cast<CInventoryOwner*>(H_Parent());
            auto ii = smart_cast<CInventoryItem*>(this);
            if (io)
            {
                if (io->inventory().InSlot(ii))
                    io->SetNextItemSlot(ii->GetSlot());
                else
                    io->SetNextItemSlot(0);
            }

            DestroyObject();
            sobj2->Spawn_Write(P, TRUE);
            Level().Send(P, net_flags(TRUE));
            F_entity_Destroy(_abstract);

            return true;
        }
    }
    return false;
}

bool CWeaponMagazined::ShouldPlayFlameParticles()
{
    if (m_bFlameParticlesHideInZoom && IsZoomed() && !IsRotatingToZoom() && Is3dssEnabled())
        return false;

    return true;
}

void CWeaponMagazined::PlayAnimBore()
{
    string128 guns_anm_bore;
    xr_strconcat(guns_anm_bore, "anm_bore", IsMisfire() ? "_jammed" : (iAmmoElapsed == 0 ? "_empty" : ""), IsGrenadeLauncherFunctional() ? (!IsGrenadeMode() ? "_w_gl" : "_g") : "");

    if (AnimationExist(guns_anm_bore))
    {
        PlayHUDMotion(guns_anm_bore, true, GetState());
		
		if (iAmmoElapsed == 0)
            PlaySound(sndBoreEmpty, get_LastFP());
        else
            PlaySound(sndBore, get_LastFP());
		
        SetPending(TRUE);
    }
    else
        SwitchState(eIdle);
}

void CWeaponMagazined::ScheduleNextBore()
{
    const u32 min_delay = static_cast<u32>(_max(0.f, m_bore_idle_time_min) * 1000.f);
    const u32 max_delay = static_cast<u32>(_max(m_bore_idle_time_min, m_bore_idle_time_max) * 1000.f);
    const u32 delay = max_delay > min_delay ? Random.randI(min_delay, max_delay + 1) : min_delay;
    m_next_bore_time = Device.dwTimeGlobal + delay;
}

void CWeaponMagazined::PlayAnimLook()
{
    if (!AnimationExist("anm_look"))
    {
        SwitchState(eIdle);
        return;
    }
    PlayHUDMotion("anm_look", true, GetState());
    if (!sndLook.empty())
        PlaySound(sndLook, get_LastFP());
    SetPending(TRUE);
}

void CWeaponMagazined::PlayAnimMagCheck()
{
    string128 variant{};
    if (m_magcheck_animation_variant.c_str() && m_magcheck_animation_variant.size())
        xr_sprintf(variant, "anm_magcheck_%s", m_magcheck_animation_variant.c_str());

    LPCSTR selected = variant[0] && AnimationExist(variant) ? variant : "anm_magcheck";
    if (!AnimationExist(selected))
    {
        SwitchState(eIdle);
        return;
    }
    PlayHUDMotion(selected, true, GetState());
    if (!sndMagazineCheckVariant.empty())
        PlaySound(sndMagazineCheckVariant, get_LastFP());
    else if (!sndMagCheck.empty())
        PlaySound(sndMagCheck, get_LastFP());
    SetPending(TRUE);
}

void CWeaponMagazined::PlayAnimMuzzleCheck()
{
    if (!AnimationExist("anm_muzzle_check"))
    {
        SwitchState(eIdle);
        return;
    }
    PlayHUDMotion("anm_muzzle_check", true, GetState());
    if (!sndMuzzleCheck.empty())
        PlaySound(sndMuzzleCheck, get_LastFP());
    SetPending(TRUE);
}

void CWeaponMagazined::PlayAnimReady()
{
    PlayHUDMotion("anm_ready", true, GetState());
    if (!sndReady.empty())
        PlaySound(sndReady, get_LastFP());
}

void CWeaponMagazined::ShowMagazineAmmoCount() const
{
    if (!ParentIsActor() || !HUD().GetUI() || !HUD().GetUI()->UIGame())
        return;
    SDrawStaticStruct* message = HUD().GetUI()->UIGame()->AddCustomStatic("item_used", true);
    if (!message)
        return;
    string128 text{};
    xr_sprintf(text, "Magazine: %d / %d", iAmmoElapsed, iMagazineSize);
    message->m_endTime = Device.fTimeGlobal + 3.f;
    message->wnd()->SetText(text);
}

int CWeaponMagazined::FireModeAnimationIndex(int mode_index) const
{
    if (mode_index < 0 || mode_index >= static_cast<int>(m_aFireModes.size()))
        return 0;

    if (m_firemode_animation_indices.size() == m_aFireModes.size())
        return m_firemode_animation_indices[mode_index];

    const int queue_size = m_aFireModes[mode_index];
    if (queue_size == WEAPON_ININITE_QUEUE)
        return 0; // raised selector: automatic fire
    if (queue_size == 1)
        return 1; // lowered selector: single fire
    return mode_index;
}

int CWeaponMagazined::CurrentFireModeAnimationIndex() const
{
    return FireModeAnimationIndex(m_iCurFireMode);
}

bool CWeaponMagazined::FindFireModeHudAnimation(char* result, size_t result_size) const
{
    const int target = CurrentFireModeAnimationIndex();
    const int previous = m_previous_firemode_animation_index;
    const LPCSTR condition = IsMisfire() ? "_jammed" : (iAmmoElapsed == 0 ? "_empty" : "");
    const LPCSTR launcher = IsGrenadeLauncherFunctional() ? "_w_gl" : "";

    auto find_variant = [&](LPCSTR base) {
        string128 candidate;
        xr_sprintf(candidate, "%s%s%s", base, condition, launcher);
        if (AnimationExist(candidate))
        {
            strcpy_s(result, result_size, candidate);
            return true;
        }

        if (condition[0])
        {
            xr_sprintf(candidate, "%s%s", base, condition);
            if (AnimationExist(candidate))
            {
                strcpy_s(result, result_size, candidate);
                return true;
            }
        }

        if (launcher[0])
        {
            xr_sprintf(candidate, "%s%s", base, launcher);
            if (AnimationExist(candidate))
            {
                strcpy_s(result, result_size, candidate);
                return true;
            }
        }

        if (AnimationExist(base))
        {
            strcpy_s(result, result_size, base);
            return true;
        }
        return false;
    };

    string64 base;
    if (previous >= 0 && previous != target)
    {
        xr_sprintf(base, "anm_firemode_%d_to_%d", previous, target);
        if (find_variant(base))
            return true;
    }

    xr_sprintf(base, "anm_firemode_%d", target);
    if (find_variant(base))
        return true;

    // Compatibility with existing weapon configs.
    return find_variant("anm_firemode");
}

void CWeaponMagazined::ClearFireModePose(IKinematics* model)
{
    if (!model)
        return;

    if (auto animated = smart_cast<IKinematicsAnimated*>(model))
        for (u16 part = 0; part < MAX_PARTS; ++part)
            animated->LL_CloseCycle(part, 1 << 1);

    if (model == m_world_firemode_pose_model)
        m_applied_world_firemode_pose = -1;
    if (model == m_hud_firemode_pose_model)
        m_applied_hud_firemode_pose = -1;
}

void CWeaponMagazined::PlayWorldFireModeTransition()
{
    auto model = smart_cast<IKinematics*>(Visual());
    auto animated = smart_cast<IKinematicsAnimated*>(Visual());
    if (!model || !animated)
        return;

    const int target = CurrentFireModeAnimationIndex();
    const int previous = m_previous_firemode_animation_index;
    LPCSTR section = cNameSect().c_str();
    LPCSTR motion = nullptr;
    string64 line;
    string64 default_motion;

    if (previous >= 0 && previous != target)
    {
        xr_sprintf(line, "world_firemode_%d_to_%d", previous, target);
        if (pSettings->line_exist(section, line))
            motion = pSettings->r_string(section, line);
    }
    if (!motion)
    {
        xr_sprintf(line, "world_firemode_%d", target);
        if (pSettings->line_exist(section, line))
            motion = pSettings->r_string(section, line);
    }
    if (!motion)
    {
        xr_sprintf(default_motion, "firemode_%d", target);
        motion = default_motion;
    }

    const MotionID motion_id = animated->ID_Cycle_Safe(motion);
    if (!motion_id.valid())
    {
        m_world_firemode_transition_end = 0;
        return;
    }

    animated->PlayCycle(motion_id, TRUE);
    const float duration = _max(0.f, animated->get_animation_length(motion_id));
    m_world_firemode_transition_end = Device.dwTimeGlobal + static_cast<u32>(duration * 1000.f);
}

void CWeaponMagazined::ApplyFireModePose(IKinematics* model, LPCSTR config_section, bool hud, bool force)
{
    if (!model || GetState() == eFiremode)
        return;
    if (!hud && m_world_firemode_transition_end && Device.dwTimeGlobal < m_world_firemode_transition_end)
        return;

    const int target = CurrentFireModeAnimationIndex();
    int& applied = hud ? m_applied_hud_firemode_pose : m_applied_world_firemode_pose;
    IKinematics*& cached_model = hud ? m_hud_firemode_pose_model : m_world_firemode_pose_model;
    if (!force && cached_model == model && applied == target)
        return;

    cached_model = model;
    applied = target;

    auto animated = smart_cast<IKinematicsAnimated*>(model);
    if (!animated)
        return;

    LPCSTR weapon_section = cNameSect().c_str();
    LPCSTR pose_motion = nullptr;
    string64 line;
    string64 default_motion;
    xr_sprintf(line, "firemode_pose_%d", target);
    if (config_section && pSettings->line_exist(config_section, line))
        pose_motion = pSettings->r_string(config_section, line);
    if (!pose_motion && hud)
    {
        xr_sprintf(line, "hud_firemode_pose_%d", target);
        if (pSettings->line_exist(weapon_section, line))
            pose_motion = pSettings->r_string(weapon_section, line);
    }
    if (!pose_motion)
    {
        xr_sprintf(line, "firemode_pose_%d", target);
        if (pSettings->line_exist(weapon_section, line))
            pose_motion = pSettings->r_string(weapon_section, line);
    }
    if (!pose_motion)
    {
        xr_sprintf(default_motion, "pose_firemode%d", target);
        pose_motion = default_motion;
    }
    if (!pose_motion[0] || !xr_strcmp(pose_motion, "none"))
        return;

    const MotionID motion_id = animated->ID_Cycle_Safe(pose_motion);
    if (!motion_id.valid())
        return;

    LPCSTR selector_bone = nullptr;
    if (config_section && pSettings->line_exist(config_section, "firemode_selector_bone"))
        selector_bone = pSettings->r_string(config_section, "firemode_selector_bone");
    if (!selector_bone && hud && pSettings->line_exist(weapon_section, "hud_firemode_selector_bone"))
        selector_bone = pSettings->r_string(weapon_section, "hud_firemode_selector_bone");
    if (!selector_bone && pSettings->line_exist(weapon_section, "firemode_selector_bone"))
        selector_bone = pSettings->r_string(weapon_section, "firemode_selector_bone");

    const CMotionDef* motion_def = animated->LL_GetMotionDef(motion_id);
    u16 partition = motion_def->bone_or_part;
    u16 selector_bone_id = BI_NONE;
    if (selector_bone && selector_bone[0])
    {
        selector_bone_id = model->LL_BoneID(selector_bone);
        if (selector_bone_id == BI_NONE)
        {
            Msg("! [%s]: firemode selector bone [%s] not found in %s model", cName().c_str(), selector_bone, hud ? "HUD" : "world");
            return;
        }

        partition = BI_NONE;
        for (u16 part = 0; part < MAX_PARTS; ++part)
        {
            const auto& bones = animated->partitions().part(part).bones;
            if (std::find(bones.begin(), bones.end(), selector_bone_id) != bones.end())
            {
                partition = part;
                break;
            }
        }
        if (partition == BI_NONE)
        {
            Msg("! [%s]: firemode selector bone [%s] is not assigned to an animation partition", cName().c_str(), selector_bone);
            return;
        }
    }

    for (u16 part = 0; part < MAX_PARTS; ++part)
        animated->LL_CloseCycle(part, 1 << 1);

    // Keep the pose looping even if the source motion has StopAtEnd set. With
    // a configured selector bone it drives exactly that bone, so unrelated
    // weapon tracks in the same animation partition remain untouched.
    if (selector_bone_id != BI_NONE)
        animated->LL_PlayCycleOnBone(partition, selector_bone_id, motion_id, FALSE, motion_def->Accrue(), motion_def->Falloff(),
            motion_def->Speed(), FALSE, nullptr, nullptr, 1);
    else
        animated->LL_PlayCycle(partition, motion_id, FALSE, motion_def->Accrue(), motion_def->Falloff(), motion_def->Speed(), FALSE,
            nullptr, nullptr, 1);
}

void CWeaponMagazined::UpdateFireModePoses(bool force)
{
    if (!m_bHasDifferentFireModes || m_aFireModes.empty())
        return;

    if (!m_world_firemode_transition_end || Device.dwTimeGlobal >= m_world_firemode_transition_end)
    {
        m_world_firemode_transition_end = 0;
        ApplyFireModePose(smart_cast<IKinematics*>(Visual()), cNameSect().c_str(), false, force);
    }

    if (auto hud_item = HudItemData())
        ApplyFireModePose(hud_item->m_model, hud_item->m_sect_name.c_str(), true, force);
}

void CWeaponMagazined::PlayAnimFiremode()
{
    ClearFireModePose(HudItemData() ? HudItemData()->m_model : nullptr);
    ClearFireModePose(smart_cast<IKinematics*>(Visual()));
    PlayWorldFireModeTransition();

    string128 animation;
    if (FindFireModeHudAnimation(animation, sizeof(animation)))
    {
        PlayHUDMotion(animation, true, GetState());
        SetPending(TRUE);
    }
    else
    {
        m_previous_firemode_animation_index = CurrentFireModeAnimationIndex();
        SwitchState(eIdle);
    }
}
