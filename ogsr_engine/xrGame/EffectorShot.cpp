// EffectorShot.cpp: implementation of the CCameraShotEffector class.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "EffectorShot.h"

//-----------------------------------------------------------------------------
// Weapon shot effector
//-----------------------------------------------------------------------------
CWeaponShotEffector::CWeaponShotEffector()
{
    fAngleHorz = 0.f;
    fAngleVert = -EPS_S;
    bActive = FALSE;
    bSingleShoot = FALSE;
    bSSActive = FALSE;
    fRelaxSpeed = EPS_L;
    fAngleVertMax = 0.f;
    fAngleVertFrac = 1.f;
    fAngleHorzMax = 0.f;
    fAngleHorzStep = 0.f;

    fLastDeltaVert = 0.f;
    fLastDeltaHorz = 0.f;
}

void CWeaponShotEffector::Initialize(float max_angle, float relax_speed, float max_angle_horz, float step_angle_horz, float angle_frac)
{
    fRelaxSpeed = _abs(relax_speed);
    VERIFY(!fis_zero(fRelaxSpeed));
    fAngleVertMax = _abs(max_angle);
    VERIFY(!fis_zero(fAngleVertMax));
    fAngleVertFrac = _abs(angle_frac);
    fAngleHorzMax = max_angle_horz;
    fAngleHorzStep = step_angle_horz;
}

void CWeaponShotEffector::Shot(float angle)
{
    float OldAngleVert = fAngleVert, OldAngleHorz = fAngleHorz;

    fAngleVert += (angle * fAngleVertFrac + ::Random.randF(-1, 1) * angle * (1 - fAngleVertFrac));
    //	VERIFY(!fis_zero(fAngleVertMax));
    clamp(fAngleVert, -fAngleVertMax, fAngleVertMax);
    if (fis_zero(fAngleVert - fAngleVertMax))
        fAngleVert *= ::Random.randF(0.9f, 1.1f);

    fAngleHorz = fAngleHorz + (fAngleVert / fAngleVertMax) * ::Random.randF(-1, 1) * fAngleHorzStep;
    //	VERIFY(_valid(fAngleHorz));

    clamp(fAngleHorz, -fAngleHorzMax, fAngleHorzMax);
    //		VERIFY(_valid(fAngleHorz));
    bActive = TRUE;

    fLastDeltaVert = fAngleVert - OldAngleVert;
    fLastDeltaHorz = fAngleHorz - OldAngleHorz;
    //	VERIFY(_valid(fLastDeltaHorz));
    bSSActive = TRUE;
}

void CWeaponShotEffector::Update()
{
    if (bActive)
    {
        float time_to_relax = _abs(fAngleVert) / fRelaxSpeed;
        //		VERIFY(_valid(time_to_relax));
        float relax_speed = (fis_zero(time_to_relax)) ? 0.0f : _abs(fAngleHorz) / time_to_relax;
        //		VERIFY(_valid(relax_speed));

        float time_to_relax_l = _abs(fLastDeltaVert) / fRelaxSpeed;
        //		VERIFY(_valid(time_to_relax_l));

        float relax_speed_l = (fis_zero(time_to_relax_l)) ? 0.0f : _abs(fLastDeltaHorz) / time_to_relax_l;
        //		VERIFY(_valid(relax_speed_l));
        //-------------------------------------------------------
        if (fAngleHorz >= 0.f)
            fAngleHorz -= relax_speed * Device.fTimeDelta;
        else
            fAngleHorz += relax_speed * Device.fTimeDelta;

        if (bSSActive)
        {
            if (fLastDeltaHorz >= 0.f)
                fLastDeltaHorz -= relax_speed_l * Device.fTimeDelta;
            else
                fLastDeltaHorz += relax_speed_l * Device.fTimeDelta;
        }
        //		VERIFY(_valid(fLastDeltaHorz));
        //-------------------------------------------------------
        if (fAngleVert >= 0.f)
        {
            fAngleVert -= fRelaxSpeed * Device.fTimeDelta;
            if (fAngleVert < 0.f)
                bActive = FALSE;
        }
        else
        {
            fAngleVert += fRelaxSpeed * Device.fTimeDelta;
            if (fAngleVert > 0.f)
                bActive = FALSE;
        }

        if (bSSActive)
        {
            if (fLastDeltaVert >= 0.f)
            {
                fLastDeltaVert -= fRelaxSpeed * Device.fTimeDelta;
                if (fLastDeltaVert < 0.f)
                    bSSActive = FALSE;
            }
            else
            {
                fLastDeltaVert += fRelaxSpeed * Device.fTimeDelta;
                if (fLastDeltaVert > 0.f)
                    bSSActive = FALSE;
            }
        };

        //-------------------------------------------------------
        if (!bActive)
        {
            fAngleVert = 0.f;
            fAngleHorz = 0.f;
            bSSActive = FALSE;
        }
        //-------------------------------------------------------
        if (!bSSActive)
        {
            fLastDeltaVert = 0.f;
            fLastDeltaHorz = 0.f;
        }
        //		VERIFY(_valid(fAngleVert));
        //		VERIFY(_valid(fAngleHorz));
        //		VERIFY(_valid(fLastDeltaHorz));
        //		VERIFY(_valid(fLastDeltaVert));
    }
}

void CWeaponShotEffector::Clear()
{
    bActive = false;
    fAngleVert = 0.f;
    fAngleHorz = 0.f;
};

void CWeaponShotEffector::GetDeltaAngle(Fvector& delta_angle)
{
    delta_angle.x = -fAngleVert;
    delta_angle.y = -fAngleHorz;
    delta_angle.z = 0.0f;
}

void CWeaponShotEffector::GetLastDelta(Fvector& delta_angle)
{
    delta_angle.x = -fLastDeltaVert;
    delta_angle.y = -fLastDeltaHorz;
    delta_angle.z = 0.0f;
};

void CWeaponShotEffector::ApplyLastAngles(float* pitch, float* yaw)
{
    *pitch -= fLastDeltaVert;
    *yaw -= fLastDeltaHorz;
}
void CWeaponShotEffector::ApplyDeltaAngles(float* pitch, float* yaw)
{
    *pitch -= fAngleVert;
    *yaw -= fAngleHorz;
};
//-----------------------------------------------------------------------------
// Camera shot effector
//-----------------------------------------------------------------------------
namespace
{
void update_recoil_spring(float& value, float& velocity, const float target, const float frequency, const float damping, float dt)
{
    // Sub-stepping keeps the spring stable after frame stalls without making
    // its response dependent on the current frame rate.
    while (dt > 0.f)
    {
        const float step = _min(dt, 1.f / 120.f);
        const float acceleration = (target - value) * frequency * frequency - 2.f * damping * frequency * velocity;
        velocity += acceleration * step;
        value += velocity * step;
        dt -= step;
    }
}

bool recoil_value_settled(const float value, const float velocity, const float epsilon)
{
    return _abs(value) < epsilon && _abs(velocity) < epsilon;
}
} // namespace

CCameraShotEffector::CCameraShotEffector(float max_angle, float relax_speed, float max_angle_horz, float step_angle_horz, float angle_frac,
    const SModernRecoilParams* modern_params)
    : CEffectorCam(eCEShot, 100000.f)
{
    CWeaponShotEffector::Initialize(max_angle, relax_speed, max_angle_horz, step_angle_horz, angle_frac);
    m_pActor = NULL;
    if (modern_params)
    {
        m_modern_params = *modern_params;
        m_modern_enabled = modern_params->enabled;
    }
}

CCameraShotEffector::~CCameraShotEffector() {}

//В ЗП здесь сделано по-другому
BOOL CCameraShotEffector::ProcessCam(SCamEffectorInfo& info)
{
    if (m_modern_enabled)
    {
        UpdateModernRecoil(Device.fTimeDelta);

        Fmatrix camera_basis;
        camera_basis.set(info.r, info.n, info.d, Fvector().set(0.f, 0.f, 0.f));
        float heading, pitch, bank;
        camera_basis.getHPB(heading, pitch, bank);
        camera_basis.setHPB(heading + m_camera_offset.y, pitch + m_camera_offset.x, bank + m_camera_roll);
        info.r.set(camera_basis.i).normalize_safe();
        info.n.set(camera_basis.j).normalize_safe();
        info.d.set(camera_basis.k).normalize_safe();

        return TRUE;
    }

    if (bActive)
    {
        float h, p;
        info.d.getHP(h, p);
        if (bSingleShoot)
        {
            if (bSSActive)
                info.d.setHP(h + fLastDeltaHorz, p + fLastDeltaVert);
        }
        else
            info.d.setHP(h + fAngleHorz, p + fAngleVert);

        Update();
    }
    return TRUE;
}

BOOL CCameraShotEffector::Valid()
{
    return m_modern_enabled ? bActive : CEffectorCam::Valid();
}

void CCameraShotEffector::Shot(float angle)
{
    Shot(angle, 1.f);
}

void CCameraShotEffector::Shot(float angle, float state_multiplier)
{
    if (!m_modern_enabled)
    {
        CWeaponShotEffector::Shot(angle);
        return;
    }

    const float now = Device.fTimeGlobal;
    if (now - m_last_shot_time > m_modern_params.burst_reset_time)
    {
        m_burst_shots = 0;
        m_horizontal_direction = ::Random.randF(-1.f, 1.f) < 0.f ? -1.f : 1.f;
    }
    else if (::Random.randF(0.f, 1.f) < m_modern_params.horizontal_change_chance)
    {
        m_horizontal_direction = -m_horizontal_direction;
    }

    const float burst_multiplier = _min(1.f + m_modern_params.burst_growth * float(m_burst_shots), m_modern_params.burst_growth_limit);
    const float first_shot_multiplier = m_burst_shots == 0 ? m_modern_params.first_shot_multiplier : 1.f;
    const float vertical_random = ::Random.randF(1.f - m_modern_params.vertical_random, 1.f + m_modern_params.vertical_random);
    const float vertical_kick = angle * state_multiplier * burst_multiplier * first_shot_multiplier * vertical_random;

    const float horizontal_random = ::Random.randF(1.f - m_modern_params.horizontal_random, 1.f + m_modern_params.horizontal_random);
    const float horizontal_kick =
        angle * state_multiplier * m_modern_params.horizontal_factor * horizontal_random * m_horizontal_direction;

    // Old weapon configs often allow 30-50 degrees because the legacy recoil
    // relaxes linearly. Keep those values as an outer safety limit, but use
    // tighter modern-recoil limits so a long burst can never overturn the view.
    const float pitch_limit = _min(fAngleVertMax, m_modern_params.camera_max_pitch);
    const float yaw_limit = _min(_abs(fAngleHorzMax), m_modern_params.camera_max_yaw);
    m_camera_target.x += vertical_kick;
    m_camera_target.y += horizontal_kick;
    clamp(m_camera_target.x, 0.f, pitch_limit);
    clamp(m_camera_target.y, -yaw_limit, yaw_limit);
    m_camera_roll_target -= horizontal_kick * m_modern_params.roll_factor;
    clamp(m_camera_roll_target, -yaw_limit * m_modern_params.roll_factor, yaw_limit * m_modern_params.roll_factor);

    // A short angular-velocity impulse supplies the sharp initial snap. The
    // target spring then carries the heavier, slower view rise through the
    // rest of the shot, avoiding both an instant teleport and a mushy delay.
    const float camera_impulse = m_modern_params.camera_frequency * m_modern_params.camera_impulse;
    m_camera_velocity.x += vertical_kick * camera_impulse;
    m_camera_velocity.y += horizontal_kick * camera_impulse;
    m_camera_roll_velocity -= horizontal_kick * m_modern_params.roll_factor * camera_impulse;

    // HUD kick is deliberately faster than view recoil. The direct displacement
    // supplies the sharp mechanical impulse; the under-damped spring supplies
    // the receiver/stock weight and a small natural overshoot.
    const float normalized_kick = _max(angle / deg2rad(0.25f), 0.1f) * state_multiplier * burst_multiplier;
    m_hud_position.z -= m_modern_params.hud_kick * normalized_kick;
    m_hud_position.y += m_modern_params.hud_up * normalized_kick;
    m_hud_position.x -= m_modern_params.hud_kick * 0.16f * normalized_kick * m_horizontal_direction;
    m_hud_rotation.x += m_modern_params.hud_pitch * normalized_kick;
    m_hud_rotation.y += m_modern_params.hud_yaw * normalized_kick * m_horizontal_direction;
    m_hud_rotation.z += m_modern_params.hud_roll * normalized_kick * m_horizontal_direction;
    clamp(m_hud_position.x, -0.04f, 0.04f);
    clamp(m_hud_position.y, -0.02f, 0.05f);
    clamp(m_hud_position.z, -0.10f, 0.02f);
    clamp(m_hud_rotation.x, -deg2rad(12.f), deg2rad(12.f));
    clamp(m_hud_rotation.y, -deg2rad(7.f), deg2rad(7.f));
    clamp(m_hud_rotation.z, -deg2rad(9.f), deg2rad(9.f));

    m_last_shot_time = now;
    ++m_burst_shots;
    bActive = TRUE;
}

void CCameraShotEffector::StopShooting()
{
    if (!m_modern_enabled)
        return;

    // Recovery starts from the time of the final shot. Keeping the effector
    // alive here is what allows a smooth return after releasing the trigger.
    bSingleShoot = FALSE;
}

void CCameraShotEffector::UpdateModernRecoil(float dt)
{
    clamp(dt, 0.f, 0.05f);
    const Fvector2 previous_camera_offset = m_camera_offset;

    if (Device.fTimeGlobal - m_last_shot_time >= m_modern_params.return_delay)
    {
        const float decay = expf(-m_modern_params.return_speed * dt);
        m_camera_target.mul(decay);
        m_camera_roll_target *= decay;
    }

    update_recoil_spring(m_camera_offset.x, m_camera_velocity.x, m_camera_target.x, m_modern_params.camera_frequency,
        m_modern_params.camera_damping, dt);
    update_recoil_spring(m_camera_offset.y, m_camera_velocity.y, m_camera_target.y, m_modern_params.camera_frequency,
        m_modern_params.camera_damping, dt);
    update_recoil_spring(m_camera_roll, m_camera_roll_velocity, m_camera_roll_target, m_modern_params.camera_frequency,
        m_modern_params.camera_damping, dt);

    update_recoil_spring(m_hud_position.x, m_hud_position_velocity.x, 0.f, m_modern_params.hud_frequency, m_modern_params.hud_damping, dt);
    update_recoil_spring(m_hud_position.y, m_hud_position_velocity.y, 0.f, m_modern_params.hud_frequency, m_modern_params.hud_damping, dt);
    update_recoil_spring(m_hud_position.z, m_hud_position_velocity.z, 0.f, m_modern_params.hud_frequency, m_modern_params.hud_damping, dt);
    update_recoil_spring(m_hud_rotation.x, m_hud_rotation_velocity.x, 0.f, m_modern_params.hud_frequency, m_modern_params.hud_damping, dt);
    update_recoil_spring(m_hud_rotation.y, m_hud_rotation_velocity.y, 0.f, m_modern_params.hud_frequency, m_modern_params.hud_damping, dt);
    update_recoil_spring(m_hud_rotation.z, m_hud_rotation_velocity.z, 0.f, m_modern_params.hud_frequency, m_modern_params.hud_damping, dt);

    // Preserve the old query API used by third-person actor orientation and
    // scripts, while the camera itself is driven by the spring values above.
    fAngleVert = m_camera_offset.x;
    fAngleHorz = m_camera_offset.y;
    fLastDeltaVert = m_camera_offset.x - previous_camera_offset.x;
    fLastDeltaHorz = m_camera_offset.y - previous_camera_offset.y;

    const bool camera_settled = recoil_value_settled(m_camera_offset.x, m_camera_velocity.x, EPS_S) &&
        recoil_value_settled(m_camera_offset.y, m_camera_velocity.y, EPS_S) &&
        recoil_value_settled(m_camera_roll, m_camera_roll_velocity, EPS_S) && m_camera_target.magnitude() < EPS_S;
    const bool hud_settled = m_hud_position.square_magnitude() < EPS_S * EPS_S && m_hud_position_velocity.square_magnitude() < EPS_S * EPS_S &&
        m_hud_rotation.square_magnitude() < EPS_S * EPS_S && m_hud_rotation_velocity.square_magnitude() < EPS_S * EPS_S;

    if (camera_settled && hud_settled && Device.fTimeGlobal - m_last_shot_time > m_modern_params.burst_reset_time)
        bActive = FALSE;
}

void CCameraShotEffector::GetHudRecoil(Fmatrix& transform) const
{
    transform.setHPB(m_hud_rotation.y, m_hud_rotation.x, m_hud_rotation.z);
    transform.translate_over(m_hud_position);
}

void CCameraShotEffector::Clear()
{
    CWeaponShotEffector::Clear();
    m_camera_offset.set(0.f, 0.f);
    m_camera_target.set(0.f, 0.f);
    m_camera_velocity.set(0.f, 0.f);
    m_camera_roll = 0.f;
    m_camera_roll_target = 0.f;
    m_camera_roll_velocity = 0.f;
    m_hud_position.set(0.f, 0.f, 0.f);
    m_hud_position_velocity.set(0.f, 0.f, 0.f);
    m_hud_rotation.set(0.f, 0.f, 0.f);
    m_hud_rotation_velocity.set(0.f, 0.f, 0.f);
    m_burst_shots = 0;
}
