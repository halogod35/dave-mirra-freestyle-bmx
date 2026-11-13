/*
 * save.h - SAVE.DAT blocks as they live in memory (from docs/formats/SAVE.md).
 *
 * memcard_LoadSave 0x00412190 copies the file body into three globals:
 *   file 0x0008 (0xC3C)  -> g_saveBlockA   0x006955E0  SaveBlockA
 *   file 0x0C44 (0x1680) -> g_saveRecords  0x00551FD8  SaveRecordEntry[12][24]
 *   file 0x22C4 (0x98)   -> g_saveProfiles 0x006952A0  SaveProfile[2]
 * [V] offsets/sizes (SAVE.md); field names are the `dmbmx save` names; [I] where SAVE.md says I.
 * Lines between GHIDRA-SKIP markers and preprocessor lines are not parsed by Ghidra (ImportTypes.java).
 */
#ifndef SAVE_H
#define SAVE_H
#include "dmtypes.h"

/* One challenge-level goal block inside a rider record (39 bytes). */
typedef struct SaveGoalInfo {
    uint8_t  byte0;            /* +0x00 unknown, 0 in the real save [I] */
    uint8_t  tier;             /* +0x01 0 amateur, 1 pro, 2 hardcore */
    uint8_t  tier_goals_done;  /* +0x02 4 = tier complete */
    uint8_t  goal[3][4][3];    /* +0x03 [tier][goal]: b0 0x80 done/0x40 this session; b1..2 objective nibbles */
} SaveGoalInfo;                /* 0x27 */

/* Career record, one per rider (0x104 bytes). */
typedef struct SaveRiderRecord {
    uint32_t     flags;            /* +0x00 bit0 all levels, bit1 hardcore, 4<<park, 0x40/0x80/0x100 notices */
    uint8_t      levels_unlocked;  /* +0x04 1..12 */
    uint8_t      styles_unlocked;  /* +0x05 1..5 */
    uint8_t      bikes_unlocked;   /* +0x06 1..5 */
    SaveGoalInfo goals[6];         /* +0x07 challenge levels 0..5 */
    uint8_t      comp_result[6];   /* +0xF1 competition levels 6..11, 3 - placing [I] */
    uint8_t      tricks_landed[11];/* +0xF7 88-bit set */
    uint8_t      unk_102[2];       /* +0x102 unknown */
} SaveRiderRecord;                 /* 0x104 */

/* Block A: options + 12 rider records + tail (0xC3C bytes, global 0x006955E0). */
typedef struct SaveBlockA {
    uint16_t        cheats_unlocked;  /* +0x000 bit c = cheat c in the CHEATS menu */
    uint16_t        cheats_active;    /* +0x002 [I] */
    uint8_t         volume_0c;        /* +0x004 [I] */
    uint8_t         volume_0d;        /* +0x005 [I] */
    uint8_t         music_track;      /* +0x006 */
    uint8_t         riders_unlocked;  /* +0x007 */
    SaveRiderRecord riders[12];       /* +0x008 (global 0x006955E8) */
    uint8_t         language;         /* +0xC38 (global 0x00696218) [I] */
    uint8_t         pad_c39[3];       /* +0xC39 */
} SaveBlockA;                         /* 0xC3C */

/* Block B entry: one record/score (20 bytes); g_saveRecords[level][id & 0xFFF]. */
typedef struct SaveRecordEntry {
    uint32_t value;      /* +0x00 */
    uint8_t  rider;      /* +0x04 [I] rider index */
    uint8_t  b5;         /* +0x05 */
    uint8_t  b6;         /* +0x06 */
    char     name[13];   /* +0x07 NUL-padded */
} SaveRecordEntry;       /* 0x14 */

/* Block C: player profile (0x4C bytes, 2 of them). */
typedef struct SaveProfile {
    uint8_t  flags;        /* +0x00 bit1 tutorial_hints [I] */
    uint8_t  index;        /* +0x01 */
    uint8_t  unk_02;       /* +0x02 */
    uint8_t  rider;        /* +0x03 0..11 */
    uint8_t  style;        /* +0x04 0..4 */
    uint8_t  bike;         /* +0x05 0..4 (4 = secret bike) */
    uint8_t  camera;       /* +0x06 0 behind .. 3 first person */
    char     name[29];     /* +0x07 */
    uint32_t control[7];   /* +0x24 button masks [I] */
    uint8_t  unk_40[12];   /* +0x40 */
} SaveProfile;             /* 0x4C */

/* GHIDRA-SKIP-BEGIN */
typedef char save_h_size_check[
    (sizeof(SaveGoalInfo) == 0x27 && sizeof(SaveRiderRecord) == 0x104 && sizeof(SaveBlockA) == 0xC3C &&
     sizeof(SaveRecordEntry) == 0x14 && sizeof(SaveProfile) == 0x4C) ? 1 : -1];
extern SaveBlockA      g_saveBlockA;           /* 0x006955E0 */
extern SaveRecordEntry g_saveRecords[12][24];  /* 0x00551FD8 */
extern SaveProfile     g_saveProfiles[2];      /* 0x006952A0 */
/* GHIDRA-SKIP-END */

#endif /* SAVE_H */
