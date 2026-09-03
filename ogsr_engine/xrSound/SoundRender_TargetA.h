#pragma once

#include "soundrender_Target.h"
#include "soundrender_CoreA.h"

class CSoundRender_TargetA : public CSoundRender_Target
{
    typedef CSoundRender_Target inherited;

    FMOD::Sound* fmod_sound{};
    FMOD::Channel* fmod_channel{};
    FMOD::DSP* steam_audio_dsp{};
    float cache_gain{};
    float cache_pitch{1.f};

    struct Biquad
    {
        float b0{1.f}, b1{}, b2{}, a1{}, a2{};
        float z1[2]{}, z2[2]{};
    };
    Biquad eq_filters[4];
    u32 eq_revision{u32(-1)};
    u32 eq_sample_rate{};

    void reset_equalizer();
    void update_equalizer(const SSoundEqualizer& settings, u32 sample_rate);
    void apply_equalizer(void* data, u32 bytes, const WAVEFORMATEX& format);
    bool create_stream();
    void release_stream();
    void attach_steam_audio();
    static FMOD_RESULT F_CALL pcm_read(FMOD_SOUND* sound, void* data, unsigned int bytes);
    static FMOD_RESULT F_CALL pcm_seek(FMOD_SOUND* sound, int subsound, unsigned int position, FMOD_TIMEUNIT unit);

public:
    CSoundRender_TargetA();
    virtual ~CSoundRender_TargetA();
    virtual BOOL _initialize();
    virtual void _destroy();
    virtual void _restart();
    virtual void start(CSoundRender_Emitter* E);
    virtual void render();
    virtual void rewind();
    virtual void stop();
    virtual void update();
    void source_changed();
    virtual void fill_parameters(CSoundRender_Core* core);
};
