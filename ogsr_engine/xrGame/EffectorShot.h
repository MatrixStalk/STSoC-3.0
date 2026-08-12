// EffectorShot.h: interface for the CCameraShotEffector class.
//
//////////////////////////////////////////////////////////////////////

#pragma once

#include "CameraEffector.h"
#include "../xr_3da/cameramanager.h"
#include "Actor.h"
#include "WeaponRecoil.h"

class CWeaponShotEffector
{
protected:
    float fAngleVert;
    float fAngleVertMax;
    float fAngleVertFrac;
    float fAngleHorz;
    float fAngleHorzMax;
    float fAngleHorzStep;
    float fRelaxSpeed;

    float fLastDeltaVert;
    float fLastDeltaHorz;

protected:
    BOOL bActive;
    BOOL bSingleShoot;
    BOOL bSSActive;

public:
    CWeaponShotEffector();
    virtual ~CWeaponShotEffector(){};

    void Initialize(float max_angle, float relax_speed, float max_angle_horz, float step_angle_horz, float angle_frac);
    IC BOOL IsActive() { return bActive; }
    virtual void SetActive(BOOL Active) { bActive = Active; };
    IC BOOL IsSingleShot() { return bSingleShoot; }
    virtual void SetSingleShoot(BOOL Single) { bSingleShoot = Single; };
    void Update();

    virtual void Shot(float angle);
    virtual void GetDeltaAngle(Fvector& delta_angle);
    virtual void GetLastDelta(Fvector& delta_angle);
    virtual void Clear();

    virtual void ApplyLastAngles(float* pitch, float* yaw);
    virtual void ApplyDeltaAngles(float* pitch, float* yaw);
};

class CCameraShotEffector : public CWeaponShotEffector, public CEffectorCam
{
protected:
    CActor* m_pActor;

    SModernRecoilParams m_modern_params{};
    bool m_modern_enabled{};
    Fvector2 m_camera_offset{};
    Fvector2 m_camera_target{};
    Fvector2 m_camera_velocity{};
    float m_camera_roll{};
    float m_camera_roll_target{};
    float m_camera_roll_velocity{};
    Fvector m_hud_position{};
    Fvector m_hud_position_velocity{};
    Fvector m_hud_rotation{};
    Fvector m_hud_rotation_velocity{};
    float m_last_shot_time{-1000.f};
    u32 m_burst_shots{};
    float m_horizontal_direction{1.f};

    void UpdateModernRecoil(float dt);

public:
    CCameraShotEffector(float max_angle, float relax_speed, float max_angle_horz, float step_angle_horz, float angle_frac,
        const SModernRecoilParams* modern_params = nullptr);
    virtual ~CCameraShotEffector();

    virtual BOOL ProcessCam(SCamEffectorInfo& info);
    virtual BOOL Valid() override;
    virtual void Shot(float angle) override;
    void Shot(float angle, float state_multiplier);
    void StopShooting();
    virtual void Clear() override;
    bool UsesModernRecoil() const { return m_modern_enabled; }
    void GetHudRecoil(Fmatrix& transform) const;

    virtual void SetActor(CActor* pActor) { m_pActor = pActor; };

    virtual CCameraShotEffector* cast_effector_shot() { return this; }
};
