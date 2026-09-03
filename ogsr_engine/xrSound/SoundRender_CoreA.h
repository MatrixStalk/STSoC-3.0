#pragma once

#include "SoundRender_Core.h"
#include "SteamAudioFMOD.h"

class CSoundRender_CoreA : public CSoundRender_Core
{
    typedef CSoundRender_Core inherited;

    FMOD::System* fmod_system{};
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

    struct SListener
    {
        Fvector position;
        Fvector orientation[2];
    } Listener{};

    void load_settings();
    bool enumerate_devices();
    bool initialize_fmod();
    bool initialize_steam_audio();
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

    FMOD::System* FmodSystem() const { return fmod_system; }
    unsigned int SteamAudioSpatializer() const { return steam_audio_spatializer; }
    bool HrtfEnabled() const { return hrtf_enabled; }
    bool AirAbsorptionEnabled() const { return air_absorption_enabled; }
    bool OcclusionEnabled() const { return occlusion_enabled; }
    virtual const Fvector& listener_position() { return Listener.position; }
};
