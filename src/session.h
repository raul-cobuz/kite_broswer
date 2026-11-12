#pragma once

#include <glib.h>

#include <string>
#include <vector>

struct SessionState {
    std::vector<std::string> urls;
    std::vector<bool> pinned;
    int active_index = 0;
};

// Reads/writes ~/.config/kite/session.ini.
bool session_load(SessionState *state);
void session_save(const SessionState &state);
void session_clear();
