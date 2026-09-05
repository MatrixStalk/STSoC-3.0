#include "stdafx.h"

#include <DirectXTex.h>

void fix_texture_name(const char* fn)
{
    char* _ext = strext(fn);
    if (_ext && (0 == _stricmp(_ext, ".tga") || 0 == _stricmp(_ext, ".dds") || 0 == _stricmp(_ext, ".png") ||
                    0 == _stricmp(_ext, ".bmp") || 0 == _stricmp(_ext, ".ogm")))
        *_ext = 0;
}

namespace
{
bool find_texture_file(string_path& result, LPCSTR root, LPCSTR name)
{
    return FS.exist(result, root, name, ".dds") || FS.exist(result, root, name, ".png");
}
} // namespace

static inline int get_texture_load_lod(const char* fn)
{
#ifdef USE_REDUCE_LOD_TEXTURE_LIST
    xr_strlwr(fn);
    auto& sect = pSettings->r_section("reduce_lod_texture_list");

    for (const auto& data : sect.Data)
    {
        if (strstr(fn, data.first.c_str()))
        {
            if (psTextureLOD < 1)
            {
                return 0;
            }
            else
            {
                if (psTextureLOD < 3)
                {
                    return 1;
                }
                else
                {
                    return 2;
                }
            }
        }
    }
#endif

    if (psTextureLOD < 2)
    {
        return 0;
    }
    else
    {
        if (psTextureLOD < 4)
        {
            return 1;
        }
        else
        {
            return 2;
        }
    }
}

static inline u32 calc_texture_size(const int lod, const size_t mip_cnt, const size_t orig_size)
{
    if (1 == mip_cnt)
        return orig_size;

    int _lod = lod;
    float res = float(orig_size);

    while (_lod > 0)
    {
        --_lod;
        res -= res / 1.333f;
    }
    return iFloor(res);
}

static inline void reduce(size_t& w, size_t& h, size_t& l, int skip)
{
    while ((l > 1) && skip)
    {
        w /= 2;
        h /= 2;
        l -= 1;

        skip--;
    }
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
}

ID3DBaseTexture* CRender::texture_load(LPCSTR fRName, u32& ret_msize)
{
    // validation
    R_ASSERT(fRName && fRName[0]);

    // make file name
    string_path fname, fn;
    xr_strcpy(fname, fRName);
    fix_texture_name(fname);

    if (strstr(fname, "_bump") && !find_texture_file(fn, "$game_textures$", fname))
    {
        Msg("! Fallback to default bump map: [%s]", fname);

        if (strstr(fname, "_bump#"))
            R_ASSERT(find_texture_file(fn, "$game_textures$", "ed\\ed_dummy_bump#"), "ed_dummy_bump#");
        else
            R_ASSERT(find_texture_file(fn, "$game_textures$", "ed\\ed_dummy_bump"), "ed_dummy_bump");
    }
    else if (!find_texture_file(fn, "$level$", fname) && !find_texture_file(fn, "$game_saves$", fname) &&
        !find_texture_file(fn, "$game_textures$", fname))
    {
        Msg("! Can't find texture [%s]", fname);

        R_ASSERT(find_texture_file(fn, "$game_textures$", "ed\\ed_not_existing_texture"));
    }

    // Load and get header
    IReader* File = FS.r_open(fn);
    R_ASSERT(File);
#ifdef DEBUG
    Msg("* Loaded: %s[%zu]", fn, File->length());
#endif
    int img_loaded_lod{};
    ID3DBaseTexture* pTexture2D{};
    DirectX::TexMetadata IMG{};
    DirectX::DDS_FLAGS dds_flags{DirectX::DDS_FLAGS_PERMISSIVE};
    bool allowFallback = true;
    const char* file_ext = strext(fn);
    const bool is_png = file_ext && 0 == _stricmp(file_ext, ".png");

    do
    {
        DirectX::ScratchImage texture{};
        HRESULT load_hr = S_OK;
        if (is_png)
        {
            load_hr = LoadFromWICMemory(reinterpret_cast<const uint8_t*>(File->pointer()), File->length(),
                DirectX::WIC_FLAGS_NONE, &IMG, texture);
            if (SUCCEEDED(load_hr) && IMG.mipLevels == 1 && IMG.dimension == DirectX::TEX_DIMENSION_TEXTURE2D &&
                IMG.arraySize == 1 && (IMG.width > 1 || IMG.height > 1))
            {
                DirectX::ScratchImage mip_chain;
                const HRESULT mip_hr = GenerateMipMaps(*texture.GetImage(0, 0, 0), DirectX::TEX_FILTER_DEFAULT, 0, mip_chain);
                if (SUCCEEDED(mip_hr))
                {
                    texture = std::move(mip_chain);
                    IMG = texture.GetMetadata();
                }
                else
                {
                    Msg("! Failed to generate PNG mipmaps: [%s], hr: [%d]", fn, mip_hr);
                }
            }
        }
        else
        {
            load_hr = LoadFromDDSMemory(reinterpret_cast<const uint8_t*>(File->pointer()), File->length(), dds_flags,
                &IMG, texture);
        }

        if (FAILED(load_hr))
        {
            Msg("! Failed to load %s texture from memory: [%s], hr: [%d]", is_png ? "PNG" : "DDS", fn, load_hr);
            break;
        }

        // Check for LMAP and compress if needed
        size_t mip_lod{};
        img_loaded_lod = get_texture_load_lod(fn);
        if (img_loaded_lod && !IMG.IsCubemap() /* && !IMG.IsVolumemap()*/)
        {
            const auto old_mipmap_cnt = IMG.mipLevels;
            reduce(IMG.width, IMG.height, IMG.mipLevels, img_loaded_lod);
            mip_lod = old_mipmap_cnt - IMG.mipLevels;
        }

        // DirectX requires compressed texture size to be
        // a multiple of 4. Make sure to meet this requirement.
        if (DirectX::IsCompressed(IMG.format))
        {
            IMG.width = (IMG.width + 3u) & ~0x3u;
            IMG.height = (IMG.height + 3u) & ~0x3u;
        }

        const auto hr = CreateTextureEx(HW.pDevice, texture.GetImages() + mip_lod, texture.GetImageCount(), IMG, D3D_USAGE_IMMUTABLE, D3D_BIND_SHADER_RESOURCE, 0, IMG.miscFlags,
                                        DirectX::CREATETEX_DEFAULT, &pTexture2D);

        if (SUCCEEDED(hr))
        {
            // Получилось. Считаем сколько весит текстура и сваливаем.
            const size_t source_size = is_png ? texture.GetPixelsSize() : File->length();
            ret_msize = calc_texture_size(img_loaded_lod, IMG.mipLevels, source_size);
            break;
        }

        if (is_png || !allowFallback)
        {
            Msg("! Failed CreateTextureEx: [%s], hr: [%d]", fn, hr);
            break; // Уже была вторая попытка, прекращаем.
        }

        // Помянем, не получилось загрузить текстуру...
        // Давай заново, с конвертацией текстур. Может помочь.
        dds_flags |= DirectX::DDS_FLAGS::DDS_FLAGS_NO_16BPP | DirectX::DDS_FLAGS_FORCE_RGB;
        allowFallback = false;
    } while (true);

    FS.r_close(File);

    return pTexture2D;
}
