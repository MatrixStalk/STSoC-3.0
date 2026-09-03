#pragma once
#include "step_manager_defs.h"

class CEntityAlive;
class CBlend;

class CStepManager
{
    u8 m_legs_count;

    STEPS_MAP m_steps_map;
    SStepInfo m_step_info;

    CEntityAlive* m_object;

    u16 m_foot_bones[MAX_LEGS_COUNT];
    CBlend* m_blend;

    u32 m_time_anim_started;

    bool m_eft_human{};
    bool m_eft_was_on_ground{true};
    bool m_eft_was_moving{};
    u8 m_eft_previous_mode{};
    u32 m_eft_last_shuffle_time{};

public:
    CStepManager();
    virtual ~CStepManager();

    // init on construction
    virtual DLL_Pure* _construct();
    virtual void reload(LPCSTR section);

    // call on set animation
    void on_animation_start(MotionID motion_id, CBlend* blend);
    // call on updateCL
    void update();

    // Explicit actor events. NPC transitions are detected from their physics state.
    void on_eft_jump();
    void on_eft_land(float contact_speed);

    // process event
    virtual void event_on_step() {}

protected:
    Fvector get_foot_position(ELegType leg_type);
    virtual bool is_on_ground() { return true; }

private:
    bool play_eft_step(float power);
    void play_eft_action(u8 action, float power);
    void update_eft_transitions();
    void reload_foot_bones();
    void load_foot_bones(CInifile::Sect& data);

    float get_blend_time();
};
