#include "stdafx.h"
#include "../COMMON_AI/ai_sounds.h"
#include "..\Include/xrRender/KinematicsAnimated.h"
#include "step_manager_defs.h"
#include "step_manager.h"
#include "entity_alive.h"
#include "level.h"
#include "gamepersistent.h"
#include "material_manager.h"

#include "IKLimbsController.h"
#include "actor.h"
#include "ai/stalker/ai_stalker.h"
#include "stalker_movement_manager.h"
#include "CharacterPhysicsSupport.h"
#include "PHMovementControl.h"
#include "game_object_space.h"
#include "script_game_object.h"

namespace
{
enum EEftMoveMode : u8
{
    eEftWalk,
    eEftRun,
    eEftSprint,
    eEftCrouch,
    eEftMoveModeCount
};

enum EEftAction : u8
{
    eEftJump,
    eEftLand,
    eEftShuffle
};

constexpr u32 eft_surface_count = 16;
LPCSTR const eft_surface_names[eft_surface_count] = {"default", "stone", "tile", "metal", "metalhollow", "MAT_METALGRATE", "grass", "dirt",
                                                     "gravel", "rubble", "sand", "snow", "wood", "glass", "water", "cloth"};
LPCSTR const eft_mode_sections[eEftMoveModeCount] = {"eft_footsteps_walk", "eft_footsteps_run", "eft_footsteps_sprint", "eft_footsteps_crouch"};
LPCSTR const eft_mode_names[eEftMoveModeCount] = {"walk", "run", "sprint", "crouch"};

struct SEftSurfaceSounds
{
    xr_vector<ref_sound> steps[eEftMoveModeCount];
    xr_vector<ref_sound> jump;
    xr_vector<ref_sound> land;
    xr_vector<ref_sound> shuffle;
    xr_vector<ref_sound> crouch_overlay;
    float crouch_overlay_volume{1.f};
};

class CEftFootstepLibrary
{
    bool m_loaded{};
    bool m_enabled{};
    float m_mode_volume[eEftMoveModeCount]{1.f, 1.f, 1.f, 1.f};
    float m_gear_volume[eEftMoveModeCount]{.35f, .55f, .7f, .25f};
    float m_jump_volume{1.f};
    float m_land_volume{1.f};
    float m_shuffle_volume{1.f};
    float m_npc_sprint_speed{5.5f};
    u32 m_shuffle_cooldown{250};
    SEftSurfaceSounds m_surfaces[eft_surface_count];
    xr_vector<ref_sound> m_actor_gear[eEftMoveModeCount];
    xr_vector<ref_sound> m_npc_gear[eEftMoveModeCount];

    void load_list(LPCSTR section, LPCSTR line, xr_vector<ref_sound>& sounds)
    {
        if (!pSettings->section_exist(section) || !pSettings->line_exist(section, line))
            return;

        LPCSTR value = pSettings->r_string(section, line);
        const u32 count = _GetItemCount(value);
        string_path sound_name;
        for (u32 i = 0; i < count; ++i)
        {
            _GetItem(value, i, sound_name);
            if (!sound_name[0])
                continue;
            ref_sound sound;
            // Footsteps must carry a top-level AI sound category as well as
            // the STEP subtype; a bare SOUND_TYPE_STEP is not a valid event.
            ::Sound->create(sound, sound_name, st_Effect, SOUND_TYPE_MONSTER_STEP);
            sounds.push_back(sound);
        }
    }

public:
    void load()
    {
        if (m_loaded)
            return;
        m_loaded = true;
        if (!pSettings->section_exist("eft_footsteps"))
            return;

        m_enabled = READ_IF_EXISTS(pSettings, r_bool, "eft_footsteps", "enabled", true);
        if (!m_enabled)
            return;

        m_mode_volume[eEftWalk] = READ_IF_EXISTS(pSettings, r_float, "eft_footsteps", "walk_volume", 1.f);
        m_mode_volume[eEftRun] = READ_IF_EXISTS(pSettings, r_float, "eft_footsteps", "run_volume", 1.f);
        m_mode_volume[eEftSprint] = READ_IF_EXISTS(pSettings, r_float, "eft_footsteps", "sprint_volume", 1.f);
        m_mode_volume[eEftCrouch] = READ_IF_EXISTS(pSettings, r_float, "eft_footsteps", "crouch_volume", .65f);
        m_gear_volume[eEftWalk] = READ_IF_EXISTS(pSettings, r_float, "eft_footsteps", "walk_gear_volume", .35f);
        m_gear_volume[eEftRun] = READ_IF_EXISTS(pSettings, r_float, "eft_footsteps", "run_gear_volume", .55f);
        m_gear_volume[eEftSprint] = READ_IF_EXISTS(pSettings, r_float, "eft_footsteps", "sprint_gear_volume", .7f);
        m_gear_volume[eEftCrouch] = READ_IF_EXISTS(pSettings, r_float, "eft_footsteps", "crouch_gear_volume", .25f);
        m_jump_volume = READ_IF_EXISTS(pSettings, r_float, "eft_footsteps", "jump_volume", 1.f);
        m_land_volume = READ_IF_EXISTS(pSettings, r_float, "eft_footsteps", "land_volume", 1.f);
        m_shuffle_volume = READ_IF_EXISTS(pSettings, r_float, "eft_footsteps", "shuffle_volume", 1.f);
        m_npc_sprint_speed = READ_IF_EXISTS(pSettings, r_float, "eft_footsteps", "npc_sprint_speed", 5.5f);
        m_shuffle_cooldown = READ_IF_EXISTS(pSettings, r_u32, "eft_footsteps", "shuffle_cooldown_ms", 250);

        for (u32 surface = 0; surface < eft_surface_count; ++surface)
        {
            for (u32 mode = 0; mode < eEftMoveModeCount; ++mode)
                load_list(eft_mode_sections[mode], eft_surface_names[surface], m_surfaces[surface].steps[mode]);

            string64 key;
            strconcat(sizeof(key), key, eft_surface_names[surface], "_jump");
            load_list("eft_footsteps_run", key, m_surfaces[surface].jump);
            strconcat(sizeof(key), key, eft_surface_names[surface], "_land");
            load_list("eft_footsteps_run", key, m_surfaces[surface].land);
            strconcat(sizeof(key), key, eft_surface_names[surface], "_shuffle");
            load_list("eft_footsteps_sprint", key, m_surfaces[surface].shuffle);
            strconcat(sizeof(key), key, eft_surface_names[surface], "_overlay");
            load_list("eft_footsteps_crouch", key, m_surfaces[surface].crouch_overlay);

            if (pSettings->section_exist("eft_footsteps_crouch_overlay_volume") &&
                pSettings->line_exist("eft_footsteps_crouch_overlay_volume", eft_surface_names[surface]))
                m_surfaces[surface].crouch_overlay_volume = pSettings->r_float("eft_footsteps_crouch_overlay_volume", eft_surface_names[surface]);
        }

        for (u32 mode = 0; mode < eEftMoveModeCount; ++mode)
        {
            load_list(eft_mode_sections[mode], "overlay", m_actor_gear[mode]);
            load_list("eft_footsteps_npc_gear", eft_mode_names[mode], m_npc_gear[mode]);
        }
    }

    bool enabled() const { return m_enabled; }
    float npc_sprint_speed() const { return m_npc_sprint_speed; }
    u32 shuffle_cooldown() const { return m_shuffle_cooldown; }

    u32 surface_index(u16 material_idx) const
    {
        if (material_idx == GAMEMTL_NONE_IDX || !pSettings->section_exist("eft_footsteps_materials"))
            return 0;
        LPCSTR material = *GMLib.GetMaterialByIdx(material_idx)->m_Name;
        LPCSTR surface = pSettings->line_exist("eft_footsteps_materials", material) ? pSettings->r_string("eft_footsteps_materials", material) : "default";
        for (u32 i = 0; i < eft_surface_count; ++i)
            if (0 == _stricmp(surface, eft_surface_names[i]))
                return i;
        return 0;
    }

    bool play(xr_vector<ref_sound>& sounds, CEntityAlive* object, float volume) const
    {
        if (sounds.empty())
            return false;
        Fvector position = object->Position();
        position.y += .5f;
        GET_RANDOM(sounds).play_no_feedback(object, 0, 0, &position, &volume);
        return true;
    }

    bool play_step(u32 surface, EEftMoveMode mode, CEntityAlive* object, bool actor, float power)
    {
        xr_vector<ref_sound>& steps = m_surfaces[surface].steps[mode].empty() ? m_surfaces[0].steps[mode] : m_surfaces[surface].steps[mode];
        bool played = play(steps, object, power * m_mode_volume[mode]);
        if (mode == eEftCrouch)
            play(m_surfaces[surface].crouch_overlay, object, power * m_surfaces[surface].crouch_overlay_volume);
        play(actor ? m_actor_gear[mode] : m_npc_gear[mode], object, power * m_gear_volume[mode]);
        return played;
    }

    void play_action(u32 surface, EEftAction action, CEntityAlive* object, float power)
    {
        switch (action)
        {
        case eEftJump:
            play(m_surfaces[surface].jump.empty() ? m_surfaces[0].jump : m_surfaces[surface].jump, object, power * m_jump_volume);
            break;
        case eEftLand:
            play(m_surfaces[surface].land.empty() ? m_surfaces[0].land : m_surfaces[surface].land, object, power * m_land_volume);
            break;
        case eEftShuffle:
            play(m_surfaces[surface].shuffle.empty() ? m_surfaces[0].shuffle : m_surfaces[surface].shuffle, object, power * m_shuffle_volume);
            break;
        }
    }
};

CEftFootstepLibrary*& eft_footstep_library_instance()
{
    static CEftFootstepLibrary* library{};
    return library;
}

CEftFootstepLibrary& eft_footsteps()
{
    CEftFootstepLibrary*& library = eft_footstep_library_instance();
    if (!library)
        library = xr_new<CEftFootstepLibrary>();
    library->load();
    return *library;
}

bool eft_is_on_ground(CEntityAlive* object)
{
    CCharacterPhysicsSupport* support = object->character_physics_support();
    return support && support->movement() && support->movement()->Environment() != CPHMovementControl::peInAir;
}

EEftMoveMode eft_move_mode(CEntityAlive* object)
{
    if (CActor* actor = smart_cast<CActor*>(object))
    {
        if (actor->is_actor_sprinting())
            return eEftSprint;
        if (actor->is_actor_crouching() || actor->is_actor_creeping())
            return eEftCrouch;
        if (actor->is_actor_running())
            return eEftRun;
        return eEftWalk;
    }

    CAI_Stalker* stalker = smart_cast<CAI_Stalker*>(object);
    if (!stalker)
        return eEftWalk;
    if (stalker->movement().body_state() == MonsterSpace::eBodyStateCrouch)
        return eEftCrouch;
    if (stalker->movement().movement_type() == MonsterSpace::eMovementTypeRun)
    {
        const float speed = stalker->movement().speed(stalker->character_physics_support()->movement());
        return speed >= eft_footsteps().npc_sprint_speed() ? eEftSprint : eEftRun;
    }
    return eEftWalk;
}

bool eft_is_moving(CEntityAlive* object)
{
    if (CActor* actor = smart_cast<CActor*>(object))
        return actor->AnyMove() && eft_is_on_ground(object);
    if (CAI_Stalker* stalker = smart_cast<CAI_Stalker*>(object))
        return stalker->movement().movement_type() != MonsterSpace::eMovementTypeStand &&
            stalker->movement().speed(stalker->character_physics_support()->movement()) > EPS_L;
    return false;
}
} // namespace

void destroy_eft_footstep_library()
{
    CEftFootstepLibrary*& library = eft_footstep_library_instance();
    xr_delete(library);
}

CStepManager::CStepManager() {}

CStepManager::~CStepManager() {}

DLL_Pure* CStepManager::_construct()
{
    m_object = smart_cast<CEntityAlive*>(this);
    VERIFY(m_object);
    return (m_object);
}

void CStepManager::reload(LPCSTR section)
{
    m_legs_count = pSettings->r_u8(section, "LegsCount");
    LPCSTR anim_section = pSettings->r_string(section, "step_params");

    if (!pSettings->section_exist(anim_section))
        return;
    VERIFY((m_legs_count >= MIN_LEGS_COUNT) && (m_legs_count <= MAX_LEGS_COUNT));

    SStepParam param;
    param.step[0].time = 0.1f; // avoid warning

    LPCSTR anim_name, val;
    string16 cur_elem;

    m_steps_map.clear();

    IKinematicsAnimated* skeleton_animated = smart_cast<IKinematicsAnimated*>(m_object->Visual());

    for (u32 i = 0; pSettings->r_line(anim_section, i, &anim_name, &val); ++i)
    {
        _GetItem(val, 0, cur_elem);

        param.cycles = u8(atoi(cur_elem));
        R_ASSERT(param.cycles >= 1);

        for (u32 j = 0; j < m_legs_count; j++)
        {
            _GetItem(val, 1 + j * 2, cur_elem);
            param.step[j].time = float(atof(cur_elem));
            _GetItem(val, 1 + j * 2 + 1, cur_elem);
            param.step[j].power = float(atof(cur_elem));
            VERIFY(_valid(param.step[j].power));
        }

        MotionID motion_id = skeleton_animated->ID_Cycle_Safe(anim_name);
        if (!motion_id)
            continue;

        m_steps_map.insert(mk_pair(motion_id, param));
    }

    // reload foot bones
    for (u32 i = 0; i < MAX_LEGS_COUNT; i++)
        m_foot_bones[i] = BI_NONE;
    reload_foot_bones();

    m_time_anim_started = 0;
    m_blend = 0;

    m_eft_human = eft_footsteps().enabled() && (smart_cast<CActor*>(m_object) || smart_cast<CAI_Stalker*>(m_object));
    m_eft_was_on_ground = eft_is_on_ground(m_object);
    m_eft_was_moving = false;
    m_eft_previous_mode = eEftWalk;
    m_eft_last_shuffle_time = 0;
}

void CStepManager::on_animation_start(MotionID motion_id, CBlend* blend)
{
    m_blend = blend;
    if (!m_blend)
        return;

    if (m_object->character_ik_controller())
        m_object->character_ik_controller()->PlayLegs(blend);

    m_time_anim_started = Device.dwTimeGlobal;

    // искать текущую анимацию в STEPS_MAP
    STEPS_MAP_IT it = m_steps_map.find(motion_id);
    if (it == m_steps_map.end())
    {
        m_step_info.disable = true;
        return;
    }

    m_step_info.disable = false;
    m_step_info.params = it->second;
    m_step_info.cur_cycle = 1; // all cycles are 1-based

    for (u32 i = 0; i < m_legs_count; i++)
    {
        m_step_info.activity[i].handled = false;
        m_step_info.activity[i].cycle = m_step_info.cur_cycle;
    }

    VERIFY(m_blend);
}

void CStepManager::update()
{
    START_PROFILE("Step Manager")

    if (m_eft_human)
        update_eft_transitions();

    if (m_step_info.disable)
        return;
    if (!m_blend)
        return;

    SGameMtlPair* mtl_pair = m_object->material().get_current_pair();
    if (!mtl_pair)
        return;

    // получить параметры шага
    SStepParam& step = m_step_info.params;
    u32 cur_time = Device.dwTimeGlobal;

    // время одного цикла анимации
    float cycle_anim_time = get_blend_time() / step.cycles;

    // пройти по всем ногам и проверить время
    for (u32 i = 0; i < m_legs_count; i++)
    {
        // если событие уже обработано для этой ноги, то skip
        if (m_step_info.activity[i].handled && (m_step_info.activity[i].cycle == m_step_info.cur_cycle))
            continue;

        // вычислить смещённое время шага в соответствии с параметрами анимации ходьбы
        u32 offset_time = m_time_anim_started + u32(1000 * (cycle_anim_time * (m_step_info.cur_cycle - 1) + cycle_anim_time * step.step[i].time));
        if (offset_time <= cur_time)
        {
            // Играть звук
            if (is_on_ground())
            {
                auto actor = smart_cast<CActor*>(m_object);

                const bool eft_played = m_eft_human && play_eft_step(m_step_info.params.step[i].power);
                if (!eft_played && mtl_pair->StepSounds.empty())
                {
                    if (actor)
                    {
                        Msg("!![%s] no sound for steps pair id0:[%d], id1:[%d]", __FUNCTION__, mtl_pair->GetMtl0(), mtl_pair->GetMtl1());
                    }

                    m_step_info.activity[i].handled = true;
                    m_step_info.activity[i].cycle = m_step_info.cur_cycle;
                    continue;
                }

                if (!eft_played)
                {
                    Fvector sound_pos = m_object->Position();
                    sound_pos.y += 0.5;
                    GET_RANDOM(mtl_pair->StepSounds).play_no_feedback(m_object, 0, 0, &sound_pos, &m_step_info.params.step[i].power);
                }

                if (actor)
                    actor->callback(GameObject::eOnActorFootStep)(actor->lua_game_object(), m_step_info.params.step[i].power);
            }

            // Играть партиклы
            if (!mtl_pair->CollideParticles.empty())
            {
                LPCSTR ps_name = *mtl_pair->CollideParticles[::Random.randI(0, mtl_pair->CollideParticles.size())];

                //отыграть партиклы столкновения материалов
                CParticlesObject* ps = CParticlesObject::Create(ps_name, TRUE);

                // вычислить позицию и направленность партикла
                Fmatrix pos;

                // установить направление
                pos.k.set(Fvector().set(0.0f, 1.0f, 0.0f));
                Fvector::generate_orthonormal_basis(pos.k, pos.j, pos.i);

                // установить позицию
                pos.c.set(get_foot_position(ELegType(i)));

                ps->UpdateParent(pos, Fvector().set(0.f, 0.f, 0.f));
                GamePersistent().ps_needtoplay.push_back(ps);
            }

            // Play Camera FXs
            event_on_step();

            // обновить поле handle
            m_step_info.activity[i].handled = true;
            m_step_info.activity[i].cycle = m_step_info.cur_cycle;
        }
    }

    // определить текущий цикл
    if (m_step_info.cur_cycle < step.cycles)
        m_step_info.cur_cycle = 1 + u8(float(cur_time - m_time_anim_started) / (1000.f * cycle_anim_time));

    // если анимация циклическая...
    u32 time_anim_end = m_time_anim_started + u32(get_blend_time() * 1000); // время завершения работы анимации
    if (!m_blend->stop_at_end && (time_anim_end < cur_time))
    {
        m_time_anim_started = time_anim_end;
        m_step_info.cur_cycle = 1;

        for (u32 i = 0; i < m_legs_count; i++)
        {
            m_step_info.activity[i].handled = false;
            m_step_info.activity[i].cycle = m_step_info.cur_cycle;
        }
    }
    STOP_PROFILE
}

bool CStepManager::play_eft_step(float power)
{
    const u32 surface = eft_footsteps().surface_index(m_object->material().last_material_idx());
    const EEftMoveMode mode = eft_move_mode(m_object);
    return eft_footsteps().play_step(surface, mode, m_object, smart_cast<CActor*>(m_object) != nullptr, power);
}

void CStepManager::play_eft_action(u8 action, float power)
{
    if (!m_eft_human)
        return;
    const u32 surface = eft_footsteps().surface_index(m_object->material().last_material_idx());
    eft_footsteps().play_action(surface, EEftAction(action), m_object, power);
}

void CStepManager::on_eft_jump() { play_eft_action(eEftJump, 1.f); }

void CStepManager::on_eft_land(float contact_speed) { play_eft_action(eEftLand, clampr(contact_speed / 6.f, .55f, 1.35f)); }

void CStepManager::update_eft_transitions()
{
    const bool on_ground = eft_is_on_ground(m_object);
    const bool moving = eft_is_moving(m_object);
    const EEftMoveMode mode = eft_move_mode(m_object);

    // Actor jump/land events are sent at the exact physics callbacks above.
    if (!smart_cast<CActor*>(m_object))
    {
        if (m_eft_was_on_ground && !on_ground)
            play_eft_action(eEftJump, 1.f);
        else if (!m_eft_was_on_ground && on_ground)
            play_eft_action(eEftLand, 1.f);
    }

    if (m_eft_was_moving && !moving && m_eft_previous_mode == eEftSprint &&
        Device.dwTimeGlobal - m_eft_last_shuffle_time >= eft_footsteps().shuffle_cooldown())
    {
        play_eft_action(eEftShuffle, 1.f);
        m_eft_last_shuffle_time = Device.dwTimeGlobal;
    }

    m_eft_was_on_ground = on_ground;
    m_eft_was_moving = moving;
    if (moving)
        m_eft_previous_mode = mode;
}

//////////////////////////////////////////////////////////////////////////
// Function for foot processing
//////////////////////////////////////////////////////////////////////////
Fvector CStepManager::get_foot_position(ELegType leg_type)
{
    R_ASSERT2(m_foot_bones[leg_type] != BI_NONE, make_string("[%s] foot bone had not been set", m_object->Name()));

    IKinematics* pK = smart_cast<IKinematics*>(m_object->Visual());
    const Fmatrix& bone_transform = pK->LL_GetBoneInstance(m_foot_bones[leg_type]).mTransform;

    Fmatrix global_transform;
    global_transform.mul_43(m_object->XFORM(), bone_transform);

    return global_transform.c;
}

void CStepManager::load_foot_bones(CInifile::Sect& data)
{
    for (const auto& item : data.Data)
    {
        u16 index = smart_cast<IKinematics*>(m_object->Visual())->LL_BoneID(item.second.c_str());
        VERIFY3(index != BI_NONE, "foot bone not found", item.second.c_str());

        if (xr_strcmp(item.first.c_str(), "front_left") == 0)
            m_foot_bones[eFrontLeft] = index;
        else if (xr_strcmp(item.first.c_str(), "front_right") == 0)
            m_foot_bones[eFrontRight] = index;
        else if (xr_strcmp(item.first.c_str(), "back_right") == 0)
            m_foot_bones[eBackRight] = index;
        else if (xr_strcmp(item.first.c_str(), "back_left") == 0)
            m_foot_bones[eBackLeft] = index;
    }
}

void CStepManager::reload_foot_bones()
{
    CInifile* ini = smart_cast<IKinematics*>(m_object->Visual())->LL_UserData();
    if (ini && ini->section_exist("foot_bones"))
    {
        load_foot_bones(ini->r_section("foot_bones"));
    }
    else
    {
        if (!pSettings->line_exist(*m_object->cNameSect(), "foot_bones"))
            R_ASSERT2(false, "section [foot_bones] not found in monster user_data");
        load_foot_bones(pSettings->r_section(pSettings->r_string(*m_object->cNameSect(), "foot_bones")));
    }

    // проверка на соответсвие
    int count = 0;
    for (u32 i = 0; i < MAX_LEGS_COUNT; i++)
        if (m_foot_bones[i] != BI_NONE)
            count++;

    VERIFY(count == m_legs_count);
}

float CStepManager::get_blend_time() { return (m_blend->timeTotal / m_blend->speed); }
