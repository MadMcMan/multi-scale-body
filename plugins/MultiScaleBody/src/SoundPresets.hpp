// Project-owned SOUND PRESET library (the browser's data source).
//
// Two axes, deliberately separate:
//
//   BODY    - one of the 25 baked modal banks in ModalData.hpp. It supplies the
//             per-mode frequencies, decays and the 16x16 sound map, plus its own
//             physical-model defaults (PresetModels.hpp). This is what the BODY
//             browser steps through.
//   CHARACTER - the player-facing controls that turn one body into a particular
//             instrument: how hard and where it is struck, how it is damped, how
//             long it rings, how bright it is, whether it is bowed.
//
// A SOUND PRESET is a body plus a character. The library is the cartesian
// product (25 x 6 = 150) so every body is reachable under every character, and
// the body browser can swap the body underneath a preset while the character
// stays put - which is the point of having both.
//
// A character field of -1 means INHERIT: leave that parameter at whatever the
// body/preset already set. PURE is all -1, so "<body> Pure" is the body's
// designed sound exactly - and the default preset (body 0, Pure) is therefore
// bit-identical to the plugin's default before this library existed.
//
// Values are in the plugin's normalized 0..1 parameter domain, the same domain
// the knobs and the host use. They are deliberately moderate: the baked tables
// already encode each body's decay and material, so these are levers on top.
#pragma once
#include "ModalData.hpp"
#include <cstdio>
#include <cstring>

namespace modal {

inline constexpr float kInherit = -1.f;

// One character: the parameter deltas applied on top of a body.
struct Character {
    const char* name;
    float decay;        // 0.5 = the body's own decay, higher = longer tail
    float brightness;
    float width;
    float radiation;
    float attack;
    float release;
    float inharm;       // partial stretch
    float strikeW;      // mallet head size (0.5 = nominal)
    float scrape;       // strike friction blend
    float bow;          // friction drive (0 = mallet strike)
    float damper;       // felt damper depth
    float contactNoise; // contact-transient noise layer
    float exciteMix;
    float support;      // boundary clamping
    float holdDamp;     // hold-point damping
    float resMorph;     // FEM resolution morph
    float morphAmt;
    float raylA;        // Rayleigh alpha
    float raylB;        // Rayleigh beta
    float velStrike;
    float detune;
    float glide;
    float modeCount;
    float strikeX;      // strike position across the body
    float strikeY;
};

inline constexpr Character kCharacters[] = {
    // The body exactly as designed: every field inherits.
    {"Pure",   kInherit,kInherit,kInherit,kInherit,kInherit,kInherit,kInherit,kInherit,
               kInherit,kInherit,kInherit,kInherit,kInherit,kInherit,kInherit,kInherit,
               kInherit,kInherit,kInherit,kInherit,kInherit,kInherit,kInherit,kInherit,
               kInherit},
    // Hard small mallet, more radiation, a touch of stretch, shorter ring.
    {"Bright", 0.44f,0.85f,0.40f,0.70f,0.10f,0.40f,0.15f,0.35f,
               0.30f,kInherit,kInherit,kInherit,kInherit,kInherit,kInherit,kInherit,
               kInherit,kInherit,kInherit,0.45f,0.18f,0.15f,0.65f,0.45f,0.50f},
    // Soft, low radiation, longer ring with felt damping on top.
    {"Dark",   0.62f,0.35f,0.20f,0.25f,0.18f,0.55f,kInherit,0.55f,
               kInherit,kInherit,0.25f,kInherit,kInherit,kInherit,kInherit,kInherit,
               kInherit,kInherit,kInherit,0.30f,0.10f,0.15f,0.70f,0.55f,0.50f},
    // Friction drive: a slow swell that keeps going, struck near the edge.
    {"Bowed",  0.70f,0.55f,0.30f,0.45f,0.50f,0.70f,kInherit,kInherit,
               0.30f,0.55f,kInherit,kInherit,0.20f,kInherit,kInherit,kInherit,
               kInherit,kInherit,kInherit,0.30f,0.15f,0.35f,0.55f,0.35f,0.50f},
    // Big soft head, friction transient, short percussive gate.
    {"Mallet", 0.40f,0.60f,0.35f,0.55f,0.05f,0.25f,0.10f,0.70f,
               0.55f,kInherit,kInherit,0.35f,kInherit,kInherit,kInherit,kInherit,
               kInherit,kInherit,kInherit,0.60f,0.15f,0.10f,0.60f,0.50f,0.50f},
    // Stretched partials, hard small head, long ring: the bell-like extreme.
    {"Bell",   0.75f,0.70f,0.45f,0.60f,0.02f,0.60f,0.50f,0.25f,
               kInherit,kInherit,kInherit,kInherit,kInherit,kInherit,kInherit,kInherit,
               kInherit,kInherit,kInherit,0.40f,0.20f,0.20f,0.70f,0.40f,0.45f},
};
inline constexpr int kNumCharacters = (int)(sizeof(kCharacters)/sizeof(kCharacters[0]));

struct SoundPreset {
    const char* name;   // "<Body> <Character>"
    uint8_t body;       // index into kPresets
    uint8_t character;  // index into kCharacters
};

inline constexpr int kNumSoundPresets = kNumPresets * kNumCharacters;

namespace detail {
// Names have to be composed, which constexpr cannot do for a cartesian product,
// so the table is built once on first use (UI thread only).
struct Library {
    SoundPreset items[kNumSoundPresets];
    char names[kNumSoundPresets][48];
    int count = 0;
};
inline Library& library(){
    static Library lib = []{
        Library l;
        for(int b=0;b<kNumPresets;++b)
            for(int c=0;c<kNumCharacters;++c){
                const int i = b*kNumCharacters + c;
                std::snprintf(l.names[i],sizeof(l.names[i]),"%s %s",kPresets[b].name,kCharacters[c].name);
                l.items[i] = SoundPreset{ l.names[i], (uint8_t)b, (uint8_t)c };
                ++l.count;
            }
        return l;
    }();
    return lib;
}
} // namespace detail

inline int soundPresetCount(){ return detail::library().count; }
inline const SoundPreset& soundPreset(int i){
    const auto& lib=detail::library();
    if(i<0) i=0; if(i>=lib.count) i=lib.count-1;
    return lib.items[i];
}
// index of (body, character), for the browser: changing the body or the
// character moves the selection instead of leaving the library
inline int soundPresetIndex(int body,int character){
    if(body<0) body=0; if(body>=kNumPresets) body=kNumPresets-1;
    if(character<0) character=0; if(character>=kNumCharacters) character=kNumCharacters-1;
    return body*kNumCharacters + character;
}

} // namespace modal
