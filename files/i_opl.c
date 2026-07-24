//
// i_opl.c -- OPL2 music backend for BuddyDoom.
//
// A self-contained, *passive* OPL music player. It is ported from
// Chocolate/Crispy-Doom's i_oplmusic.c (the GENMIDI instrument parser, the
// 9-voice allocator, the SetVoiceInstrument / SetVoiceVolume / SetVoicePan /
// FrequencyForVoice / VoiceKeyOn / VoiceKeyOff logic and the frequency_curve /
// volume_mapping_table tables are reproduced as faithfully as possible), but
// with Crispy's OPL hardware abstraction (opl.c / opl_queue.c, the SDL audio
// callback, the MIDI-file timer machinery) stripped out. In its place a single
// Nuked-OPL3 emulator instance (opl3.c) is driven directly: every Crispy
// OPL_WriteRegister(reg,val) becomes WR(reg,val) -> OPL3_WriteRegBuffered, and
// audio is produced on demand via OPL3_GenerateStream. The caller (i_mus.c)
// pushes MIDI-style events (program/noteon/noteoff/volume/bend) and pulls
// rendered S16 interleaved-stereo samples; this module owns no timer, no SDL,
// and no song scheduling. OPL2-only (9 voices, array 0) since DOOM music is
// OPL2.
//

#include <stdint.h>
#include <string.h>

#include "i_opl.h"
#include "opl3.h"

//
// Nuked-OPL3 hardware abstraction: one chip, register writes via WR().
//
static opl3_chip chip;

static void WR(unsigned int reg, unsigned int val)
{
    OPL3_WriteRegBuffered(&chip, (uint16_t) reg, (uint8_t) val);
}

//
// OPL register-offset constants (from Crispy's opl/opl.h).
//
#define OPL_NUM_VOICES      9

#define OPL_REG_WAVEFORM_ENABLE   0x01
#define OPL_REG_FM_MODE           0x08

// Operator registers (21 of each):
#define OPL_REGS_TREMOLO          0x20
#define OPL_REGS_LEVEL            0x40
#define OPL_REGS_ATTACK           0x60
#define OPL_REGS_SUSTAIN          0x80
#define OPL_REGS_WAVEFORM         0xE0

// Voice registers (9 of each):
#define OPL_REGS_FREQ_1           0xA0
#define OPL_REGS_FREQ_2           0xB0
#define OPL_REGS_FEEDBACK         0xC0

//
// GENMIDI layout.
//
#define GENMIDI_NUM_INSTRS      128
#define GENMIDI_NUM_PERCUSSION  47
#define GENMIDI_HEADER          "#OPL_II#"
#define GENMIDI_FLAG_FIXED      0x0001  // fixed pitch
#define GENMIDI_FLAG_2VOICE     0x0004  // double voice (OPL3)

#define MIDI_CHANNELS_PER_TRACK 16

// GENMIDI records are byte-exact little-endian on disk; pack tightly.
#pragma pack(push, 1)

typedef struct
{
    uint8_t tremolo;
    uint8_t attack;
    uint8_t sustain;
    uint8_t waveform;
    uint8_t scale;
    uint8_t level;
} genmidi_op_t;

typedef struct
{
    genmidi_op_t modulator;     // 6
    uint8_t feedback;           // 1
    genmidi_op_t carrier;       // 6
    uint8_t unused;             // 1
    int16_t base_note_offset;   // 2  (little-endian)
} genmidi_voice_t;              // = 16 bytes

typedef struct
{
    uint16_t flags;             // 2  (little-endian)
    uint8_t fine_tuning;        // 1
    uint8_t fixed_note;         // 1
    genmidi_voice_t voices[2];  // 32
} genmidi_instr_t;              // = 36 bytes

#pragma pack(pop)

//
// Per-channel playing state.
//
typedef struct
{
    genmidi_instr_t *instrument;
    int volume;
    int volume_base;
    int pan;
    int bend;
} opl_channel_data_t;

//
// Per-voice state.
//
typedef struct opl_voice_s opl_voice_t;
struct opl_voice_s
{
    int index;                          // 0..8
    int op1, op2;                       // operator offsets
    int array;                          // OPL3 bank (always 0 here)
    genmidi_instr_t *current_instr;
    unsigned int current_instr_voice;
    opl_channel_data_t *channel;
    unsigned int key;                   // midi key playing
    unsigned int note;                  // note (may differ for fixed-pitch)
    unsigned int freq;
    unsigned int note_volume;
    unsigned int car_volume;
    unsigned int mod_volume;
    unsigned int reg_pan;
    unsigned int priority;
};

// Operators used by the different voices (Crispy voice_operators).
static const int voice_operators[2][OPL_NUM_VOICES] = {
    { 0x00, 0x01, 0x02, 0x08, 0x09, 0x0a, 0x10, 0x11, 0x12 },
    { 0x03, 0x04, 0x05, 0x0b, 0x0c, 0x0d, 0x13, 0x14, 0x15 }
};

// Frequency values to use for each note (verbatim from Crispy).
static const unsigned short frequency_curve[] = {

    0x133, 0x133, 0x134, 0x134, 0x135, 0x136, 0x136, 0x137,   // -1
    0x137, 0x138, 0x138, 0x139, 0x139, 0x13a, 0x13b, 0x13b,
    0x13c, 0x13c, 0x13d, 0x13d, 0x13e, 0x13f, 0x13f, 0x140,
    0x140, 0x141, 0x142, 0x142, 0x143, 0x143, 0x144, 0x144,

    0x145, 0x146, 0x146, 0x147, 0x147, 0x148, 0x149, 0x149,   // -2
    0x14a, 0x14a, 0x14b, 0x14c, 0x14c, 0x14d, 0x14d, 0x14e,
    0x14f, 0x14f, 0x150, 0x150, 0x151, 0x152, 0x152, 0x153,
    0x153, 0x154, 0x155, 0x155, 0x156, 0x157, 0x157, 0x158,

    // These are used for the first seven MIDI note values:

    0x158, 0x159, 0x15a, 0x15a, 0x15b, 0x15b, 0x15c, 0x15d,   // 0
    0x15d, 0x15e, 0x15f, 0x15f, 0x160, 0x161, 0x161, 0x162,
    0x162, 0x163, 0x164, 0x164, 0x165, 0x166, 0x166, 0x167,
    0x168, 0x168, 0x169, 0x16a, 0x16a, 0x16b, 0x16c, 0x16c,

    0x16d, 0x16e, 0x16e, 0x16f, 0x170, 0x170, 0x171, 0x172,   // 1
    0x172, 0x173, 0x174, 0x174, 0x175, 0x176, 0x176, 0x177,
    0x178, 0x178, 0x179, 0x17a, 0x17a, 0x17b, 0x17c, 0x17c,
    0x17d, 0x17e, 0x17e, 0x17f, 0x180, 0x181, 0x181, 0x182,

    0x183, 0x183, 0x184, 0x185, 0x185, 0x186, 0x187, 0x188,   // 2
    0x188, 0x189, 0x18a, 0x18a, 0x18b, 0x18c, 0x18d, 0x18d,
    0x18e, 0x18f, 0x18f, 0x190, 0x191, 0x192, 0x192, 0x193,
    0x194, 0x194, 0x195, 0x196, 0x197, 0x197, 0x198, 0x199,

    0x19a, 0x19a, 0x19b, 0x19c, 0x19d, 0x19d, 0x19e, 0x19f,   // 3
    0x1a0, 0x1a0, 0x1a1, 0x1a2, 0x1a3, 0x1a3, 0x1a4, 0x1a5,
    0x1a6, 0x1a6, 0x1a7, 0x1a8, 0x1a9, 0x1a9, 0x1aa, 0x1ab,
    0x1ac, 0x1ad, 0x1ad, 0x1ae, 0x1af, 0x1b0, 0x1b0, 0x1b1,

    0x1b2, 0x1b3, 0x1b4, 0x1b4, 0x1b5, 0x1b6, 0x1b7, 0x1b8,   // 4
    0x1b8, 0x1b9, 0x1ba, 0x1bb, 0x1bc, 0x1bc, 0x1bd, 0x1be,
    0x1bf, 0x1c0, 0x1c0, 0x1c1, 0x1c2, 0x1c3, 0x1c4, 0x1c4,
    0x1c5, 0x1c6, 0x1c7, 0x1c8, 0x1c9, 0x1c9, 0x1ca, 0x1cb,

    0x1cc, 0x1cd, 0x1ce, 0x1ce, 0x1cf, 0x1d0, 0x1d1, 0x1d2,   // 5
    0x1d3, 0x1d3, 0x1d4, 0x1d5, 0x1d6, 0x1d7, 0x1d8, 0x1d8,
    0x1d9, 0x1da, 0x1db, 0x1dc, 0x1dd, 0x1de, 0x1de, 0x1df,
    0x1e0, 0x1e1, 0x1e2, 0x1e3, 0x1e4, 0x1e5, 0x1e5, 0x1e6,

    0x1e7, 0x1e8, 0x1e9, 0x1ea, 0x1eb, 0x1ec, 0x1ed, 0x1ed,   // 6
    0x1ee, 0x1ef, 0x1f0, 0x1f1, 0x1f2, 0x1f3, 0x1f4, 0x1f5,
    0x1f6, 0x1f6, 0x1f7, 0x1f8, 0x1f9, 0x1fa, 0x1fb, 0x1fc,
    0x1fd, 0x1fe, 0x1ff, 0x200, 0x201, 0x201, 0x202, 0x203,

    // First note of looped range used for all octaves:

    0x204, 0x205, 0x206, 0x207, 0x208, 0x209, 0x20a, 0x20b,   // 7
    0x20c, 0x20d, 0x20e, 0x20f, 0x210, 0x210, 0x211, 0x212,
    0x213, 0x214, 0x215, 0x216, 0x217, 0x218, 0x219, 0x21a,
    0x21b, 0x21c, 0x21d, 0x21e, 0x21f, 0x220, 0x221, 0x222,

    0x223, 0x224, 0x225, 0x226, 0x227, 0x228, 0x229, 0x22a,   // 8
    0x22b, 0x22c, 0x22d, 0x22e, 0x22f, 0x230, 0x231, 0x232,
    0x233, 0x234, 0x235, 0x236, 0x237, 0x238, 0x239, 0x23a,
    0x23b, 0x23c, 0x23d, 0x23e, 0x23f, 0x240, 0x241, 0x242,

    0x244, 0x245, 0x246, 0x247, 0x248, 0x249, 0x24a, 0x24b,   // 9
    0x24c, 0x24d, 0x24e, 0x24f, 0x250, 0x251, 0x252, 0x253,
    0x254, 0x256, 0x257, 0x258, 0x259, 0x25a, 0x25b, 0x25c,
    0x25d, 0x25e, 0x25f, 0x260, 0x262, 0x263, 0x264, 0x265,

    0x266, 0x267, 0x268, 0x269, 0x26a, 0x26c, 0x26d, 0x26e,   // 10
    0x26f, 0x270, 0x271, 0x272, 0x273, 0x275, 0x276, 0x277,
    0x278, 0x279, 0x27a, 0x27b, 0x27d, 0x27e, 0x27f, 0x280,
    0x281, 0x282, 0x284, 0x285, 0x286, 0x287, 0x288, 0x289,

    0x28b, 0x28c, 0x28d, 0x28e, 0x28f, 0x290, 0x292, 0x293,   // 11
    0x294, 0x295, 0x296, 0x298, 0x299, 0x29a, 0x29b, 0x29c,
    0x29e, 0x29f, 0x2a0, 0x2a1, 0x2a2, 0x2a4, 0x2a5, 0x2a6,
    0x2a7, 0x2a9, 0x2aa, 0x2ab, 0x2ac, 0x2ae, 0x2af, 0x2b0,

    0x2b1, 0x2b2, 0x2b4, 0x2b5, 0x2b6, 0x2b7, 0x2b9, 0x2ba,   // 12
    0x2bb, 0x2bd, 0x2be, 0x2bf, 0x2c0, 0x2c2, 0x2c3, 0x2c4,
    0x2c5, 0x2c7, 0x2c8, 0x2c9, 0x2cb, 0x2cc, 0x2cd, 0x2ce,
    0x2d0, 0x2d1, 0x2d2, 0x2d4, 0x2d5, 0x2d6, 0x2d8, 0x2d9,

    0x2da, 0x2dc, 0x2dd, 0x2de, 0x2e0, 0x2e1, 0x2e2, 0x2e4,   // 13
    0x2e5, 0x2e6, 0x2e8, 0x2e9, 0x2ea, 0x2ec, 0x2ed, 0x2ee,
    0x2f0, 0x2f1, 0x2f2, 0x2f4, 0x2f5, 0x2f6, 0x2f8, 0x2f9,
    0x2fb, 0x2fc, 0x2fd, 0x2ff, 0x300, 0x302, 0x303, 0x304,

    0x306, 0x307, 0x309, 0x30a, 0x30b, 0x30d, 0x30e, 0x310,   // 14
    0x311, 0x312, 0x314, 0x315, 0x317, 0x318, 0x31a, 0x31b,
    0x31c, 0x31e, 0x31f, 0x321, 0x322, 0x324, 0x325, 0x327,
    0x328, 0x329, 0x32b, 0x32c, 0x32e, 0x32f, 0x331, 0x332,

    0x334, 0x335, 0x337, 0x338, 0x33a, 0x33b, 0x33d, 0x33e,   // 15
    0x340, 0x341, 0x343, 0x344, 0x346, 0x347, 0x349, 0x34a,
    0x34c, 0x34d, 0x34f, 0x350, 0x352, 0x353, 0x355, 0x357,
    0x358, 0x35a, 0x35b, 0x35d, 0x35e, 0x360, 0x361, 0x363,

    0x365, 0x366, 0x368, 0x369, 0x36b, 0x36c, 0x36e, 0x370,   // 16
    0x371, 0x373, 0x374, 0x376, 0x378, 0x379, 0x37b, 0x37c,
    0x37e, 0x380, 0x381, 0x383, 0x384, 0x386, 0x388, 0x389,
    0x38b, 0x38d, 0x38e, 0x390, 0x392, 0x393, 0x395, 0x397,

    0x398, 0x39a, 0x39c, 0x39d, 0x39f, 0x3a1, 0x3a2, 0x3a4,   // 17
    0x3a6, 0x3a7, 0x3a9, 0x3ab, 0x3ac, 0x3ae, 0x3b0, 0x3b1,
    0x3b3, 0x3b5, 0x3b7, 0x3b8, 0x3ba, 0x3bc, 0x3bd, 0x3bf,
    0x3c1, 0x3c3, 0x3c4, 0x3c6, 0x3c8, 0x3ca, 0x3cb, 0x3cd,

    // The last note has an incomplete range, and loops round back to
    // the start.  Note that the last value is actually a buffer overrun
    // and does not fit with the other values.

    0x3cf, 0x3d1, 0x3d2, 0x3d4, 0x3d6, 0x3d8, 0x3da, 0x3db,   // 18
    0x3dd, 0x3df, 0x3e1, 0x3e3, 0x3e4, 0x3e6, 0x3e8, 0x3ea,
    0x3ec, 0x3ed, 0x3ef, 0x3f1, 0x3f3, 0x3f5, 0x3f6, 0x3f8,
    0x3fa, 0x3fc, 0x3fe, 0x36c,
};

// Mapping from MIDI volume level to OPL level value (verbatim from Crispy).
static const unsigned int volume_mapping_table[] = {
    0, 1, 3, 5, 6, 8, 10, 11,
    13, 14, 16, 17, 19, 20, 22, 23,
    25, 26, 27, 29, 30, 32, 33, 34,
    36, 37, 39, 41, 43, 45, 47, 49,
    50, 52, 54, 55, 57, 59, 60, 61,
    63, 64, 66, 67, 68, 69, 71, 72,
    73, 74, 75, 76, 77, 79, 80, 81,
    82, 83, 84, 84, 85, 86, 87, 88,
    89, 90, 91, 92, 92, 93, 94, 95,
    96, 96, 97, 98, 99, 99, 100, 101,
    101, 102, 103, 103, 104, 105, 105, 106,
    107, 107, 108, 109, 109, 110, 110, 111,
    112, 112, 113, 113, 114, 114, 115, 115,
    116, 117, 117, 118, 118, 119, 119, 120,
    120, 121, 121, 122, 122, 123, 123, 123,
    124, 124, 125, 125, 126, 126, 127, 127
};

//
// Module state.
//
static int music_ready = 0;
static int perc_channel = 15;

// GENMIDI instrument data: a private copy of the 175 records, so the caller
// is free to release the lump after SetGenmidi.
static genmidi_instr_t main_instrs[GENMIDI_NUM_INSTRS];
static genmidi_instr_t percussion_instrs[GENMIDI_NUM_PERCUSSION];

// Voices (OPL2: exactly OPL_NUM_VOICES).
static opl_voice_t voices[OPL_NUM_VOICES];
static opl_voice_t *voice_free_list[OPL_NUM_VOICES];
static opl_voice_t *voice_alloced_list[OPL_NUM_VOICES];
static int voice_free_num;
static int voice_alloced_num;
static int num_opl_voices = OPL_NUM_VOICES;

// Per-channel data.
static opl_channel_data_t channels[MIDI_CHANNELS_PER_TRACK];

// Volume of the song (0..127). DOOM music plays at full; we keep it simple.
static int current_music_volume = 127;

//
// --- Voice freelist management (ported from Crispy) ---
//

static opl_voice_t *GetFreeVoice(void)
{
    opl_voice_t *result;
    int i;

    if (voice_free_num == 0)
    {
        return NULL;
    }

    result = voice_free_list[0];
    voice_free_num--;

    for (i = 0; i < voice_free_num; i++)
    {
        voice_free_list[i] = voice_free_list[i + 1];
    }

    voice_alloced_list[voice_alloced_num++] = result;

    return result;
}

static void VoiceKeyOff(opl_voice_t *voice);

static void ReleaseVoice(int index)
{
    opl_voice_t *voice;
    int i;

    if (index >= voice_alloced_num)
    {
        voice_alloced_num = 0;
        voice_free_num = 0;
        return;
    }

    voice = voice_alloced_list[index];

    VoiceKeyOff(voice);

    voice->channel = NULL;
    voice->note = 0;

    // Remove from alloced list.
    voice_alloced_num--;

    for (i = index; i < voice_alloced_num; i++)
    {
        voice_alloced_list[i] = voice_alloced_list[i + 1];
    }

    // Append to the freelist (this is how Doom behaves).
    voice_free_list[voice_free_num++] = voice;
}

//
// --- Operator / instrument / volume / pan / frequency (ported from Crispy) ---
//

static void LoadOperatorData(int op, genmidi_op_t *data,
                             int max_level, unsigned int *volume)
{
    int level;

    level = data->scale;

    if (max_level)
    {
        level |= 0x3f;
    }
    else
    {
        level |= data->level;
    }

    *volume = level;

    WR(OPL_REGS_LEVEL + op, level);
    WR(OPL_REGS_TREMOLO + op, data->tremolo);
    WR(OPL_REGS_ATTACK + op, data->attack);
    WR(OPL_REGS_SUSTAIN + op, data->sustain);
    WR(OPL_REGS_WAVEFORM + op, data->waveform);
}

static void SetVoiceInstrument(opl_voice_t *voice,
                               genmidi_instr_t *instr,
                               unsigned int instr_voice)
{
    genmidi_voice_t *data;
    unsigned int modulating;

    if (voice->current_instr == instr
     && voice->current_instr_voice == instr_voice)
    {
        return;
    }

    voice->current_instr = instr;
    voice->current_instr_voice = instr_voice;

    data = &instr->voices[instr_voice];

    modulating = (data->feedback & 0x01) == 0;

    LoadOperatorData(voice->op2 | voice->array, &data->carrier, 1,
                     &voice->car_volume);
    LoadOperatorData(voice->op1 | voice->array, &data->modulator, !modulating,
                     &voice->mod_volume);

    WR((OPL_REGS_FEEDBACK + voice->index) | voice->array,
       data->feedback | voice->reg_pan);

    voice->priority = 0x0f - (data->carrier.attack >> 4)
                    + 0x0f - (data->carrier.sustain & 0x0f);
}

static void SetVoiceVolume(opl_voice_t *voice, unsigned int volume)
{
    genmidi_voice_t *opl_voice;
    unsigned int midi_volume;
    unsigned int full_volume;
    unsigned int car_volume;
    unsigned int mod_volume;

    voice->note_volume = volume;

    opl_voice = &voice->current_instr->voices[voice->current_instr_voice];

    midi_volume = 2 * (volume_mapping_table[voice->channel->volume] + 1);

    full_volume = (volume_mapping_table[voice->note_volume] * midi_volume) >> 9;

    car_volume = 0x3f - full_volume;

    if (car_volume != (voice->car_volume & 0x3f))
    {
        voice->car_volume = car_volume | (voice->car_volume & 0xc0);

        WR((OPL_REGS_LEVEL + voice->op2) | voice->array, voice->car_volume);

        // Non-modulated feedback mode: set both operators' volume.
        if ((opl_voice->feedback & 0x01) != 0
         && opl_voice->modulator.level != 0x3f)
        {
            mod_volume = opl_voice->modulator.level;
            if (mod_volume < car_volume)
            {
                mod_volume = car_volume;
            }

            mod_volume |= voice->mod_volume & 0xc0;

            if (mod_volume != voice->mod_volume)
            {
                voice->mod_volume = mod_volume;
                WR((OPL_REGS_LEVEL + voice->op1) | voice->array,
                   mod_volume | (opl_voice->modulator.scale & 0xc0));
            }
        }
    }
}

static void SetVoicePan(opl_voice_t *voice, unsigned int pan)
{
    genmidi_voice_t *opl_voice;

    voice->reg_pan = pan;
    opl_voice = &voice->current_instr->voices[voice->current_instr_voice];

    WR((OPL_REGS_FEEDBACK + voice->index) | voice->array,
       opl_voice->feedback | pan);
}

static void VoiceKeyOff(opl_voice_t *voice)
{
    WR((OPL_REGS_FREQ_2 + voice->index) | voice->array, voice->freq >> 8);
}

static unsigned int FrequencyForVoice(opl_voice_t *voice)
{
    genmidi_voice_t *gm_voice;
    signed int freq_index;
    unsigned int octave;
    unsigned int sub_index;
    signed int note;

    note = voice->note;

    gm_voice = &voice->current_instr->voices[voice->current_instr_voice];

    if ((voice->current_instr->flags & GENMIDI_FLAG_FIXED) == 0)
    {
        note += (signed short) gm_voice->base_note_offset;
    }

    while (note < 0)
    {
        note += 12;
    }

    while (note > 95)
    {
        note -= 12;
    }

    freq_index = 64 + 32 * note + voice->channel->bend;

    if (voice->current_instr_voice != 0)
    {
        freq_index += (voice->current_instr->fine_tuning / 2) - 64;
    }

    if (freq_index < 0)
    {
        freq_index = 0;
    }

    if (freq_index < 284)
    {
        return frequency_curve[freq_index];
    }

    sub_index = (freq_index - 284) % (12 * 32);
    octave = (freq_index - 284) / (12 * 32);

    if (octave >= 7)
    {
        octave = 7;
    }

    return frequency_curve[sub_index + 284] | (octave << 10);
}

static void UpdateVoiceFrequency(opl_voice_t *voice)
{
    unsigned int freq;

    freq = FrequencyForVoice(voice);

    if (voice->freq != freq)
    {
        WR((OPL_REGS_FREQ_1 + voice->index) | voice->array, freq & 0xff);
        WR((OPL_REGS_FREQ_2 + voice->index) | voice->array,
           (freq >> 8) | 0x20);

        voice->freq = freq;
    }
}

static void VoiceKeyOn(opl_channel_data_t *channel,
                       genmidi_instr_t *instrument,
                       unsigned int instrument_voice,
                       unsigned int note,
                       unsigned int key,
                       unsigned int volume)
{
    opl_voice_t *voice;

    voice = GetFreeVoice();

    if (voice == NULL)
    {
        return;
    }

    voice->channel = channel;
    voice->key = key;

    if ((instrument->flags & GENMIDI_FLAG_FIXED) != 0)
    {
        voice->note = instrument->fixed_note;
    }
    else
    {
        voice->note = note;
    }

    voice->reg_pan = channel->pan;

    SetVoiceInstrument(voice, instrument, instrument_voice);

    SetVoiceVolume(voice, volume);

    voice->freq = 0;
    UpdateVoiceFrequency(voice);
}

// When all voices are in use, free an existing one (opl_doom_1_9 behaviour).
static void ReplaceExistingVoice(void)
{
    int i;
    int result;

    result = 0;

    for (i = 0; i < voice_alloced_num; i++)
    {
        if (voice_alloced_list[i]->current_instr_voice != 0
         || voice_alloced_list[i]->channel
         >= voice_alloced_list[result]->channel)
        {
            result = i;
        }
    }

    ReleaseVoice(result);
}

//
// --- Channel / event handling ---
//

static void InitChannel(opl_channel_data_t *channel)
{
    channel->instrument = &main_instrs[0];
    channel->volume = current_music_volume;
    channel->volume_base = 100;
    if (channel->volume > channel->volume_base)
    {
        channel->volume = channel->volume_base;
    }
    channel->pan = 0x30;
    channel->bend = 0;
}

static void SetChannelVolume(opl_channel_data_t *channel, unsigned int volume)
{
    int i;

    channel->volume_base = volume;

    if (volume > (unsigned int) current_music_volume)
    {
        volume = current_music_volume;
    }

    channel->volume = volume;

    for (i = 0; i < num_opl_voices; ++i)
    {
        if (voices[i].channel == channel)
        {
            SetVoiceVolume(&voices[i], voices[i].note_volume);
        }
    }
}

static void ChannelAllNotesOff(opl_channel_data_t *channel)
{
    int i;

    for (i = 0; i < voice_alloced_num; i++)
    {
        if (voice_alloced_list[i]->channel == channel)
        {
            ReleaseVoice(i);
            i--;
        }
    }
}

//
// --- Public API ---
//

void OPL_Music_SetPercChannel(int ch)
{
    perc_channel = ch;
}

int OPL_Music_Ready(void)
{
    return music_ready;
}

void OPL_Music_SetGenmidi(const unsigned char *lump, int len)
{
    int hdr;
    const genmidi_instr_t *src;

    music_ready = 0;

    hdr = (int) strlen(GENMIDI_HEADER);  // 8

    if (lump == NULL
     || len < hdr + (GENMIDI_NUM_INSTRS + GENMIDI_NUM_PERCUSSION)
                    * (int) sizeof(genmidi_instr_t))
    {
        return;
    }

    if (memcmp(lump, GENMIDI_HEADER, hdr) != 0)
    {
        return;
    }

    src = (const genmidi_instr_t *) (lump + hdr);

    memcpy(main_instrs, src, GENMIDI_NUM_INSTRS * sizeof(genmidi_instr_t));
    memcpy(percussion_instrs, src + GENMIDI_NUM_INSTRS,
           GENMIDI_NUM_PERCUSSION * sizeof(genmidi_instr_t));

    music_ready = 1;
}

static void InitVoices(void)
{
    int i;

    voice_free_num = num_opl_voices;
    voice_alloced_num = 0;

    for (i = 0; i < num_opl_voices; ++i)
    {
        voices[i].index = i % OPL_NUM_VOICES;
        voices[i].op1 = voice_operators[0][i % OPL_NUM_VOICES];
        voices[i].op2 = voice_operators[1][i % OPL_NUM_VOICES];
        voices[i].array = (i / OPL_NUM_VOICES) << 8;  // 0 for OPL2
        voices[i].current_instr = NULL;
        voices[i].current_instr_voice = 0;
        voices[i].channel = NULL;

        voice_free_list[i] = &voices[i];
    }
}

void OPL_Music_Init(int samplerate)
{
    int i;

    if (samplerate <= 0)
    {
        samplerate = 11025;
    }

    OPL3_Reset(&chip, (uint32_t) samplerate);

    // Put the chip into OPL2 (not OPL3 "new") mode and enable the
    // waveform-select registers, then clear the rhythm/test registers.
    WR(OPL_REG_WAVEFORM_ENABLE, 0x20);  // enable waveform select
    WR(OPL_REG_FM_MODE, 0x00);
    WR(0x01, 0x20);                     // (Test) waveform-select enable

    InitVoices();

    for (i = 0; i < MIDI_CHANNELS_PER_TRACK; ++i)
    {
        InitChannel(&channels[i]);
    }
}

void OPL_Music_Reset(void)
{
    int i;

    OPL_Music_AllNotesOff();

    InitVoices();

    for (i = 0; i < MIDI_CHANNELS_PER_TRACK; ++i)
    {
        InitChannel(&channels[i]);
    }

    // Re-arm the basic mode registers in case the chip was reset.
    WR(OPL_REG_WAVEFORM_ENABLE, 0x20);
    WR(OPL_REG_FM_MODE, 0x00);
}

void OPL_Music_Program(int chan, int prog)
{
    if (chan < 0 || chan >= MIDI_CHANNELS_PER_TRACK)
    {
        return;
    }
    if (prog < 0)
    {
        prog = 0;
    }
    if (prog >= GENMIDI_NUM_INSTRS)
    {
        prog = GENMIDI_NUM_INSTRS - 1;
    }

    channels[chan].instrument = &main_instrs[prog];
}

void OPL_Music_NoteOn(int chan, int note, int vel)
{
    genmidi_instr_t *instrument;
    opl_channel_data_t *channel;
    unsigned int n, key, volume;
    int double_voice;

    if (!music_ready || chan < 0 || chan >= MIDI_CHANNELS_PER_TRACK)
    {
        return;
    }

    // Velocity 0 means note off.
    if (vel <= 0)
    {
        OPL_Music_NoteOff(chan, note);
        return;
    }
    if (vel > 127)
    {
        vel = 127;
    }

    key = (unsigned int) note;
    n = (unsigned int) note;
    volume = (unsigned int) vel;

    channel = &channels[chan];

    if (chan == perc_channel)
    {
        if (note < 35 || note > 81)
        {
            return;
        }

        instrument = &percussion_instrs[note - 35];
        n = 60;  // fixed note for percussion
    }
    else
    {
        instrument = channel->instrument;
    }

    if (instrument == NULL)
    {
        return;
    }

    double_voice = (instrument->flags & GENMIDI_FLAG_2VOICE) != 0;

    // opl_doom_1_9 voice allocation.
    if (voice_free_num == 0)
    {
        ReplaceExistingVoice();
    }

    VoiceKeyOn(channel, instrument, 0, n, key, volume);

    if (double_voice)
    {
        VoiceKeyOn(channel, instrument, 1, n, key, volume);
    }
}

void OPL_Music_NoteOff(int chan, int note)
{
    opl_channel_data_t *channel;
    unsigned int key;
    int i;

    if (chan < 0 || chan >= MIDI_CHANNELS_PER_TRACK)
    {
        return;
    }

    channel = &channels[chan];
    key = (unsigned int) note;

    for (i = 0; i < voice_alloced_num; i++)
    {
        if (voice_alloced_list[i]->channel == channel
         && voice_alloced_list[i]->key == key)
        {
            ReleaseVoice(i);
            i--;
        }
    }
}

void OPL_Music_ChannelVolume(int chan, int vol)
{
    if (chan < 0 || chan >= MIDI_CHANNELS_PER_TRACK)
    {
        return;
    }
    if (vol < 0)
    {
        vol = 0;
    }
    if (vol > 127)
    {
        vol = 127;
    }

    SetChannelVolume(&channels[chan], (unsigned int) vol);
}

void OPL_Music_PitchBend(int chan, int bend)
{
    opl_channel_data_t *channel;
    int i;

    if (chan < 0 || chan >= MIDI_CHANNELS_PER_TRACK)
    {
        return;
    }

    // 7-bit MSB-only bend (MUS-style), centred at 64 like Crispy/DMX.
    if (bend < 0)
    {
        bend = 0;
    }
    if (bend > 127)
    {
        bend = 127;
    }

    channel = &channels[chan];
    channel->bend = bend - 64;

    for (i = 0; i < voice_alloced_num; ++i)
    {
        if (voice_alloced_list[i]->channel == channel)
        {
            UpdateVoiceFrequency(voice_alloced_list[i]);
        }
    }
}

void OPL_Music_AllNotesOff(void)
{
    int i;

    for (i = 0; i < MIDI_CHANNELS_PER_TRACK; ++i)
    {
        ChannelAllNotesOff(&channels[i]);
    }
}

void OPL_Music_Render(short *stereo, int frames)
{
    if (stereo == NULL || frames <= 0)
    {
        return;
    }

    OPL3_GenerateStream(&chip, (int16_t *) stereo, (uint32_t) frames);
}
