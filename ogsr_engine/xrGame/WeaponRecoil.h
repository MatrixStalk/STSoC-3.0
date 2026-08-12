#pragma once

// Optional modern first-person recoil parameters. Angles are converted to
// radians by CWeapon::Load; positional HUD values remain in HUD metres.
struct SModernRecoilParams
{
    bool enabled{true};

    float camera_frequency{18.f};
    float camera_damping{0.78f};
    float camera_impulse{0.55f};
    float return_delay{0.075f};
    float return_speed{7.5f};
    float camera_max_pitch{8.f};
    float camera_max_yaw{3.f};

    float vertical_random{0.10f};
    float horizontal_factor{0.32f};
    float horizontal_random{0.45f};
    float horizontal_change_chance{0.22f};
    float roll_factor{0.08f};

    float first_shot_multiplier{1.12f};
    float burst_growth{0.035f};
    float burst_growth_limit{1.32f};
    float burst_reset_time{0.20f};
    float zoom_multiplier{0.72f};
    float crouch_multiplier{0.82f};

    float hud_kick{0.018f};
    float hud_up{0.0045f};
    float hud_pitch{1.15f};
    float hud_yaw{0.35f};
    float hud_roll{0.45f};
    float hud_frequency{23.f};
    float hud_damping{0.66f};
};
