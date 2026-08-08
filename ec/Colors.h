// Colors.h - the one palette the whole manager draws with.
//
// These values lived in five separate files, identical in each, which meant a
// colour could only be changed by finding every copy and hoping none was
// missed - and a change made in one place quietly left the editor and the
// viewer in the old scheme. One definition, one place to edit.
//
// The palette itself is the classic 16-colour set a Norton-style manager is
// expected to look like: full-intensity foreground on a blue field, with the
// bright variants (85 rather than 0 in the low channel) for text that has to
// stay legible against it.
#pragma once

// panel and dialog field
#define COLOR_BLUE_R 0
#define COLOR_BLUE_G 0
#define COLOR_BLUE_B 170

// borders, titles, the descriptions in the function-key legend
#define COLOR_YELLOW_R 255
#define COLOR_YELLOW_G 255
#define COLOR_YELLOW_B 85

// file names, ordinary dialog text
#define COLOR_WHITE_R 255
#define COLOR_WHITE_G 255
#define COLOR_WHITE_B 255

// hints along the bottom of a dialog
#define COLOR_CYAN_R 85
#define COLOR_CYAN_G 255
#define COLOR_CYAN_B 255

// secondary text: sizes, dates, entries that are present but not the point
#define COLOR_GRAY_R 170
#define COLOR_GRAY_G 170
#define COLOR_GRAY_B 170

// the key names in the legend
#define COLOR_RED_R 170
#define COLOR_RED_G 0
#define COLOR_RED_B 0

// editor: the marker for a modified buffer
#define COLOR_GREEN_R 0
#define COLOR_GREEN_G 170
#define COLOR_GREEN_B 0

// the legend bar, and the ground the screen is cleared to
#define COLOR_BLACK_R 0
#define COLOR_BLACK_G 0
#define COLOR_BLACK_B 0
