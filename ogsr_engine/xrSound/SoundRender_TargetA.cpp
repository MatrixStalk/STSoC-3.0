#include "stdafx.h"

#include "soundrender_TargetA.h"
#include "soundrender_emitter.h"
#include "soundrender_source.h"
#include "../COMMON_AI/ai_sounds.h"

#include <fmod_errors.h>

CSoundRender_TargetA::CSoundRender_TargetA() = default;
CSoundRender_TargetA::~CSoundRender_TargetA() = default;

void CSoundRender_TargetA::reset_equalizer()
{
    for (auto& filter : eq_filters)
    {
        filter.z1[0] = filter.z1[1] = 0.f;
        filter.z2[0] = filter.z2[1] = 0.f;
    }
    eq_revision = u32(-1);
    eq_sample_rate = 0;
}

void CSoundRender_TargetA::update_equalizer(const SSoundEqualizer& settings, u32 sample_rate)
{
    constexpr float pi = 3.14159265358979323846f;
    for (u32 i = 0; i < 4; ++i)
    {
        Biquad& f = eq_filters[i];
        const auto& band = settings.bands[i];
        const float frequency = clampr(band.frequency, 20.f, sample_rate * 0.475f);
        const float q = _max(0.1f, band.q);
        const float A = powf(10.f, clampr(band.gain_db, -24.f, 24.f) / 40.f);
        const float w0 = 2.f * pi * frequency / sample_rate;
        const float c = cosf(w0);
        const float s = sinf(w0);
        const float alpha = s / (2.f * q);
        float a0 = 1.f;
        if (i == 0)
        {
            const float root = 2.f * sqrtf(A) * alpha;
            f.b0 = A * ((A + 1.f) - (A - 1.f) * c + root);
            f.b1 = 2.f * A * ((A - 1.f) - (A + 1.f) * c);
            f.b2 = A * ((A + 1.f) - (A - 1.f) * c - root);
            a0 = (A + 1.f) + (A - 1.f) * c + root;
            f.a1 = -2.f * ((A - 1.f) + (A + 1.f) * c);
            f.a2 = (A + 1.f) + (A - 1.f) * c - root;
        }
        else if (i == 3)
        {
            const float root = 2.f * sqrtf(A) * alpha;
            f.b0 = A * ((A + 1.f) + (A - 1.f) * c + root);
            f.b1 = -2.f * A * ((A - 1.f) + (A + 1.f) * c);
            f.b2 = A * ((A + 1.f) + (A - 1.f) * c - root);
            a0 = (A + 1.f) - (A - 1.f) * c + root;
            f.a1 = 2.f * ((A - 1.f) - (A + 1.f) * c);
            f.a2 = (A + 1.f) - (A - 1.f) * c - root;
        }
        else
        {
            f.b0 = 1.f + alpha * A;
            f.b1 = -2.f * c;
            f.b2 = 1.f - alpha * A;
            a0 = 1.f + alpha / A;
            f.a1 = -2.f * c;
            f.a2 = 1.f - alpha / A;
        }
        f.b0 /= a0;
        f.b1 /= a0;
        f.b2 /= a0;
        f.a1 /= a0;
        f.a2 /= a0;
    }
    eq_revision = settings.revision;
    eq_sample_rate = sample_rate;
}

void CSoundRender_TargetA::apply_equalizer(void* data, u32 bytes, const WAVEFORMATEX& format)
{
    const SSoundEqualizer& settings = SoundRender->Equalizer();
    if (!settings.enabled || !m_pEmitter)
        return;
    if (settings.revision != eq_revision || format.nSamplesPerSec != eq_sample_rate)
        update_equalizer(settings, format.nSamplesPerSec);

    const u32 channels = format.nChannels == 1 ? 1u : 2u;
    const float preamp = powf(10.f, clampr(settings.preamp_db, -24.f, 12.f) / 20.f);
    auto process = [&](float sample, u32 channel) {
        float value = sample * preamp;
        for (auto& filter : eq_filters)
        {
            const float out = filter.b0 * value + filter.z1[channel];
            filter.z1[channel] = filter.b1 * value - filter.a1 * out + filter.z2[channel];
            filter.z2[channel] = filter.b2 * value - filter.a2 * out;
            value = out;
        }
        // FMOD mixes in float and provides its own output headroom. Clamping
        // each source here would flatten transients before spatialization.
        return value;
    };

    if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
    {
        float* samples = static_cast<float*>(data);
        const u32 count = bytes / sizeof(float);
        for (u32 i = 0; i < count; ++i)
            samples[i] = process(samples[i], i % channels);
    }
    else
    {
        s16* samples = static_cast<s16*>(data);
        const u32 count = bytes / sizeof(s16);
        for (u32 i = 0; i < count; ++i)
            samples[i] = static_cast<s16>(clampr(process(samples[i] / 32768.f, i % channels), -1.f, 1.f) * 32767.f);
    }
}

FMOD_RESULT F_CALL CSoundRender_TargetA::pcm_read(FMOD_SOUND* sound, void* data, unsigned int bytes)
{
    void* user = nullptr;
    reinterpret_cast<FMOD::Sound*>(sound)->getUserData(&user);
    auto* target = static_cast<CSoundRender_TargetA*>(user);
    if (!target)
    {
        ZeroMemory(data, bytes);
        return FMOD_OK;
    }
    target->read_pcm_ring(data, bytes);
    return FMOD_OK;
}

FMOD_RESULT F_CALL CSoundRender_TargetA::pcm_seek(FMOD_SOUND*, int, unsigned int, FMOD_TIMEUNIT) { return FMOD_OK; }

void CSoundRender_TargetA::reset_pcm_ring()
{
    std::scoped_lock lock(pcm_mutex);
    pcm_read_offset = 0;
    pcm_write_offset = 0;
    pcm_available = 0;
}

void CSoundRender_TargetA::read_pcm_ring(void* data, u32 bytes)
{
    u8* destination = static_cast<u8*>(data);
    std::scoped_lock lock(pcm_mutex);
    const u32 amount = _min(bytes, pcm_available);
    if (amount && !pcm_ring.empty())
    {
        const u32 first = _min(amount, u32(pcm_ring.size()) - pcm_read_offset);
        CopyMemory(destination, pcm_ring.data() + pcm_read_offset, first);
        if (amount > first)
            CopyMemory(destination + first, pcm_ring.data(), amount - first);
        pcm_read_offset = (pcm_read_offset + amount) % u32(pcm_ring.size());
        pcm_available -= amount;
    }
    if (amount < bytes)
        ZeroMemory(destination + amount, bytes - amount);
}

void CSoundRender_TargetA::refill_pcm_ring()
{
    if (!m_pEmitter || pcm_ring.empty())
        return;

    u32 free_bytes = 0;
    {
        std::scoped_lock lock(pcm_mutex);
        free_bytes = u32(pcm_ring.size()) - pcm_available;
    }
    const u32 block_align = _max(1u, u32(m_pEmitter->source()->m_wformat.nBlockAlign));
    free_bytes -= free_bytes % block_align;
    if (!free_bytes)
        return;

    pcm_staging.resize(free_bytes);
    m_pEmitter->fill_block(pcm_staging.data(), free_bytes);
    apply_equalizer(pcm_staging.data(), free_bytes, m_pEmitter->source()->m_wformat);

    std::scoped_lock lock(pcm_mutex);
    const u32 writable = _min(free_bytes, u32(pcm_ring.size()) - pcm_available);
    const u32 first = _min(writable, u32(pcm_ring.size()) - pcm_write_offset);
    CopyMemory(pcm_ring.data() + pcm_write_offset, pcm_staging.data(), first);
    if (writable > first)
        CopyMemory(pcm_ring.data(), pcm_staging.data() + first, writable - first);
    pcm_write_offset = (pcm_write_offset + writable) % u32(pcm_ring.size());
    pcm_available += writable;
}

bool CSoundRender_TargetA::create_stream()
{
    if (!m_pEmitter)
        return false;
    auto* core = static_cast<CSoundRender_CoreA*>(SoundRender);
    const WAVEFORMATEX& format = m_pEmitter->source()->m_wformat;
    FMOD_CREATESOUNDEXINFO info{};
    info.cbsize = sizeof(info);
    info.length = _max(1u, m_pEmitter->get_bytes_total());
    info.numchannels = format.nChannels;
    info.defaultfrequency = format.nSamplesPerSec;
    info.format = format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT ? FMOD_SOUND_FORMAT_PCMFLOAT : FMOD_SOUND_FORMAT_PCM16;
    info.decodebuffersize = _max(256u, u32(format.nSamplesPerSec / 20));
    info.pcmreadcallback = &CSoundRender_TargetA::pcm_read;
    info.pcmsetposcallback = &CSoundRender_TargetA::pcm_seek;
    info.userdata = this;
    const FMOD_MODE mode = FMOD_OPENUSER | FMOD_CREATESTREAM | FMOD_LOOP_NORMAL | FMOD_2D;
    const FMOD_RESULT result = core->FmodSystem()->createStream(nullptr, mode, &info, &fmod_sound);
    if (result != FMOD_OK)
    {
        Msg("! FMOD: create stream failed for %s: %s", m_pEmitter->source()->file_name(), FMOD_ErrorString(result));
        return false;
    }
    return true;
}

void CSoundRender_TargetA::attach_steam_audio()
{
    if (!fmod_channel || !m_pEmitter || m_pEmitter->b2D)
        return;
    auto* core = static_cast<CSoundRender_CoreA*>(SoundRender);
    if (!core->SteamAudioSpatializer())
        return;
    FMOD_RESULT result = core->FmodSystem()->createDSPByPlugin(core->SteamAudioSpatializer(), &steam_audio_dsp);
    if (result != FMOD_OK)
    {
        Msg("! Steam Audio: cannot create spatializer DSP: %s", FMOD_ErrorString(result));
        return;
    }

    result = fmod_channel->addDSP(FMOD_CHANNELCONTROL_DSP_HEAD, steam_audio_dsp);
    if (result != FMOD_OK)
    {
        Msg("! Steam Audio: cannot attach spatializer DSP: %s", FMOD_ErrorString(result));
        steam_audio_dsp->release();
        steam_audio_dsp = nullptr;
        return;
    }

    // A low-level FMOD channel can still be mono at DSP_HEAD. Force the
    // spatializer to render to the final output layout (normally stereo),
    // otherwise Steam Audio cannot initialize its binaural/panning path and
    // returns a silent buffer for every 3D source.
    steam_audio_dsp->setParameterInt(IPL_SPATIALIZE_OUTPUT_FORMAT, 1);
    steam_audio_dsp->setParameterFloat(IPL_SPATIALIZE_DIRECT_MIXLEVEL, 1.f);

    // Curve-driven attenuation preserves the min/max ranges authored for
    // X-Ray sounds. Physics-based inverse falloff made distant shots and NPCs
    // much quieter than the engine's original attenuation model.
    steam_audio_dsp->setParameterInt(IPL_SPATIALIZE_APPLY_DISTANCEATTENUATION, 2);
    // Feed Steam Audio gently shaped three-band values instead of its full
    // physical air loss and a broadband volume-only occlusion. This keeps
    // distant sounds intelligible and makes cover sound naturally muffled.
    steam_audio_dsp->setParameterInt(IPL_SPATIALIZE_APPLY_AIRABSORPTION, core->AirAbsorptionEnabled() ? 2 : 0);
    steam_audio_dsp->setParameterInt(IPL_SPATIALIZE_APPLY_OCCLUSION, core->OcclusionEnabled() ? 2 : 0);
    steam_audio_dsp->setParameterInt(IPL_SPATIALIZE_APPLY_TRANSMISSION, core->OcclusionEnabled() ? 2 : 0);
    steam_audio_dsp->setParameterInt(IPL_SPATIALIZE_TRANSMISSION_TYPE, 1);
    steam_audio_dsp->setParameterFloat(IPL_SPATIALIZE_AIRABSORPTION_LOW, 1.f);
    steam_audio_dsp->setParameterFloat(IPL_SPATIALIZE_AIRABSORPTION_MID, 1.f);
    steam_audio_dsp->setParameterFloat(IPL_SPATIALIZE_AIRABSORPTION_HIGH, 1.f);
    steam_audio_dsp->setParameterFloat(IPL_SPATIALIZE_TRANSMISSION_LOW, 1.f);
    steam_audio_dsp->setParameterFloat(IPL_SPATIALIZE_TRANSMISSION_MID, 1.f);
    steam_audio_dsp->setParameterFloat(IPL_SPATIALIZE_TRANSMISSION_HIGH, 1.f);
    steam_audio_dsp->setParameterInt(IPL_SPATIALIZE_HRTF_INTERPOLATION, 1);
    steam_audio_dsp->setParameterBool(IPL_SPATIALIZE_DIRECT_BINAURAL, core->HrtfEnabled());
    steam_audio_dsp->setParameterInt(IPL_SPATIALIZE_DISTANCEATTENUATION_ROLLOFFTYPE, 2);
    steam_audio_dsp->setParameterFloat(IPL_SPATIALIZE_DISTANCEATTENUATION_MINDISTANCE, m_pEmitter->p_source.min_distance);
    steam_audio_dsp->setParameterFloat(IPL_SPATIALIZE_DISTANCEATTENUATION_MAXDISTANCE, m_pEmitter->p_source.max_distance);
}

void CSoundRender_TargetA::release_stream()
{
    if (fmod_channel)
    {
        fmod_channel->stop();
        fmod_channel = nullptr;
    }
    if (steam_audio_dsp)
    {
        steam_audio_dsp->release();
        steam_audio_dsp = nullptr;
    }
    if (fmod_sound)
    {
        fmod_sound->release();
        fmod_sound = nullptr;
    }
    {
        std::scoped_lock lock(pcm_mutex);
        pcm_read_offset = pcm_write_offset = pcm_available = 0;
        pcm_ring.clear();
    }
    pcm_staging.clear();
}

BOOL CSoundRender_TargetA::_initialize() { return inherited::_initialize(); }
void CSoundRender_TargetA::_destroy() { release_stream(); }
void CSoundRender_TargetA::_restart() { release_stream(); }

void CSoundRender_TargetA::start(CSoundRender_Emitter* emitter)
{
    inherited::start(emitter);
    reset_equalizer();
    cache_gain = 0.f;
    cache_pitch = 1.f;
    const WAVEFORMATEX& format = emitter->source()->m_wformat;
    u32 ring_size = _max(u32(format.nAvgBytesPerSec / 2), u32(format.nBlockAlign * 2048));
    ring_size -= ring_size % _max(1u, u32(format.nBlockAlign));
    pcm_ring.resize(ring_size);
    reset_pcm_ring();
    refill_pcm_ring();
    create_stream();
}

void CSoundRender_TargetA::render()
{
    if (!fmod_sound)
        return;
    auto* core = static_cast<CSoundRender_CoreA*>(SoundRender);
    if (core->FmodSystem()->playSound(fmod_sound, nullptr, true, &fmod_channel) != FMOD_OK)
        return;
    attach_steam_audio();
    fill_parameters(core);
    fmod_channel->setPaused(false);
    inherited::render();
}

void CSoundRender_TargetA::stop()
{
    release_stream();
    inherited::stop();
}

void CSoundRender_TargetA::rewind()
{
    inherited::rewind();
    reset_equalizer();
    reset_pcm_ring();
    refill_pcm_ring();
    if (fmod_channel)
        fmod_channel->setPosition(0, FMOD_TIMEUNIT_PCMBYTES);
}

void CSoundRender_TargetA::update()
{
    inherited::update();
    refill_pcm_ring();
    if (!fmod_channel)
        return;
    bool playing = false;
    if (fmod_channel->isPlaying(&playing) != FMOD_OK || !playing)
    {
        fmod_channel = nullptr;
        rendering = FALSE;
    }
}

void CSoundRender_TargetA::fill_parameters(CSoundRender_Core* base_core)
{
    inherited::fill_parameters(base_core);
    if (!m_pEmitter || !fmod_channel)
        return;

    auto* core = static_cast<CSoundRender_CoreA*>(base_core);
    float listener_gain = 1.f;
    if (m_pEmitter->owner_data && m_pEmitter->owner_data->s_type == st_Effect && m_pEmitter->owner_data->g_type != sg_Interface)
    {
        const int type = m_pEmitter->owner_data->g_type;
        if ((type & SOUND_TYPE_OBJECT_EXPLODING) == SOUND_TYPE_OBJECT_EXPLODING)
            listener_gain = core->ListenerExplosionGain();
        else if ((type & SOUND_TYPE_WEAPON_SHOOTING) == SOUND_TYPE_WEAPON_SHOOTING)
            listener_gain = core->ListenerShotGain();
        else if (m_pEmitter->b2D)
            listener_gain = core->ListenerHudGain();
        else
            listener_gain = core->ListenerWorldGain();
    }
    float gain = clampr(m_pEmitter->smooth_volume * listener_gain, 0.f, 2.f);
    if (!fsimilar(gain, cache_gain, 0.005f))
    {
        cache_gain = gain;
        fmod_channel->setVolume(gain);
    }
    float pitch = clampr(m_pEmitter->p_source.freq * psSoundTimeFactor, EPS_L, 100.f);
    if (!fsimilar(pitch, cache_pitch))
    {
        cache_pitch = pitch;
        fmod_channel->setPitch(pitch);
    }
    if (!steam_audio_dsp)
        return;

    FMOD_DSP_PARAMETER_3DATTRIBUTES attributes{};
    attributes.absolute.position = {m_pEmitter->p_source.position.x, m_pEmitter->p_source.position.y, -m_pEmitter->p_source.position.z};
    attributes.absolute.forward = {0.f, 0.f, 1.f};
    attributes.absolute.up = {0.f, 1.f, 0.f};

    // Steam Audio consumes the relative member for binaural direction. Passing
    // world coordinates here makes panning rotate and drift with the listener.
    // Transform the emitter into the listener basis explicitly because these
    // streams are intentionally FMOD_2D and use a manually attached spatializer.
    Fvector listener_right;
    listener_right.crossproduct(core->ListenerUp(), core->ListenerForward()).normalize_safe();
    Fvector listener_delta;
    listener_delta.sub(m_pEmitter->p_source.position, core->listener_position());
    attributes.relative.position = {
        listener_delta.dotproduct(listener_right), listener_delta.dotproduct(core->ListenerUp()), listener_delta.dotproduct(core->ListenerForward())};
    attributes.relative.forward = {0.f, 0.f, 1.f};
    attributes.relative.up = {0.f, 1.f, 0.f};
    steam_audio_dsp->setParameterData(IPL_SPATIALIZE_SOURCE_POSITION, &attributes, sizeof(attributes));

    const float min_distance = m_pEmitter->p_source.min_distance;
    const float max_distance = _max(min_distance + EPS_L, m_pEmitter->p_source.max_distance);
    const float distance = core->listener_position().distance_to(m_pEmitter->p_source.position);
    const float normalized_distance = clampr((distance - min_distance) / (max_distance - min_distance), 0.f, 1.f);
    const float air = core->AirAbsorptionStrength() * normalized_distance;
    steam_audio_dsp->setParameterFloat(IPL_SPATIALIZE_AIRABSORPTION_LOW, 1.f - air * 0.12f);
    steam_audio_dsp->setParameterFloat(IPL_SPATIALIZE_AIRABSORPTION_MID, 1.f - air * 0.40f);
    steam_audio_dsp->setParameterFloat(IPL_SPATIALIZE_AIRABSORPTION_HIGH, 1.f - air);

    const float visibility = clampr(m_pEmitter->occluder_volume, 0.f, 1.f);
    const float low_visibility = sqrtf(visibility);
    const float high_visibility = visibility * sqrtf(visibility);
    const float transmission_low = core->OcclusionLowFloor() + (1.f - core->OcclusionLowFloor()) * low_visibility;
    const float transmission_mid = core->OcclusionMidFloor() + (1.f - core->OcclusionMidFloor()) * visibility;
    const float transmission_high = core->OcclusionHighFloor() + (1.f - core->OcclusionHighFloor()) * high_visibility;
    steam_audio_dsp->setParameterFloat(IPL_SPATIALIZE_OCCLUSION, visibility);
    steam_audio_dsp->setParameterFloat(IPL_SPATIALIZE_TRANSMISSION_LOW, transmission_low);
    steam_audio_dsp->setParameterFloat(IPL_SPATIALIZE_TRANSMISSION_MID, transmission_mid);
    steam_audio_dsp->setParameterFloat(IPL_SPATIALIZE_TRANSMISSION_HIGH, transmission_high);
    steam_audio_dsp->setParameterFloat(IPL_SPATIALIZE_DISTANCEATTENUATION_MINDISTANCE, m_pEmitter->p_source.min_distance);
    steam_audio_dsp->setParameterFloat(IPL_SPATIALIZE_DISTANCEATTENUATION_MAXDISTANCE, m_pEmitter->p_source.max_distance);
}

void CSoundRender_TargetA::source_changed()
{
    // The PCM ring is produced on the engine sound-update thread. Source
    // loading normalizes tails to the active PCM format, so only the
    // compressed stream reader must be rebound here.
    dettach();
    attach();
}
