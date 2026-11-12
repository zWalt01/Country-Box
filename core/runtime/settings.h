// Country-Box: the player-facing settings model (runtime/settings.c). One table of rows, owned by the runtime, drives
// the in-game settings menu (ui/menu.cpp), the rows added to the game's own Options screen, and the launcher through
// the command line (--settings-dump prints it as JSON, --settings-set changes it), so they cannot disagree. Each row
// stores into one cfg key (cfg.h, mp4.ini next to the executable): the menu and the launcher write the same file.
//
// A row belongs to some games (the table says which), has a stable public key ("video.display_mode"), a label and a
// description (ui_text.c ids), options with public values (each may be unavailable, with the reason), the default for
// this machine (Steam Deck LCD, Steam Deck OLED, PC), whether a change needs a restart, and may be unavailable as a
// whole (with the reason).
#pragma once
#include <stddef.h>

#include "ui_text.h"

enum { SETTINGS_GAME_DKCR = 1, SETTINGS_GAME_DKCTF = 2, SETTINGS_GAME_ALL = 3 };
enum { SETTINGS_PC, SETTINGS_DECK_LCD, SETTINGS_DECK_OLED };
typedef enum { SETTING_CHOICE, SETTING_RANGE } SettingKind;

#define SETTING_LITERAL ((UiTextId)-1)   // an option whose label is its literal text (a size, a count)

typedef struct {
    const char *value;          // public value (the launcher's); for a literal label also the text shown
    const char *stored;         // what goes into the cfg key; "" leaves the key unset (the consumer's automatic)
    UiTextId text;              // label; SETTING_LITERAL: the literal (or value)
    const char *literal;        // label when text is SETTING_LITERAL; NULL: value
    UiTextId unavailable;       // reason it cannot be chosen; SETTING_LITERAL: available
} SettingOption;

typedef struct SettingRow {
    const char *key;            // public, stable: "<group>.<name>"
    const char *cfg;            // the cfg key it stores into, MP4_ prefixed
    const char *group;          // "video", "sound", "controls", "interface"
    unsigned games;             // SETTINGS_GAME_* mask
    SettingKind kind;
    UiTextId label, desc;
    const SettingOption *opts;  // SETTING_CHOICE
    int n_opts;
    float lo, hi, step;         // SETTING_RANGE
    const char *unit;           // SETTING_RANGE: "%" or ""
    // (whether a change needs a restart is the cfg row's: cfg_meta(cfg)->live)
    // default per machine (public value); NULL: the PC's
    const char *def_pc, *def_deck_lcd, *def_deck_oled;
    UiTextId unavailable;       // the whole row; SETTING_LITERAL: available
} SettingRow;

// The game this binary runs (exefs main.npdm program id), SETTINGS_GAME_DKCR when unknown; the machine (DMI product
// name: Jupiter, Galileo; CB_MACHINE=pc|deck-lcd|deck-oled overrides).
unsigned settings_game(void);
int settings_machine(void);
const char *settings_machine_name(int machine);

// The rows of this game in table order; NULL past the last. settings_find: by public key or cfg key (NULL: none or
// not this game's).
const SettingRow *settings_row(int i);
const SettingRow *settings_find(const char *key);
// The public default of row on this machine.
const char *settings_default(const SettingRow *r);
// The default to store for cfg key name (MP4_ prefixed) on this machine, when it differs from the cfg table's;
// NULL: the cfg table's own (cfg.c cfg_value).
const char *settings_cfg_default(const char *name);
// The public value in effect (env or mp4.ini, else the default) into out.
const char *settings_value(const SettingRow *r, char *out, size_t cap);
// The option of a choice row whose public value is v (NULL none); the option in effect.
const SettingOption *settings_option(const SettingRow *r, const char *v);
const SettingOption *settings_current(const SettingRow *r);
// Short names for places with little room (the game's own Options rows): the row's label, an option's name (NULL: none
// shorter than the label).
const char *settings_short_label(const SettingRow *r);
const char *settings_short_value(const SettingRow *r, const SettingOption *o);
// Checks v for row r: NULL when it can be set, else why not (English, for the CLI). On success stores into the cfg
// key (cfg_set; "default" resets it); the caller saves (cfg_save).
const char *settings_set(const SettingRow *r, const char *v);

// The command line (main.c, before anything else starts): --settings-dump [--settings-lang en|ru] prints the model
// as JSON on stdout; --settings-set key=value ... checks every pair, then stores and saves them all, or none, and
// prints the result as JSON. CB_SETTINGS_FILE=<path> uses that file in place of mp4.ini next to the executable.
// Returns the exit code: 0 done, 2 a bad pair or usage, 1 the file could not be written.
int settings_cli(int argc, char **argv);
