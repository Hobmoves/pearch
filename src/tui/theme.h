#pragma once

// pearch's palette: a pear-lime accent and a warm gold, on neutral slate.

#include <cstdint>
#include <string>
#include <string_view>

#include "core/text.h"
#include "engine/index_store.h"

namespace tui::theme {

constexpr uint32_t kText = 0xE6E9EF;
constexpr uint32_t kMuted = 0x9AA3B2;
constexpr uint32_t kFaint = 0x5B6475;
constexpr uint32_t kBorder = 0x3A4254;
constexpr uint32_t kBorderFocus = 0x6E7F4A;
constexpr uint32_t kLime = 0xB5E655;
constexpr uint32_t kGold = 0xF5C451;
constexpr uint32_t kLeaf = 0x63B84A;
constexpr uint32_t kStem = 0x8A6A4F;
constexpr uint32_t kSelBg = 0x252B39;
constexpr uint32_t kMatch = 0xF7D774;
constexpr uint32_t kError = 0xFF7B72;
constexpr uint32_t kOk = 0x9BDB6A;

struct Badge {
    std::string label;
    uint32_t color;
};

// Short coloured type label for an entry: "PDF", "DIR", "APP", ...
inline Badge badge(const engine::IndexStore& s, uint32_t id) {
    const uint8_t f = s.flags(id);
    if (f & engine::kApp) return {"APP", kLime};
    if (f & engine::kDir) return {f & engine::kLink ? "LINK" : "DIR", kGold};
    std::string_view ext = text::extension_of(s.folded(id));
    struct Kind { const char* exts; uint32_t color; };
    static const Kind kinds[] = {
        {" pdf doc docx odt rtf txt md tex epub pages ", 0x6CB6FF},
        {" xls xlsx csv ods numbers tsv ", 0x57D38C},
        {" ppt pptx odp key ", 0xFF9E64},
        {" png jpg jpeg gif bmp svg webp heic ico tif tiff psd raw avif ", 0xD291FF},
        {" mp3 wav flac ogg m4a aac wma opus mid ", 0xFF7AB6},
        {" mp4 mkv mov avi webm wmv m4v flv ", 0xFF6B6B},
        {" zip rar 7z tar gz bz2 xz tgz iso cab zst ", 0xE0AF68},
        {" exe msi bat cmd ps1 sh com appx msix dll sys ", 0xB5E655},
        {" c cc cpp cxx h hpp hxx cs java kt py rb rs go js jsx ts tsx json html htm css scss "
         "xml yml yaml toml ini cfg lua php swift sql vue ipynb cmake ", 0x7DCFFF},
    };
    std::string key = " " + std::string(ext) + " ";
    uint32_t color = kMuted;
    if (!ext.empty()) {
        for (const auto& k : kinds)
            if (std::string_view(k.exts).find(key) != std::string_view::npos) { color = k.color; break; }
    }
    std::string label;
    if (ext.empty() || ext.size() > 4) label = "FILE";
    else for (char c : ext) label.push_back(static_cast<char>((c >= 'a' && c <= 'z') ? c - 32 : c));
    return {label, color};
}

// The name to show: shortcuts drop their ".lnk", like Windows does.
inline std::string_view display_name(const engine::IndexStore& s, uint32_t id) {
    std::string_view n = s.name(id);
    if (s.flags(id) & engine::kApp) {
        std::string_view ext = text::extension_of(s.folded(id));
        if (ext == "lnk" || ext == "url" || ext == "appref-ms" || ext == "desktop")
            n = n.substr(0, n.size() - ext.size() - 1);
    }
    return n;
}

}  // namespace tui::theme
