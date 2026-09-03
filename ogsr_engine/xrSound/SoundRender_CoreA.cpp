#include "stdafx.h"

#include "soundrender_coreA.h"
#include "soundrender_targetA.h"

#include <fmod_errors.h>

namespace
{
bool fmod_ok(FMOD_RESULT result, LPCSTR operation)
{
    if (result == FMOD_OK)
        return true;
    Msg("! FMOD: %s failed: %s (%d)", operation, FMOD_ErrorString(result), result);
    return false;
}

void clear_device_tokens()
{
    if (!snd_devices_token)
        return;
    for (xr_token* token = snd_devices_token; token->name; ++token)
        xr_free(const_cast<char*>(token->name));
    xr_free(snd_devices_token);
    snd_devices_token = nullptr;
}

FMOD_VECTOR fmod_vector(const Fvector& value)
{
    return {value.x, value.y, -value.z};
}
} // namespace

CSoundRender_CoreA::CSoundRender_CoreA() = default;
CSoundRender_CoreA::~CSoundRender_CoreA() = default;

void CSoundRender_CoreA::load_settings()
{
    string_path path;
    FS.update_path(path, "$game_config$", "sound_fmod.ltx");
    if (!FS.exist(path))
        return;

    CInifile ini(path, TRUE, TRUE, FALSE);
    if (!ini.section_exist("fmod"))
        return;

    sample_rate = ini.line_exist("fmod", "sample_rate") ? ini.r_s32("fmod", "sample_rate") : sample_rate;
    dsp_buffer_length = ini.line_exist("fmod", "dsp_buffer_length") ? ini.r_s32("fmod", "dsp_buffer_length") : dsp_buffer_length;
    dsp_buffer_count = ini.line_exist("fmod", "dsp_buffer_count") ? ini.r_s32("fmod", "dsp_buffer_count") : dsp_buffer_count;
    hrtf_enabled = ini.line_exist("fmod", "hrtf") ? ini.r_bool("fmod", "hrtf") : hrtf_enabled;
    air_absorption_enabled = ini.line_exist("fmod", "air_absorption") ? ini.r_bool("fmod", "air_absorption") : air_absorption_enabled;
    occlusion_enabled = ini.line_exist("fmod", "occlusion") ? ini.r_bool("fmod", "occlusion") : occlusion_enabled;

    sample_rate = clampr(sample_rate, 22050, 192000);
    dsp_buffer_length = clampr(dsp_buffer_length, 256, 4096);
    dsp_buffer_count = clampr(dsp_buffer_count, 2, 8);
}

bool CSoundRender_CoreA::enumerate_devices()
{
    if (!fmod_system && !fmod_ok(FMOD::System_Create(&fmod_system), "System_Create"))
        return false;

    int count = 0;
    if (!fmod_ok(fmod_system->getNumDrivers(&count), "getNumDrivers") || count <= 0)
        return false;

    clear_device_tokens();
    snd_devices_token = xr_alloc<xr_token>(count + 1);
    for (int i = 0; i < count; ++i)
    {
        char name[256]{};
        fmod_system->getDriverInfo(i, name, sizeof(name), nullptr, nullptr, nullptr, nullptr);
        snd_devices_token[i].id = i;
        snd_devices_token[i].name = xr_strdup(name[0] ? name : "FMOD output");
        Msg("- FMOD output %d: %s", i, snd_devices_token[i].name);
    }
    snd_devices_token[count].id = -1;
    snd_devices_token[count].name = nullptr;
    if (snd_device_id == u32(-1) || snd_device_id >= u32(count) || psSoundFlags.test(ss_UseDefaultDevice))
        snd_device_id = 0;
    return true;
}

bool CSoundRender_CoreA::initialize_steam_audio()
{
    IPLContextSettings context_settings{};
    context_settings.version = STEAMAUDIO_VERSION;
    context_settings.simdLevel = IPL_SIMDLEVEL_AVX2;
    if (iplContextCreate(&context_settings, &steam_audio_context) != IPL_STATUS_SUCCESS)
    {
        Msg("! Steam Audio: cannot create context");
        return false;
    }

    IPLAudioSettings audio_settings{};
    audio_settings.samplingRate = sample_rate;
    audio_settings.frameSize = dsp_buffer_length;
    IPLHRTFSettings hrtf_settings{};
    hrtf_settings.type = IPL_HRTFTYPE_DEFAULT;
    hrtf_settings.volume = 1.f;
    hrtf_settings.normType = IPL_HRTFNORMTYPE_RMS;
    if (iplHRTFCreate(steam_audio_context, &audio_settings, &hrtf_settings, &steam_audio_hrtf) != IPL_STATUS_SUCCESS)
    {
        Msg("! Steam Audio: cannot create HRTF");
        return false;
    }

    iplFMODInitialize(steam_audio_context);
    iplFMODSetHRTF(steam_audio_hrtf);
    iplFMODSetHRTFDisabled(!hrtf_enabled);

    IPLSimulationSettings simulation{};
    simulation.flags = IPL_SIMULATIONFLAGS_DIRECT;
    simulation.sceneType = IPL_SCENETYPE_DEFAULT;
    simulation.reflectionType = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
    simulation.maxNumOcclusionSamples = 32;
    simulation.samplingRate = sample_rate;
    simulation.frameSize = dsp_buffer_length;
    iplFMODSetSimulationSettings(simulation);

    unsigned int major = 0, minor = 0, patch = 0;
    iplFMODGetVersion(&major, &minor, &patch);
    Msg("- Steam Audio FMOD integration: %u.%u.%u, HRTF: %s", major, minor, patch, hrtf_enabled ? "on" : "off");
    return true;
}

bool CSoundRender_CoreA::initialize_fmod()
{
    if (!fmod_system && !enumerate_devices())
        return false;

    fmod_system->setDriver(static_cast<int>(snd_device_id));
    fmod_system->setSoftwareFormat(sample_rate, FMOD_SPEAKERMODE_DEFAULT, 0);
    fmod_system->setDSPBufferSize(dsp_buffer_length, dsp_buffer_count);
    fmod_system->set3DSettings(1.f, 1.f, psSoundRolloff);

    if (!fmod_ok(fmod_system->loadPlugin("phonon_fmod.dll", &steam_audio_plugin), "loadPlugin(phonon_fmod.dll)"))
        return false;

    int nested_count = 0;
    fmod_system->getNumNestedPlugins(steam_audio_plugin, &nested_count);
    for (int i = 0; i < nested_count; ++i)
    {
        unsigned int nested = 0;
        char name[128]{};
        FMOD_PLUGINTYPE type{};
        fmod_system->getNestedPlugin(steam_audio_plugin, i, &nested);
        fmod_system->getPluginInfo(nested, &type, name, sizeof(name), nullptr);
        if (type == FMOD_PLUGINTYPE_DSP && strstr(name, "Steam Audio Spatializer"))
            steam_audio_spatializer = nested;
    }
    if (!steam_audio_spatializer)
    {
        Msg("! Steam Audio: spatializer DSP was not found in phonon_fmod.dll");
        return false;
    }

    if (!fmod_ok(fmod_system->init(psSoundTargets, FMOD_INIT_3D_RIGHTHANDED, nullptr), "System::init"))
        return false;
    if (!initialize_steam_audio())
        return false;

    unsigned int version = 0;
    fmod_system->getVersion(&version);
    Msg("- FMOD Core initialized: %x, %d Hz, DSP %dx%d", version, sample_rate, dsp_buffer_length, dsp_buffer_count);
    return true;
}

void CSoundRender_CoreA::_initialize(int stage)
{
    if (stage == 0)
    {
        load_settings();
        bPresent = enumerate_devices();
        return;
    }
    if (!bPresent || !initialize_fmod())
    {
        bPresent = FALSE;
        release_backend();
        return;
    }

    supports_float_pcm = snd_enable_float_pcm;
    inherited::_initialize(stage);
    for (u32 i = 0; i < u32(psSoundTargets); ++i)
    {
        CSoundRender_Target* target = xr_new<CSoundRender_TargetA>();
        if (!target->_initialize())
        {
            target->_destroy();
            xr_delete(target);
            Msg("! FMOD: maximum targets reached at %u", i);
            break;
        }
        s_targets.push_back(target);
    }
    bReady = TRUE;
}

void CSoundRender_CoreA::set_master_volume(float volume)
{
    if (!fmod_system)
        return;
    FMOD::ChannelGroup* master = nullptr;
    if (fmod_system->getMasterChannelGroup(&master) == FMOD_OK && master)
        master->setVolume(clampr(volume, 0.f, 1.f));
}

void CSoundRender_CoreA::release_backend()
{
    if (steam_audio_context)
        iplFMODTerminate();
    if (steam_audio_hrtf)
        iplHRTFRelease(&steam_audio_hrtf);
    if (steam_audio_context)
        iplContextRelease(&steam_audio_context);
    if (fmod_system)
    {
        fmod_system->close();
        fmod_system->release();
        fmod_system = nullptr;
    }
    steam_audio_plugin = 0;
    steam_audio_spatializer = 0;
}

void CSoundRender_CoreA::_clear()
{
    bReady = FALSE;
    inherited::_clear();
    for (CSoundRender_Target*& target : s_targets)
    {
        target->_destroy();
        xr_delete(target);
    }
    s_targets.clear();
    release_backend();
    clear_device_tokens();
    bPresent = FALSE;
}

void CSoundRender_CoreA::_restart()
{
    if (!bReady || !fmod_system)
        return;
    int count = 0;
    fmod_system->getNumDrivers(&count);
    if (snd_device_id < u32(count))
    {
        const FMOD_RESULT result = fmod_system->setDriver(static_cast<int>(snd_device_id));
        if (result != FMOD_OK)
            Msg("! FMOD: output change requires a game restart: %s", FMOD_ErrorString(result));
    }
}

void CSoundRender_CoreA::update(const Fvector& P, const Fvector& D, const Fvector& N)
{
    inherited::update(P, D, N);
    if (fmod_system)
        fmod_system->update();
}

void CSoundRender_CoreA::update_listener(const Fvector& P, const Fvector& D, const Fvector& N, float dt)
{
    inherited::update_listener(P, D, N, dt);
    if (!Listener.position.similar(P))
    {
        Listener.position.set(P);
        bListenerMoved = TRUE;
    }
    Listener.orientation[0].set(D);
    Listener.orientation[1].set(N);
    if (!fmod_system)
        return;
    const FMOD_VECTOR position = fmod_vector(P);
    const FMOD_VECTOR velocity{};
    const FMOD_VECTOR forward = fmod_vector(D);
    const FMOD_VECTOR up = fmod_vector(N);
    fmod_system->set3DListenerAttributes(0, &position, &velocity, &forward, &up);
}
