#pragma once

#include "embedded_editor_window.h"

class CImGuiSectionsWnd : public CImGuiEditorWnd
{
public:
    CImGuiSectionsWnd();
    void Render() override;

private:
    struct SRuntimeChange
    {
        shared_str section;
        shared_str key;
        shared_str original;
        shared_str value;
        bool originally_existed{};
    };

    void RefreshSections();
    void SelectSection(LPCSTR section);
    void SelectLine(LPCSTR key);
    void ApplyValue(LPCSTR value, bool reload_objects = true);
    void ApplyValue(LPCSTR section, LPCSTR key, LPCSTR value, bool reload_objects);
    void RevertSelected();
    void RevertAll();
    u32 ReloadMatchingObjects(LPCSTR section);
    u32 ReloadAllObjects();
    bool SaveOverrides();
    bool LoadOverrides();

    SRuntimeChange* FindChange(LPCSTR section, LPCSTR key);
    const SRuntimeChange* FindChange(LPCSTR section, LPCSTR key) const;

    xr_vector<shared_str> m_Sections;
    xr_vector<SRuntimeChange> m_Changes;
    shared_str m_SelectedSection;
    shared_str m_SelectedLine;

    char m_SectionFilter[128]{};
    char m_LineFilter[128]{};
    char m_ValueBuffer[4096]{};
    char m_NewKey[256]{};
    char m_NewValue[256]{};
    xr_string m_Status;
    bool m_AutoReload{true};
    bool m_ValueDirty{};
};
