// Per-preset PHYSICAL MODEL settings (project-owned, hand-authored).
//
// Every preset now carries a full set of the PHYSICAL MODEL strip values, so
// loading a preset gives a complete, consistent instrument instead of leaving
// the strip at whatever the previous preset/session had. Values are
// CHARACTERISTIC (a little extra damping / inharmonicity / excitation flavour
// per body) and deliberately conservative, because the baked classic tables
// already encode each body's own decay and material: these are live levers on
// TOP of that, so big values would drift a preset away from its designed sound.
//
// material: index into the engine's kMaterials/10 (0 = DEFAULT, no override).
//   Kept 0 everywhere on purpose - the baked body already encodes its material,
//   and a nonzero override would re-apply the sqrt(E/rho) frequency law (and
//   snap Rayleigh when chosen from the dropdown). Set it per preset only if you
//   want a deliberate material override.
// modelMode: 0 = Classic baked bank (the preset's own sound), 1 = Elastic FEM.
//   Default 0 so loading a preset reproduces the calibrated classic bank; the
//   user can flip the ELASTIC switch afterwards.
#pragma once
#include "ModalData.hpp"

namespace modal {

struct PresetModel {
    float support;       // boundary support (stiffens + damps high modes)
    float holdDamp;      // hold-point damping depth
    float resMorph;      // FEM resolution morph (fine tables)
    float morphAmt;      // body-morph crossfade toward morphTarget
    float morphTarget;   // morph target preset (normalized like body)
    float material;      // material override (0 = body default)
    float raylA;         // Rayleigh alpha (mass damping)
    float raylB;         // Rayleigh beta (stiffness damping -> high-freq)
    float inharm;        // inharmonicity / partial stretch
    float strikeW;       // mallet head size
    float scrape;        // strike friction blend
    float contactNoise;  // contact-transient noise layer
    float bow;           // bow/friction excitation pressure
    float damper;        // felt damper depth
    float ecoMode;       // 0/1 scene-adaptive mode budget
    float ecoBudget;     // ringing-mode budget
    float modelMode;     // 0 classic / 1 elastic
};

// Index order mirrors tools/modal_bake.py PRESETS and modal::kPresets[].
inline constexpr PresetModel kPresetModels[kNumPresets] = {
  //          supp  hold  resM  mAmt  mTgt  mat   rayA  rayB  inh   hdSz  scrp  cNoi  bow   damp  eco  ecob  modl
    /*0  Bowl      */ {0.00f,0.00f,0.00f,0.00f,0.00f,0.00f,0.06f,0.16f,0.03f,0.45f,0.00f,0.20f,0.00f,0.00f,0.f,0.50f,0.f},
    /*1  WoodBlock */ {0.05f,0.10f,0.00f,0.00f,0.00f,0.00f,0.10f,0.34f,0.08f,0.35f,0.05f,0.35f,0.00f,0.00f,0.f,0.50f,0.f},
    /*2  Plate     */ {0.00f,0.00f,0.00f,0.00f,0.00f,0.00f,0.05f,0.13f,0.18f,0.40f,0.00f,0.15f,0.00f,0.00f,0.f,0.60f,0.f},
    /*3  Squirrel  */ {0.08f,0.12f,0.00f,0.00f,0.00f,0.00f,0.14f,0.45f,0.10f,0.30f,0.08f,0.40f,0.00f,0.00f,0.f,0.50f,0.f},
    /*4  Blade     */ {0.00f,0.00f,0.00f,0.00f,0.00f,0.00f,0.06f,0.20f,0.22f,0.40f,0.00f,0.18f,0.00f,0.00f,0.f,0.55f,0.f},
    /*5  Shell     */ {0.00f,0.00f,0.00f,0.00f,0.00f,0.00f,0.05f,0.18f,0.06f,0.45f,0.00f,0.15f,0.00f,0.00f,0.f,0.55f,0.f},
    /*6  Bar       */ {0.00f,0.00f,0.00f,0.00f,0.00f,0.00f,0.04f,0.13f,0.05f,0.40f,0.00f,0.12f,0.00f,0.00f,0.f,0.60f,0.f},
    /*7  Membrane  */ {0.00f,0.20f,0.00f,0.00f,0.00f,0.00f,0.12f,0.60f,0.15f,0.50f,0.00f,0.35f,0.00f,0.10f,0.f,0.50f,0.f},
    /*8  Bell      */ {0.00f,0.00f,0.00f,0.00f,0.00f,0.00f,0.04f,0.12f,0.04f,0.40f,0.00f,0.15f,0.00f,0.00f,0.f,0.70f,0.f},
    /*9  Glass     */ {0.00f,0.00f,0.00f,0.00f,0.00f,0.00f,0.05f,0.11f,0.12f,0.45f,0.00f,0.15f,0.00f,0.00f,0.f,0.60f,0.f},
    /*10 Chime     */ {0.00f,0.00f,0.00f,0.00f,0.00f,0.00f,0.04f,0.10f,0.05f,0.40f,0.00f,0.12f,0.00f,0.00f,0.f,0.70f,0.f},
    /*11 Gong      */ {0.00f,0.00f,0.00f,0.00f,0.00f,0.00f,0.06f,0.18f,0.09f,0.45f,0.00f,0.18f,0.00f,0.00f,0.f,0.65f,0.f},
    /*12 Handpan   */ {0.00f,0.00f,0.00f,0.00f,0.00f,0.00f,0.05f,0.13f,0.03f,0.40f,0.00f,0.15f,0.00f,0.00f,0.f,0.60f,0.f},
    /*13 LogDrum   */ {0.06f,0.10f,0.00f,0.00f,0.00f,0.00f,0.12f,0.40f,0.07f,0.30f,0.06f,0.30f,0.00f,0.00f,0.f,0.50f,0.f},
    /*14 Marimba   */ {0.04f,0.06f,0.00f,0.00f,0.00f,0.00f,0.10f,0.30f,0.05f,0.32f,0.04f,0.28f,0.00f,0.00f,0.f,0.50f,0.f},
    /*15 Cowbell   */ {0.00f,0.00f,0.00f,0.00f,0.00f,0.00f,0.06f,0.26f,0.13f,0.38f,0.00f,0.20f,0.00f,0.00f,0.f,0.55f,0.f},
    /*16 Kalimba   */ {0.03f,0.05f,0.00f,0.00f,0.00f,0.00f,0.08f,0.30f,0.16f,0.35f,0.00f,0.22f,0.00f,0.00f,0.f,0.55f,0.f},
    /*17 Celesta   */ {0.00f,0.00f,0.00f,0.00f,0.00f,0.00f,0.04f,0.10f,0.04f,0.38f,0.00f,0.12f,0.00f,0.00f,0.f,0.65f,0.f},
    /*18 Cymbal    */ {0.00f,0.00f,0.00f,0.00f,0.00f,0.00f,0.05f,0.13f,0.10f,0.42f,0.02f,0.16f,0.00f,0.00f,0.f,0.75f,0.f},
    /*19 Bottle    */ {0.00f,0.00f,0.00f,0.00f,0.00f,0.00f,0.05f,0.12f,0.08f,0.45f,0.00f,0.15f,0.00f,0.00f,0.f,0.60f,0.f},
    /*20 Can       */ {0.04f,0.06f,0.00f,0.00f,0.00f,0.00f,0.10f,0.28f,0.12f,0.35f,0.04f,0.30f,0.00f,0.00f,0.f,0.50f,0.f},
    /*21 Xylo      */ {0.04f,0.06f,0.00f,0.00f,0.00f,0.00f,0.09f,0.30f,0.05f,0.32f,0.04f,0.26f,0.00f,0.00f,0.f,0.50f,0.f},
    /*22 Tongue    */ {0.04f,0.06f,0.00f,0.00f,0.00f,0.00f,0.08f,0.24f,0.10f,0.34f,0.00f,0.24f,0.00f,0.00f,0.f,0.55f,0.f},
    /*23 Tube      */ {0.00f,0.00f,0.00f,0.00f,0.00f,0.00f,0.05f,0.14f,0.06f,0.40f,0.00f,0.16f,0.00f,0.00f,0.f,0.70f,0.f},
    /*24 Cone      */ {0.00f,0.08f,0.00f,0.00f,0.00f,0.00f,0.09f,0.32f,0.14f,0.42f,0.00f,0.28f,0.00f,0.00f,0.f,0.55f,0.f},
};

static_assert(sizeof(kPresetModels)/sizeof(kPresetModels[0]) == kNumPresets,
              "kPresetModels must have one row per preset");

} // namespace modal
