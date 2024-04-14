// Mini memory editor for Dear ImGui (to embed in your game/tools)
// Get latest version at http://www.github.com/ocornut/imgui_club
//
// Right-click anywhere to access the Options menu!
// You can adjust the keyboard repeat delay/rate in ImGuiIO.
// The code assume a mono-space font for simplicity!
// If you don't use the default font, use ImGui::PushFont()/PopFont() to switch to a mono-space font before calling this.
//
// Usage:
//   // Create a window and draw memory editor inside it:
//   static MemoryEditor mem_edit_1;
//   static char data[0x10000];
//   size_t data_size = 0x10000;
//   mem_edit_1.DrawWindow("Memory Editor", data, data_size);
//
// Usage:
//   // If you already have a window, use DrawContents() instead:
//   static MemoryEditor mem_edit_2;
//   ImGui::Begin("MyWindow")
//   mem_edit_2.DrawContents(this, sizeof(*this), (size_t)this);
//   ImGui::End();
//
// Changelog:
// - v0.10: initial version
// - v0.23 (2017/08/17): added to github. fixed right-arrow triggering a byte write.
// - v0.24 (2018/06/02): changed DragInt("Rows" to use a %d data format (which is desirable since imgui 1.61).
// - v0.25 (2018/07/11): fixed wording: all occurrences of "Rows" renamed to "Columns".
// - v0.26 (2018/08/02): fixed clicking on hex region
// - v0.30 (2018/08/02): added data preview for common data types
// - v0.31 (2018/10/10): added OptUpperCaseHex option to select lower/upper casing display [@samhocevar]
// - v0.32 (2018/10/10): changed signatures to use void* instead of unsigned char*
// - v0.33 (2018/10/10): added OptShowOptions option to hide all the interactive option setting.
// - v0.34 (2019/05/07): binary preview now applies endianness setting [@nicolasnoble]
// - v0.35 (2020/01/29): using ImGuiDataType available since Dear ImGui 1.69.
// - v0.36 (2020/05/05): minor tweaks, minor refactor.
// - v0.40 (2020/10/04): fix misuse of ImGuiListClipper API, broke with Dear ImGui 1.79. made cursor position appears on left-side of edit box. option popup appears on mouse release. fix MSVC warnings where _CRT_SECURE_NO_WARNINGS wasn't working in recent versions.
// - v0.41 (2020/10/05): fix when using with keyboard/gamepad navigation enabled.
// - v0.42 (2020/10/14): fix for . character in ASCII view always being greyed out.
// - v0.43 (2021/03/12): added OptFooterExtraHeight to allow for custom drawing at the bottom of the editor [@leiradel]
// - v0.44 (2021/03/12): use ImGuiInputTextFlags_AlwaysOverwrite in 1.82 + fix hardcoded width.
// - v0.50 (2021/11/12): various fixes for recent dear imgui versions (fixed misuse of clipper, relying on SetKeyboardFocusHere() handling scrolling from 1.85). added default size.
//
// Todo/Bugs:
// - This is generally old/crappy code, it should work but isn't very good.. to be rewritten some day.
// - PageUp/PageDown are supported because we use _NoNav. This is a good test scenario for working out idioms of how to mix natural nav and our own...
// - Arrows are being sent to the InputText() about to disappear which for LeftArrow makes the text cursor appear at position 1 for one frame.
// - Using InputText() is awkward and maybe overkill here, consider implementing something custom.

#pragma once

#include <stdio.h>      // sprintf, scanf
#include <ctype.h>      // toupper
#include <stdint.h>     // uint8_t, etc.
#include <vector>      // std::vector
#include <limits>      // std::numeric_limits
#include <../../../include/log.hpp>
#include <../../../include/algorithms/binary_search.hpp>

#include "../../tracy/public/tracy/Tracy.hpp"

#ifdef _MSC_VER
#define _PRISizeT   "I"
#define ImSnprintf  _snprintf
#else
#define _PRISizeT   "z"
#define ImSnprintf  snprintf
#endif

#ifdef _MSC_VER
#pragma warning (push)
#pragma warning (disable: 4996) // warning C4996: 'sprintf': This function or variable may be unsafe.
#endif

struct MemoryEditor
{
    // A primary data type
    enum DataType_
    {
        DataType_S8,       // signed char / char (with sensible compilers)
        DataType_U8,       // unsigned char
        DataType_S16,      // short
        DataType_U16,      // unsigned short
        DataType_S32,      // int
        DataType_U32,      // unsigned int
        DataType_S64,      // long long / __int64
        DataType_U64,      // unsigned long long / unsigned __int64
        DataType_HalfFloat,// _Float16
        DataType_Float,    // float
        DataType_Double,   // double
        DataType_COUNT
    };

    enum DataFormat
    {
        DataFormat_Bin = 0,
        DataFormat_Dec = 1,
        DataFormat_Hex = 2,
        DataFormat_COUNT
    };

    union Color {
        ImU32 id;
        struct {
            ImU8 r;
            ImU8 g;
            ImU8 b;
            ImU8 a;
        };
    };

    struct HighlightRange {
        size_t RangeStartAddress; // Inclusive
        size_t RangeEndAddress; // Exclusive
        Color  RangeColor;
        bool   isActive;
    };

    struct NoteRange {
        size_t       RangeStartAddress; // Inclusive
        size_t       RangeEndAddress; // Exclusive
        Color        RangeColor;
        std::string  Description;
        bool         isActive;
    };

    enum class RangePosition : u8 {
        NotInRange = 0,
        Start,
        Middle,
        End,
    };

    template<typename T>
    static RangePosition IsInRange(std::vector<T> const& ranges, size_t const addr, Color& color, size_t* out_range_idx = nullptr) {
        ZoneScopedN("MemoryEditor::IsInRange");

        if (ranges.size() == 0L) {
            return RangePosition::NotInRange;
        }

        // NOTE: Assume ordered ranges
        size_t startRange = ranges.front().RangeStartAddress;
        size_t endRange = ranges.back().RangeEndAddress;

        if (addr < startRange || addr > endRange) {
            return RangePosition::NotInRange;
        }

        if (ranges.size() < 100L) {
            for (size_t idx = 0L; idx < ranges.size(); idx += 1) {
                auto const& highlight_range = ranges.at(idx);
                if (highlight_range.RangeStartAddress <= addr && addr < highlight_range.RangeEndAddress) {
                    color = highlight_range.RangeColor;

                    if (!highlight_range.isActive) {
                        return RangePosition::NotInRange;
                    }

                    if (out_range_idx != nullptr) {
                        *out_range_idx = idx;
                    }

                    if (addr == highlight_range.RangeStartAddress) {
                        return RangePosition::Start;
                    }

                    if (addr == (highlight_range.RangeEndAddress-1)) {
                        return RangePosition::End;
                    }

                    return RangePosition::Middle;
                }
            }
        } else {
            T val; // unitialized to save time since it is not used
            LT_UNINITIALIZED(val);
            auto const [found, idx] = lt::algorithms::binary_find<T, s32, typename std::vector<T>::const_iterator>(ranges.cbegin(), ranges.cend(), val, [addr, &color](T const& highlight_range, T const& unused) -> s32 {
                LT_UNUSED(unused);

                if (addr < highlight_range.RangeStartAddress) {
                    return s32(-1);
                }

                if (addr >= highlight_range.RangeEndAddress) {
                    return s32(1);
                }

                if (highlight_range.isActive) {
                    color = highlight_range.RangeColor;
                    return s32(0);
                };

                return lt::algorithms::cancel_search<s32>();
            });


            if (!found) {
                return RangePosition::NotInRange;
            }

            if (out_range_idx != nullptr) {
                *out_range_idx = idx;
            }

            auto const& range = ranges[idx];

            if (addr == range.RangeStartAddress) {
                return RangePosition::Start;
            }

            if (addr == (range.RangeEndAddress-1)) {
                return RangePosition::End;
            }

            return RangePosition::Middle;
        }

        return RangePosition::NotInRange;
    }

    // Settings
    bool Open;                                                  // = true   // set to false when DrawWindow() was closed. ignore if not using DrawWindow().
    bool            ReadOnly;                                   // = false  // disable any editing.
    int             Cols;                                       // = 16     // number of columns to display.
    bool            OptShowOptions;                             // = true   // display options button/context menu. when disabled, options will be locked unless you provide your own UI for them.
    bool            OptShowAscii;                               // = true   // display ASCII representation on the right side.
    bool            OptGreyOutZeroes;                           // = true   // display null/zero bytes using the TextDisabled color.
    bool            OptUpperCaseHex;                            // = true   // display hexadecimal values as "FF" instead of "ff".
    int             OptMidColsCount;                            // = 8      // set to 0 to disable extra spacing between every mid-cols.
    int             OptAddrDigitsCount;                         // = 0      // number of addr digits to display (default calculated based on maximum displayed addr).
    float           OptFooterExtraHeight;                       // = 0      // space to reserve at the bottom of the widget to add custom widgets
    ImU32           HighlightColor;                             //          // background color of highlighted bytes.
    ImU8            (*ReadFn)(ImU8 const* data, size_t off);    // = 0      // optional handler to read bytes.
    void            (*WriteFn)(ImU8* data, size_t off, ImU8 d); // = 0      // optional handler to write bytes.
    bool            (*HighlightFn)(ImU8 const* data, size_t off);//= 0      // optional handler to return Highlight property (to support non-contiguous highlighting).
    ImU32           DEFAULT_NOTE_COLOR;

    // [Internal State]
    bool            ContentsWidthChanged;
    size_t          DataPreviewAddr;
    size_t          DataEditingAddr;
    bool            DataEditingTakeFocus;
    char            AddrInputBuf[32];
    char            ValueConverterInputBuf[32];
    size_t          GotoAddr;
    size_t          HighlightMin, HighlightMax;
    size_t          ValueToConvert;
    int             PreviewEndianess;
    DataType_       PreviewDataType;
    DataType_       ConvertValueType;
    DataFormat      ConvertValueFormat;
    std::vector<HighlightRange> Ranges;
    std::vector<NoteRange> Notes;

    MemoryEditor(HighlightRange* ranges, size_t const ranges_size)
    {
        if (ranges != nullptr) {
            Ranges = std::vector<HighlightRange>(ranges, ranges + ranges_size);
        }

        // Settings
        Open = true;
        ReadOnly = false;
        Cols = 14;
        OptShowOptions = true;
        OptShowAscii = true;
        OptGreyOutZeroes = true;
        OptUpperCaseHex = true;
        OptMidColsCount = 8;
        OptAddrDigitsCount = 0;
        OptFooterExtraHeight = 5.0f;
        ReadFn = NULL;
        WriteFn = NULL;
        HighlightFn = NULL;
        HighlightColor = IM_COL32(255, 127, 255, 150);

        // State/Internals
        ContentsWidthChanged = false;
        DataPreviewAddr = DataEditingAddr = std::numeric_limits<size_t>::max();
        DataEditingTakeFocus = false;
        memset(AddrInputBuf, 0, sizeof(AddrInputBuf));
        memset(ValueConverterInputBuf, 0, sizeof(ValueConverterInputBuf));
        memcpy(ValueConverterInputBuf, "0", 1);
        GotoAddr = std::numeric_limits<size_t>::max();
        ValueToConvert = 0;
        HighlightMin = HighlightMax = std::numeric_limits<size_t>::max();
        PreviewEndianess = 0;
        PreviewDataType = DataType_S32;
        ConvertValueType = DataType_U32;
        ConvertValueFormat = DataFormat_Hex;
        DEFAULT_NOTE_COLOR = IM_COL32(255, 200, 0, 255);
    }

    void GotoAddrAndHighlight(size_t addr_min, size_t addr_max)
    {
        GotoAddr = addr_min;
        HighlightMin = addr_min;
        HighlightMax = addr_max;
    }

    struct Sizes
    {
        int     AddrDigitsCount;
        float   LineHeight;
        float   GlyphWidth;
        float   HexCellWidth;
        float   SpacingBetweenMidCols;
        float   PosHexStart;
        float   PosHexEnd;
        float   PosAsciiStart;
        float   PosAsciiEnd;
        float   WindowWidth;

        Sizes() { memset(this, 0, sizeof(*this)); }
    };

    void CalcSizes(Sizes& s, size_t mem_size, size_t base_display_addr)
    {
        ImGuiStyle& style = ImGui::GetStyle();
        s.AddrDigitsCount = OptAddrDigitsCount;
        if (s.AddrDigitsCount == 0)
            for (size_t n = base_display_addr + mem_size - 1; n > 0; n >>= 4)
                s.AddrDigitsCount++;
        s.LineHeight = ImGui::GetTextLineHeight();
        s.GlyphWidth = ImGui::CalcTextSize("F").x + 1;                  // We assume the font is mono-space
        s.HexCellWidth = (float)(int)(s.GlyphWidth * 2.5f);             // "FF " we include trailing space in the width to easily catch clicks everywhere
        s.SpacingBetweenMidCols = (float)(int)(s.HexCellWidth * 0.25f); // Every OptMidColsCount columns we add a bit of extra spacing
        s.PosHexStart = (s.AddrDigitsCount + 2) * s.GlyphWidth;
        s.PosHexEnd = s.PosHexStart + (s.HexCellWidth * Cols);
        s.PosAsciiStart = s.PosAsciiEnd = s.PosHexEnd;
        if (OptShowAscii)
        {
            s.PosAsciiStart = s.PosHexEnd + s.GlyphWidth * 1;
            if (OptMidColsCount > 0)
                s.PosAsciiStart += (float)((Cols + OptMidColsCount - 1) / OptMidColsCount) * s.SpacingBetweenMidCols;
            s.PosAsciiEnd = s.PosAsciiStart + Cols * s.GlyphWidth;
        }
        s.WindowWidth = s.PosAsciiEnd + style.ScrollbarSize + style.WindowPadding.x * 2 + s.GlyphWidth;
    }

    // Standalone Memory Editor window
    void DrawWindow(const char* title, void* mem_data, size_t mem_size, size_t base_display_addr = 0x0000)
    {
        Sizes s;
        CalcSizes(s, mem_size, base_display_addr);
        ImGui::SetNextWindowSize(ImVec2(s.WindowWidth, s.WindowWidth * 0.60f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(s.WindowWidth, FLT_MAX));

        Open = true;
        if (ImGui::Begin(title, &Open, ImGuiWindowFlags_NoScrollbar))
        {
            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
                ImGui::OpenPopup("context");
            DrawContents(mem_data, mem_size, base_display_addr);
            if (ContentsWidthChanged)
            {
                CalcSizes(s, mem_size, base_display_addr);
                ImGui::SetWindowSize(ImVec2(s.WindowWidth, ImGui::GetWindowSize().y));
            }
        }
        ImGui::End();
    }

    // Memory Editor contents only
    void DrawContents(void* mem_data_void, size_t mem_size, size_t base_display_addr = 0x0000)
    {
        if (Cols < 1)
            Cols = 1;

        ImU8* mem_data = (ImU8*)mem_data_void;
        Sizes s;
        CalcSizes(s, mem_size, base_display_addr);
        ImGuiStyle& style = ImGui::GetStyle();

        // We begin into our scrolling region with the 'ImGuiWindowFlags_NoMove' in order to prevent click from moving the window.
        // This is used as a facility since our main click detection code doesn't assign an ActiveId so the click would normally be caught as a window-move.
        const float height_separator = style.ItemSpacing.y;
        float footer_height = OptFooterExtraHeight;
        if (OptShowOptions)
            footer_height += height_separator + ImGui::GetFrameHeightWithSpacing() * 1;
        footer_height += height_separator + ImGui::GetFrameHeightWithSpacing() * 1 + ImGui::GetTextLineHeightWithSpacing() * 15;
        ImGui::BeginChild("##scrolling", ImVec2(0, -footer_height), false, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav);
        ImDrawList* draw_list = ImGui::GetWindowDrawList();

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));

        // We are not really using the clipper API correctly here, because we rely on visible_start_addr/visible_end_addr for our scrolling function.
        const int line_total_count = (int)((mem_size + Cols - 1) / Cols);
        ImGuiListClipper clipper;
        clipper.Begin(line_total_count, s.LineHeight);

        bool data_next = false;

        if (ReadOnly || DataEditingAddr >= mem_size)
            DataEditingAddr = std::numeric_limits<size_t>::max();
        if (DataPreviewAddr >= mem_size)
            DataPreviewAddr = std::numeric_limits<size_t>::max();

        size_t preview_data_type_size = DataTypeGetSize(PreviewDataType);
        size_t data_editing_addr_next = std::numeric_limits<size_t>::max();

        // Draw vertical separator
        ImVec2 window_pos = ImGui::GetWindowPos();
        if (OptShowAscii)
            draw_list->AddLine(ImVec2(window_pos.x + s.PosAsciiStart - s.GlyphWidth, window_pos.y), ImVec2(window_pos.x + s.PosAsciiStart - s.GlyphWidth, window_pos.y + 9999), ImGui::GetColorU32(ImGuiCol_Border));

        const ImU32 color_text = ImGui::GetColorU32(ImGuiCol_Text);
        const ImU32 color_disabled = OptGreyOutZeroes ? ImGui::GetColorU32(ImGuiCol_TextDisabled) : color_text;

        const char* format_address = OptUpperCaseHex ? "%0*" _PRISizeT "X: " : "%0*" _PRISizeT "x: ";
        const char* format_byte_space = OptUpperCaseHex ? "%02X " : "%02x ";

        while (clipper.Step()) {
            ZoneScopedN("MemoryEditor::DrawContents-clipperStep");
            for (int line_i = clipper.DisplayStart; line_i < clipper.DisplayEnd; line_i++) // display only visible lines
            {
                size_t line_idx = line_i - clipper.DisplayStart;
                size_t line_max_idx = (clipper.DisplayEnd - clipper.DisplayStart) - 1;
                size_t addr = (size_t)(line_i * Cols);
                ImGui::Text(format_address, s.AddrDigitsCount, base_display_addr + addr);
                {
                ZoneScopedN("MemoryEditor::DrawContents-DrawHexadecimal");

                // Draw Hexadecimal
                for (int colIdx = 0; colIdx < Cols && addr < mem_size; colIdx++, addr++)
                {
                    float byte_pos_x = s.PosHexStart + s.HexCellWidth * colIdx;
                    if (OptMidColsCount > 0)
                        byte_pos_x += (float)(colIdx / OptMidColsCount) * s.SpacingBetweenMidCols;
                    ImGui::SameLine(byte_pos_x);

                    Color highlight_range_color;
                    ImU8 b = mem_data[addr];
                    size_t note_idx = std::numeric_limits<size_t>::max();
                    size_t range_idx = note_idx;

                    // Draw highlight
                    bool is_highlight_from_user_range = ((addr >= HighlightMin) && (addr < HighlightMax));
                    bool is_highlight_from_user_func  = (HighlightFn && HighlightFn(mem_data, addr));
                    bool is_highlight_from_preview    = ((addr >= DataPreviewAddr) && (addr < (DataPreviewAddr + preview_data_type_size)));
                    if (is_highlight_from_user_range || is_highlight_from_user_func || is_highlight_from_preview) {
                        ZoneScopedN("MemoryEditor::DrawContents-Regular");

                        Color highlight_color;
                        highlight_color.id = HighlightColor;
                        if (IsInRange(Ranges, addr, highlight_range_color, &range_idx) != RangePosition::NotInRange) {
                            highlight_color.r = ((uint)highlight_color.r + (uint)highlight_range_color.r) / 2;
                            highlight_color.g = ((uint)highlight_color.g + (uint)highlight_range_color.g) / 2;
                            highlight_color.b = ((uint)highlight_color.b + (uint)highlight_range_color.b) / 2;
                        }

                        ImVec2 pos = ImGui::GetCursorScreenPos();
                        float highlight_width = s.GlyphWidth * 2;
                        bool is_next_byte_highlighted = isNextByteHighlighted(addr, mem_size, preview_data_type_size, range_idx, mem_data);
                        if (is_next_byte_highlighted || (colIdx + 1 == Cols)) {
                            highlight_width = s.HexCellWidth;
                            if (OptMidColsCount > 0 && colIdx > 0 && (colIdx + 1) < Cols && ((colIdx + 1) % OptMidColsCount) == 0)
                                highlight_width += s.SpacingBetweenMidCols;
                        }

                        draw_list->AddRectFilled(pos, ImVec2(pos.x + highlight_width, pos.y + s.LineHeight), highlight_color.id);
                        if (auto range_position = IsInRange(Notes, addr, highlight_range_color, &note_idx); range_position != RangePosition::NotInRange) {
                            highlight_width = s.HexCellWidth;
                            drawNoteRect(draw_list, note_idx, addr, pos, highlight_range_color, range_position, highlight_width, colIdx, line_idx, line_max_idx, s);
                        }
                    } else if (IsInRange(Ranges, addr, highlight_range_color, &range_idx) != RangePosition::NotInRange) {
                        ImVec2 pos = ImGui::GetCursorScreenPos();
                        float highlight_width = s.GlyphWidth * 2;
                        bool is_next_byte_highlighted = isNextByteHighlighted(addr, mem_size, preview_data_type_size, range_idx, mem_data);
                        if (is_next_byte_highlighted || (colIdx + 1 == Cols)) {
                            highlight_width = s.HexCellWidth;
                            if (OptMidColsCount > 0 && colIdx > 0 && (colIdx + 1) < Cols && ((colIdx + 1) % OptMidColsCount) == 0)
                                highlight_width += s.SpacingBetweenMidCols;
                        }
                        draw_list->AddRectFilled(pos, ImVec2(pos.x + highlight_width, pos.y + s.LineHeight), highlight_range_color.id);
                        if (auto range_position = IsInRange(Notes, addr, highlight_range_color, &note_idx); range_position != RangePosition::NotInRange) {
                            highlight_width = s.HexCellWidth;
                            drawNoteRect(draw_list, note_idx, addr, pos, highlight_range_color, range_position, highlight_width, colIdx, line_idx, line_max_idx, s);
                        }
                    } else if (auto range_position = IsInRange(Notes, addr, highlight_range_color, &note_idx); range_position != RangePosition::NotInRange) {
                        ImVec2 pos = ImGui::GetCursorScreenPos();
                        float highlight_width = s.HexCellWidth;
                        drawNoteRect(draw_list, note_idx, addr, pos, highlight_range_color, range_position, highlight_width, colIdx, line_idx, line_max_idx, s);
                    }

                    ImGui::Text(format_byte_space, b);

                    if (!ReadOnly && ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) {
                        DataEditingTakeFocus = true;
                        data_editing_addr_next = addr;
                    }
                }
                }
                if (OptShowAscii)
                {
                    ZoneScopedN("MemoryEditor::DrawContents-OptShowAscii");

                    // Draw ASCII values
                    ImGui::SameLine(s.PosAsciiStart);
                    ImVec2 pos = ImGui::GetCursorScreenPos();
                    addr = (size_t)(line_i * Cols);
                    ImGui::PushID(line_i);
                    if (ImGui::InvisibleButton("ascii", ImVec2(s.PosAsciiEnd - s.PosAsciiStart, s.LineHeight)))
                    {
                        DataEditingAddr = DataPreviewAddr = addr + (size_t)((ImGui::GetIO().MousePos.x - pos.x) / s.GlyphWidth);
                        DataEditingTakeFocus = true;
                    }
                    ImGui::PopID();

                    for (int n = 0; n < Cols && addr < mem_size; n++, addr++)
                    {
                        bool is_highlight_from_user_range = (addr >= HighlightMin && addr < HighlightMax);
                        bool is_highlight_from_user_func = (HighlightFn && HighlightFn(mem_data, addr));
                        bool is_highlight_from_preview = (addr >= DataPreviewAddr && addr < DataPreviewAddr + preview_data_type_size);
                        if (is_highlight_from_user_range || is_highlight_from_user_func || is_highlight_from_preview)
                        {
                            draw_list->AddRectFilled(pos, ImVec2(pos.x + s.GlyphWidth, pos.y + s.LineHeight), HighlightColor);
                        }
                        unsigned char c = ReadFn ? ReadFn(mem_data, addr) : mem_data[addr];
                        char display_c = (c < 32 || c >= 128) ? '.' : c;
                        draw_list->AddText(pos, (display_c == c) ? color_text : color_disabled, &display_c, &display_c + 1);

                        pos.x += s.GlyphWidth;
                    }
                }
            }
        }

        ImGui::PopStyleVar(2);
        ImGui::EndChild();

        // Notify the main window of our ideal child content size (FIXME: we are missing an API to get the contents size from the child)
        ImGui::SetCursorPosX(s.WindowWidth);

        if (data_next && DataEditingAddr + 1 < mem_size)
        {
            DataEditingAddr = DataPreviewAddr = DataEditingAddr + 1;
            DataEditingTakeFocus = true;
        }
        else if (data_editing_addr_next != std::numeric_limits<size_t>::max())
        {
            DataEditingAddr = DataPreviewAddr = data_editing_addr_next;
            DataEditingTakeFocus = true;
        }

        const bool lock_show_data_preview = true;
        if (OptShowOptions)
        {
            ImGui::Separator();
            DrawOptionsLine(s, mem_data, mem_size, base_display_addr);
        }

        if (lock_show_data_preview)
        {
            ImGui::Separator();
            DrawPreviewLine(s, mem_data, mem_size, base_display_addr);
        }
    }

    bool isNextByteHighlighted(size_t const addr, size_t const mem_size, size_t const preview_data_type_size, size_t const rangeIdx, ImU8 const* mem_data) {
        auto const isNotLastAddr         = (addr + 1)                  < mem_size;
        auto const highlighMaxWasSet     = HighlightMax               != std::numeric_limits<size_t>::max();
        auto const highlighFromFn        = HighlightFn                && HighlightFn(mem_data, addr + 1);
        auto const isNotLastHighlighAddr = (addr + 1)                  < HighlightMax;
        auto const highlightFromPreview  = (addr + 1)                  < (DataPreviewAddr + preview_data_type_size);
        auto const isNotLastInRange      = (rangeIdx < Ranges.size()) && ((addr + 1 ) < Ranges[rangeIdx].RangeEndAddress);
        return isNotLastAddr && ((highlighMaxWasSet && isNotLastHighlighAddr) || highlighFromFn || highlightFromPreview || isNotLastInRange);
    }

    void drawNoteRect(
        ImDrawList *draw_list,
        size_t const note_idx,
        size_t const addr,
        ImVec2 pos,
        MemoryEditor::Color const highlight_range_color,
        RangePosition const range_position,
        float highlight_width,
        int const colIdx,
        int const lineIdx,
        int const line_max_idx,
        MemoryEditor::Sizes const&s
    ) {
        constexpr float LINE_THICKNESS = 2.f;
        constexpr float HORIZONTAL_PADDING = 2.f;
        constexpr float VERTICAL_PADDING = 0.0f;// LINE_THICKNESS / 4.f;

        bool is_space_in_between = false;
        if ((OptMidColsCount > 0) && (colIdx > 0) && ((colIdx + 1) < Cols) && (((colIdx + 1) % OptMidColsCount) == 0)){
            is_space_in_between = true;
            if (range_position == RangePosition::End) {
                highlight_width -= s.SpacingBetweenMidCols;
            } else {
                highlight_width += s.SpacingBetweenMidCols;
            }
        }
        
        if (!is_space_in_between && range_position == RangePosition::End) {
            highlight_width -= HORIZONTAL_PADDING * 2;
        }
        if (!is_space_in_between && range_position == RangePosition::Start) {
            pos.x -= HORIZONTAL_PADDING;
            highlight_width += HORIZONTAL_PADDING;
        }

        // Horizontal Top
        draw_list->AddLine(ImVec2(pos.x, pos.y + VERTICAL_PADDING), ImVec2(pos.x + highlight_width, pos.y + VERTICAL_PADDING), highlight_range_color.id, LINE_THICKNESS);

        size_t const firstLineAddr = addr - colIdx;

        size_t const lastLineAddr = addr + size_t(Cols - colIdx - 1);

        bool const notRangeBelow = !hasRangeBelow(lastLineAddr, note_idx, lineIdx, colIdx, line_max_idx);

        if (is_space_in_between || notRangeBelow) {
            // Horizontal Bottom
            draw_list->AddLine(ImVec2(pos.x, pos.y + s.LineHeight - VERTICAL_PADDING), ImVec2(pos.x + highlight_width, pos.y + s.LineHeight - VERTICAL_PADDING), highlight_range_color.id, LINE_THICKNESS);
        }

        if (range_position == RangePosition::Start || (addr == firstLineAddr)) {
            // Vertical Left
            draw_list->AddLine(ImVec2(pos.x, pos.y + VERTICAL_PADDING), ImVec2(pos.x, pos.y + s.LineHeight + VERTICAL_PADDING), highlight_range_color.id, LINE_THICKNESS);
        }
        if (range_position == RangePosition::End  || (addr == lastLineAddr)) {
            // Vertical Right
            draw_list->AddLine(ImVec2(pos.x + highlight_width, pos.y + VERTICAL_PADDING), ImVec2(pos.x + highlight_width, pos.y + s.LineHeight + VERTICAL_PADDING), highlight_range_color.id, LINE_THICKNESS);
        }
    }

    inline bool hasRangeBelow(size_t lastLineAddr, size_t note_idx, size_t lineIdx, size_t colIdx, size_t line_max_idx) {
        size_t const nextLineFirstAddr = lastLineAddr + 1;
        size_t const cellAddrBellow = nextLineFirstAddr + colIdx;

        NoteRange rangesInNextLine = {};
        bool noteDoesntEndInSameLine = false;
        bool cellBelowInSameNote = false;
        if (note_idx < this->Notes.size()){
            auto const &note = this->Notes[note_idx];

            noteDoesntEndInSameLine = nextLineFirstAddr < note.RangeEndAddress;
            cellBelowInSameNote     = cellAddrBellow    < note.RangeEndAddress;

            size_t const noteStartIdx = note_idx + 1;
            for (size_t noteIdx = noteStartIdx; noteIdx < Notes.size(); noteIdx += 1) {
                auto const &note = Notes[noteIdx];
                if (nextLineFirstAddr >= note.RangeStartAddress) {
                    if (cellAddrBellow < note.RangeEndAddress) {
                        if (noteIdx == noteStartIdx) {
                            rangesInNextLine.RangeStartAddress = note.RangeStartAddress;
                        }
                        rangesInNextLine.RangeEndAddress = note.RangeEndAddress;
                        rangesInNextLine.isActive = note.isActive;
                    } else {
                        break;
                    }
                }
            }
        }

        bool const cellAddrBellowBellongToRangesInNextLine = cellAddrBellow < rangesInNextLine.RangeEndAddress;
        bool const cellAddrBellowRangeIsActive = rangesInNextLine.isActive;
        bool const hasAnotherNoteBelow = cellAddrBellowBellongToRangesInNextLine && cellAddrBellowRangeIsActive;
        bool const hasSameNoteBelow = noteDoesntEndInSameLine && cellBelowInSameNote;
        bool const hasNoteBelow = hasSameNoteBelow || hasAnotherNoteBelow;
        bool const isNotLastLine = lineIdx < line_max_idx;
        bool const hasRangeBelow = isNotLastLine && hasNoteBelow;
        return hasRangeBelow;
    }

    void DrawOptionsLine(const Sizes& s, void* mem_data, size_t mem_size, size_t base_display_addr)
    {
        IM_UNUSED(mem_data);
        ImGuiStyle& style = ImGui::GetStyle();
        const char* format_range = OptUpperCaseHex ? "Range %0*" _PRISizeT "X..%0*" _PRISizeT "X" : "Range %0*" _PRISizeT "x..%0*" _PRISizeT "x";

        // Options menu
        if (ImGui::Button("Options"))
            ImGui::OpenPopup("context");
        if (ImGui::BeginPopup("context"))
        {
            ImGui::SetNextItemWidth(s.GlyphWidth * 7 + style.FramePadding.x * 2.0f);
            if (ImGui::DragInt("##cols", &Cols, 0.2f, 4, 32, "%d cols")) { ContentsWidthChanged = true; if (Cols < 1) Cols = 1; }
            if (ImGui::Checkbox("Show Ascii", &OptShowAscii)) { ContentsWidthChanged = true; }
            ImGui::Checkbox("Grey out zeroes", &OptGreyOutZeroes);
            ImGui::Checkbox("Uppercase Hex", &OptUpperCaseHex);

            ImGui::EndPopup();
        }

        ImGui::SameLine();
        ImGui::Text(format_range, s.AddrDigitsCount, base_display_addr, s.AddrDigitsCount, base_display_addr + mem_size - 1);
        ImGui::SameLine();
        ImGui::SetNextItemWidth((s.AddrDigitsCount + 1) * s.GlyphWidth + style.FramePadding.x * 2.0f);
        if (ImGui::InputText("##addr", AddrInputBuf, IM_ARRAYSIZE(AddrInputBuf), ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue))
        {
            size_t goto_addr;
            if (sscanf(AddrInputBuf, "%" _PRISizeT "X", &goto_addr) == 1)
            {
                GotoAddr = goto_addr - base_display_addr;
                HighlightMin = HighlightMax = std::numeric_limits<size_t>::max();
            }
        }

        if (GotoAddr != std::numeric_limits<size_t>::max())
        {
            if (GotoAddr < mem_size)
            {
                ImGui::BeginChild("##scrolling");
                ImGui::SetScrollFromPosY(ImGui::GetCursorStartPos().y + (GotoAddr / Cols) * ImGui::GetTextLineHeight());
                ImGui::EndChild();
                DataEditingAddr = DataPreviewAddr = GotoAddr;
                DataEditingTakeFocus = true;
            }
            GotoAddr = std::numeric_limits<size_t>::max();
        }
    }

    void DrawPreviewLine(const Sizes& s, void* mem_data_void, size_t mem_size, size_t base_display_addr)
    {
        size_t const note_fields_count = 6;
        IM_UNUSED(base_display_addr);
        ImU8* mem_data = (ImU8*)mem_data_void;
        ImGuiStyle& style = ImGui::GetStyle();
        if (ImGui::CollapsingHeader("Preview Data")) {
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Preview as:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth((s.GlyphWidth * 10.0f) + style.FramePadding.x * 2.0f + style.ItemInnerSpacing.x);
            if (ImGui::BeginCombo("##combo_type", DataTypeGetDesc(PreviewDataType), ImGuiComboFlags_HeightLargest))
            {
                for (int n = 0; n < DataType_COUNT; n++)
                    if (ImGui::Selectable(DataTypeGetDesc(n), PreviewDataType == n))
                        PreviewDataType = (DataType_)n;
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth((s.GlyphWidth * 6.0f) + style.FramePadding.x * 2.0f + style.ItemInnerSpacing.x);
            ImGui::Combo("##combo_endianess", &PreviewEndianess, "LE\0BE\0\0");

            char buf[128] = "";
            float x = s.GlyphWidth * 6.0f;
            bool has_value = DataPreviewAddr != std::numeric_limits<size_t>::max();
            if (has_value)
                DrawPreviewData(DataPreviewAddr, mem_data, mem_size, PreviewDataType, DataFormat_Dec, buf, (size_t)IM_ARRAYSIZE(buf));
            ImGui::Text("Dec"); ImGui::SameLine(x); ImGui::TextUnformatted(has_value ? buf : "N/A");
            if (has_value)
                DrawPreviewData(DataPreviewAddr, mem_data, mem_size, PreviewDataType, DataFormat_Hex, buf, (size_t)IM_ARRAYSIZE(buf));
            ImGui::Text("Hex"); ImGui::SameLine(x); ImGui::TextUnformatted(has_value ? buf : "N/A");
            if (has_value)
                DrawPreviewData(DataPreviewAddr, mem_data, mem_size, PreviewDataType, DataFormat_Bin, buf, (size_t)IM_ARRAYSIZE(buf));
            buf[IM_ARRAYSIZE(buf) - 1] = 0;
            ImGui::Text("Bin"); ImGui::SameLine(x); ImGui::TextUnformatted(has_value ? buf : "N/A");
        }
        if (ImGui::CollapsingHeader("Converter WIP")) {
            char buf[128] = "";
            bool force_input_update = false;
            float x = s.GlyphWidth * 6.0f;
            ImGuiInputTextFlags flags = ImGuiInputTextFlags_AutoSelectAll;

            ImGui::AlignTextToFramePadding();
            ImGui::Text("From:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth((s.GlyphWidth * 10.0f) + style.FramePadding.x * 2.0f + style.ItemInnerSpacing.x);
            if (ImGui::BeginCombo("##combo_convert_format", DataFormatGetDesc(ConvertValueFormat)))
            {
                auto const prev_format = ConvertValueFormat;
                for (int n = 0; n < DataFormat_COUNT; n++)
                    if (ImGui::Selectable(DataFormatGetDesc(n), ConvertValueFormat == n))
                        ConvertValueFormat = (DataFormat)n;

                switch (ConvertValueFormat) {
                    case DataFormat_Hex: {
                        flags |= ImGuiInputTextFlags_CharsHexadecimal;
                        /* HEX view of the value */
                        if (prev_format != ConvertValueFormat)
                            ImSnprintf(ValueConverterInputBuf, sizeof(ValueConverterInputBuf), "%lX", ValueToConvert);
                        break;
                    }
                    case DataFormat_Bin:
                    case DataFormat_Dec:
                    default: {
                        flags |= ImGuiInputTextFlags_CharsDecimal;
                        /* Decimal view of the value */
                        if (prev_format != ConvertValueFormat) {
                            if ((ConvertValueType % 2) == 0) {
                                ImSnprintf(ValueConverterInputBuf, sizeof(ValueConverterInputBuf), "%ld", ValueToConvert);
                            } else {
                                ImSnprintf(ValueConverterInputBuf, sizeof(ValueConverterInputBuf), "%lu", ValueToConvert);
                            }
                        }
                        break;
                    }
                }

                force_input_update = true;
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::Text("Value:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth((s.GlyphWidth * 10.0f) + style.FramePadding.x * 2.0f + style.ItemInnerSpacing.x);
            if (ImGui::BeginCombo("##combo_convert_type", DataTypeGetDesc(ConvertValueType), ImGuiComboFlags_HeightLargest))
            {
                auto prev_type = ConvertValueType;
                for (int n = 0; n < DataType_COUNT; n++)
                    if (ImGui::Selectable(DataTypeGetDesc(n), ConvertValueType == n))
                        ConvertValueType = (DataType_)n;

                if (ConvertValueFormat == DataFormat_Hex) {
                    /* HEX is not converted */
                    ImSnprintf(ValueConverterInputBuf, sizeof(ValueConverterInputBuf), "%lX", ValueToConvert);
                } else if (prev_type >= DataType_HalfFloat) {
                    /* From Float -> SOME */
                    if (ConvertValueType <= DataType_U64) {
                        /* From Float -> Int */
                        if ((ConvertValueType % 2) == 0) {
                            ImSnprintf(ValueConverterInputBuf, sizeof(ValueConverterInputBuf), "%ld", ValueToConvert);
                        } else {
                            ImSnprintf(ValueConverterInputBuf, sizeof(ValueConverterInputBuf), "%lu", ValueToConvert);
                        }
                    }
                } else {
                    /* From Int -> SOME */
                    if (ConvertValueType <= DataType_U64) {
                        /* From Int -> Int */
                        if (((prev_type % 2) == 0) && ((ConvertValueType % 2) == 1)) {
                            /* From Int -> Uint */
                            ImSnprintf(ValueConverterInputBuf, sizeof(ValueConverterInputBuf), "%lu", ValueToConvert);
                        }
                    }
                    if (ConvertValueType >= DataType_HalfFloat) {
                        /* From Int -> Float */
                        ImSnprintf(ValueConverterInputBuf, sizeof(ValueConverterInputBuf), "%lf", *(double*)&ValueToConvert);
                    }
                }

                force_input_update = true;
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth((s.AddrDigitsCount + 1) * s.GlyphWidth + style.FramePadding.x * 2.0f);
            if (force_input_update || ImGui::InputText("##value_to_convert", ValueConverterInputBuf, IM_ARRAYSIZE(ValueConverterInputBuf), flags))
            {
                switch (ConvertValueFormat) {
                    case DataFormat_Hex: {
                        sscanf(ValueConverterInputBuf, "%" _PRISizeT "X", &ValueToConvert);
                        break;
                    }
                    case DataFormat_Bin:
                    case DataFormat_Dec:
                    default: {
                        if (ConvertValueType == DataType_HalfFloat) {
                            f32 _f16 = *(f16*)&ValueToConvert;
                            sscanf(ValueConverterInputBuf, "%f", &_f16);
                        } else if (ConvertValueType == DataType_Float) {
                            sscanf(ValueConverterInputBuf, "%f", (f32*)&ValueToConvert);
                        } else if (ConvertValueType == DataType_Double) {
                            sscanf(ValueConverterInputBuf, "%lf", (f64*) &ValueToConvert);
                        } else {
                            if ((ConvertValueType % 2) == 0) {
                                /* Read signed */
                                sscanf(ValueConverterInputBuf, "%ld", &ValueToConvert);
                            } else {
                                /* Read unsigned */
                                sscanf(ValueConverterInputBuf, "%lu", &ValueToConvert);
                            }
                        }
                        break;
                    }
                }
            }
            DrawPreviewData(ValueToConvert, nullptr, 0, ConvertValueType, DataFormat_Dec, buf, (size_t)IM_ARRAYSIZE(buf));
            ImGui::Text("Dec"); ImGui::SameLine(x); ImGui::TextUnformatted(buf);
            DrawPreviewData(ValueToConvert, nullptr, 0, ConvertValueType, DataFormat_Hex, buf, (size_t)IM_ARRAYSIZE(buf));
            ImGui::Text("Hex"); ImGui::SameLine(x); ImGui::TextUnformatted(buf);
            DrawPreviewData(ValueToConvert, nullptr, 0, ConvertValueType, DataFormat_Bin, buf, (size_t)IM_ARRAYSIZE(buf));
            ImGui::Text("Bin"); ImGui::SameLine(x); ImGui::TextUnformatted(buf);
        }
        if (ImGui::CollapsingHeader("Notes")) {
            float const TEXT_BASE_WIDTH = ImGui::CalcTextSize("A").x;
            auto const table_flags = ImGuiTableFlags_RowBg
                             | ImGuiTableFlags_Borders
                             | ImGuiTableFlags_BordersH
                             | ImGuiTableFlags_BordersOuterH
                             | ImGuiTableFlags_BordersV
                             | ImGuiTableFlags_BordersOuterV
                             | ImGuiTableFlags_SizingFixedFit;

            if (ImGui::BeginTable("##NotesTable", note_fields_count, table_flags))
            {
                ImGui::TableSetupColumn("#Add/Del");
                ImGui::TableSetupColumn("Active");
                ImGui::TableSetupColumn("Color");
                ImGui::TableSetupColumn("Start");
                ImGui::TableSetupColumn("End");
                ImGui::TableSetupColumn("Description");

                ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
                for (size_t column = 0; column < note_fields_count; column++)
                {
                    ImGui::TableSetColumnIndex(column);
                    const char* column_name = ImGui::TableGetColumnName(column); // Retrieve name passed to TableSetupColumn()
                    ImGui::PushID(column);
                    if (column == 0) {
                        if (ImGui::Button("+##add", ImVec2(TEXT_BASE_WIDTH * 4.0f, 0.0f))) {
                            Notes.push_back(NoteRange{
                                .RangeStartAddress = 0,
                                .RangeEndAddress = 1,
                                .RangeColor = DEFAULT_NOTE_COLOR,
                                .Description = "Some description",
                                .isActive = true
                            });
                        }
                    } else {
                        ImGui::TableHeader(column_name);
                    }
                    ImGui::PopID();
                }

                for (size_t row = 0; row < Notes.size(); row++) {
                    ImGui::TableNextRow();

                    char buf[16+2];
                    auto &note = Notes[row];

                    int column = 0;
                    ImGui::TableSetColumnIndex(column);
                    ImGui::PushID(row * note_fields_count + column);
                    if (ImGui::Button("Del", ImVec2(TEXT_BASE_WIDTH * 4.0f, 0.0f))) {
                        Notes.erase(Notes.begin() + row);
                    }
                    ImGui::PopID();
                    
                    ++column;
                    ImGui::TableSetColumnIndex(column);
                    ImGui::PushID(row * note_fields_count + column);
                    if (ImGui::Checkbox("##isActive", &note.isActive)) {
                    };
                    ImGui::PopID();

                    ++column;
                    ImGui::TableSetColumnIndex(column);
                    ImGui::PushID(row * note_fields_count + column);
                    float color[3] = {note.RangeColor.r/255.0f, note.RangeColor.g/255.0f, note.RangeColor.b/255.0f};
                    auto const color_edit_flags = ImGuiColorEditFlags_NoInputs
                                                | ImGuiColorEditFlags_NoLabel
                                                | ImGuiColorEditFlags_NoAlpha
                                                | ImGuiColorEditFlags_NoOptions;
                    if (ImGui::ColorEdit4("##color", (float *)color, color_edit_flags)) {
                        note.RangeColor.r = color[0] * 255;
                        note.RangeColor.g = color[1] * 255;
                        note.RangeColor.b = color[2] * 255;
                    };
                    ImGui::PopID();

                    ++column;
                    ImGui::TableSetColumnIndex(column);
                    ImGui::PushID(row * note_fields_count + column);
                    ImGui::PushItemWidth(TEXT_BASE_WIDTH * 18.0f);
                    /* InputScalar */ {
                        ImSnprintf(buf, IM_ARRAYSIZE(buf), "0x%lX", note.RangeStartAddress);
                        if (ImGui::InputText("##range_start", buf, IM_ARRAYSIZE(buf), ImGuiInputTextFlags_CharsHexadecimal)) {
                            sscanf(buf, "0x%lX", &note.RangeStartAddress);
                        }
                    }
                    ImGui::PopItemWidth();
                    ImGui::PopID();

                    ++column;
                    ImGui::TableSetColumnIndex(column);
                    ImGui::PushID(row * note_fields_count + column);
                    ImGui::PushItemWidth(TEXT_BASE_WIDTH * 18.0f);
                    /* InputScalar */ {
                        ImSnprintf(buf, IM_ARRAYSIZE(buf), "0x%lX", note.RangeEndAddress);
                        if (ImGui::InputText("##range_end", buf, IM_ARRAYSIZE(buf), ImGuiInputTextFlags_CharsHexadecimal)) {
                            sscanf(buf, "0x%lX", &note.RangeEndAddress);
                        }
                    }
                    ImGui::PopItemWidth();
                    ImGui::PopID();

                    ++column;
                    ImGui::TableSetColumnIndex(column);
                    ImGui::PushID(row * note_fields_count + column);
                    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 1.1f);
                    ImGui::InputText("##description", note.Description.data(), note.Description.size());
                    ImGui::PopItemWidth();
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
    }

    // Utilities for Data Preview
    const char* DataTypeGetDesc(int data_type) const
    {
        const char* descs[DataType_COUNT] = { "Int8", "Uint8", "Int16", "Uint16", "Int32", "Uint32", "Int64", "Uint64", "HalfFloat", "Float", "Double" };
        IM_ASSERT(data_type >= 0 && data_type < DataType_COUNT);
        return descs[data_type];
    }

    size_t DataTypeGetSize(ImGuiDataType data_type) const
    {
        static size_t const sizes[] = { 1, 1, 2, 2, 4, 4, 8, 8, sizeof(_Float16), sizeof(float), sizeof(double) };
        IM_ASSERT(data_type >= 0 && data_type < DataType_COUNT);
        return sizes[data_type];
    }

    const char* DataFormatGetDesc(int data_format) const
    {
        const char* descs[] = { "Bin", "Dec", "Hex" };
        IM_ASSERT(data_format >= 0 && data_format < DataFormat_COUNT);
        return descs[data_format];
    }

    bool IsBigEndian() const
    {
        uint16_t x = 1;
        char c[2];
        memcpy(c, &x, 2);
        return c[0] != 0;
    }

    static void* EndianessCopyBigEndian(void* _dst, void* _src, size_t s, int is_little_endian)
    {
        if (is_little_endian)
        {
            uint8_t* dst = (uint8_t*)_dst;
            uint8_t* src = (uint8_t*)_src + s - 1;
            for (int i = 0, n = (int)s; i < n; ++i)
                memcpy(dst++, src--, 1);
            return _dst;
        }
        else
        {
            return memcpy(_dst, _src, s);
        }
    }

    static void* EndianessCopyLittleEndian(void* _dst, void* _src, size_t s, int is_little_endian)
    {
        if (is_little_endian)
        {
            return memcpy(_dst, _src, s);
        }
        else
        {
            uint8_t* dst = (uint8_t*)_dst;
            uint8_t* src = (uint8_t*)_src + s - 1;
            for (int i = 0, n = (int)s; i < n; ++i)
                memcpy(dst++, src--, 1);
            return _dst;
        }
    }

    void* EndianessCopy(void* dst, void* src, size_t size) const
    {
        static void* (*fp)(void*, void*, size_t, int) = NULL;
        if (fp == NULL)
            fp = IsBigEndian() ? EndianessCopyBigEndian : EndianessCopyLittleEndian;
        return fp(dst, src, size, PreviewEndianess);
    }

    const char* FormatBinary(const uint8_t* buf, int width) const
    {
        IM_ASSERT(width <= 64);
        size_t out_n = 0;
        static char out_buf[64 + 8 + 1];
        int n = width / 8;
        for (int j = n - 1; j >= 0; --j)
        {
            for (int i = 0; i < 8; ++i)
                out_buf[out_n++] = (buf[j] & (1 << (7 - i))) ? '1' : '0';
            out_buf[out_n++] = ' ';
        }
        IM_ASSERT(out_n < IM_ARRAYSIZE(out_buf));
        out_buf[out_n] = 0;
        return out_buf;
    }

    // [Internal]
    void DrawPreviewData(size_t addr, const ImU8* mem_data, size_t mem_size, ImGuiDataType data_type, DataFormat data_format, char* out_buf, size_t out_buf_size) const
    {
        uint8_t buf[8];
        size_t elem_size = DataTypeGetSize(data_type);
        size_t size = addr + elem_size > mem_size ? mem_size - addr : elem_size;
        if (mem_data != nullptr) {
            if (ReadFn)
                for (int i = 0, n = (int)size; i < n; ++i)
                    buf[i] = ReadFn(mem_data, addr + i);
            else
                memcpy(buf, mem_data + addr, size);
        } else {
            size = elem_size;
            memcpy(buf, (void*)&addr, size);
        }

        if (data_format == DataFormat_Bin)
        {
            uint8_t binbuf[8];
            EndianessCopy(binbuf, buf, size);
            ImSnprintf(out_buf, out_buf_size, "%s", FormatBinary(binbuf, (int)size * 8));
            return;
        }

        out_buf[0] = 0;
        switch (data_type)
        {
        case DataType_S8:
        {
            int8_t int8 = 0;
            EndianessCopy(&int8, buf, size);
            if (data_format == DataFormat_Dec) { ImSnprintf(out_buf, out_buf_size, "%hhd", int8); return; }
            if (data_format == DataFormat_Hex) { ImSnprintf(out_buf, out_buf_size, "0x%02x", int8 & 0xFF); return; }
            break;
        }
        case DataType_U8:
        {
            uint8_t uint8 = 0;
            EndianessCopy(&uint8, buf, size);
            if (data_format == DataFormat_Dec) { ImSnprintf(out_buf, out_buf_size, "%hhu", uint8); return; }
            if (data_format == DataFormat_Hex) { ImSnprintf(out_buf, out_buf_size, "0x%02x", uint8 & 0XFF); return; }
            break;
        }
        case DataType_S16:
        {
            int16_t int16 = 0;
            EndianessCopy(&int16, buf, size);
            if (data_format == DataFormat_Dec) { ImSnprintf(out_buf, out_buf_size, "%hd", int16); return; }
            if (data_format == DataFormat_Hex) { ImSnprintf(out_buf, out_buf_size, "0x%04x", int16 & 0xFFFF); return; }
            break;
        }
        case DataType_U16:
        {
            uint16_t uint16 = 0;
            EndianessCopy(&uint16, buf, size);
            if (data_format == DataFormat_Dec) { ImSnprintf(out_buf, out_buf_size, "%hu", uint16); return; }
            if (data_format == DataFormat_Hex) { ImSnprintf(out_buf, out_buf_size, "0x%04x", uint16 & 0xFFFF); return; }
            break;
        }
        case DataType_S32:
        {
            int32_t int32 = 0;
            EndianessCopy(&int32, buf, size);
            if (data_format == DataFormat_Dec) { ImSnprintf(out_buf, out_buf_size, "%d", int32); return; }
            if (data_format == DataFormat_Hex) { ImSnprintf(out_buf, out_buf_size, "0x%08x", int32); return; }
            break;
        }
        case DataType_U32:
        {
            uint32_t uint32 = 0;
            EndianessCopy(&uint32, buf, size);
            if (data_format == DataFormat_Dec) { ImSnprintf(out_buf, out_buf_size, "%u", uint32); return; }
            if (data_format == DataFormat_Hex) { ImSnprintf(out_buf, out_buf_size, "0x%08x", uint32); return; }
            break;
        }
        case DataType_S64:
        {
            int64_t int64 = 0;
            EndianessCopy(&int64, buf, size);
            if (data_format == DataFormat_Dec) { ImSnprintf(out_buf, out_buf_size, "%lld", (long long)int64); return; }
            if (data_format == DataFormat_Hex) { ImSnprintf(out_buf, out_buf_size, "0x%016llx", (long long)int64); return; }
            break;
        }
        case DataType_U64:
        {
            uint64_t uint64 = 0;
            EndianessCopy(&uint64, buf, size);
            if (data_format == DataFormat_Dec) { ImSnprintf(out_buf, out_buf_size, "%llu", (long long)uint64); return; }
            if (data_format == DataFormat_Hex) { ImSnprintf(out_buf, out_buf_size, "0x%016llx", (long long)uint64); return; }
            break;
        }
        case DataType_HalfFloat:
        {
            _Float16 float16 = 0;
            EndianessCopy(&float16, buf, size);
            float float32 = float16;
            if (data_format == DataFormat_Dec) { ImSnprintf(out_buf, out_buf_size, "%f", float32); return; }
            if (data_format == DataFormat_Hex) { ImSnprintf(out_buf, out_buf_size, "%a", float32); return; }
            break;
        }
        case DataType_Float:
        {
            float float32 = 0.0f;
            EndianessCopy(&float32, buf, size);
            if (data_format == DataFormat_Dec) { ImSnprintf(out_buf, out_buf_size, "%f", float32); return; }
            if (data_format == DataFormat_Hex) { ImSnprintf(out_buf, out_buf_size, "%a", float32); return; }
            break;
        }
        case DataType_Double:
        {
            double float64 = 0.0;
            EndianessCopy(&float64, buf, size);
            if (data_format == DataFormat_Dec) { ImSnprintf(out_buf, out_buf_size, "%f", float64); return; }
            if (data_format == DataFormat_Hex) { ImSnprintf(out_buf, out_buf_size, "%a", float64); return; }
            break;
        }
        case DataType_COUNT:
            break;
        } // Switch
        IM_ASSERT(0); // Shouldn't reach
    }
};

#undef _PRISizeT
#undef ImSnprintf

#ifdef _MSC_VER
#pragma warning (pop)
#endif
