/*
USB to Commodore 64, AMIGA and ATARI adaptor V 4.1 by Emanuele Laface

WARNING: DON'T CONNECT THE COMMODORE 64 AND THE USB PORT TO A SOURCE OF POWER AT THE SAME TIME.
THE POWER WILL ARRIVE DIRECTLY TO THE SID OF THE COMMODORE AND MAY DESTROY IT.

---

THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include "driver/gpio.h"
#include "soc/gpio_struct.h"
#include "usb/usb_host.h"
#include "hid_host.h"
#include "hid_usage_mouse.h"
#include "esp_log_buffer.h"
#include "esp_timer.h"
#include "Adafruit_NeoPixel.h"
#include "EEPROM.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "JoystickMapping.h"

#define PIN_WS2812B       21 // Pin for RGB LED
#define NUM_PIXELS         1 // 1 LED
Adafruit_NeoPixel ws2812b(NUM_PIXELS, PIN_WS2812B, NEO_RGB + NEO_KHZ800); // Keep NEO_RGB (project convention)

// LED color helpers (keep the original project channel convention)
// With NEO_RGB on typical WS2812, R and G appear swapped in this project:
// - green as Color(25,0,0)
// - red   as Color(0,25,0)
// - blue  as Color(0,0,25)
static inline uint32_t LED_BLUE()  { return ws2812b.Color(0, 0, 25); }
static inline uint32_t LED_GREEN() { return ws2812b.Color(25, 0, 0); }
static inline uint32_t LED_RED()   { return ws2812b.Color(0, 25, 0); }

// Define GPIOs for C64
#define C64_FIRE           7
#define C64_UP             8
#define C64_DOWN           9
#define C64_LEFT          10
#define C64_RIGHT         11
#define C64_POTX           4
#define C64_POTY           6
// Define GPIO for interrupt from C64
#define C64_INT            1
// Define GPIO for switch Mouse - Joystick
#define SWITCH_MJ         13 // HIGH = mouse, LOW = joystick

// Define the default timers for the mouse delay, all empirical for PAL version
#define PAL                1 // Select if it is PAL or NTSC and adjust the timings

#if PAL
  #define MINdelayOnX   2450
  #define MAXdelayOnX   5040
  #define MINdelayOnY   2440
  #define MAXdelayOnY   5100
  #define STEPdelayOnX    10.16689245
  #define STEPdelayOnY    10.14384171
  // Define the timing for mouse used as Joystick
  #define M2JCalib       833 // 20000 at 240 MHz
#else
  #define MINdelayOnX   2360
  #define MAXdelayOnX   4855
  #define MINdelayOnY   2351
  #define MAXdelayOnY   4913
  #define STEPdelayOnX     9.794315054
  #define STEPdelayOnY     9.772109035
  // Define the timing for mouse used as Joystick
  #define M2JCalib      802 // 20000 at 240 MHz
#endif

#define A_FIRE            7
#define A_UP              8
#define A_DOWN            9
#define A_LEFT           10
#define A_RIGHT          11
#define A_BUTTON2         5
#define A_BUTTON3         3
#define PULSE_LENGTH    150 // Length of pulse for Amiga mouse

#define CONFIG            0 // Set the configuration switch to the "Boot" button
#define JOYBUTTONS        7 // 4 directions and 3 fire

// EEPROM layout: first JOYBUTTONS*2 bytes are joystick mapping (original),
// then 2 bytes are reserved for the target mode selection
#define EEPROM_MODE_MAGIC_ADDR   (JOYBUTTONS * 2)
#define EEPROM_MODE_VALUE_ADDR   (JOYBUTTONS * 2 + 1)
#define EEPROM_MODE_MAGIC        0xA5
#define EEPROM_SIZE              (JOYBUTTONS * 2 + 2)

// Target modes selectable by the user with a 5 seconds mouse button hold
#define MODE_C64   0
#define MODE_AMIGA 1
#define MODE_ATARI 2

uint8_t gMode = MODE_C64;

// Keep original flags, but they are forced from gMode
int ISC64 = 0;
int ISAMIGA = 1;

uint8_t H[4]  = { LOW, LOW, HIGH, HIGH };
uint8_t HQ[4] = { LOW, HIGH, HIGH, LOW };

uint8_t QX = 3;
uint8_t QY = 3;

// Define the volatile variables for the hardware timers
volatile uint64_t delayOnX = MINdelayOnX;
volatile uint64_t delayOnY = MINdelayOnY;
volatile uint64_t delayOffX = 10;
volatile uint64_t delayOffY = 10;

// Define the hardware timers
hw_timer_t *timerOnX = NULL;
hw_timer_t *timerOnY = NULL;
hw_timer_t *timerOffX = NULL;
hw_timer_t *timerOffY = NULL;

// Variables used to configure the board for the joystick connected
bool configMode = false;
uint8_t defaultConfigData[100];
int defaultConfigLength = 0;

// These two in particular use the configuration to match the usage of the controller
uint8_t joyVal[JOYBUTTONS];
uint8_t joyPos[JOYBUTTONS];

// -------------------- Mode switching by mouse hold --------------------
static esp_timer_handle_t modeHoldTimer = NULL;
static volatile uint8_t modeHoldMask = 0;      // 0x01 left, 0x02 right, 0x04 middle
static volatile uint8_t modeHoldTarget = MODE_C64;
static volatile bool modeHoldArmed = false;
static volatile bool modeSwitchInProgress = false;

// ---- NEW: allow mode switching only in the first 30 seconds after boot ----
#define MODE_SWITCH_WINDOW_US (30LL * 1000000LL)   // 30 seconds
static int64_t gModeSwitchDeadlineUs = 0;
static volatile bool gModeSwitchEnabled = true;

static inline void clearModeHoldState() {
  modeHoldArmed = false;
  modeHoldMask = 0;
}

// Checks if mode switching is still allowed (and permanently disables it after the window)
static inline bool modeSwitchAllowedNow() {
  if (!gModeSwitchEnabled) return false;

  if (esp_timer_get_time() > gModeSwitchDeadlineUs) {
    // Window expired: lock permanently
    gModeSwitchEnabled = false;

    // Stop any pending hold
    if (modeHoldTimer) esp_timer_stop(modeHoldTimer);
    clearModeHoldState();

    return false;
  }
  return true;
}

static void setRuntimeLed() {
  // Runtime behavior: mouse = blue, joystick = green
  if (digitalRead(SWITCH_MJ)) ws2812b.setPixelColor(0, LED_BLUE());
  else                        ws2812b.setPixelColor(0, LED_GREEN());
  ws2812b.show();
}

static void blinkModeLed(uint8_t mode) {
  uint32_t c = LED_BLUE();               // C64
  if (mode == MODE_AMIGA) c = LED_GREEN();
  if (mode == MODE_ATARI) c = LED_RED();

  for (int i = 0; i < 3; i++) {
    ws2812b.clear();
    ws2812b.show();
    delay(120);
    ws2812b.setPixelColor(0, c);
    ws2812b.show();
    delay(120);
  }
  ws2812b.clear();
  ws2812b.show();
}

static uint8_t loadModeFromEEPROM() {
  uint8_t magic = EEPROM.read(EEPROM_MODE_MAGIC_ADDR);
  uint8_t mode  = EEPROM.read(EEPROM_MODE_VALUE_ADDR);

  if (magic != EEPROM_MODE_MAGIC || mode > MODE_ATARI) {
    // Default mode on first boot: C64
    mode = MODE_C64;
    EEPROM.write(EEPROM_MODE_MAGIC_ADDR, EEPROM_MODE_MAGIC);
    EEPROM.write(EEPROM_MODE_VALUE_ADDR, mode);
    EEPROM.commit();
  }
  return mode;
}

static void saveModeToEEPROM(uint8_t mode) {
  EEPROM.write(EEPROM_MODE_MAGIC_ADDR, EEPROM_MODE_MAGIC);
  EEPROM.write(EEPROM_MODE_VALUE_ADDR, mode);
  EEPROM.commit();
}

static void applyModeToOriginalFlags() {
  if (gMode == MODE_C64) {
    ISC64 = 10;      // Force the original "C64 path"
    ISAMIGA = 1;     // Irrelevant in C64 path
  } else {
    ISC64 = 0;       // Force the original "AMIGA/ATARI path"
    ISAMIGA = (gMode == MODE_AMIGA) ? 1 : 0;
  }
}

static void requestModeChange(uint8_t newMode) {
  // NEW: also enforce the 30s window here (covers edge-case scheduling)
  if (!modeSwitchAllowedNow()) { modeSwitchInProgress = false; return; }

  if (newMode > MODE_ATARI) { modeSwitchInProgress = false; return; }
  if (newMode == gMode)     { modeSwitchInProgress = false; clearModeHoldState(); return; }

  gMode = newMode;
  saveModeToEEPROM(gMode);

  // Visual feedback: 3 blinks with selected mode color, then return to runtime color
  blinkModeLed(gMode);
  setRuntimeLed();

  // Reboot to re-init pins/timers/USB in a clean state
  esp_restart();
}

static void modeSwitchTask(void *arg) {
  // NEW: double-check window in task context too
  if (!modeSwitchAllowedNow()) {
    modeSwitchInProgress = false;
    clearModeHoldState();
    vTaskDelete(NULL);
  }

  uint8_t mode = (uint8_t)(uintptr_t)arg;
  requestModeChange(mode);
  vTaskDelete(NULL);
}

static void modeHoldTimerCb(void *arg) {
  (void)arg;

  // NEW: if window expired, do nothing
  if (!modeSwitchAllowedNow()) { modeSwitchInProgress = false; return; }

  if (modeSwitchInProgress) return;
  if (!modeHoldArmed) return;

  uint8_t m = modeHoldMask;
  if (!(m == 0x01 || m == 0x02 || m == 0x04)) { clearModeHoldState(); return; }

  uint8_t target = modeHoldTarget;

  if (target == gMode) {
    clearModeHoldState();
    modeSwitchInProgress = false;
    return;
  }

  if (!((m == 0x01 && target == MODE_C64) ||
        (m == 0x02 && target == MODE_AMIGA) ||
        (m == 0x04 && target == MODE_ATARI))) {
    clearModeHoldState();
    return;
  }

  modeSwitchInProgress = true;

  BaseType_t ok = xTaskCreatePinnedToCore(
    modeSwitchTask, "mode_switch", 4096, (void*)(uintptr_t)target, 3, NULL, 1
  );

  if (ok != pdPASS) {
    modeSwitchInProgress = false;
    clearModeHoldState();
  }
}

static inline void stopModeHold() {
  if (modeHoldArmed) esp_timer_stop(modeHoldTimer);
  clearModeHoldState();
}

static void updateModeHoldFromMouse(hid_mouse_input_report_boot_t *mouse_report) {
  // NEW: only allow arming/holding in the first 30 seconds after boot
  if (!modeSwitchAllowedNow()) return;

  uint8_t mask = 0;
  if (mouse_report->buttons.button1) mask |= 0x01; // Left
  if (mouse_report->buttons.button2) mask |= 0x02; // Right
  if (mouse_report->buttons.button3) mask |= 0x04; // Middle

  // Only accept exactly one button held
  if (!(mask == 0x01 || mask == 0x02 || mask == 0x04)) {
    stopModeHold();
    return;
  }

  modeHoldMask = mask;

  uint8_t target = MODE_C64;
  if (mask == 0x01) target = MODE_C64;
  else if (mask == 0x02) target = MODE_AMIGA;
  else target = MODE_ATARI;

  // If already armed with same target, do nothing
  if (modeHoldArmed && modeHoldTarget == target) return;

  modeHoldTarget = target;
  modeHoldArmed = true;
  esp_timer_stop(modeHoldTimer);
  esp_timer_start_once(modeHoldTimer, 5000000ULL);
}

// -------------------- USB host plumbing --------------------
static const char *TAG = "USB MESSAGE";
QueueHandle_t hid_host_event_queue;

typedef struct {
  hid_host_device_handle_t hid_device_handle;
  hid_host_driver_event_t event;
  void *arg;
} hid_host_event_queue_t;

static const char *hid_proto_name_str[] = {"NONE", "KEYBOARD", "MOUSE"};

static inline int16_t signExtend12(uint16_t v) {
  v &= 0x0FFF;
  if (v & 0x0800) return (int16_t)(v | 0xF000);
  return (int16_t)v;
}

static inline int8_t clampToI8(int16_t v) {
  if (v > 127) return 127;
  if (v < -127) return -127;
  return (int8_t)v;
}

// -------------------- Micromys wheel -> C64 pulses (LEFT/RIGHT lines) --------------------
// Protocol details: pulses active-low, ~50ms low, ~50ms high between pulses.
// We implement non-blocking by using a FreeRTOS task fed by the USB callback.
#define MICROMYS_LOW_MS  50
#define MICROMYS_GAP_MS  50

// If scrolling "up" gives wheel byte 0x01 (and down gives 0xFF), keep 1.
// If it's inverted for your mouse, set to 0.
#define WHEEL_01_IS_UP   1

static TaskHandle_t micromysWheelTaskH = NULL;
static volatile uint16_t mm_up_pending = 0;   // UP pulses  -> C64_LEFT  (bit2)
static volatile uint16_t mm_dn_pending = 0;   // DN pulses  -> C64_RIGHT (bit3)
static portMUX_TYPE mm_mux = portMUX_INITIALIZER_UNLOCKED;

static inline void micromysEnqueueWheel(int8_t wheel)
{
  if (wheel == 0 || micromysWheelTaskH == NULL) return;

  uint16_t steps = (wheel < 0) ? (uint16_t)(-wheel) : (uint16_t)wheel;

#if WHEEL_01_IS_UP
  bool up = (wheel > 0);
#else
  bool up = (wheel < 0);
#endif

  portENTER_CRITICAL(&mm_mux);
  if (up) mm_up_pending += steps;
  else    mm_dn_pending += steps;
  portEXIT_CRITICAL(&mm_mux);

  xTaskNotifyGive(micromysWheelTaskH);
}

static void micromysWheelTask(void *arg)
{
  (void)arg;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    while (true) {
      uint8_t pin = 0;

      portENTER_CRITICAL(&mm_mux);
      if (mm_up_pending) { mm_up_pending--; pin = C64_LEFT; }
      else if (mm_dn_pending) { mm_dn_pending--; pin = C64_RIGHT; }
      portEXIT_CRITICAL(&mm_mux);

      if (pin == 0) break;

      // Active-low pulse
      pinMode(pin, OUTPUT);
      digitalWrite(pin, LOW);
      vTaskDelay(pdMS_TO_TICKS(MICROMYS_LOW_MS));

      // Release line (C64 pull-up brings it to 1)
      digitalWrite(pin, LOW);
      pinMode(pin, INPUT);
      vTaskDelay(pdMS_TO_TICKS(MICROMYS_GAP_MS));
    }
  }
}

// -------------------- Amiga wheel -> "middle button + vertical move" --------------------
#define AMIGA_WHEEL_STEP_Y   12 // Scroll "intensity"
#define AMIGA_WHEEL_INVERT   0 // Scroll direction

static inline int8_t amigaWheelToDeltaY(int8_t wheel)
{
  if (wheel == 0) return 0;

  uint16_t steps = (wheel < 0) ? (uint16_t)(-wheel) : (uint16_t)wheel;
  int16_t delta = (int16_t)steps * (int16_t)AMIGA_WHEEL_STEP_Y;
  if (delta > 127) delta = 127;

#if WHEEL_01_IS_UP
  bool up = (wheel > 0);
#else
  bool up = (wheel < 0);
#endif

#if AMIGA_WHEEL_INVERT
  up = !up;
#endif

  // In a_mouse_m(): y_displacement > 0 => A_Down, y_displacement < 0 => A_Up
  return (int8_t)(up ? -delta : delta);
}

static inline void amigaWheelEmulate(const hid_mouse_input_report_boot_t *base, int8_t wheel)
{
  int8_t y = amigaWheelToDeltaY(wheel);
  if (y == 0) return;

  hid_mouse_input_report_boot_t r = *base;
  r.x_displacement = 0;
  r.y_displacement = y;

  r.buttons.button3 = 1;
  a_mouse_m(&r);

  r.buttons.button3 = base->buttons.button3;
  r.y_displacement = 0;
  a_mouse_m(&r);
}

// -------------------- HID callbacks --------------------
static void hid_host_mouse_report_callback(const uint8_t *const data, const int length) {
  Serial.printf("len=%d  ", length);
  for (int i=0;i<length;i++) Serial.printf("%02X ", data[i]);
  Serial.print("\r\n");
  
  if (length < 3) return;

  hid_mouse_input_report_boot_t boot;
  memset(&boot, 0, sizeof(boot));

  int8_t wheel = 0;

  if (length == 3) {
    memcpy(&boot.buttons, &data[0], 1);
    boot.x_displacement = (int8_t)data[1];
    boot.y_displacement = (int8_t)data[2];
  } else if (length == 4) {
    memcpy(&boot.buttons, &data[0], 1);
    boot.x_displacement = (int8_t)data[1];
    boot.y_displacement = (int8_t)data[2];
    wheel               = (int8_t)data[3];
  } else if (length == 5) {
    memcpy(&boot.buttons, &data[0], 1);
    uint8_t b1 = data[1];
    uint8_t b2 = data[2];
    uint8_t b3 = data[3];
    int16_t x12 = signExtend12((uint16_t)b1 | ((uint16_t)(b2 & 0x0F) << 8));
    int16_t y12 = signExtend12((uint16_t)((b2 >> 4) & 0x0F) | ((uint16_t)b3 << 4));
    boot.x_displacement = clampToI8(x12);
    boot.y_displacement = clampToI8(y12);
    wheel = (int8_t)data[4];
  } else if (length == 6) {
    memcpy(&boot.buttons, &data[1], 1);
    uint8_t b1 = data[2];
    uint8_t b2 = data[3];
    uint8_t b3 = data[4];
    int16_t x12 = signExtend12((uint16_t)b1 | ((uint16_t)(b2 & 0x0F) << 8));
    int16_t y12 = signExtend12((uint16_t)((b2 >> 4) & 0x0F) | ((uint16_t)b3 << 4));
    boot.x_displacement = clampToI8(x12);
    boot.y_displacement = clampToI8(y12);
    wheel = (int8_t)data[5];
  } else if (length == 8) {
    memcpy(&boot.buttons, &data[1], 1);
    uint8_t b1 = data[2];
    uint8_t b2 = data[3];
    uint8_t b3 = data[4];
    uint8_t b4 = data[5];
    int16_t x16 = (int16_t)((uint16_t)b1 | ((uint16_t)b2 << 8));
    int16_t y16 = (int16_t)((uint16_t)b3 | ((uint16_t)b4 << 8));
    boot.x_displacement = x16;
    boot.y_displacement = y16;
    wheel = (int8_t)data[6];
  }

  // Micromys wheel support only in C64 + mouse mode:
  if ((ISC64 > 0) && digitalRead(SWITCH_MJ)) {
    micromysEnqueueWheel(wheel);
  }

  hid_mouse_input_report_boot_t *mouse_report = &boot;

  updateModeHoldFromMouse(mouse_report);

  if (digitalRead(SWITCH_MJ)) {       // Mouse mode
    if (ISC64 > 0) c64_mouse_m(mouse_report);
    else           a_mouse_m(mouse_report);
  } else {                            // Joystick mode (mouse as joystick)
    if (ISC64 > 0) c64_mouse_j(mouse_report);
    else           a_mouse_j(mouse_report);
  }
  
  // ---- NEW: wheel mapping for AMIGA in mouse mode ----
  if ((ISC64 <= 0) && (ISAMIGA == 1) && digitalRead(SWITCH_MJ) && (wheel != 0)) {
    amigaWheelEmulate(mouse_report, wheel);
  }
}


static void hid_host_generic_report_callback(const uint8_t *const data, const int length) {
#if (JOY_MAPPING_MODE == JOY_MAP_LEARN)
  if (configMode) {
    c64_joystick_config(data, length);
    return;
  }
#endif

  if (digitalRead(SWITCH_MJ)) {       // Mouse mode (joystick as mouse)
    if (ISC64 > 0) c64_joystick_m(data, length);
    else           a_joystick_m(data, length);
  } else {                            // Joystick mode
    if (ISC64 > 0) c64_joystick_j(data, length);
    else           a_joystick_j(data, length);
  }
}

void hid_host_interface_callback(hid_host_device_handle_t hid_device_handle, const hid_host_interface_event_t event, void *arg) {
  uint8_t data[64] = {0};
  size_t data_length = 0;
  hid_host_dev_params_t dev_params;
  ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));

  switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
      ESP_ERROR_CHECK(hid_host_device_get_raw_input_report_data(hid_device_handle, data, 64, &data_length));
      if (HID_SUBCLASS_BOOT_INTERFACE == dev_params.sub_class) {
        if (HID_PROTOCOL_MOUSE == dev_params.proto) {
          hid_host_mouse_report_callback(data, (int)data_length);
        }
      } else {
        hid_host_generic_report_callback(data, (int)data_length);
      }
      break;

    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
      ESP_LOGI(TAG, "HID Device, protocol '%s' DISCONNECTED", hid_proto_name_str[dev_params.proto]);
      ESP_ERROR_CHECK(hid_host_device_close(hid_device_handle));
      break;

    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
      ESP_LOGI(TAG, "HID Device, protocol '%s' TRANSFER_ERROR", hid_proto_name_str[dev_params.proto]);
      break;

    default:
      ESP_LOGE(TAG, "HID Device, protocol '%s' Unhandled event", hid_proto_name_str[dev_params.proto]);
      break;
  }
}

void hid_host_device_event(hid_host_device_handle_t hid_device_handle, const hid_host_driver_event_t event, void *arg) {
  hid_host_dev_params_t dev_params;
  ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));
  const hid_host_device_config_t dev_config = {.callback = hid_host_interface_callback, .callback_arg = NULL};

  switch (event) {
    case HID_HOST_DRIVER_EVENT_CONNECTED:
      ESP_LOGI(TAG, "HID Device, protocol '%s' CONNECTED", hid_proto_name_str[dev_params.proto]);
      ESP_ERROR_CHECK(hid_host_device_open(hid_device_handle, &dev_config));
      if (HID_SUBCLASS_BOOT_INTERFACE == dev_params.sub_class) {

        // Mouse: REPORT (needed for wheel + packed 12-bit XY in your case)
        // Keyboard: keep BOOT
        if (HID_PROTOCOL_MOUSE == dev_params.proto) {
          ESP_ERROR_CHECK(hid_class_request_set_protocol(hid_device_handle, HID_REPORT_PROTOCOL_REPORT));
        } else {
          ESP_ERROR_CHECK(hid_class_request_set_protocol(hid_device_handle, HID_REPORT_PROTOCOL_BOOT));
          if (HID_PROTOCOL_KEYBOARD == dev_params.proto) {
            ESP_ERROR_CHECK(hid_class_request_set_idle(hid_device_handle, 0, 0));
          }
        }
      }
      ESP_ERROR_CHECK(hid_host_device_start(hid_device_handle));
      break;
    default:
      break;
  }
}

static void usb_lib_task(void *arg) {
  const usb_host_config_t host_config = {.skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1};
  ESP_ERROR_CHECK(usb_host_install(&host_config));
  xTaskNotifyGive((TaskHandle_t)arg);

  while (true) {
    uint32_t event_flags;
    usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
    if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
      usb_host_device_free_all();
      ESP_LOGI(TAG, "USB Event flags: NO_CLIENTS");
    }
    if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
      ESP_LOGI(TAG, "USB Event flags: ALL_FREE");
    }
  }
}

void hid_host_task(void *pvParameters) {
  hid_host_event_queue_t evt_queue;
  hid_host_event_queue = xQueueCreate(10, sizeof(hid_host_event_queue_t));

  while (true) {
    if (xQueueReceive(hid_host_event_queue, &evt_queue, pdMS_TO_TICKS(50))) {
      hid_host_device_event(evt_queue.hid_device_handle, evt_queue.event, evt_queue.arg);
    }
  }
}

void hid_host_device_callback(hid_host_device_handle_t hid_device_handle, const hid_host_driver_event_t event, void *arg) {
  const hid_host_event_queue_t evt_queue = {.hid_device_handle = hid_device_handle, .event = event, .arg = arg};
  xQueueSend(hid_host_event_queue, &evt_queue, 0);
}

void app_main(void) {
  BaseType_t task_created;
  ESP_LOGI(TAG, "HID Host example");
  task_created = xTaskCreatePinnedToCore(usb_lib_task, "usb_events", 4096, xTaskGetCurrentTaskHandle(), 2, NULL, 0);
  assert(task_created == pdTRUE);
  ulTaskNotifyTake(false, 1000);

  const hid_host_driver_config_t hid_host_driver_config = {
    .create_background_task = true,
    .task_priority = 5,
    .stack_size = 4096,
    .core_id = 0,
    .callback = hid_host_device_callback,
    .callback_arg = NULL
  };
  ESP_ERROR_CHECK(hid_host_install(&hid_host_driver_config));

  task_created = xTaskCreate(&hid_host_task, "hid_task", 4 * 1024, NULL, 2, NULL);
  assert(task_created == pdTRUE);
}

// -------------------- C64 timers/interrupts --------------------
void IRAM_ATTR handleInterrupt() {
  // Workaround: sometimes the interrupt triggers twice, check pin level
  if (!((GPIO.in >> C64_INT) & 1)) return;
  timerWrite(timerOnX, 0);
  timerAlarm(timerOnX, delayOnX, false, 0);
  timerWrite(timerOnY, 0);
  timerAlarm(timerOnY, delayOnY, false, 0);
}

void IRAM_ATTR turnOnPotX() {
  GPIO.out_w1ts = (1 << C64_POTX);
  timerWrite(timerOffX, 0);
  timerAlarm(timerOffX, delayOffX, false, 0);
}

void IRAM_ATTR turnOffPotX() { GPIO.out_w1tc = (1 << C64_POTX); }

void IRAM_ATTR turnOnPotY() {
  GPIO.out_w1ts = (1 << C64_POTY);
  timerWrite(timerOffY, 0);
  timerAlarm(timerOffY, delayOffY, false, 0);
}

void IRAM_ATTR turnOffPotY() { GPIO.out_w1tc = (1 << C64_POTY); }

void IRAM_ATTR turnOffJoyX() {
  digitalWrite(C64_LEFT, LOW);
  digitalWrite(C64_RIGHT, LOW);
  pinMode(C64_LEFT, INPUT);
  pinMode(C64_RIGHT, INPUT);
}

void IRAM_ATTR turnOffJoyY() {
  digitalWrite(C64_UP, LOW);
  digitalWrite(C64_DOWN, LOW);
  pinMode(C64_UP, INPUT);
  pinMode(C64_DOWN, INPUT);
}

// -------------------- C64 mouse (mouse mode) --------------------
void c64_mouse_m(hid_mouse_input_report_boot_t *mouse_report) {
  if (mouse_report->buttons.button1) { pinMode(C64_FIRE, OUTPUT); digitalWrite(C64_FIRE, LOW); }
  else { digitalWrite(C64_FIRE, LOW); pinMode(C64_FIRE, INPUT); }

  if (mouse_report->buttons.button2) { pinMode(C64_UP, OUTPUT); digitalWrite(C64_UP, LOW); }
  else { digitalWrite(C64_UP, LOW); pinMode(C64_UP, INPUT); }

  if (mouse_report->buttons.button3) { pinMode(C64_DOWN, OUTPUT); digitalWrite(C64_DOWN, LOW); }
  else { digitalWrite(C64_DOWN, LOW); pinMode(C64_DOWN, INPUT); }

  delayOnX += (STEPdelayOnX * mouse_report->x_displacement);
  if (delayOnX > MAXdelayOnX) delayOnX = MINdelayOnX;
  if (delayOnX < MINdelayOnX) delayOnX = MAXdelayOnX;

  delayOnY -= (STEPdelayOnY * mouse_report->y_displacement);
  if (delayOnY > MAXdelayOnY) delayOnY = MINdelayOnY;
  if (delayOnY < MINdelayOnY) delayOnY = MAXdelayOnY;
}

// -------------------- C64 mouse (joystick mode) --------------------
void c64_mouse_j(hid_mouse_input_report_boot_t *mouse_report) {
  if (mouse_report->buttons.button1) { pinMode(C64_FIRE, OUTPUT); digitalWrite(C64_FIRE, LOW); }
  else { digitalWrite(C64_FIRE, LOW); pinMode(C64_FIRE, INPUT); }

  if (mouse_report->x_displacement > 0) { pinMode(C64_RIGHT, OUTPUT); digitalWrite(C64_RIGHT, LOW); }
  else { pinMode(C64_LEFT, OUTPUT); digitalWrite(C64_LEFT, LOW); }

  if (mouse_report->y_displacement > 0) { pinMode(C64_DOWN, OUTPUT); digitalWrite(C64_DOWN, LOW); }
  else { pinMode(C64_UP, OUTPUT); digitalWrite(C64_UP, LOW); }

  timerWrite(timerOffX, 0);
  timerAlarm(timerOffX, abs(mouse_report->x_displacement) * M2JCalib, false, 0);
  timerWrite(timerOffY, 0);
  timerAlarm(timerOffY, abs(mouse_report->y_displacement) * M2JCalib, false, 0);
}

// -------------------- Pin apply helpers --------------------
static inline void applyPins_C64(bool up, bool down, bool left, bool right, bool fire) {
  if (up)    { pinMode(C64_UP, OUTPUT);    digitalWrite(C64_UP, LOW); }    else { digitalWrite(C64_UP, LOW);    pinMode(C64_UP, INPUT); }
  if (down)  { pinMode(C64_DOWN, OUTPUT);  digitalWrite(C64_DOWN, LOW); }  else { digitalWrite(C64_DOWN, LOW);  pinMode(C64_DOWN, INPUT); }
  if (left)  { pinMode(C64_LEFT, OUTPUT);  digitalWrite(C64_LEFT, LOW); }  else { digitalWrite(C64_LEFT, LOW);  pinMode(C64_LEFT, INPUT); }
  if (right) { pinMode(C64_RIGHT, OUTPUT); digitalWrite(C64_RIGHT, LOW); } else { digitalWrite(C64_RIGHT, LOW); pinMode(C64_RIGHT, INPUT); }
  if (fire)  { pinMode(C64_FIRE, OUTPUT);  digitalWrite(C64_FIRE, LOW); }  else { digitalWrite(C64_FIRE, LOW);  pinMode(C64_FIRE, INPUT); }
}

static inline void applyPins_A(bool up, bool down, bool left, bool right, bool fire) {
  if (up)    { pinMode(A_UP, OUTPUT);    digitalWrite(A_UP, LOW); }    else { digitalWrite(A_UP, LOW);    pinMode(A_UP, INPUT); }
  if (down)  { pinMode(A_DOWN, OUTPUT);  digitalWrite(A_DOWN, LOW); }  else { digitalWrite(A_DOWN, LOW);  pinMode(A_DOWN, INPUT); }
  if (left)  { pinMode(A_LEFT, OUTPUT);  digitalWrite(A_LEFT, LOW); }  else { digitalWrite(A_LEFT, LOW);  pinMode(A_LEFT, INPUT); }
  if (right) { pinMode(A_RIGHT, OUTPUT); digitalWrite(A_RIGHT, LOW); } else { digitalWrite(A_RIGHT, LOW); pinMode(A_RIGHT, INPUT); }
  if (fire)  { pinMode(A_FIRE, OUTPUT);  digitalWrite(A_FIRE, LOW); }  else { digitalWrite(A_FIRE, LOW);  pinMode(A_FIRE, INPUT); }
}

static inline void applyAutofirePulse_C64(bool enabled) {
  if (!enabled) return;
  pinMode(C64_FIRE, OUTPUT);
  digitalWrite(C64_FIRE, LOW);
  delay(5);
  digitalWrite(C64_FIRE, LOW);
  pinMode(C64_FIRE, INPUT);
}

static inline void applyAutofirePulse_A(bool enabled) {
  if (!enabled) return;
  pinMode(A_FIRE, OUTPUT);
  digitalWrite(A_FIRE, LOW);
  delay(5);
  digitalWrite(A_FIRE, LOW);
  pinMode(A_FIRE, INPUT);
}

// Auto left/right pulse (C64)
static inline void applyAutoLeftRight_C64(bool enabled) {
  if (!enabled) return;

  pinMode(C64_LEFT, OUTPUT);
  digitalWrite(C64_LEFT, LOW);
  delay(8);
  digitalWrite(C64_LEFT, LOW);
  pinMode(C64_LEFT, INPUT);
  delay(8);

  pinMode(C64_RIGHT, OUTPUT);
  digitalWrite(C64_RIGHT, LOW);
  delay(8);
  digitalWrite(C64_RIGHT, LOW);
  pinMode(C64_RIGHT, INPUT);
  delay(8);
}

// Auto left/right pulse (Amiga/Atari joystick pins)
static inline void applyAutoLeftRight_A(bool enabled) {
  if (!enabled) return;

  pinMode(A_LEFT, OUTPUT);
  digitalWrite(A_LEFT, LOW);
  delay(8);
  digitalWrite(A_LEFT, LOW);
  pinMode(A_LEFT, INPUT);
  delay(8);

  pinMode(A_RIGHT, OUTPUT);
  digitalWrite(A_RIGHT, LOW);
  delay(8);
  digitalWrite(A_RIGHT, LOW);
  pinMode(A_RIGHT, INPUT);
  delay(8);
}

// -------------------- Joystick (joystick mode) --------------------
void c64_joystick_j(const uint8_t *const data, const int length) {
#if (JOY_MAPPING_MODE == JOY_MAP_CUSTOM)
  bool up=false, down=false, left=false, right=false, fire=false;
  bool autofireEnabled = false;
  bool autoleftrightEnabled = false;

  JM_DecodeJoystickMode(data, length, up, down, left, right, fire, autofireEnabled, autoleftrightEnabled);

  applyAutofirePulse_C64(autofireEnabled);
  applyAutoLeftRight_C64(autoleftrightEnabled);
  applyPins_C64(up, down, left, right, fire);
#else
  if (data[joyPos[0]] == joyVal[0]) { pinMode(C64_UP, OUTPUT); digitalWrite(C64_UP, LOW); }
  else { digitalWrite(C64_UP, LOW); pinMode(C64_UP, INPUT); }

  if (data[joyPos[1]] == joyVal[1]) { pinMode(C64_DOWN, OUTPUT); digitalWrite(C64_DOWN, LOW); }
  else { digitalWrite(C64_DOWN, LOW); pinMode(C64_DOWN, INPUT); }

  if (data[joyPos[2]] == joyVal[2]) { pinMode(C64_LEFT, OUTPUT); digitalWrite(C64_LEFT, LOW); }
  else { digitalWrite(C64_LEFT, LOW); pinMode(C64_LEFT, INPUT); }

  if (data[joyPos[3]] == joyVal[3]) { pinMode(C64_RIGHT, OUTPUT); digitalWrite(C64_RIGHT, LOW); }
  else { digitalWrite(C64_RIGHT, LOW); pinMode(C64_RIGHT, INPUT); }

  if ((data[joyPos[4]] == joyVal[4]) | (data[joyPos[5]] == joyVal[5]) | (data[joyPos[6]] == joyVal[6])) {
    pinMode(C64_FIRE, OUTPUT);
    digitalWrite(C64_FIRE, LOW);
  } else {
    digitalWrite(C64_FIRE, LOW);
    pinMode(C64_FIRE, INPUT);
  }
#endif
}

void a_joystick_j(const uint8_t *const data, const int length) {
#if (JOY_MAPPING_MODE == JOY_MAP_CUSTOM)
  bool up=false, down=false, left=false, right=false, fire=false;
  bool autofireEnabled = false;
  bool autoleftrightEnabled = false;

  JM_DecodeJoystickMode(data, length, up, down, left, right, fire, autofireEnabled, autoleftrightEnabled);

  applyAutofirePulse_A(autofireEnabled);
  applyAutoLeftRight_A(autoleftrightEnabled);
  applyPins_A(up, down, left, right, fire);
#else
  if (data[joyPos[0]] == joyVal[0]) { pinMode(A_UP, OUTPUT); digitalWrite(A_UP, LOW); }
  else { digitalWrite(A_UP, LOW); pinMode(A_UP, INPUT); }

  if (data[joyPos[1]] == joyVal[1]) { pinMode(A_DOWN, OUTPUT); digitalWrite(A_DOWN, LOW); }
  else { digitalWrite(A_DOWN, LOW); pinMode(A_DOWN, INPUT); }

  if (data[joyPos[2]] == joyVal[2]) { pinMode(A_LEFT, OUTPUT); digitalWrite(A_LEFT, LOW); }
  else { digitalWrite(A_LEFT, LOW); pinMode(A_LEFT, INPUT); }

  if (data[joyPos[3]] == joyVal[3]) { pinMode(A_RIGHT, OUTPUT); digitalWrite(A_RIGHT, LOW); }
  else { digitalWrite(A_RIGHT, LOW); pinMode(A_RIGHT, INPUT); }

  if ((data[joyPos[4]] == joyVal[4]) | (data[joyPos[5]] == joyVal[5]) | (data[joyPos[6]] == joyVal[6])) {
    pinMode(A_FIRE, OUTPUT);
    digitalWrite(A_FIRE, LOW);
  } else {
    digitalWrite(A_FIRE, LOW);
    pinMode(A_FIRE, INPUT);
  }
#endif
}

// -------------------- Joystick (mouse mode) --------------------
void c64_joystick_m(const uint8_t *const data, const int length) {
#if (JOY_MAPPING_MODE == JOY_MAP_CUSTOM)
  bool fire=false, up=false;
  bool autofireEnabled=false;
  int dX = 0, dY = 0;

  JM_DecodeMouseModeButtons_C64(data, length, fire, up, autofireEnabled);
  JM_AnalogToMouseDelta_C64(data, length, dX, dY);

  if (fire) { pinMode(C64_FIRE, OUTPUT); digitalWrite(C64_FIRE, LOW); }
  else      { digitalWrite(C64_FIRE, LOW); pinMode(C64_FIRE, INPUT); }

  if (up)   { pinMode(C64_UP, OUTPUT); digitalWrite(C64_UP, LOW); }
  else      { digitalWrite(C64_UP, LOW); pinMode(C64_UP, INPUT); }

  delayOnX += dX;
  if (delayOnX > MAXdelayOnX) delayOnX = MINdelayOnX;
  if (delayOnX < MINdelayOnX) delayOnX = MAXdelayOnX;

  delayOnY += dY;
  if (delayOnY > MAXdelayOnY) delayOnY = MINdelayOnY;
  if (delayOnY < MINdelayOnY) delayOnY = MAXdelayOnY;

  applyAutofirePulse_C64(autofireEnabled);
#else
  float x = 0;
  float y = 0;

  if (data[joyPos[0]] == joyVal[0]) y =  3 * STEPdelayOnY;
  if (data[joyPos[1]] == joyVal[1]) y = -3 * STEPdelayOnY;
  if (data[joyPos[2]] == joyVal[2]) x = -3 * STEPdelayOnX;
  if (data[joyPos[3]] == joyVal[3]) x =  3 * STEPdelayOnY;

  if (data[joyPos[4]] == joyVal[4]) { pinMode(C64_FIRE, OUTPUT); digitalWrite(C64_FIRE, LOW); }
  else { digitalWrite(C64_FIRE, LOW); pinMode(C64_FIRE, INPUT); }

  if (data[joyPos[5]] == joyVal[5]) { pinMode(C64_UP, OUTPUT); digitalWrite(C64_UP, LOW); }
  else { digitalWrite(C64_UP, LOW); pinMode(C64_UP, INPUT); }

  delayOnX += x;
  if (delayOnX > MAXdelayOnX) delayOnX = MINdelayOnX;
  if (delayOnX < MINdelayOnX) delayOnX = MAXdelayOnX;

  delayOnY += y;
  if (delayOnY > MAXdelayOnY) delayOnY = MINdelayOnY;
  if (delayOnY < MINdelayOnY) delayOnY = MAXdelayOnY;
#endif
}

// -------------------- Amiga/Atari quadrature helpers --------------------
void AHorizontalMove(int pulse) {
  digitalWrite(A_DOWN, H[QX]);
  if (ISAMIGA == 1) digitalWrite(A_RIGHT, HQ[QX]);
  else              digitalWrite(A_UP, HQ[QX]);
  delayMicroseconds(pulse);
}

void AVerticalMove(int pulse) {
  if (ISAMIGA == 1) digitalWrite(A_UP, H[QY]);
  else              digitalWrite(A_RIGHT, H[QY]);
  digitalWrite(A_LEFT, HQ[QY]);
  delayMicroseconds(pulse);
}

void A_Left(int pulse)  { AHorizontalMove(pulse); QX = (QX >= 3) ? 0 : ++QX; }
void A_Right(int pulse) { AHorizontalMove(pulse); QX = (QX <= 0) ? 3 : --QX; }
void A_Down(int pulse)  { AVerticalMove(pulse);   QY = (QY <= 0) ? 3 : --QY; }
void A_Up(int pulse)    { AVerticalMove(pulse);   QY = (QY >= 3) ? 0 : ++QY; }

// -------------------- Amiga/Atari joystick-as-mouse (custom supports analog) --------------------
void a_joystick_m(const uint8_t *const data, const int length) {
#if (JOY_MAPPING_MODE == JOY_MAP_CUSTOM)
  bool fire = false;
  bool button2 = false;
  bool autofireEnabled = false;

  JM_DecodeMouseModeButtons_A(data, length, fire, button2, autofireEnabled);

  // Apply buttons (same behavior as your custom firmware: fire + button2)
  if (fire) { pinMode(A_FIRE, OUTPUT); digitalWrite(A_FIRE, LOW); }
  else      { digitalWrite(A_FIRE, LOW); pinMode(A_FIRE, INPUT); }

  if (button2) { pinMode(A_BUTTON2, OUTPUT); digitalWrite(A_BUTTON2, LOW); }
  else         { digitalWrite(A_BUTTON2, LOW); pinMode(A_BUTTON2, INPUT); }

  // Analog -> signed steps (same concept as your custom function)
  int xStepsSigned = 0;
  int yStepsSigned = 0;
  JM_AnalogToMouseSteps_A(data, length, xStepsSigned, yStepsSigned);

  int xsign = (xStepsSigned > 0 ? 1 : 0);
  int ysign = (yStepsSigned > 0 ? 1 : 0);
  int xsteps = abs(xStepsSigned);
  int ysteps = abs(yStepsSigned);

  int xpulse = PULSE_LENGTH;
  int ypulse = PULSE_LENGTH;

  // Atari speed scaling (keeps your original idea, avoids division by zero)
  if (ISAMIGA == 0) {
    int xs = (xsteps <= 0) ? 1 : xsteps;
    int ys = (ysteps <= 0) ? 1 : ysteps;
    xpulse = (int)(JM_ATARI_PULSE_SCALE * (float)PULSE_LENGTH / (float)xs);
    ypulse = (int)(JM_ATARI_PULSE_SCALE * (float)PULSE_LENGTH / (float)ys);
    if (xpulse < 1) xpulse = 1;
    if (ypulse < 1) ypulse = 1;
  }

  while ((xsteps | ysteps) != 0) {
    if (xsteps != 0) {
      if (xsign) A_Right(xpulse);
      else       A_Left(xpulse);
      xsteps--;
    }
    if (ysteps != 0) {
      if (ysign) A_Down(ypulse);
      else       A_Up(ypulse);
      ysteps--;
    }
  }

  applyAutofirePulse_A(autofireEnabled);
#else
  if (data[joyPos[0]] == joyVal[0]) A_Up(PULSE_LENGTH);
  if (data[joyPos[1]] == joyVal[1]) A_Down(PULSE_LENGTH);
  if (data[joyPos[2]] == joyVal[2]) A_Left(PULSE_LENGTH);
  if (data[joyPos[3]] == joyVal[3]) A_Right(PULSE_LENGTH);

  if ((data[joyPos[4]] == joyVal[4]) | (data[joyPos[5]] == joyVal[5]) | (data[joyPos[6]] == joyVal[6])) {
    pinMode(A_FIRE, OUTPUT);
    digitalWrite(A_FIRE, LOW);
  } else {
    digitalWrite(A_FIRE, LOW);
    pinMode(A_FIRE, INPUT);
  }
#endif
}

// -------------------- Amiga/Atari mouse (mouse mode) --------------------
void a_mouse_m(hid_mouse_input_report_boot_t *mouse_report) {
  int xsteps = abs(mouse_report->x_displacement);
  int ysteps = abs(mouse_report->y_displacement);
  int xsign = (mouse_report->x_displacement > 0 ? 1 : 0);
  int ysign = (mouse_report->y_displacement > 0 ? 1 : 0);
  int xpulse = 0;
  int ypulse = 0;

  if (ISAMIGA == 1) {
    xpulse = PULSE_LENGTH;
    ypulse = PULSE_LENGTH;
  } else {
    if (xsteps > 15 & xsteps <= 100) xsteps = xsteps / 15;
    if (xsteps > 100) xsteps = xsteps / 30;

    if (ysteps > 15 & ysteps <= 100) ysteps = ysteps / 15;
    if (ysteps > 100) ysteps = ysteps / 30;

    xpulse = 18.6 * PULSE_LENGTH / xsteps;
    ypulse = 18.6 * PULSE_LENGTH / ysteps;
  }

  if (mouse_report->buttons.button1) { pinMode(A_FIRE, OUTPUT); digitalWrite(A_FIRE, LOW); }
  else { digitalWrite(A_FIRE, LOW); pinMode(A_FIRE, INPUT); }

  if (mouse_report->buttons.button2) { pinMode(A_BUTTON2, OUTPUT); digitalWrite(A_BUTTON2, LOW); }
  else { digitalWrite(A_BUTTON2, LOW); pinMode(A_BUTTON2, INPUT); }

  if (mouse_report->buttons.button3) { pinMode(A_BUTTON3, OUTPUT); digitalWrite(A_BUTTON3, LOW); }
  else { digitalWrite(A_BUTTON3, LOW); pinMode(A_BUTTON3, INPUT); }

  while ((xsteps | ysteps) != 0) {
    if (xsteps != 0) {
      if (xsign) A_Right(xpulse);
      else       A_Left(xpulse);
      xsteps--;
    }
    if (ysteps != 0) {
      if (ysign) A_Down(ypulse);
      else       A_Up(ypulse);
      ysteps--;
    }
  }
}

// -------------------- Amiga/Atari mouse (joystick mode) --------------------
void a_mouse_j(hid_mouse_input_report_boot_t *mouse_report) {
  if (mouse_report->buttons.button1) { pinMode(C64_FIRE, OUTPUT); digitalWrite(C64_FIRE, LOW); }
  else { digitalWrite(C64_FIRE, LOW); pinMode(C64_FIRE, INPUT); }

  if (mouse_report->x_displacement > 0) { pinMode(C64_RIGHT, OUTPUT); digitalWrite(C64_RIGHT, LOW); }
  else { pinMode(C64_LEFT, OUTPUT); digitalWrite(C64_LEFT, LOW); }

  if (mouse_report->y_displacement > 0) { pinMode(C64_DOWN, OUTPUT); digitalWrite(C64_DOWN, LOW); }
  else { pinMode(C64_UP, OUTPUT); digitalWrite(C64_UP, LOW); }

  timerWrite(timerOffX, 0);
  timerAlarm(timerOffX, abs(mouse_report->x_displacement) * M2JCalib, false, 0);
  timerWrite(timerOffY, 0);
  timerAlarm(timerOffY, abs(mouse_report->y_displacement) * M2JCalib, false, 0);
}

// -------------------- Joystick learning support (original) --------------------
void c64_joystick_config(const uint8_t *const data, const int length){
  for (int i = 0; i < length; i++) {
    defaultConfigData[i] = data[i];
    defaultConfigLength = length;
  }
}

// This interrupt is called when the switch mouse/joystick is activated.
void IRAM_ATTR switchMJHandler() {
  esp_restart();
}

void configurator() {
  uint8_t tmpData[100];

  for (int i = 0; i < 10; i++) {
    ws2812b.clear();
    ws2812b.show();
    delay(50);
    ws2812b.setPixelColor(0, LED_RED());
    ws2812b.show();
    delay(50);
  }

  delay(100);
  ws2812b.setPixelColor(0, LED_GREEN());
  ws2812b.show();

  for (int i = 0; i < 100; i++){
    defaultConfigData[i] = 0;
    tmpData[i] = 0;
  }

  configMode = true;
  while (defaultConfigLength == 0) {
    delay(100);
  }

  while (memcmp(tmpData, defaultConfigData, sizeof(uint8_t) * defaultConfigLength) != 0) {
    for (int i = 0; i < defaultConfigLength; i++) {
      tmpData[i] = defaultConfigData[i];
    }
  }

  delay(300);
  ws2812b.clear();
  ws2812b.show();

  for (int joy = 0; joy < JOYBUTTONS; joy++) {
    while (digitalRead(CONFIG) == HIGH) {
      delay(100);
    }

    ws2812b.setPixelColor(0, LED_BLUE());
    ws2812b.show();

    for (int i = 0; i < defaultConfigLength; i++) {
      tmpData[i] = defaultConfigData[i];
    }

    while (memcmp(tmpData, defaultConfigData, sizeof(uint8_t) * defaultConfigLength) == 0) {
      delay(100);
    }

    for (int i = 0; i < defaultConfigLength; i++) {
      if (tmpData[i] != defaultConfigData[i]) {
        joyVal[joy] = defaultConfigData[i];
        joyPos[joy] = i;
        break;
      }
    }

    ws2812b.clear();
    ws2812b.show();
  }

  for (int i = 0; i < 10; i++) {
    ws2812b.clear();
    ws2812b.show();
    delay(50);
    ws2812b.setPixelColor(0, LED_BLUE());
    ws2812b.show();
    delay(50);
  }

  for (int i = 0; i < JOYBUTTONS; i++) {
    EEPROM.write(2 * i, joyPos[i]);
    EEPROM.write(2 * i + 1, joyVal[i]);
  }
  EEPROM.commit();

  ws2812b.clear();
  ws2812b.show();
  esp_restart();
}

// -------------------- Setup --------------------
void setup() {
  Serial.begin(115200);
  ws2812b.begin();
  ws2812b.clear();
  ws2812b.setPixelColor(0, LED_RED());
  ws2812b.show();

  delay(500);

  ws2812b.clear();
  ws2812b.setPixelColor(0, LED_RED());
  ws2812b.show();

  // Start EEPROM
  EEPROM.begin(EEPROM_SIZE);

  // Read learned joystick mapping (used only in LEARN mode)
  for (int i = 0; i < JOYBUTTONS; i++) {
    joyPos[i] = EEPROM.read(2 * i);
    joyVal[i] = EEPROM.read(2 * i + 1);
  }

  // Load target machine mode (default C64 on first boot)
  gMode = loadModeFromEEPROM();
  applyModeToOriginalFlags();

  // NEW: start the 30s mode-switch window from now
  gModeSwitchDeadlineUs = esp_timer_get_time() + MODE_SWITCH_WINDOW_US;
  gModeSwitchEnabled = true;

  // Create the 5 seconds hold timer (one-shot)
  esp_timer_create_args_t targs = {
    .callback = &modeHoldTimerCb,
    .arg = NULL,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "mode_hold"
  };
  ESP_ERROR_CHECK(esp_timer_create(&targs, &modeHoldTimer));

  // Start USB host
  app_main();

  // Configure BOOT button
  pinMode(CONFIG, INPUT_PULLUP);
  delay(1000);

#if (JOY_MAPPING_MODE == JOY_MAP_LEARN)
  if (digitalRead(CONFIG) == LOW) {
    configurator();
  }
#endif

  // Define the GPIO and interrupt for the mouse/joystick switch
  pinMode(SWITCH_MJ, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(SWITCH_MJ), switchMJHandler, CHANGE);

  // -------------------- Per-target initialization (forced by gMode) --------------------
  if (ISC64 >= 5 && ISC64 <= 15) {
    // C64 path
    pinMode(C64_UP, OUTPUT);    digitalWrite(C64_UP, LOW);    pinMode(C64_UP, INPUT);
    pinMode(C64_DOWN, OUTPUT);  digitalWrite(C64_DOWN, LOW);  pinMode(C64_DOWN, INPUT);
    pinMode(C64_LEFT, OUTPUT);  digitalWrite(C64_LEFT, LOW);  pinMode(C64_LEFT, INPUT);
    pinMode(C64_RIGHT, OUTPUT); digitalWrite(C64_RIGHT, LOW); pinMode(C64_RIGHT, INPUT);
    pinMode(C64_FIRE, OUTPUT);  digitalWrite(C64_FIRE, LOW);  pinMode(C64_FIRE, INPUT);

    pinMode(A_BUTTON2, INPUT_PULLDOWN);
    pinMode(A_BUTTON3, INPUT_PULLDOWN);

    pinMode(C64_POTX, OUTPUT);
    pinMode(C64_POTY, OUTPUT);

    // Define the hardware timer frequencies, and turn on the timers
    timerOnX = timerBegin(10000000);
    timerAlarm(timerOnX, delayOnX, false, 0);
    timerOffX = timerBegin(10000000);
    timerAlarm(timerOffX, delayOffX, false, 0);
    timerOnY = timerBegin(10000000);
    timerAlarm(timerOnY, delayOnY, false, 0);
    timerOffY = timerBegin(10000000);
    timerAlarm(timerOffY, delayOffY, false, 0);

    if (digitalRead(SWITCH_MJ)) {
      // Mouse mode
      timerAttachInterrupt(timerOnX, &turnOnPotX);
      timerAttachInterrupt(timerOffX, &turnOffPotX);
      timerAttachInterrupt(timerOnY, &turnOnPotY);
      timerAttachInterrupt(timerOffY, &turnOffPotY);

      pinMode(C64_INT, INPUT_PULLUP);
      attachInterrupt(digitalPinToInterrupt(C64_INT), handleInterrupt, RISING);

      // Start Micromys wheel pulser task (only used in C64 mouse mode)
      xTaskCreatePinnedToCore(micromysWheelTask, "mm_wheel", 2048, NULL, 2, &micromysWheelTaskH, 1);

      ws2812b.setPixelColor(0, LED_BLUE());
      ws2812b.show();
    } else {
      // Joystick mode
      ws2812b.setPixelColor(0, LED_GREEN());
      ws2812b.show();

      setCpuFrequencyMhz(10);
      timerAttachInterrupt(timerOffX, &turnOffJoyX);
      timerAttachInterrupt(timerOffY, &turnOffJoyY);
    }
  } else {
    // Amiga/Atari path
    if (digitalRead(SWITCH_MJ)) {
      // Mouse mode
      pinMode(A_UP, OUTPUT);
      pinMode(A_DOWN, OUTPUT);
      pinMode(A_LEFT, OUTPUT);
      pinMode(A_RIGHT, OUTPUT);

      pinMode(A_FIRE, OUTPUT);    digitalWrite(A_FIRE, LOW);    pinMode(A_FIRE, INPUT);
      pinMode(A_BUTTON2, OUTPUT); digitalWrite(A_BUTTON2, LOW); pinMode(A_BUTTON2, INPUT);
      pinMode(A_BUTTON3, OUTPUT); digitalWrite(A_BUTTON3, LOW); pinMode(A_BUTTON3, INPUT);

      ws2812b.setPixelColor(0, LED_BLUE());
      ws2812b.show();
    } else {
      // Joystick mode
      pinMode(A_UP, OUTPUT);    digitalWrite(A_UP, LOW);    pinMode(A_UP, INPUT);
      pinMode(A_DOWN, OUTPUT);  digitalWrite(A_DOWN, LOW);  pinMode(A_DOWN, INPUT);
      pinMode(A_LEFT, OUTPUT);  digitalWrite(A_LEFT, LOW);  pinMode(A_LEFT, INPUT);
      pinMode(A_RIGHT, OUTPUT); digitalWrite(A_RIGHT, LOW); pinMode(A_RIGHT, INPUT);

      pinMode(A_FIRE, OUTPUT);  digitalWrite(A_FIRE, LOW);  pinMode(A_FIRE, INPUT);

      pinMode(A_BUTTON2, OUTPUT); digitalWrite(A_BUTTON2, HIGH); pinMode(A_BUTTON2, INPUT);
      pinMode(A_BUTTON3, OUTPUT); digitalWrite(A_BUTTON3, HIGH); pinMode(A_BUTTON3, INPUT);

      ws2812b.setPixelColor(0, LED_GREEN());
      ws2812b.show();

      setCpuFrequencyMhz(10);
    }
  }
}

void loop() {}

