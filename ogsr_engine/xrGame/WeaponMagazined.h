#pragma once

#include "weapon.h"
#include "hudsound.h"
#include "ai_sounds.h"
#include <ik_calculate_data.h>

class ENGINE_API CMotionDef;

//размер очереди считается бесконечность
//заканчиваем стрельбу, только, если кончились патроны
#define WEAPON_ININITE_QUEUE -1

class CBinocularsVision;

class CWeaponMagazined : public CWeapon
{
    friend class CWeaponScript;

private:
    typedef CWeapon inherited;

protected:
    // Media :: sounds
    HUD_SOUND sndShow;
    HUD_SOUND sndHide;
    HUD_SOUND sndShot;
    HUD_SOUND sndEmptyClick;
    HUD_SOUND sndReload, sndReloadPartly, sndReloadJammed, sndReloadJammedLast;
    HUD_SOUND sndFireModes;
    HUD_SOUND sndZoomChange;
    HUD_SOUND sndTactItemOn;
    HUD_SOUND sndAimStart, sndAimEnd;
    HUD_SOUND sndItemOn;
    //звук текущего выстрела
    shared_str m_sSndShotCurrent;
	HUD_SOUND sndBore;
	HUD_SOUND sndBoreEmpty;
    HUD_SOUND sndLook;
    HUD_SOUND sndMagCheck;
    HUD_SOUND sndMuzzleCheck;
    HUD_SOUND sndReady;
    HUD_SOUND sndLoadSingle;
    HUD_SOUND sndMagazineReload;
    HUD_SOUND sndMagazineReloadTactical;
    HUD_SOUND sndMagazineReloadEmpty;
    HUD_SOUND sndMagazineCheckVariant;

    virtual void StopHUDSounds();

    //дополнительная информация о глушителе
    LPCSTR m_sSilencerFlameParticles;
    LPCSTR m_sSilencerSmokeParticles;
    HUD_SOUND sndSilencerShot;

    ESoundTypes m_eSoundShow;
    ESoundTypes m_eSoundHide;
    ESoundTypes m_eSoundShot;
    ESoundTypes m_eSoundEmptyClick;
    ESoundTypes m_eSoundReload;

    // General
    //кадр момента пересчета UpdateSounds
    u32 dwUpdateSounds_Frame;

protected:
    virtual void OnMagazineEmpty();

    virtual void switch2_Idle();
    virtual void switch2_Fire();
    virtual void switch2_Fire2() {}
    void switch2_Empty(const bool empty_click_anim_play);
    virtual void switch2_Reload();
    virtual void switch2_Hiding();
    virtual void switch2_Hidden();
    virtual void switch2_Showing();

    virtual void OnShot();
    virtual void PlaySoundShot();
    virtual void PlaySound(HUD_SOUND& snd, const Fvector& position, bool overlap = false) override;
    virtual void OnHudAnimationSoundStart(LPCSTR motion, float speed) override;

    virtual void OnEmptyClick();

    virtual void OnAnimationEnd(u32 state);
    virtual void OnStateSwitch(u32 S, u32 oldState);

    virtual void UpdateSounds();

    bool TryReload();

protected:
    virtual void ReloadMagazine();
    void ApplySilencerKoeffs(LPCSTR silencer_section);

    virtual void state_Fire(float dt);

public:
    CWeaponMagazined(LPCSTR name = "AK74", ESoundTypes eSoundType = SOUND_TYPE_WEAPON_SUBMACHINEGUN);
    virtual ~CWeaponMagazined();

    virtual void Load(LPCSTR section);
    virtual CWeaponMagazined* cast_weapon_magazined() { return this; }

    virtual void SetDefaults();
    virtual void FireStart();
    virtual void FireEnd();
    virtual void Reload();
    virtual void Misfire() override;
    virtual void DeviceSwitch() override;

protected:
    virtual void DeviceUpdate() override;

public:
    virtual void UpdateCL();
    virtual BOOL net_Spawn(CSE_Abstract* DC);
    virtual void net_Destroy();
    virtual void net_Export(CSE_Abstract* E);

    virtual void OnH_A_Chield();

    virtual bool Attach(PIItem pIItem, bool b_send_event);
    virtual bool Detach(const char* item_section_name, bool b_spawn_item);
    virtual bool CanAttach(PIItem pIItem);
    virtual bool CanDetach(const char* item_section_name);

    virtual void InitAddons();
    virtual void InitZoomParams(LPCSTR section, bool useTexture);

    virtual bool Action(s32 cmd, u32 flags);
    virtual void UnloadMagazine(bool spawn_ammo = true);

    virtual void GetBriefInfo(xr_string& str_name, xr_string& icon_sect_name, xr_string& str_count);

    //////////////////////////////////////////////
    // для стрельбы очередями или одиночными
    //////////////////////////////////////////////
public:
    virtual bool SwitchMode();
    virtual bool SingleShotMode() { return 1 == m_iQueueSize; }
    virtual void SetQueueSize(int size);
    IC int GetQueueSize() const { return m_iQueueSize; };
    virtual bool StopedAfterQueueFired() { return m_bStopedAfterQueueFired; }
    virtual void StopedAfterQueueFired(bool value) { m_bStopedAfterQueueFired = value; }

protected:
    //максимальный размер очереди, которой можно стрельнуть
    int m_iQueueSize;
    //количество реально выстреляных патронов
    int m_iShotNum;
    //  [7/20/2005]
    //после какого патрона, при непрерывной стрельбе, начинается отдача (сделано из-зи Абакана)
    int m_iShootEffectorStart;
    Fvector m_vStartPos, m_vStartDir;
    //  [7/20/2005]
    //флаг того, что мы остановились после того как выстреляли
    //ровно столько патронов, сколько было задано в m_iQueueSize
    bool m_bStopedAfterQueueFired;
    //флаг того, что хотя бы один выстрел мы должны сделать
    //(даже если очень быстро нажали на курок и вызвалось FireEnd)
    bool m_bFireSingleShot;
    //режимы стрельбы
    bool m_bHasDifferentFireModes;
    xr_vector<int> m_aFireModes;
    // Visual selector states can be mapped independently from queue sizes.
    // By convention automatic fire is state 0 and single fire is state 1.
    xr_vector<int> m_firemode_animation_indices;
    int m_iCurFireMode;
    string16 m_sCurFireMode;
    int m_iPrefferedFireMode;
    u32 m_fire_zoomout_time = u32(-1);
    int m_previous_firemode_animation_index{-1};
    int m_applied_world_firemode_pose{-1};
    int m_applied_hud_firemode_pose{-1};
    IKinematics* m_world_firemode_pose_model{};
    IKinematics* m_hud_firemode_pose_model{};
    u32 m_world_firemode_transition_end{};
    bool m_firemode_changed{};

    //переменная блокирует использование
    //только разных типов патронов
    bool m_bLockType;

    const char* m_str_count_tmpl;

    bool m_bFlameParticlesHideInZoom{};

protected:
    // режим выделения рамкой противников
    bool m_bVision;
    CBinocularsVision* m_binoc_vision;

    //////////////////////////////////////////////
    // режим приближения
    //////////////////////////////////////////////
public:
    virtual void OnZoomIn();
    virtual void OnZoomOut();
    virtual void OnZoomChanged();
    virtual void OnNextFireMode(bool = false);
    virtual void OnPrevFireMode(bool = false);
    virtual bool HasFireModes() { return m_bHasDifferentFireModes; };
    virtual int GetCurrentFireMode() { return m_bHasDifferentFireModes ? m_aFireModes[m_iCurFireMode] : 1; };
    virtual LPCSTR GetCurrentFireModeStr() { return m_sCurFireMode; };

    virtual void save(NET_Packet& output_packet);
    virtual void load(IReader& input_packet);

protected:
    virtual bool AllowFireWhileWorking() { return false; }

    //виртуальные функции для проигрывания анимации HUD
    virtual void PlayAnimBore();
    virtual void PlayAnimLook();
    virtual void PlayAnimMagCheck();
    virtual void PlayAnimMuzzleCheck();
    virtual void PlayAnimReady();
    virtual void PlayAnimShow();
    virtual void PlayAnimHide();
    virtual void PlayAnimReload();
    virtual void PlayAnimIdle();
	virtual void PlayAnimFiremode();

    int FireModeAnimationIndex(int mode_index) const;
    int CurrentFireModeAnimationIndex() const;
    bool FindFireModeHudAnimation(char* result, size_t result_size) const;
    void PlayWorldFireModeTransition();
    void UpdateFireModePoses(bool force = false);
    void ApplyFireModePose(IKinematics* model, LPCSTR config_section, bool hud, bool force);
    void ClearFireModePose(IKinematics* model);

    bool LaserSwitch{}, TorchSwitch{}, HeadLampSwitch{}, NightVisionSwitch{};
    bool CartridgeInTheChamberEnabled{};
    u32 CartridgeInTheChamber{};

private:
    string128 guns_aim_anm;

protected:
    virtual const char* GetAnimAimName();

    virtual void PlayAnimAim();
    virtual void PlayAnimShoot();
    virtual void PlayAnimFakeShoot();
    virtual void PlayAnimDeviceSwitch() override;
    virtual void PlayAnimCheckMisfire();
    virtual void PlayReloadSound();

    void ApplyMagazineAddonConfiguration(bool trim_ammo);
    void TrimMagazineToCapacity(bool return_ammo);
    void ScheduleNextBore();
    void ShowMagazineAmmoCount() const;
    bool DetachableMagazineSystemActive() const;
    const shared_str& InstalledMagazineSection() const;
    int ReloadTargetCapacity() const;
    void LoadMagazineVariantSounds();

    shared_str m_reload_animation_variant;
    shared_str m_magcheck_animation_variant;
    shared_str m_applied_magazine_section;
    bool m_applied_detachable_magazines{};
    bool m_first_ready_played{};
    u32 m_next_bore_time{};
    float m_bore_idle_time_min{30.f};
    float m_bore_idle_time_max{60.f};

    virtual int ShotsFired() { return m_iShotNum; }
    virtual float GetWeaponDeterioration();

    virtual void OnDrawUI();
    virtual void net_Relcase(CObject* object);

    bool ScopeRespawn(PIItem);

    // Lazy cache for base and attachment-provided snd_* definitions. Keeping
    // resolution at playback time lets every weapon sound be overridden by
    // an attachment, including sounds owned by derived weapon classes.
    xr_map<shared_str, HUD_SOUND*> m_resolved_sounds;
    u32 m_sound_timeline_revision{};
    xr_map<shared_str, shared_str> m_shot_alias_lines;
    bool m_animation_sounds_replace_legacy{};
    float m_indoor_sound_check_distance{30.f};

    shared_str ResolveSoundProvider(LPCSTR line) const;
    HUD_SOUND* ResolveSound(HUD_SOUND& fallback);
    HUD_SOUND* ResolveSoundLine(LPCSTR line, int type);
    void ClearResolvedSounds();
    bool IsShotIndoor();
    void RegisterShotSound(LPCSTR section, LPCSTR line, LPCSTR alias);
    bool HasShotSound(LPCSTR alias) const;
    void PlayShotSound(LPCSTR alias);

    virtual void OnMotionMark(u32 state, const motion_marks& M) override;
    int CheckAmmoBeforeReload(u32& v_ammoType);

    bool ShouldPlayFlameParticles();

public:
    void SuppressFirstReadyAnimation() { m_first_ready_played = true; }
};
