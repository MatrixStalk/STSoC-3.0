#pragma once

// ARC9 first-person recoil parameters. The names and defaults mirror ARC9;
// angles stay in degrees until the final X-Ray HUD/camera transform.
struct SModernRecoilParams
{
    bool enabled{true};

    float recoil{1.f};
    float recoil_up{1.f};
    float recoil_side{1.f};
    float recoil_random_up{0.1f};
    float recoil_random_side{0.1f};
    float recoil_auto_control{1.f};
    float recoil_pattern_drift{12.f};
    float recoil_per_shot{1.f};
    float recoil_dissipation_rate{10.f};
    float recoil_reset_time{0.1f};
    float recoil_full_reset_time{2.f};
    float camera_recoil_scale{1.f};
    float camera_impulse_duration{0.03f};
    float camera_max_pitch{12.f};
    float camera_max_yaw{8.f};

    float visual_recoil{1.f};
    float visual_recoil_up{0.01f};
    float visual_recoil_up_semi{0.01f};
    float visual_recoil_side{0.05f};
    float visual_recoil_side_semi{0.05f};
    float visual_recoil_roll{0.23f};
    float visual_recoil_punch{1.5f};
    float visual_recoil_punch_sights{1.5f};
    float visual_recoil_spring_constant{120.f};
    float visual_recoil_spring_magnitude{1.f};
    float visual_recoil_spring_damping{6.f};
    float visual_recoil_bump_up{0.08f};
    float visual_recoil_bump_up_hip{0.08f};
    float visual_recoil_position_bump{1.5f};
    float visual_recoil_scale{0.025f};
    Fvector visual_recoil_center{};
    u32 shots_to_full_auto{3};

    float subtle_visual_recoil{0.f};
    float subtle_visual_recoil_direction{0.f};
    float subtle_visual_recoil_speed{1.f};

    float zoom_multiplier{1.f};
    float crouch_multiplier{1.f};
};
