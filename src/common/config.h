#pragma once

// acvfidelity.ini next to the game's exe.

void config_open();
float config_float(const char *section, const char *key, float fallback);
void config_set(const char *section, const char *key, float value);
void config_set(const char *section, const char *key, int value);
