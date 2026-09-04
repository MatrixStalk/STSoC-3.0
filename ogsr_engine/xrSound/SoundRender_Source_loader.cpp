#include "stdafx.h"

#include "soundrender_core.h"
#include "soundrender_source.h"

#define DR_WAV_IMPLEMENTATION
#include "../../3rd_party/Src/dr_libs/dr_wav.h"
#define DR_MP3_IMPLEMENTATION
#include "../../3rd_party/Src/dr_libs/dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#include "../../3rd_party/Src/dr_libs/dr_flac.h"

//	SEEK_SET	0	File beginning
//	SEEK_CUR	1	Current file pointer position
//	SEEK_END	2	End-of-file
int ov_seek_func(void* datasource, s64 offset, int whence)
{
    switch (whence)
    {
    case SEEK_SET: ((IReader*)datasource)->seek((int)offset); break;
    case SEEK_CUR: ((IReader*)datasource)->advance((int)offset); break;
    case SEEK_END: ((IReader*)datasource)->seek((int)offset + ((IReader*)datasource)->length()); break;
    }
    return 0;
}
size_t ov_read_func(void* ptr, size_t size, size_t nmemb, void* datasource)
{
    IReader* F = (IReader*)datasource;
    size_t exist_block = _max(0ul, iFloor(F->elapsed() / (float)size));
    size_t read_block = _min(exist_block, nmemb);
    F->r(ptr, (int)(read_block * size));
    return read_block;
}
int ov_close_func(void* datasource) { return 0; }
long ov_tell_func(void* datasource) { return ((IReader*)datasource)->tell(); }

void CSoundRender_Source::decompress(u32 line, OggVorbis_File* ovf)
{
    // decompression of one cache-line
    u32 line_size = SoundRender->cache.get_linesize();
    u32 byte_offset = line * line_size;
    u32 buf_offs = byte_offset / m_wformat.nBlockAlign;
    u32 left_file = dwBytesTotal - byte_offset;
    u32 left = (u32)_min(left_file, line_size);
    const auto dest = SoundRender->cache.get_dataptr(CAT, line);

    if (!streamed())
    {
        CopyMemory(dest, m_decoded_data.data() + byte_offset, left);
        if (left < line_size)
            Memory.mem_fill(static_cast<u8*>(dest) + left, 0, line_size - left);
        return;
    }

    VERIFY(ovf);

    // seek
    u32 cur_pos = u32(ov_pcm_tell(ovf));
    if (cur_pos != buf_offs)
        ov_pcm_seek(ovf, buf_offs);

     // decompress
    if (m_wformat.wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        i_decompress(ovf, static_cast<float*>(dest), left);
    else
        i_decompress(ovf, static_cast<char*>(dest), left);
}

static CSoundRender_Source::ECodec sound_codec(LPCSTR extension)
{
    if (0 == _stricmp(extension, ".wav"))
        return CSoundRender_Source::ECodec::Wav;
    if (0 == _stricmp(extension, ".mp3"))
        return CSoundRender_Source::ECodec::Mp3;
    if (0 == _stricmp(extension, ".flac"))
        return CSoundRender_Source::ECodec::Flac;
    return CSoundRender_Source::ECodec::Ogg;
}

static void setup_pcm_format(WAVEFORMATEX& format, u32 channels, u32 sample_rate, bool use_float)
{
    ZeroMemory(&format, sizeof(format));
    format.nChannels = u16(channels);
    format.nSamplesPerSec = sample_rate;
    format.wFormatTag = use_float ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
    format.wBitsPerSample = use_float ? 32 : 16;
    format.nBlockAlign = format.wBitsPerSample / 8 * format.nChannels;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
}

static void copy_decoded_pcm(xr_vector<u8>& destination, const void* source, u64 frames, const WAVEFORMATEX& format)
{
    const u64 byte_count = frames * format.nBlockAlign;
    R_ASSERT2(byte_count <= u64(u32(-1)), "Decoded sound is too large");
    destination.resize(u32(byte_count));
    CopyMemory(destination.data(), source, destination.size());
}

static void load_non_ogg(CSoundRender_Source& source, IReader& file, LPCSTR name)
{
    u32 channels = 0;
    u32 sample_rate = 0;
    u64 frames = 0;
    void* decoded = nullptr;
    // Keep the whole processing path in float when the backend supports it.
    // This preserves 24-bit WAV/FLAC detail and avoids requantizing every
    // WAV/MP3/FLAC source to PCM16 before EQ and spatial processing.
    const bool use_float = SoundRender->supports_float_pcm;

    if (source.m_codec == CSoundRender_Source::ECodec::Wav)
    {
        unsigned int wav_channels = 0;
        unsigned int wav_rate = 0;
        drwav_uint64 wav_frames = 0;
        decoded = use_float ? static_cast<void*>(drwav_open_memory_and_read_pcm_frames_f32(file.pointer(), file.length(), &wav_channels, &wav_rate, &wav_frames, nullptr)) :
                              static_cast<void*>(drwav_open_memory_and_read_pcm_frames_s16(file.pointer(), file.length(), &wav_channels, &wav_rate, &wav_frames, nullptr));
        channels = wav_channels;
        sample_rate = wav_rate;
        frames = wav_frames;
    }
    else if (source.m_codec == CSoundRender_Source::ECodec::Mp3)
    {
        drmp3_config config{};
        drmp3_uint64 mp3_frames = 0;
        decoded = use_float ? static_cast<void*>(drmp3_open_memory_and_read_pcm_frames_f32(file.pointer(), file.length(), &config, &mp3_frames, nullptr)) :
                              static_cast<void*>(drmp3_open_memory_and_read_pcm_frames_s16(file.pointer(), file.length(), &config, &mp3_frames, nullptr));
        channels = config.channels;
        sample_rate = config.sampleRate;
        frames = mp3_frames;
    }
    else
    {
        unsigned int flac_channels = 0;
        unsigned int flac_rate = 0;
        drflac_uint64 flac_frames = 0;
        decoded = use_float ? static_cast<void*>(drflac_open_memory_and_read_pcm_frames_f32(file.pointer(), file.length(), &flac_channels, &flac_rate, &flac_frames, nullptr)) :
                              static_cast<void*>(drflac_open_memory_and_read_pcm_frames_s16(file.pointer(), file.length(), &flac_channels, &flac_rate, &flac_frames, nullptr));
        channels = flac_channels;
        sample_rate = flac_rate;
        frames = flac_frames;
    }

    R_ASSERT3(decoded && channels && sample_rate && frames, "Cannot decode sound:", name);
    setup_pcm_format(source.m_wformat, channels, sample_rate, use_float);
    copy_decoded_pcm(source.m_decoded_data, decoded, frames, source.m_wformat);

    if (source.m_codec == CSoundRender_Source::ECodec::Wav)
        drwav_free(decoded, nullptr);
    else if (source.m_codec == CSoundRender_Source::ECodec::Mp3)
        drmp3_free(decoded, nullptr);
    else
        drflac_free(decoded, nullptr);

    source.dwBytesTotal = u32(source.m_decoded_data.size());
    source.fTimeTotal = s_f_def_source_footer + source.dwBytesTotal / float(source.m_wformat.nAvgBytesPerSec);
}

void CSoundRender_Source::LoadWave(LPCSTR pName)
{
    pname = pName;

    // Load file into memory and parse WAV-format
    OggVorbis_File ovf;
    constexpr ov_callbacks ovc = {ov_read_func, ov_seek_func, ov_close_func, ov_tell_func};
    IReader* wave = FS.r_open(pname.c_str());
    R_ASSERT3(wave && wave->length(), "Can't open wave file:", pname.c_str());
    ov_open_callbacks(wave, &ovf, NULL, 0, ovc);

    vorbis_info* ovi = ov_info(&ovf, -1);
    // verify
    R_ASSERT3(ovi, "Invalid source info:", pName);

#ifdef DEBUG
    if (ovi->channels == 2)
    {
        Msg("stereo sound source [%s]", pName);
    }
#endif

    ZeroMemory(&m_wformat, sizeof(WAVEFORMATEX));

    m_wformat.nSamplesPerSec = (ovi->rate);
    m_wformat.nChannels = u16(ovi->channels);

    if (SoundRender->supports_float_pcm)
    {
        m_wformat.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        m_wformat.wBitsPerSample = 32;
    }
    else
    {
        m_wformat.wFormatTag = WAVE_FORMAT_PCM;
        m_wformat.wBitsPerSample = 16;
    }

    m_wformat.nBlockAlign = m_wformat.wBitsPerSample / 8 * m_wformat.nChannels;
    m_wformat.nAvgBytesPerSec = m_wformat.nSamplesPerSec * m_wformat.nBlockAlign;

    s64 pcm_total = ov_pcm_total(&ovf, -1);
    dwBytesTotal = u32(pcm_total * m_wformat.nBlockAlign);
    fTimeTotal = s_f_def_source_footer + dwBytesTotal / float(m_wformat.nAvgBytesPerSec);

    vorbis_comment* ovm = ov_comment(&ovf, -1);
    if (ovm->comments)
    {
        IReader F(ovm->user_comments[0], ovm->comment_lengths[0]);

        u32 vers{};
        if (F.elapsed() <= static_cast<int>(sizeof vers))
            Msg("! Invalid ogg-comment, file: [%s]", pName);
        else
            vers = F.r_u32();

        if (vers == 0x0001)
        {
            m_fMinDist = F.r_float();
            m_fMaxDist = F.r_float();
            m_fBaseVolume = 1.f;
            m_uGameType = F.r_u32();
            m_fMaxAIDist = m_fMaxDist;
        }
        else if (vers == 0x0002)
        {
            m_fMinDist = F.r_float();
            m_fMaxDist = F.r_float();
            m_fBaseVolume = F.r_float();
            m_uGameType = F.r_u32();
            m_fMaxAIDist = m_fMaxDist;
        }
        else if (vers == OGG_COMMENT_VERSION)
        {
            m_fMinDist = F.r_float();
            m_fMaxDist = F.r_float();
            m_fBaseVolume = F.r_float();
            m_uGameType = F.r_u32();
            m_fMaxAIDist = F.r_float();
        }
        else
        {
            Msg("! Invalid ogg-comment version, file: [%s]", pName);
        }
    }
    else
    {
        Msg("! Missing ogg-comment, file: [%s]", pName);
    }
    R_ASSERT3((m_fMaxAIDist >= 0.1f) && (m_fMaxDist >= 0.1f), "Invalid max distance.", pName);

    ov_clear(&ovf);
    FS.r_close(wave);
}

void CSoundRender_Source::load(LPCSTR name)
{
    string_path fn, N;
    xr_strcpy(N, name);
    _strlwr(N);
    fname = N;

    LPCSTR extension = strext(N);
    if (extension)
    {
        xr_strcpy(fn, N);
    }
    else
    {
        static LPCSTR extensions[] = {".ogg", ".wav", ".mp3", ".flac"};
        fn[0] = 0;
        for (LPCSTR candidate_extension : extensions)
        {
            string_path candidate;
            strconcat(sizeof(candidate), candidate, N, candidate_extension);
            if (FS.exist("$level$", candidate) || FS.exist("$game_sounds$", candidate))
            {
                xr_strcpy(fn, candidate);
                break;
            }
        }
    }

    if (fn[0] && !FS.exist("$level$", fn))
        FS.update_path(fn, "$game_sounds$", fn);

    ASSERT_FMT_DBG(fn[0] && FS.exist(fn), "! Can't find sound [%s] (.ogg/.wav/.mp3/.flac)", N);
    if (!FS.exist(fn))
        FS.update_path(fn, "$game_sounds$", "$no_sound.ogg");

    m_codec = sound_codec(strext(fn));
    if (streamed())
    {
        LoadWave(fn);
    }
    else
    {
        pname = fn;
        IReader* file = FS.r_open(fn);
        R_ASSERT3(file && file->length(), "Can't open sound file:", fn);
        load_non_ogg(*this, *file, fn);
        FS.r_close(file);
    }
    SoundRender->cache.cat_create(CAT, dwBytesTotal);
}

void CSoundRender_Source::unload()
{
    SoundRender->cache.cat_destroy(CAT);
    fTimeTotal = 0.0f;
    dwBytesTotal = 0;
    m_decoded_data.clear();
}
