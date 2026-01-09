#pragma once
#include <Arduino.h>
#include <stdint.h>

// -------------------- Mapping mode selection --------------------
// JOY_MAP_LEARN  : use learning procedure + EEPROM mapping (handled by main sketch)
// JOY_MAP_CUSTOM : use hardcoded custom rules defined here

#define JOY_MAP_LEARN   0
#define JOY_MAP_CUSTOM  1

#ifndef JOY_MAPPING_MODE
  #define JOY_MAPPING_MODE JOY_MAP_LEARN
//  #define JOY_MAPPING_MODE JOY_MAP_CUSTOM
#endif

// -------------------- Custom mapping definitions --------------------
#if (JOY_MAPPING_MODE == JOY_MAP_CUSTOM)

enum JM_Op : uint8_t {
  JM_EQ = 0,      // (data[index] == value)
  JM_BITANY = 1   // ((data[index] & value) != 0)
};

enum JM_Func : uint8_t {
  JM_UP = 0,
  JM_UP_RIGHT,
  JM_RIGHT,
  JM_RIGHT_DOWN,
  JM_DOWN,
  JM_DOWN_LEFT,
  JM_LEFT,
  JM_LEFT_UP,
  JM_FIRE,
  JM_AUTOFIRE_ON,
  JM_AUTOFIRE_OFF,
  JM_AUTOLEFTRIGHT_ON,
  JM_AUTOLEFTRIGHT_OFF,

  // Extra mouse button for Amiga/Atari in joystick-as-mouse mode
  JM_BUTTON2
};

struct JM_Rule {
  uint8_t index;
  uint8_t value;
  JM_Op op;
  JM_Func func;
};

// Persistent states (custom mapping only)
static bool JM_autofire = false;
static bool JM_autoleftright = false;

// -------------------- Custom rules (JOYSTICK MODE) --------------------
// Edit this table for your controller.
// Example based on your snippet.
static const JM_Rule JM_JOY_RULES[] = {
  // Hat / D-pad directions on data[1]
  { 1, 0, JM_EQ, JM_UP },
  { 1, 1, JM_EQ, JM_UP_RIGHT },
  { 1, 2, JM_EQ, JM_RIGHT },
  { 1, 3, JM_EQ, JM_RIGHT_DOWN },
  { 1, 4, JM_EQ, JM_DOWN },
  { 1, 5, JM_EQ, JM_DOWN_LEFT },
  { 1, 6, JM_EQ, JM_LEFT },
  { 1, 7, JM_EQ, JM_LEFT_UP },

  // Extra "UP" source (example: data[8] == 8)
  { 8, 8, JM_EQ, JM_UP },

  // Fire sources (example: data[8] == 64 or 128 or 1)
  { 8, 64,  JM_EQ, JM_FIRE },
  { 8, 128, JM_EQ, JM_FIRE },
  { 8, 1,   JM_EQ, JM_FIRE },

  // Autofire toggles
  { 8, 16, JM_EQ, JM_AUTOFIRE_ON },
  { 8, 2,  JM_EQ, JM_AUTOFIRE_OFF },

  // Auto left/right toggles
  { 8, 32, JM_EQ, JM_AUTOLEFTRIGHT_ON },
  { 8, 4,  JM_EQ, JM_AUTOLEFTRIGHT_OFF }
};
static const size_t JM_JOY_RULES_COUNT = sizeof(JM_JOY_RULES) / sizeof(JM_JOY_RULES[0]);

// -------------------- Custom rules (JOYSTICK AS MOUSE MODE - C64 buttons) --------------------
// Typical: fire + "UP pin" used as right-click substitute on C64 setups.
static const JM_Rule JM_MOUSE_BTN_RULES_C64[] = {
  { 8, 64, JM_EQ, JM_FIRE },
  { 8, 1,  JM_EQ, JM_FIRE },
  { 8, 128, JM_EQ, JM_UP },        // Example: map to C64_UP in joystick-as-mouse mode
  { 8, 16, JM_EQ, JM_AUTOFIRE_ON },
  { 8, 2,  JM_EQ, JM_AUTOFIRE_OFF }
};
static const size_t JM_MOUSE_BTN_RULES_C64_COUNT = sizeof(JM_MOUSE_BTN_RULES_C64) / sizeof(JM_MOUSE_BTN_RULES_C64[0]);

// -------------------- Custom rules (JOYSTICK AS MOUSE MODE - Amiga/Atari buttons) --------------------
// Typical: fire + button2 (right mouse button line).
static const JM_Rule JM_MOUSE_BTN_RULES_A[] = {
  { 8, 64,  JM_EQ, JM_FIRE },
  { 8, 1,   JM_EQ, JM_FIRE },
  { 8, 128, JM_EQ, JM_BUTTON2 },   // Your custom firmware behavior
  { 8, 16,  JM_EQ, JM_AUTOFIRE_ON },
  { 8, 2,   JM_EQ, JM_AUTOFIRE_OFF }
};
static const size_t JM_MOUSE_BTN_RULES_A_COUNT = sizeof(JM_MOUSE_BTN_RULES_A) / sizeof(JM_MOUSE_BTN_RULES_A[0]);

// -------------------- Analog configuration --------------------

// Enable analog usage in joystick-as-mouse mode
#define JM_USE_ANALOG_MOUSE  1

// Axis sources (can be different for C64 vs Amiga/Atari if needed)
static const uint8_t JM_C64_MOUSE_X_INDEXES[] = { 2, 4 };
static const uint8_t JM_C64_MOUSE_Y_INDEXES[] = { 3, 5 };

static const uint8_t JM_A_MOUSE_X_INDEXES[]   = { 2, 4 };
static const uint8_t JM_A_MOUSE_Y_INDEXES[]   = { 3, 5 };

#define JM_ANALOG_CENTER      127
#define JM_ANALOG_DEAD_LOW     64
#define JM_ANALOG_DEAD_HIGH   190

// C64: delta added directly to delayOnX/delayOnY
#define JM_C64_ANALOG_DIV      3
#define JM_C64_Y_INVERT        1   // Keep C64 behavior (center - v)

// Amiga/Atari: convert analog to signed "steps"
#define JM_A_STEP_DIV          40  // Your custom firmware used /40
#define JM_A_Y_INVERT          0   // Your custom firmware used (v - center)

// Atari pulse scaling (your firmware used ~18.6 * PULSE_LENGTH / steps)
#define JM_ATARI_PULSE_SCALE   18.6f

static inline bool JM_match(const JM_Rule &r, const uint8_t *data, int length) {
  if ((int)r.index >= length) return false;
  uint8_t v = data[r.index];
  if (r.op == JM_EQ) return (v == r.value);
  return ((v & r.value) != 0);
}

static inline void JM_applyDirFunc(JM_Func f, bool &up, bool &down, bool &left, bool &right, bool &fire) {
  switch (f) {
    case JM_UP:         up = true; break;
    case JM_UP_RIGHT:   up = true; right = true; break;
    case JM_RIGHT:      right = true; break;
    case JM_RIGHT_DOWN: down = true; right = true; break;
    case JM_DOWN:       down = true; break;
    case JM_DOWN_LEFT:  down = true; left = true; break;
    case JM_LEFT:       left = true; break;
    case JM_LEFT_UP:    up = true; left = true; break;
    case JM_FIRE:       fire = true; break;
    default: break;
  }
}

// Decode joystick mode rules into flags + persistent toggles.
static inline void JM_DecodeJoystickMode(const uint8_t *data, int length,
                                        bool &up, bool &down, bool &left, bool &right, bool &fire,
                                        bool &autofireEnabled, bool &autoleftrightEnabled) {
  up = down = left = right = fire = false;

  for (size_t i = 0; i < JM_JOY_RULES_COUNT; i++) {
    const JM_Rule &r = JM_JOY_RULES[i];
    if (!JM_match(r, data, length)) continue;

    if (r.func == JM_AUTOFIRE_ON)        { JM_autofire = true;  continue; }
    if (r.func == JM_AUTOFIRE_OFF)       { JM_autofire = false; continue; }
    if (r.func == JM_AUTOLEFTRIGHT_ON)   { JM_autoleftright = true;  continue; }
    if (r.func == JM_AUTOLEFTRIGHT_OFF)  { JM_autoleftright = false; continue; }

    JM_applyDirFunc(r.func, up, down, left, right, fire);
  }

  autofireEnabled = JM_autofire;
  autoleftrightEnabled = JM_autoleftright;
}

// Decode joystick-as-mouse buttons for C64.
static inline void JM_DecodeMouseModeButtons_C64(const uint8_t *data, int length,
                                                bool &fire, bool &up, bool &autofireEnabled) {
  fire = false;
  up = false;

  for (size_t i = 0; i < JM_MOUSE_BTN_RULES_C64_COUNT; i++) {
    const JM_Rule &r = JM_MOUSE_BTN_RULES_C64[i];
    if (!JM_match(r, data, length)) continue;

    if (r.func == JM_AUTOFIRE_ON)  { JM_autofire = true;  continue; }
    if (r.func == JM_AUTOFIRE_OFF) { JM_autofire = false; continue; }

    if (r.func == JM_FIRE) fire = true;
    if (r.func == JM_UP)   up = true;
  }

  autofireEnabled = JM_autofire;
}

// Decode joystick-as-mouse buttons for Amiga/Atari.
static inline void JM_DecodeMouseModeButtons_A(const uint8_t *data, int length,
                                              bool &fire, bool &button2, bool &autofireEnabled) {
  fire = false;
  button2 = false;

  for (size_t i = 0; i < JM_MOUSE_BTN_RULES_A_COUNT; i++) {
    const JM_Rule &r = JM_MOUSE_BTN_RULES_A[i];
    if (!JM_match(r, data, length)) continue;

    if (r.func == JM_AUTOFIRE_ON)  { JM_autofire = true;  continue; }
    if (r.func == JM_AUTOFIRE_OFF) { JM_autofire = false; continue; }

    if (r.func == JM_FIRE)     fire = true;
    if (r.func == JM_BUTTON2)  button2 = true;
  }

  autofireEnabled = JM_autofire;
}

// C64 analog -> mouse delta to be added to delayOnX/delayOnY.
static inline void JM_AnalogToMouseDelta_C64(const uint8_t *data, int length, int &dX, int &dY) {
  dX = 0;
  dY = 0;

#if (JM_USE_ANALOG_MOUSE == 1)
  // X
  for (size_t i = 0; i < (sizeof(JM_C64_MOUSE_X_INDEXES) / sizeof(JM_C64_MOUSE_X_INDEXES[0])); i++) {
    uint8_t idx = JM_C64_MOUSE_X_INDEXES[i];
    if ((int)idx >= length) continue;
    uint8_t v = data[idx];
    if (v < JM_ANALOG_DEAD_LOW || v > JM_ANALOG_DEAD_HIGH) {
      dX += ((int)v - JM_ANALOG_CENTER) / JM_C64_ANALOG_DIV;
    }
  }

  // Y
  for (size_t i = 0; i < (sizeof(JM_C64_MOUSE_Y_INDEXES) / sizeof(JM_C64_MOUSE_Y_INDEXES[0])); i++) {
    uint8_t idx = JM_C64_MOUSE_Y_INDEXES[i];
    if ((int)idx >= length) continue;
    uint8_t v = data[idx];
    if (v < JM_ANALOG_DEAD_LOW || v > JM_ANALOG_DEAD_HIGH) {
#if (JM_C64_Y_INVERT == 1)
      dY += (JM_ANALOG_CENTER - (int)v) / JM_C64_ANALOG_DIV;
#else
      dY += ((int)v - JM_ANALOG_CENTER) / JM_C64_ANALOG_DIV;
#endif
    }
  }
#endif
}

// Amiga/Atari analog -> signed step counts (to be converted to quadrature pulses in the main sketch).
static inline void JM_AnalogToMouseSteps_A(const uint8_t *data, int length, int &xStepsSigned, int &yStepsSigned) {
  xStepsSigned = 0;
  yStepsSigned = 0;

#if (JM_USE_ANALOG_MOUSE == 1)
  // X
  for (size_t i = 0; i < (sizeof(JM_A_MOUSE_X_INDEXES) / sizeof(JM_A_MOUSE_X_INDEXES[0])); i++) {
    uint8_t idx = JM_A_MOUSE_X_INDEXES[i];
    if ((int)idx >= length) continue;
    uint8_t v = data[idx];
    if (v < JM_ANALOG_DEAD_LOW || v > JM_ANALOG_DEAD_HIGH) {
      xStepsSigned += ((int)v - JM_ANALOG_CENTER) / JM_A_STEP_DIV;
    }
  }

  // Y
  for (size_t i = 0; i < (sizeof(JM_A_MOUSE_Y_INDEXES) / sizeof(JM_A_MOUSE_Y_INDEXES[0])); i++) {
    uint8_t idx = JM_A_MOUSE_Y_INDEXES[i];
    if ((int)idx >= length) continue;
    uint8_t v = data[idx];
    if (v < JM_ANALOG_DEAD_LOW || v > JM_ANALOG_DEAD_HIGH) {
#if (JM_A_Y_INVERT == 1)
      yStepsSigned += (JM_ANALOG_CENTER - (int)v) / JM_A_STEP_DIV;
#else
      yStepsSigned += ((int)v - JM_ANALOG_CENTER) / JM_A_STEP_DIV;
#endif
    }
  }
#endif
}

#else // (JOY_MAPPING_MODE != JOY_MAP_CUSTOM)

// Stubs for LEARN mode (main sketch uses EEPROM learned mapping)
static inline void JM_DecodeJoystickMode(const uint8_t*, int,
                                        bool &up, bool &down, bool &left, bool &right, bool &fire,
                                        bool &autofireEnabled, bool &autoleftrightEnabled) {
  up = down = left = right = fire = false;
  autofireEnabled = false;
  autoleftrightEnabled = false;
}

static inline void JM_DecodeMouseModeButtons_C64(const uint8_t*, int,
                                                bool &fire, bool &up, bool &autofireEnabled) {
  fire = false;
  up = false;
  autofireEnabled = false;
}

static inline void JM_DecodeMouseModeButtons_A(const uint8_t*, int,
                                              bool &fire, bool &button2, bool &autofireEnabled) {
  fire = false;
  button2 = false;
  autofireEnabled = false;
}

static inline void JM_AnalogToMouseDelta_C64(const uint8_t*, int, int &dX, int &dY) {
  dX = 0;
  dY = 0;
}

static inline void JM_AnalogToMouseSteps_A(const uint8_t*, int, int &xStepsSigned, int &yStepsSigned) {
  xStepsSigned = 0;
  yStepsSigned = 0;
}

// Provide the same constant for the main sketch compilation
#define JM_ATARI_PULSE_SCALE 18.6f

#endif

