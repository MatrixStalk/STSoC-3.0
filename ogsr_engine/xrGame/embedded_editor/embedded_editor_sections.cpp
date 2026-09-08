#include "stdafx.h"

#include "imgui.h"
#include "embedded_editor_sections.h"

#include "../Actor.h"
#include "../GameObject.h"
#include "../Inventory.h"
#include "../inventory_item.h"
#include "../Level.h"

#include <cerrno>
#include <cctype>

namespace
{
constexpr LPCSTR overrides_file_name = "ltx_editor_overrides.ltx";

bool contains_case_insensitive(LPCSTR text, LPCSTR filter)
{
    if (!filter || !filter[0])
        return true;
    if (!text)
        return false;

    const size_t filter_length = xr_strlen(filter);
    for (LPCSTR candidate = text; *candidate; ++candidate)
    {
        size_t i = 0;
        while (i < filter_length && candidate[i] &&
            std::tolower(static_cast<unsigned char>(candidate[i])) == std::tolower(static_cast<unsigned char>(filter[i])))
            ++i;
        if (i == filter_length)
            return true;
    }
    return false;
}

bool is_boolean(LPCSTR value, bool& result)
{
    // Numeric 0/1 is deliberately kept as an integer: in arbitrary LTX
    // sections those values are just as likely to be counts or coefficients.
    if (!_stricmp(value, "true") || !_stricmp(value, "on") || !_stricmp(value, "yes"))
    {
        result = true;
        return true;
    }
    if (!_stricmp(value, "false") || !_stricmp(value, "off") || !_stricmp(value, "no"))
    {
        result = false;
        return true;
    }
    return false;
}

bool parse_numbers(LPCSTR value, double (&numbers)[4], int& count, bool& integers_only)
{
    count = 0;
    integers_only = true;
    LPCSTR cursor = value;
    while (*cursor && std::isspace(static_cast<unsigned char>(*cursor)))
        ++cursor;
    if (!*cursor)
        return false;

    while (*cursor)
    {
        if (count == static_cast<int>(std::size(numbers)))
            return false;

        errno = 0;
        char* end = nullptr;
        numbers[count] = strtod(cursor, &end);
        if (end == cursor || errno == ERANGE)
            return false;

        for (LPCSTR p = cursor; p < end; ++p)
            if (*p == '.' || *p == 'e' || *p == 'E')
                integers_only = false;

        ++count;
        cursor = end;
        while (*cursor && std::isspace(static_cast<unsigned char>(*cursor)))
            ++cursor;
        if (!*cursor)
            return true;
        if (*cursor != ',')
            return false;
        ++cursor;
        while (*cursor && std::isspace(static_cast<unsigned char>(*cursor)))
            ++cursor;
        if (!*cursor)
            return false;
    }
    return count > 0;
}

void write_runtime_setting(LPCSTR section, LPCSTR key, LPCSTR value)
{
    const bool was_read_only = pSettings->bReadOnly;
    pSettings->bReadOnly = false;
    pSettings->w_string(section, key, value);
    pSettings->bReadOnly = was_read_only;
}
} // namespace

CImGuiSectionsWnd::CImGuiSectionsWnd() : CImGuiEditorWnd("LTX Sections###LTXSections")
{
    RefreshSections();
}

void CImGuiSectionsWnd::RefreshSections()
{
    m_Sections.clear();
    if (!pSettings)
        return;

    m_Sections.reserve(pSettings->sections().size());
    for (const auto& section : pSettings->sections())
        m_Sections.emplace_back(section.first);

    std::sort(m_Sections.begin(), m_Sections.end(), [](const shared_str& left, const shared_str& right) {
        return _stricmp(left.c_str(), right.c_str()) < 0;
    });
}

void CImGuiSectionsWnd::SelectSection(LPCSTR section)
{
    if (!section || !pSettings || !pSettings->section_exist(section))
        return;
    m_SelectedSection = section;
    m_SelectedLine = nullptr;
    m_ValueBuffer[0] = 0;
    m_ValueDirty = false;
}

void CImGuiSectionsWnd::SelectLine(LPCSTR key)
{
    if (!m_SelectedSection.c_str() || !key || !pSettings->line_exist(m_SelectedSection, key))
        return;
    m_SelectedLine = key;
    strcpy_s(m_ValueBuffer, pSettings->r_string(m_SelectedSection, key));
    m_ValueDirty = false;
}

CImGuiSectionsWnd::SRuntimeChange* CImGuiSectionsWnd::FindChange(LPCSTR section, LPCSTR key)
{
    const auto found = std::find_if(m_Changes.begin(), m_Changes.end(), [section, key](const SRuntimeChange& change) {
        return !_stricmp(change.section.c_str(), section) && !_stricmp(change.key.c_str(), key);
    });
    return found == m_Changes.end() ? nullptr : &*found;
}

const CImGuiSectionsWnd::SRuntimeChange* CImGuiSectionsWnd::FindChange(LPCSTR section, LPCSTR key) const
{
    const auto found = std::find_if(m_Changes.begin(), m_Changes.end(), [section, key](const SRuntimeChange& change) {
        return !_stricmp(change.section.c_str(), section) && !_stricmp(change.key.c_str(), key);
    });
    return found == m_Changes.end() ? nullptr : &*found;
}

void CImGuiSectionsWnd::ApplyValue(LPCSTR value, bool reload_objects)
{
    if (m_SelectedSection.c_str() && m_SelectedLine.c_str())
        ApplyValue(m_SelectedSection.c_str(), m_SelectedLine.c_str(), value, reload_objects);
}

void CImGuiSectionsWnd::ApplyValue(LPCSTR section, LPCSTR key, LPCSTR value, bool reload_objects)
{
    if (!pSettings || !section || !section[0] || !key || !key[0] || !value)
        return;
    if (xr_strlen(section) >= 256 || xr_strlen(key) >= 256 || xr_strlen(value) >= 256)
    {
        m_Status = "Section, key and value must each be shorter than 256 characters";
        return;
    }

    SRuntimeChange* change = FindChange(section, key);
    if (!change)
    {
        SRuntimeChange added;
        added.section = section;
        added.key = key;
        added.originally_existed = !!pSettings->line_exist(section, key);
        if (added.originally_existed)
            added.original = pSettings->r_string(section, key);
        m_Changes.push_back(added);
        change = &m_Changes.back();
    }

    write_runtime_setting(section, key, value);
    change->value = value;

    u32 reloaded = 0;
    if (reload_objects && m_AutoReload)
        reloaded = ReloadMatchingObjects(section);

    string512 status{};
    xr_sprintf(status, "Applied [%s]:%s%s", section, key,
        reload_objects && m_AutoReload ? (reloaded ? " and reloaded live objects" : " (no matching live objects)") : " to runtime cache");
    m_Status = status;

    if (change->originally_existed && !xr_strcmp(change->original.c_str(), value))
    {
        const auto index = static_cast<size_t>(change - m_Changes.data());
        m_Changes.erase(m_Changes.begin() + index);
    }

    if (m_SelectedSection.c_str() && m_SelectedLine.c_str() &&
        !_stricmp(m_SelectedSection.c_str(), section) && !_stricmp(m_SelectedLine.c_str(), key))
    {
        strcpy_s(m_ValueBuffer, value);
        m_ValueDirty = false;
    }
}

void CImGuiSectionsWnd::RevertSelected()
{
    if (!m_SelectedSection.c_str() || !m_SelectedLine.c_str())
        return;
    SRuntimeChange* change = FindChange(m_SelectedSection.c_str(), m_SelectedLine.c_str());
    if (!change)
        return;

    const size_t index = static_cast<size_t>(change - m_Changes.data());
    if (change->originally_existed)
        write_runtime_setting(change->section.c_str(), change->key.c_str(), change->original.c_str());
    else
        pSettings->remove_line(change->section.c_str(), change->key.c_str());

    m_Changes.erase(m_Changes.begin() + index);
    if (pSettings->line_exist(m_SelectedSection, m_SelectedLine))
        strcpy_s(m_ValueBuffer, pSettings->r_string(m_SelectedSection.c_str(), m_SelectedLine.c_str()));
    else
    {
        m_SelectedLine = nullptr;
        m_ValueBuffer[0] = 0;
    }
    m_ValueDirty = false;
    const u32 reloaded = m_AutoReload ? ReloadMatchingObjects(m_SelectedSection.c_str()) : 0;
    m_Status = reloaded ? "Reverted value and reloaded live objects" : "Reverted value";
}

void CImGuiSectionsWnd::RevertAll()
{
    xr_vector<shared_str> affected_sections;
    for (const SRuntimeChange& change : m_Changes)
    {
        if (change.originally_existed)
            write_runtime_setting(change.section.c_str(), change.key.c_str(), change.original.c_str());
        else
            pSettings->remove_line(change.section.c_str(), change.key.c_str());
        if (std::find(affected_sections.begin(), affected_sections.end(), change.section) == affected_sections.end())
            affected_sections.push_back(change.section);
    }
    m_Changes.clear();

    u32 reloaded = 0;
    if (m_AutoReload)
        for (const shared_str& section : affected_sections)
            reloaded += ReloadMatchingObjects(section.c_str());

    if (m_SelectedSection.c_str() && m_SelectedLine.c_str() && pSettings->line_exist(m_SelectedSection, m_SelectedLine))
        strcpy_s(m_ValueBuffer, pSettings->r_string(m_SelectedSection.c_str(), m_SelectedLine.c_str()));
    else if (m_SelectedLine.c_str())
    {
        m_SelectedLine = nullptr;
        m_ValueBuffer[0] = 0;
    }
    m_ValueDirty = false;
    string128 status{};
    xr_sprintf(status, "Reverted all session changes; reloaded %u object(s)", reloaded);
    m_Status = status;
}

u32 CImGuiSectionsWnd::ReloadMatchingObjects(LPCSTR section)
{
    if (!g_pGameLevel || Level().is_removing_objects())
        return 0;

    u32 count = 0;
    for (u32 i = 0; i < Level().Objects.o_count(); ++i)
    {
        CGameObject* object = smart_cast<CGameObject*>(Level().Objects.o_get_by_iterator(i));
        if (!object || _stricmp(object->cNameSect().c_str(), section))
            continue;
        object->reload(object->cNameSect().c_str());
        ++count;
    }
    return count;
}

u32 CImGuiSectionsWnd::ReloadAllObjects()
{
    if (!g_pGameLevel || Level().is_removing_objects())
        return 0;

    u32 count = 0;
    for (u32 i = 0; i < Level().Objects.o_count(); ++i)
    {
        CGameObject* object = smart_cast<CGameObject*>(Level().Objects.o_get_by_iterator(i));
        if (!object)
            continue;
        object->reload(object->cNameSect().c_str());
        ++count;
    }
    return count;
}

bool CImGuiSectionsWnd::SaveOverrides()
{
    string_path path{};
    FS.update_path(path, fsgame::app_data_root, overrides_file_name);
    CInifile output(path, FALSE, FALSE, FALSE);
    for (const SRuntimeChange& change : m_Changes)
        output.w_string(change.section.c_str(), change.key.c_str(), change.value.c_str());
    const bool result = output.save_as();
    m_Status = result ? xr_string("Saved overrides to ") + path : xr_string("Failed to save ") + path;
    return result;
}

bool CImGuiSectionsWnd::LoadOverrides()
{
    string_path path{};
    FS.update_path(path, fsgame::app_data_root, overrides_file_name);
    if (!FS.exist(path))
    {
        m_Status = xr_string("Override file does not exist: ") + path;
        return false;
    }

    CInifile input(path, TRUE, TRUE, FALSE);
    for (const auto& section : input.sections_ordered())
        for (const auto& line : section.second->Ordered_Data)
            ApplyValue(section.first.c_str(), line.first.c_str(), line.second.c_str(), false);

    const u32 count = ReloadAllObjects();
    string512 status{};
    xr_sprintf(status, "Loaded overrides and reloaded %u live object(s)", count);
    m_Status = status;
    RefreshSections();
    return true;
}

void CImGuiSectionsWnd::Render()
{
    if (!RenderBegin())
    {
        RenderEnd();
        return;
    }

    if (!pSettings)
    {
        ImGui::TextDisabled("pSettings is not initialized");
        RenderEnd();
        return;
    }

    if (ImGui::Button("Active item"))
    {
        CActor* actor = g_pGameLevel ? smart_cast<CActor*>(Level().CurrentEntity()) : nullptr;
        CInventoryItem* item = actor ? actor->inventory().ActiveItem() : nullptr;
        CGameObject* object = item ? item->cast_game_object() : nullptr;
        if (object)
            SelectSection(object->cNameSect().c_str());
        else
            m_Status = "No active inventory item";
    }
    ImGui::SameLine();
    if (ImGui::Button("Current entity"))
    {
        CGameObject* object = g_pGameLevel ? smart_cast<CGameObject*>(Level().CurrentEntity()) : nullptr;
        if (object)
            SelectSection(object->cNameSect().c_str());
        else
            m_Status = "No current game entity";
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh sections"))
        RefreshSections();
    ImGui::SameLine();
    ImGui::Checkbox("Auto-reload matching objects", &m_AutoReload);

    if (ImGui::Button("Reload selected section"))
    {
        const u32 count = m_SelectedSection.c_str() ? ReloadMatchingObjects(m_SelectedSection.c_str()) : 0;
        string128 status{};
        xr_sprintf(status, "Reloaded %u object(s) of selected section", count);
        m_Status = status;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload all live objects"))
    {
        const u32 count = ReloadAllObjects();
        string128 status{};
        xr_sprintf(status, "Reloaded %u live object(s)", count);
        m_Status = status;
    }
    ImGui::SameLine();
    if (ImGui::Button("Save overrides"))
        SaveOverrides();
    ImGui::SameLine();
    if (ImGui::Button("Load overrides"))
        LoadOverrides();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_Changes.empty());
    if (ImGui::Button("Revert all"))
        RevertAll();
    ImGui::EndDisabled();

    if (!m_Status.empty())
        ImGui::TextDisabled("%s", m_Status.c_str());
    ImGui::TextDisabled("Runtime changes: %zu. Saved patch: $app_data_root$\\%s", m_Changes.size(), overrides_file_name);
    ImGui::Separator();

    const float available_width = ImGui::GetContentRegionAvail().x;
    const float available_height = ImGui::GetContentRegionAvail().y;
    const float sections_width = _max(220.f, available_width * 0.25f);
    const float lines_width = _max(300.f, available_width * 0.36f);

    ImGui::BeginChild("Sections", ImVec2(sections_width, available_height), true);
    ImGui::InputTextWithHint("##SectionFilter", "Filter sections...", m_SectionFilter, std::size(m_SectionFilter));
    xr_vector<shared_str> filtered_sections;
    filtered_sections.reserve(m_Sections.size());
    for (const shared_str& section : m_Sections)
        if (contains_case_insensitive(section.c_str(), m_SectionFilter))
            filtered_sections.push_back(section);
    ImGui::TextDisabled("%zu / %zu sections", filtered_sections.size(), m_Sections.size());
    ImGuiListClipper section_clipper;
    section_clipper.Begin(static_cast<int>(filtered_sections.size()));
    while (section_clipper.Step())
        for (int i = section_clipper.DisplayStart; i < section_clipper.DisplayEnd; ++i)
        {
            const shared_str& section = filtered_sections[i];
            if (ImGui::Selectable(section.c_str(), m_SelectedSection.c_str() && !_stricmp(section.c_str(), m_SelectedSection.c_str())))
                SelectSection(section.c_str());
        }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("Lines", ImVec2(lines_width, available_height), true);
    if (!m_SelectedSection.c_str())
        ImGui::TextDisabled("Select a section");
    else
    {
        ImGui::Text("[%s]", m_SelectedSection.c_str());
        ImGui::InputTextWithHint("##LineFilter", "Filter keys or values...", m_LineFilter, std::size(m_LineFilter));
        const auto& data = pSettings->r_section(m_SelectedSection).Ordered_Data;
        xr_vector<const CInifile::Item*> filtered_lines;
        filtered_lines.reserve(data.size());
        for (const CInifile::Item& line : data)
            if (contains_case_insensitive(line.first.c_str(), m_LineFilter) || contains_case_insensitive(line.second.c_str(), m_LineFilter))
                filtered_lines.push_back(&line);

        if (ImGui::BeginTable("Parameters", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                ImVec2(0.f, _max(100.f, ImGui::GetContentRegionAvail().y - 100.f))))
        {
            ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, lines_width * 0.42f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            ImGuiListClipper line_clipper;
            line_clipper.Begin(static_cast<int>(filtered_lines.size()));
            while (line_clipper.Step())
                for (int i = line_clipper.DisplayStart; i < line_clipper.DisplayEnd; ++i)
                {
                    const CInifile::Item& line = *filtered_lines[i];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushID(line.first.c_str());
                    const bool changed = FindChange(m_SelectedSection.c_str(), line.first.c_str()) != nullptr;
                    if (changed)
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.75f, 0.25f, 1.f));
                    if (ImGui::Selectable(line.first.c_str(), m_SelectedLine.c_str() && !_stricmp(line.first.c_str(), m_SelectedLine.c_str()), ImGuiSelectableFlags_SpanAllColumns))
                        SelectLine(line.first.c_str());
                    if (changed)
                        ImGui::PopStyleColor();
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(line.second.c_str());
                    ImGui::PopID();
                }
            ImGui::EndTable();
        }

        ImGui::InputTextWithHint("##NewKey", "new_key", m_NewKey, std::size(m_NewKey));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-80.f);
        ImGui::InputTextWithHint("##NewValue", "value", m_NewValue, std::size(m_NewValue));
        ImGui::SameLine();
        if (ImGui::Button("Add"))
        {
            if (m_NewKey[0])
            {
                ApplyValue(m_SelectedSection.c_str(), m_NewKey, m_NewValue, true);
                SelectLine(m_NewKey);
                m_NewKey[0] = 0;
                m_NewValue[0] = 0;
            }
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("ValueEditor", ImVec2(0.f, available_height), true);
    if (!m_SelectedLine.c_str())
        ImGui::TextDisabled("Select a parameter");
    else
    {
        ImGui::TextWrapped("[%s]", m_SelectedSection.c_str());
        ImGui::SeparatorText(m_SelectedLine.c_str());
        const SRuntimeChange* change = FindChange(m_SelectedSection.c_str(), m_SelectedLine.c_str());
        if (change)
            ImGui::TextColored(ImVec4(1.f, 0.75f, 0.25f, 1.f), "Modified this session");

        bool bool_value = false;
        double numbers[4]{};
        int number_count = 0;
        bool integers_only = false;
        const bool boolean = is_boolean(m_ValueBuffer, bool_value);
        const bool numeric = !boolean && parse_numbers(m_ValueBuffer, numbers, number_count, integers_only);

        if (boolean)
        {
            ImGui::TextDisabled("Detected type: boolean");
            if (ImGui::Checkbox("Value", &bool_value))
                ApplyValue(bool_value ? "true" : "false");
        }
        else if (numeric && number_count == 1 && integers_only)
        {
            ImGui::TextDisabled("Detected type: integer");
            int value = static_cast<int>(numbers[0]);
            if (ImGui::InputInt("Value", &value))
            {
                string64 formatted{};
                xr_sprintf(formatted, "%d", value);
                ApplyValue(formatted);
            }
        }
        else if (numeric)
        {
            ImGui::TextDisabled("Detected type: %s", number_count == 1 ? "number" : "numeric vector");
            float values[4] = {static_cast<float>(numbers[0]), static_cast<float>(numbers[1]), static_cast<float>(numbers[2]), static_cast<float>(numbers[3])};
            bool edited = false;
            if (number_count == 1)
                edited = ImGui::InputFloat("Value", values, 0.f, 0.f, "%.7g");
            else if (number_count == 2)
                edited = ImGui::InputFloat2("Value", values, "%.7g");
            else if (number_count == 3)
                edited = ImGui::InputFloat3("Value", values, "%.7g");
            else if (number_count == 4)
                edited = ImGui::InputFloat4("Value", values, "%.7g");
            if (edited)
            {
                string256 formatted{};
                if (number_count == 1)
                    xr_sprintf(formatted, "%.7g", values[0]);
                else if (number_count == 2)
                    xr_sprintf(formatted, "%.7g, %.7g", values[0], values[1]);
                else if (number_count == 3)
                    xr_sprintf(formatted, "%.7g, %.7g, %.7g", values[0], values[1], values[2]);
                else
                    xr_sprintf(formatted, "%.7g, %.7g, %.7g, %.7g", values[0], values[1], values[2], values[3]);
                ApplyValue(formatted);
            }
        }
        else
            ImGui::TextDisabled("Detected type: text/list");

        ImGui::SeparatorText("Raw value");
        if (ImGui::InputTextMultiline("##RawValue", m_ValueBuffer, std::size(m_ValueBuffer), ImVec2(-1.f, 100.f)))
            m_ValueDirty = true;
        ImGui::BeginDisabled(!m_ValueDirty);
        if (ImGui::Button("Apply raw value"))
            ApplyValue(m_ValueBuffer);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!change);
        if (ImGui::Button("Revert"))
            RevertSelected();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Copy LTX line"))
        {
            xr_string line = xr_string(m_SelectedLine.c_str()) + " = " + m_ValueBuffer;
            ImGui::SetClipboardText(line.c_str());
        }
        ImGui::TextWrapped("Typed controls apply immediately. Raw text is committed with Apply to avoid reloading objects while the value is incomplete.");
    }
    ImGui::EndChild();

    RenderEnd();
}
