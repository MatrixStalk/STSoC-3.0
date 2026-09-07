// EffectorShot.cpp: implementation of the CCameraShotEffector class.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "EffectorShot.h"
#include "Inventory.h"
#include "Weapon.h"

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
void update_arc9_spring(Fvector& value, Fvector& velocity, Fvector& acceleration, const float spring_constant,
    const float spring_magnitude, const float spring_damping, float dt, const bool stop_at_origin)
{
    // ARC9 adds a constant-magnitude pull toward the origin. Normalizing the
    // displacement directly makes that force discontinuous at zero and creates
    // a small perpetual limit cycle. Soften only that tiny central region while
    // retaining the authored response at normal recoil amplitudes.
    constexpr float return_soft_zone = 0.01f;
    constexpr float settle_value = 0.0005f;
    constexpr float settle_velocity = 0.005f;
    constexpr float state_limit = 210.f;

    if (stop_at_origin)
    {
        // Once no recoil impulse remains, solve a critically damped return
        // analytically. Continuing the constant-magnitude ARC9 spring here can
        // enter a visible limit cycle around zero, especially when ProcessCam
        // is evaluated by more than one viewport. This preserves both position
        // and velocity at the transition but cannot generate another oscillation.
        const float omega = _max(_max(sqrtf(_max(spring_constant, 0.f)), spring_damping * 0.5f), 1.f);
        const float decay = expf(-omega * dt);
        const Fvector old_value = value;
        const Fvector old_velocity = velocity;
        Fvector coefficient = value;
        coefficient.mul(omega);
        coefficient.add(velocity);
        value.mad(coefficient, dt);
        value.mul(decay);
        velocity.mad(coefficient, -omega * dt);
        velocity.mul(decay);
        if (dt > EPS_S)
            acceleration.sub(velocity, old_velocity).div(dt);
        else
            acceleration.set(0.f, 0.f, 0.f);

        const auto stop_crossed_axis = [](const float previous, float& current, float& speed, float& force)
        {
            if ((previous > 0.f && current <= 0.f) || (previous < 0.f && current >= 0.f))
            {
                current = 0.f;
                speed = 0.f;
                force = 0.f;
            }
        };
        stop_crossed_axis(old_value.x, value.x, velocity.x, acceleration.x);
        stop_crossed_axis(old_value.y, value.y, velocity.y, acceleration.y);
        stop_crossed_axis(old_value.z, value.z, velocity.z, acceleration.z);

        if (!_valid(value) || !_valid(velocity) || !_valid(acceleration) ||
            (value.square_magnitude() < settle_value * settle_value &&
                velocity.square_magnitude() < settle_velocity * settle_velocity))
        {
            value.set(0.f, 0.f, 0.f);
            velocity.set(0.f, 0.f, 0.f);
            acceleration.set(0.f, 0.f, 0.f);
        }
        return;
    }

    while (dt > 0.f)
    {
        const float step = _min(dt, 1.f / 240.f);

        Fvector drag = velocity;
        drag.mul(-velocity.magnitude() * 0.5f);

        Fvector restoring = value;
        const float value_length = restoring.magnitude();
        restoring.mul(-value_length * spring_constant);
        if (value_length > EPS_S)
            restoring.mad(value, -spring_magnitude / sqrtf(value_length * value_length + return_soft_zone * return_soft_zone));
        restoring.mad(velocity, -spring_damping);

        acceleration = drag;
        acceleration.add(restoring);
        clamp(acceleration.x, -state_limit, state_limit);
        clamp(acceleration.y, -state_limit, state_limit);
        clamp(acceleration.z, -state_limit, state_limit);
        velocity.mad(acceleration, step);
        value.mad(velocity, step);

        if (!_valid(value) || !_valid(velocity) || !_valid(acceleration) || value.magnitude() > state_limit || velocity.magnitude() > state_limit)
        {
            value.set(0.f, 0.f, 0.f);
            velocity.set(0.f, 0.f, 0.f);
            acceleration.set(0.f, 0.f, 0.f);
            return;
        }
        dt -= step;
    }

    // EPS_S is intentionally much smaller than a visible HUD displacement and
    // kept the effector alive for minutes. Snap only genuinely sub-pixel motion.
    if (value.square_magnitude() < settle_value * settle_value && velocity.square_magnitude() < settle_velocity * settle_velocity)
    {
        value.set(0.f, 0.f, 0.f);
        velocity.set(0.f, 0.f, 0.f);
        acceleration.set(0.f, 0.f, 0.f);
    }
}

bool recoil_vector_settled(const Fvector& value, const Fvector& velocity)
{
    constexpr float settle_value = 0.0005f;
    constexpr float settle_velocity = 0.005f;
    return value.square_magnitude() < settle_value * settle_value &&
        velocity.square_magnitude() < settle_velocity * settle_velocity;
}

float recoil_impulse_blend(const float dt, const float duration)
{
    // Exponential interpolation keeps the transition frame-rate independent
    // and lets consecutive shots merge instead of restarting a linear kick.
    return 1.f - expf(-dt / _max(duration, EPS_S));
}

void apply_recoil_impulse(Fvector2& value, Fvector2& impulse, const float blend)
{
    value.x += impulse.x * blend;
    value.y += impulse.y * blend;
    impulse.x *= 1.f - blend;
    impulse.y *= 1.f - blend;

    if (impulse.magnitude() < EPS_S)
        impulse.set(0.f, 0.f);
}

void apply_recoil_impulse(Fvector& value, Fvector& impulse, const float blend)
{
    Fvector step = impulse;
    step.mul(blend);
    value.add(step);
    impulse.mul(1.f - blend);

    if (impulse.square_magnitude() < EPS_S * EPS_S)
        impulse.set(0.f, 0.f, 0.f);
}

void clear_recoil_spring(Fvector& value, Fvector& velocity, Fvector& acceleration)
{
    value.set(0.f, 0.f, 0.f);
    velocity.set(0.f, 0.f, 0.f);
    acceleration.set(0.f, 0.f, 0.f);
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
        // Camera managers may evaluate this effector several times in one
        // render frame (main view, scopes and UI previews). Advancing the same
        // spring on every evaluation makes its damping frame-count dependent.
        if (m_last_update_frame != Device.dwFrame)
        {
            UpdateModernRecoil(Device.fTimeDelta);
            m_last_update_frame = Device.dwFrame;
        }

        Fmatrix camera_basis;
        camera_basis.set(info.r, info.n, info.d, Fvector().set(0.f, 0.f, 0.f));
        float heading, pitch, bank;
        camera_basis.getHPB(heading, pitch, bank);
        camera_basis.setHPB(heading + m_camera_offset.y, pitch + m_camera_offset.x, bank);
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
    if (now - m_last_shot_time > m_modern_params.recoil_full_reset_time)
    {
        m_burst_shots = 0;
        m_recoil_amount = 0.f;
        m_pattern_direction = 0.f;
    }

    const u32 shot = u32(floorf(m_recoil_amount)) + 1;
    if (shot > 1)
        m_pattern_direction += ::Random.randF(-m_modern_params.recoil_pattern_drift, m_modern_params.recoil_pattern_drift);

    const float direction = deg2rad(m_pattern_direction - 90.f);
    m_recoil_up = (sinf(direction) * m_modern_params.recoil_up +
                      ::Random.randF(-1.f, 0.f) * m_modern_params.recoil_random_up) *
        m_modern_params.recoil * state_multiplier;
    m_recoil_side = (cosf(direction) * m_modern_params.recoil_side +
                        ::Random.randF(-1.f, 1.f) * m_modern_params.recoil_random_side) *
        m_modern_params.recoil * state_multiplier;
    m_recoil_amount += m_modern_params.recoil_per_shot;

    // ARC9 spreads each camera kick across a fixed 30 ms interval. Its
    // multiplier of 25 therefore produces 0.75 degrees per recoil unit.
    // X-Ray pitch grows in the opposite direction to Source/GMod pitch.
    // Do not replace an unfinished kick. Accumulating it prevents a new shot
    // from cutting the previous transition short during automatic fire.
    m_camera_impulse.x += -deg2rad(m_recoil_up * 0.75f * m_modern_params.camera_recoil_scale);
    m_camera_impulse.y += deg2rad(m_recoil_side * 0.75f * m_modern_params.camera_recoil_scale);
    m_camera_impulse_time = _max(m_camera_impulse_time, m_modern_params.camera_impulse_duration);

    const float transition = clampr((float(m_burst_shots) - float(m_modern_params.shots_to_full_auto)) * 0.5f, 0.f, 1.f);
    const float visual_up_value = m_modern_params.visual_recoil_up_semi * (1.f - transition) +
        m_modern_params.visual_recoil_up * transition;
    const float visual_side_value = m_modern_params.visual_recoil_side_semi * (1.f - transition) +
        m_modern_params.visual_recoil_side * transition;
    const float visual_up = visual_up_value * m_modern_params.visual_recoil;
    const float visual_side = visual_side_value * m_modern_params.visual_recoil * m_recoil_side;
    const float visual_roll = m_modern_params.visual_recoil_roll * ::Random.randF(-1.f, 1.f) * 0.1f *
        m_modern_params.visual_recoil;
    CWeapon* active_weapon = m_pActor ? smart_cast<CWeapon*>(m_pActor->inventory().ActiveItem()) : nullptr;
    const bool zoomed = active_weapon && active_weapon->IsZoomed();
    const float visual_punch = (zoomed ? m_modern_params.visual_recoil_punch_sights :
                                         m_modern_params.visual_recoil_punch) * m_modern_params.visual_recoil;
    const float bump_up = zoomed ? m_modern_params.visual_recoil_bump_up : m_modern_params.visual_recoil_bump_up_hip;
    const float position_bump = m_modern_params.visual_recoil_position_bump * 0.66f;

    // Source pitch has the opposite sign. Positions remain in Source axes
    // here (X right, Y forward, Z up) and are mapped once in GetHudRecoil.
    m_hud_rotation_impulse.add(Fvector().set(-visual_up, visual_side * 15.f, visual_roll));
    m_hud_position_impulse.x += visual_side;
    m_hud_position_impulse.y -= visual_punch * position_bump;
    m_hud_position_impulse.z -= visual_up * bump_up * position_bump;

    if (m_modern_params.subtle_visual_recoil > 0.f)
    {
        const float subtle = m_modern_params.subtle_visual_recoil * 0.75f * (zoomed ? 1.f : 2.f);
        const float direction_scale = 1.3f - _min(m_recoil_amount, 4.5f) / 4.5f;
        m_subtle_position_impulse.add(Fvector().set(::Random.randF(-0.05f, 0.03f), -1.f,
            ::Random.randF(-0.06f, 0.03f)).mul(subtle));
        Fvector subtle_rotation;
        subtle_rotation.set(::Random.randF(0.1f, 0.2f), 0.f,
            m_modern_params.subtle_visual_recoil_direction * direction_scale + ::Random.randF(-1.35f, 1.35f)).mul(subtle);
        subtle_rotation.x *= 0.25f;
        m_subtle_rotation_impulse.add(subtle_rotation);
    }

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

    if (m_camera_impulse.magnitude() > EPS_S)
    {
        const float blend = recoil_impulse_blend(dt, m_modern_params.camera_impulse_duration);
        apply_recoil_impulse(m_camera_offset, m_camera_impulse, blend);
        m_camera_impulse_time = _max(0.f, m_camera_impulse_time - dt);
    }
    else
        m_camera_impulse_time = 0.f;

    // ARC9's RecoilRise auto-control continuously pulls only the recoil-added
    // view angle back toward zero, leaving the player's own mouse input alone.
    const float control = clampr(m_modern_params.recoil_auto_control * dt * 2.f, 0.f, 1.f);
    m_camera_offset.mul(1.f - control);
    clamp(m_camera_offset.x, -deg2rad(m_modern_params.camera_max_pitch), deg2rad(m_modern_params.camera_max_pitch));
    clamp(m_camera_offset.y, -deg2rad(m_modern_params.camera_max_yaw), deg2rad(m_modern_params.camera_max_yaw));

    const float recoil_decay = _max(0.f, 1.f - dt * 10.f);
    m_recoil_up *= recoil_decay;
    m_recoil_side *= recoil_decay;
    if (Device.fTimeGlobal - m_last_shot_time > m_modern_params.recoil_reset_time)
        m_recoil_amount = _max(0.f, m_recoil_amount - dt * m_modern_params.recoil_dissipation_rate);
    if (Device.fTimeGlobal - m_last_shot_time > m_modern_params.recoil_full_reset_time)
    {
        m_recoil_amount = 0.f;
        m_burst_shots = 0;
        m_pattern_direction = 0.f;
    }

    const float visual_blend = recoil_impulse_blend(dt, m_modern_params.camera_impulse_duration);
    apply_recoil_impulse(m_hud_position, m_hud_position_impulse, visual_blend);
    apply_recoil_impulse(m_hud_rotation, m_hud_rotation_impulse, visual_blend);
    // Subtle recoil is another input to the same mechanical HUD spring. Two
    // independent springs with different constants produced random beat
    // frequencies: hands could keep trembling until a later shot cancelled
    // their phase. Speed now controls only how quickly this impulse enters the
    // common spring, so every contribution has one coherent return path.
    const float subtle_duration = m_modern_params.camera_impulse_duration /
        _max(m_modern_params.subtle_visual_recoil_speed, 0.01f);
    const float subtle_blend = recoil_impulse_blend(dt, subtle_duration);
    apply_recoil_impulse(m_hud_position, m_subtle_position_impulse, subtle_blend);
    apply_recoil_impulse(m_hud_rotation, m_subtle_rotation_impulse, subtle_blend);

    // Four impulse time constants retain more than 98% of the authored kick.
    // The tiny tail must not keep feeding the spring after the trigger stops,
    // otherwise it can re-excite an axis immediately after it reaches zero.
    const float visual_return_delay = _max(_max(m_modern_params.camera_impulse_duration, subtle_duration) * 4.f, 0.15f);
    const bool returning_to_rest = Device.fTimeGlobal - m_last_shot_time > visual_return_delay;
    if (returning_to_rest)
    {
        m_hud_position_impulse.set(0.f, 0.f, 0.f);
        m_hud_rotation_impulse.set(0.f, 0.f, 0.f);
        m_subtle_position_impulse.set(0.f, 0.f, 0.f);
        m_subtle_rotation_impulse.set(0.f, 0.f, 0.f);
    }

    update_arc9_spring(m_hud_position, m_hud_position_velocity, m_hud_position_acceleration,
        m_modern_params.visual_recoil_spring_constant, m_modern_params.visual_recoil_spring_magnitude,
        m_modern_params.visual_recoil_spring_damping, dt, returning_to_rest);
    update_arc9_spring(m_hud_rotation, m_hud_rotation_velocity, m_hud_rotation_acceleration,
        m_modern_params.visual_recoil_spring_constant, m_modern_params.visual_recoil_spring_magnitude,
        m_modern_params.visual_recoil_spring_damping, dt, returning_to_rest);

    // A damped multi-axis spring can retain a tiny amount of angular momentum
    // and circle the origin without crossing an individual axis near enough to
    // trigger the regular settling checks. A later shot changes that orbit,
    // which made the stuck hand/weapon tremble appear to clear at random.
    // The visual recoil is no longer meaningful after the recoil sequence has
    // fully reset, so give it a finite lifetime and put every spring state at
    // the exact neutral pose. Keep a short margin for unusually slow impulses.
    const float visual_settle_timeout = _max(m_modern_params.recoil_full_reset_time,
        visual_return_delay + 0.5f);
    if (Device.fTimeGlobal - m_last_shot_time > visual_settle_timeout)
    {
        m_hud_position_impulse.set(0.f, 0.f, 0.f);
        m_hud_rotation_impulse.set(0.f, 0.f, 0.f);
        m_subtle_position_impulse.set(0.f, 0.f, 0.f);
        m_subtle_rotation_impulse.set(0.f, 0.f, 0.f);
        clear_recoil_spring(m_hud_position, m_hud_position_velocity, m_hud_position_acceleration);
        clear_recoil_spring(m_hud_rotation, m_hud_rotation_velocity, m_hud_rotation_acceleration);
    }
    // Preserve the old query API used by third-person actor orientation and
    // scripts, while the camera itself is driven by the spring values above.
    fAngleVert = m_camera_offset.x;
    fAngleHorz = m_camera_offset.y;
    fLastDeltaVert = m_camera_offset.x - previous_camera_offset.x;
    fLastDeltaHorz = m_camera_offset.y - previous_camera_offset.y;

    const bool camera_settled = m_camera_offset.magnitude() < EPS_S && m_camera_impulse.magnitude() < EPS_S;
    const bool hud_settled = m_hud_position_impulse.square_magnitude() < EPS_S * EPS_S &&
        m_hud_rotation_impulse.square_magnitude() < EPS_S * EPS_S &&
        m_subtle_position_impulse.square_magnitude() < EPS_S * EPS_S &&
        m_subtle_rotation_impulse.square_magnitude() < EPS_S * EPS_S &&
        recoil_vector_settled(m_hud_position, m_hud_position_velocity) &&
        recoil_vector_settled(m_hud_rotation, m_hud_rotation_velocity);

    if (camera_settled && hud_settled && Device.fTimeGlobal - m_last_shot_time > m_modern_params.recoil_full_reset_time)
        bActive = FALSE;
}

void CCameraShotEffector::GetHudRecoil(Fmatrix& transform) const
{
    Fvector rotation = m_hud_rotation;
    rotation.mul(deg2rad(1.f) * 2.5f);
    rotation.y = -rotation.y;

    Fvector position = m_hud_position;
    position.mul(m_modern_params.visual_recoil_scale);

    Fvector center;
    center.set(m_modern_params.visual_recoil_center.x, m_modern_params.visual_recoil_center.z,
        m_modern_params.visual_recoil_center.y);
    center.mul(m_modern_params.visual_recoil_scale);

    transform.setHPB(rotation.y, rotation.x, rotation.z);
    Fvector rotated_center;
    transform.transform_dir(rotated_center, center);
    transform.c.sub(center, rotated_center);
    Fvector mapped_position;
    mapped_position.set(position.x, position.z, position.y);
    transform.c.add(mapped_position);
}

void CCameraShotEffector::Clear()
{
    CWeaponShotEffector::Clear();
    m_camera_offset.set(0.f, 0.f);
    m_camera_impulse.set(0.f, 0.f);
    m_camera_impulse_time = 0.f;
    m_recoil_amount = 0.f;
    m_recoil_up = 0.f;
    m_recoil_side = 0.f;
    m_pattern_direction = 0.f;
    m_hud_position.set(0.f, 0.f, 0.f);
    m_hud_position_impulse.set(0.f, 0.f, 0.f);
    m_hud_position_velocity.set(0.f, 0.f, 0.f);
    m_hud_position_acceleration.set(0.f, 0.f, 0.f);
    m_hud_rotation.set(0.f, 0.f, 0.f);
    m_hud_rotation_impulse.set(0.f, 0.f, 0.f);
    m_hud_rotation_velocity.set(0.f, 0.f, 0.f);
    m_hud_rotation_acceleration.set(0.f, 0.f, 0.f);
    m_subtle_position_impulse.set(0.f, 0.f, 0.f);
    m_subtle_rotation_impulse.set(0.f, 0.f, 0.f);
    m_last_update_frame = u32(-1);
    m_burst_shots = 0;
}
