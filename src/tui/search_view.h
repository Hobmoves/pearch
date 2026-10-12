#pragma once

// The interactive, type-as-you-search screen.

#include <functional>
#include <memory>
#include <string>

#include "engine/history.h"
#include "engine/index_store.h"
#include "engine/searcher.h"
#include "fs/scanner.h"
#include "platform/console.h"

namespace tui {

struct SearchSession {
    std::shared_ptr<const engine::IndexStore> store;
    std::string query;                 // initial text
    uint32_t scope = engine::kNone;    // limit to this folder
    std::string scope_label;
    engine::History* history = nullptr;
    std::function<void()> save_history;
    // Rebuilds the index in the background (nullptr: refresh unavailable).
    std::function<std::shared_ptr<engine::IndexStore>(scan::Progress&)> rebuild;
    std::function<void(const engine::IndexStore&)> on_rebuilt;
    bool refresh_now = false;          // start a background refresh right away
    std::string home;                  // shown as "~"
};

int run_search_view(term::Terminal& t, term::ColorMode mode, SearchSession& s);

}  // namespace tui
