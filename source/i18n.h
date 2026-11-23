#pragma once

// Tiny runtime dictionary: Russian strings are the source keys, English is
// the translation. Missing keys fall back to Russian.
extern bool g_english;
const char* tr(const char* ru);
