#pragma once

// acvfidelity.ini next to the game's exe.

#include <string>
#include <utility>
#include <vector>

void config_open();
float config_float(const char *section, const char *key, float fallback);
void config_set(const char *section, const char *key, float value);
void config_set(const char *section, const char *key, int value);
// A section's key=value lines (comments left out). False when the section is not there.
bool config_section(const char *section, std::vector<std::pair<std::string, std::string>> &lines);
void config_remove_section(const char *section);
void config_remove_key(const char *section, const char *key);
