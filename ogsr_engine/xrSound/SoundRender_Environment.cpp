#include "stdafx.h"

#include "soundrender.h"
#include "soundrender_environment.h"
namespace
{
constexpr u32 kGenericEnvironment = 0;
constexpr float kMinRoom = -10000.f, kMaxRoom = 0.f, kDefaultRoom = -1000.f;
constexpr float kMinRoomHF = -10000.f, kMaxRoomHF = 0.f, kDefaultRoomHF = -100.f;
constexpr float kMinRolloff = 0.f, kMaxRolloff = 10.f, kDefaultRolloff = 0.f;
constexpr float kMinDecay = .1f, kMaxDecay = 20.f, kDefaultDecay = 1.49f;
constexpr float kMinDecayHF = .1f, kMaxDecayHF = 2.f, kDefaultDecayHF = .83f;
constexpr float kMinReflections = -10000.f, kMaxReflections = 1000.f, kDefaultReflections = -2602.f;
constexpr float kMinReflectionsDelay = 0.f, kMaxReflectionsDelay = .3f, kDefaultReflectionsDelay = .007f;
constexpr float kMinReverb = -10000.f, kMaxReverb = 2000.f, kDefaultReverb = 200.f;
constexpr float kMinReverbDelay = 0.f, kMaxReverbDelay = .1f, kDefaultReverbDelay = .011f;
constexpr float kMinSize = 1.f, kMaxSize = 100.f, kDefaultSize = 7.5f;
constexpr float kMinDiffusion = 0.f, kMaxDiffusion = 1.f, kDefaultDiffusion = 1.f;
constexpr float kMinAirAbsorption = -100.f, kMaxAirAbsorption = 0.f, kDefaultAirAbsorption = -5.f;
}

CSoundRender_Environment::CSoundRender_Environment(void)
{
    version = sdef_env_version;
    name = "_engine_default_";
    set_default();
}

CSoundRender_Environment::~CSoundRender_Environment(void) {}

void CSoundRender_Environment::set_default()
{
    Environment = kGenericEnvironment;
    Room = kDefaultRoom;
    RoomHF = kDefaultRoomHF;
    RoomRolloffFactor = kDefaultRolloff;
    DecayTime = kDefaultDecay;
    DecayHFRatio = kDefaultDecayHF;
    Reflections = kDefaultReflections;
    ReflectionsDelay = kDefaultReflectionsDelay;
    Reverb = kDefaultReverb;
    ReverbDelay = kDefaultReverbDelay;
    EnvironmentSize = kDefaultSize;
    EnvironmentDiffusion = kDefaultDiffusion;
    AirAbsorptionHF = kDefaultAirAbsorption;
}

void CSoundRender_Environment::set_identity()
{
    set_default();
    Room = kMinRoom;
    clamp();
}

void CSoundRender_Environment::lerp(CSoundRender_Environment& A, CSoundRender_Environment& B, float f)
{
    float fi = 1.f - f;

    Room = fi * A.Room + f * B.Room;
    RoomHF = fi * A.RoomHF + f * B.RoomHF;
    RoomRolloffFactor = fi * A.RoomRolloffFactor + f * B.RoomRolloffFactor;
    DecayTime = fi * A.DecayTime + f * B.DecayTime;
    DecayHFRatio = fi * A.DecayHFRatio + f * B.DecayHFRatio;
    Reflections = fi * A.Reflections + f * B.Reflections;
    ReflectionsDelay = fi * A.ReflectionsDelay + f * B.ReflectionsDelay;
    Reverb = fi * A.Reverb + f * B.Reverb;
    ReverbDelay = fi * A.ReverbDelay + f * B.ReverbDelay;
    EnvironmentSize = fi * A.EnvironmentSize + f * B.EnvironmentSize;
    EnvironmentDiffusion = fi * A.EnvironmentDiffusion + f * B.EnvironmentDiffusion;
    AirAbsorptionHF = fi * A.AirAbsorptionHF + f * B.AirAbsorptionHF;

    clamp();
}

void CSoundRender_Environment::set_from(CSoundRender_Environment& A)
{
    float fi = 1.f;

    Room = fi * A.Room;
    RoomHF = fi * A.RoomHF;
    RoomRolloffFactor = fi * A.RoomRolloffFactor;
    DecayTime = fi * A.DecayTime;
    DecayHFRatio = fi * A.DecayHFRatio;
    Reflections = fi * A.Reflections;
    ReflectionsDelay = fi * A.ReflectionsDelay;
    Reverb = fi * A.Reverb;
    ReverbDelay = fi * A.ReverbDelay;
    EnvironmentSize = fi * A.EnvironmentSize;
    EnvironmentDiffusion = fi * A.EnvironmentDiffusion;
    AirAbsorptionHF = fi * A.AirAbsorptionHF;

    clamp();
}

void CSoundRender_Environment::clamp()
{
    ::clamp(Room, kMinRoom, kMaxRoom);
    ::clamp(RoomHF, kMinRoomHF, kMaxRoomHF);
    ::clamp(RoomRolloffFactor, kMinRolloff, kMaxRolloff);
    ::clamp(DecayTime, kMinDecay, kMaxDecay);
    ::clamp(DecayHFRatio, kMinDecayHF, kMaxDecayHF);
    ::clamp(Reflections, kMinReflections, kMaxReflections);
    ::clamp(ReflectionsDelay, kMinReflectionsDelay, kMaxReflectionsDelay);
    ::clamp(Reverb, kMinReverb, kMaxReverb);
    ::clamp(ReverbDelay, kMinReverbDelay, kMaxReverbDelay);
    ::clamp(EnvironmentSize, kMinSize, kMaxSize);
    ::clamp(EnvironmentDiffusion, kMinDiffusion, kMaxDiffusion);
    ::clamp(AirAbsorptionHF, kMinAirAbsorption, kMaxAirAbsorption);
}

bool CSoundRender_Environment::load(IReader* fs)
{
    version = fs->r_u32();

    if (version >= 0x0003)
    {
        fs->r_stringZ(name);

        Room = fs->r_float();
        RoomHF = fs->r_float();
        RoomRolloffFactor = fs->r_float();
        DecayTime = fs->r_float();
        DecayHFRatio = fs->r_float();
        Reflections = fs->r_float();
        ReflectionsDelay = fs->r_float();
        Reverb = fs->r_float();
        ReverbDelay = fs->r_float();
        EnvironmentSize = fs->r_float();
        EnvironmentDiffusion = fs->r_float();
        AirAbsorptionHF = fs->r_float();

        if (version > 0x0003)
            Environment = fs->r_u32();

        return true;
    }

    return false;
}

void CSoundRender_Environment::save(IWriter* fs) const
{
    fs->w_u32(sdef_env_version);
    fs->w_stringZ(name);

    fs->w_float(Room);
    fs->w_float(RoomHF);
    fs->w_float(RoomRolloffFactor);
    fs->w_float(DecayTime);
    fs->w_float(DecayHFRatio);
    fs->w_float(Reflections);
    fs->w_float(ReflectionsDelay);
    fs->w_float(Reverb);
    fs->w_float(ReverbDelay);
    fs->w_float(EnvironmentSize);
    fs->w_float(EnvironmentDiffusion);
    fs->w_float(AirAbsorptionHF);

    fs->w_u32(Environment);
}

void CSoundRender_Environment::loadIni(CInifile* ini, LPCSTR name)
{
    this->name = name;

    Room = ini->r_float(name, "root");
    RoomHF = ini->r_float(name, "room_hf");
    RoomRolloffFactor = ini->r_float(name, "room_rolloff_factor");
    DecayTime = ini->r_float(name, "decay_time");
    DecayHFRatio = ini->r_float(name, "decay_hf_ratio");
    Reflections = ini->r_float(name, "reflections");
    ReflectionsDelay = ini->r_float(name, "reflections_delay");
    Reverb = ini->r_float(name, "reverb");
    ReverbDelay = ini->r_float(name, "reverb_delay");
    EnvironmentSize = ini->r_float(name, "environment_size");
    EnvironmentDiffusion = ini->r_float(name, "environment_diffusion");
    AirAbsorptionHF = ini->r_float(name, "air_absorption_hf");

    Environment = ini->r_u32(name, "environment");
}

void CSoundRender_Environment::saveIni(CInifile* ini, LPCSTR name) const
{
    ini->w_float(name, "root", Room);
    ini->w_float(name, "room_hf", RoomHF);
    ini->w_float(name, "room_rolloff_factor", RoomRolloffFactor);
    ini->w_float(name, "decay_time", DecayTime);
    ini->w_float(name, "decay_hf_ratio", DecayHFRatio);
    ini->w_float(name, "reflections", Reflections);
    ini->w_float(name, "reflections_delay", ReflectionsDelay);
    ini->w_float(name, "reverb", Reverb);
    ini->w_float(name, "reverb_delay", ReverbDelay);
    ini->w_float(name, "environment_size", EnvironmentSize);
    ini->w_float(name, "environment_diffusion", EnvironmentDiffusion);
    ini->w_float(name, "air_absorption_hf", AirAbsorptionHF);

    ini->w_u32(name, "environment", Environment);
}

//////////////////////////////////////////////////////////////////////////
void SoundEnvironment_LIB::Load(LPCSTR f_name)
{
    // R_ASSERT(library.empty());

    IReader* F = FS.r_open(f_name);
    IReader* C;

    for (u32 chunk = 0; 0 != (C = F->open_chunk(chunk)); chunk++)
    {
        CSoundRender_Environment* E = xr_new<CSoundRender_Environment>();
        if (E->load(C))
            library.push_back(E);
        else
            xr_delete(E);

        C->close();
    }

    FS.r_close(F);
}

bool SoundEnvironment_LIB::Save(LPCSTR f_name) const
{
    IWriter* F = FS.w_open(f_name);
    if (F)
    {
        for (u32 chunk = 0; chunk < library.size(); chunk++)
        {
            F->open_chunk(chunk);
            library[chunk]->save(F);
            F->close_chunk();
        }
        FS.w_close(F);
        return true;
    }
    return false;
}

void SoundEnvironment_LIB::LoadIni(CInifile* ini)
{
    for (const auto& it : ini->sections())
    {
        CSoundRender_Environment* E = xr_new<CSoundRender_Environment>();

        auto& name = it.first;

        E->loadIni(ini, name.c_str());

        library.push_back(E);
    }
}

bool SoundEnvironment_LIB::SaveIni(CInifile* ini) const
{
    for (u32 chunk = 0; chunk < library.size(); chunk++)
    {
        library[chunk]->saveIni(ini, library[chunk]->name.c_str());
    }
    return true;
}

void SoundEnvironment_LIB::Unload()
{
    for (u32 chunk = 0; chunk < library.size(); chunk++)
        xr_delete(library[chunk]);
    library.clear();
}

int SoundEnvironment_LIB::GetID(LPCSTR name)
{
    for (SE_IT it = library.begin(); it != library.end(); ++it)
        if (0 == _stricmp(name, *(*it)->name))
            return int(it - library.begin());

    return -1;
}

CSoundRender_Environment* SoundEnvironment_LIB::Get(int id) const { return library[id]; }

SoundEnvironment_LIB::SE_VEC& SoundEnvironment_LIB::Library() { return library; }
