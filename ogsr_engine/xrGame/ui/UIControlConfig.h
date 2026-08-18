#pragma once

namespace UIControlConfig
{
LPCSTR ReadString(LPCSTR section, LPCSTR attribute, LPCSTR fallback);
int ReadInt(LPCSTR section, LPCSTR attribute, int fallback);
float ReadFloat(LPCSTR section, LPCSTR attribute, float fallback);
bool ReadBool(LPCSTR section, LPCSTR attribute, bool fallback);
u32 ReadColor(LPCSTR section, u32 fallback);
} // namespace UIControlConfig
