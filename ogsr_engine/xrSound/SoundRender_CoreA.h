#pragma once

#include "SoundRender_Core.h"
#include "SteamAudioFMOD.h"

class CSoundRender_CoreA : public CSoundRender_Core
{
    typedef CSoundRender_Core inherited;

    FMOD::System* fmod_system{};
    FMOD::ChannelGroup* environment_group{};
    FMOD::DSP* environment_reverb{};
    unsigned int steam_audio_plugin{};
    unsigned int steam_audio_spatializer{};
    IPLContext steam_audio_context{};
    IPLHRTF steam_audio_hrtf{};

    int sample_rate{48000};
    int dsp_buffer_length{1024};
    int dsp_buffer_count{4};
    bool hrtf_enabled{true};
    bool air_absorption_enabled{true};
    bool occlusion_enabled{true};
    float air_absorption_strength{0.35f};
    float occlusion_low_floor{0.25f};
    float occlusion_mid_floor{0.08f};
    float occlusion_high_floor{0.025f};
    float default_shot_gain{1.10f};
    float default_explosion_gain{1.f};
    float default_world_gain{1.f};
    float default_hud_gain{1.f};
    bool environment_reverb_enabled{true};
    float environment_reverb_strength{1.25f};
    float environment_reverb_transition{0.2f};
    float listener_shot_gain{1.10f};
    float listener_explosion_gain{1.f};
    float listener_world_gain{1.f};
    float listener_hud_gain{1.f};

    struct SListener
    {
        Fvector position;
        Fvector orientation[2];
    } Listener{};

    void load_settings();
    bool enumerate_devices();
    bool initialize_fmod();
    bool initialize_steam_audio();
    bool initialize_environment_reverb();
    void update_environment_reverb(const Fvector& position, float dt);
    void release_backend();

protected:
    virtual void update_listener(const Fvector& P, const Fvector& D, const Fvector& N, float dt);

public:
    CSoundRender_CoreA();
    virtual ~CSoundRender_CoreA();
    virtual void _initialize(int stage);
    virtual void _clear();
    virtual void _restart();
    virtual void update(const Fvector& P, const Fvector& D, const Fvector& N);
    virtual void set_master_volume(float f);
    virtual void set_listener_sound_profile(float shot_gain, float explosion_gain, float world_gain, float hud_gain) override;

    FMOD::System* FmodSystem() const { return fmod_system; }
    FMOD::ChannelGroup* EnvironmentChannelGroup() const { return environment_group; }
    unsigned int SteamAudioSpatializer() const { return steam_audio_spatializer; }
    bool HrtfEnabled() const { return hrtf_enabled; }
    bool AirAbsorptionEnabled() const { return air_absorption_enabled; }
    bool OcclusionEnabled() const { return occlusion_enabled; }
    float AirAbsorptionStrength() const { return air_absorption_strength; }
    float OcclusionLowFloor() const { return occlusion_low_floor; }
    float OcclusionMidFloor() const { return occlusion_mid_floor; }
    float OcclusionHighFloor() const { return occlusion_high_floor; }
    float ListenerShotGain() const { return listener_shot_gain; }
    float ListenerExplosionGain() const { return listener_explosion_gain; }
    float ListenerWorldGain() const { return listener_world_gain; }
    float ListenerHudGain() const { return listener_hud_gain; }
    virtual const Fvector& listener_position() { return Listener.position; }
    const Fvector& ListenerForward() const { return Listener.orientation[0]; }
    const Fvector& ListenerUp() const { return Listener.orientation[1]; }
};
