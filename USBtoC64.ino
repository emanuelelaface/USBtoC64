/*
USB to Commodore 64 adaptor V 1.0 by Emanuele Laface

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
#include "hid_usage_keyboard.h"
#include "hid_usage_mouse.h"
#include "esp_timer.h"
#include "Adafruit_NeoPixel.h"

#define PIN_WS2812B 21 // Pin for RGB LED
#define NUM_PIXELS 1 // 1 LED
Adafruit_NeoPixel ws2812b(NUM_PIXELS, PIN_WS2812B, NEO_GRB + NEO_KHZ800); // Initialize LED

// Define GPIOs for Joystick Switches
#define C64_FIRE          7
#define C64_UP            8
#define C64_DOWN          9 
#define C64_LEFT         10
#define C64_RIGHT        11
// Define GPIOs for analog mouse ports
#define C64_POTX          4
#define C64_POTY          6
// Define GPIO for interrupt from C64
#define C64_INT           1
// Define GPIO for switch Mouse - Joystick
#define SWITCH_MJ        13 // 2 // HIGH = mouse, LOW = Joystick
// Define the default timers for the mouse delay, all empirical for PAL version
#define MINdelayOnX    2450
#define MAXdelayOnX    5040
#define MINdelayOnY    2440
#define MAXdelayOnY    5100
#define STEPdelayOnX   10.16689245
#define STEPdelayOnY   10.14384171
// Define the timing for mouse used as Joystick
#define M2JCalib      833 // 20000 at 240 MHz

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

// From now till further comment is all the Bluetooth stuff that is copyed from the example fo the module
static const char *TAG = "USB MESSAGE";
QueueHandle_t hid_host_event_queue;
bool user_shutdown = false;

typedef struct {
  hid_host_device_handle_t hid_device_handle;
  hid_host_driver_event_t event;
  void *arg;
} hid_host_event_queue_t;

static const char *hid_proto_name_str[] = {"NONE", "KEYBOARD", "MOUSE"};

static void hid_host_mouse_report_callback(const uint8_t *const data, const int length) {
  hid_mouse_input_report_boot_t *mouse_report = (hid_mouse_input_report_boot_t *)data;
  if (length < sizeof(hid_mouse_input_report_boot_t)) {
    return;
  }
  // Here is where the USB mouse receive the report from USB.
  if (digitalRead(SWITCH_MJ)) {       // If we are in Mouse mode
    c64_mouse_m(mouse_report);        // The mouse function in mouse mode is called
  }
  else {                              // If we are in joystick mode
    c64_mouse_j(mouse_report);        // The mouse function in joystick mode is called
  }
}

static void hid_host_generic_report_callback(const uint8_t *const data, const int length) {
  // Here is where the USB joystick receive the report from USB.
  if (digitalRead(SWITCH_MJ)) {     // If we are in Mouse mode
    c64_joystick_m(data, length);   // The joystick function in mouse mode is called
  }
  else {                            // If we are in joystick mode
    c64_joystick_j(data, length);   // The joystick function in joystick mode is called
  }
}
// More USB stuff
void hid_host_interface_callback(hid_host_device_handle_t hid_device_handle, const hid_host_interface_event_t event, void *arg) {
  uint8_t data[64] = {0};
  size_t data_length = 0;
  hid_host_dev_params_t dev_params;
  ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));

  switch (event) {
  case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
    ESP_ERROR_CHECK(hid_host_device_get_raw_input_report_data(hid_device_handle, data, 64, &data_length));
    if (HID_SUBCLASS_BOOT_INTERFACE == dev_params.sub_class) {
      if (HID_PROTOCOL_KEYBOARD == dev_params.proto) {
        ;
      } else if (HID_PROTOCOL_MOUSE == dev_params.proto) {
        hid_host_mouse_report_callback(data, data_length);
      }
    } else {
      hid_host_generic_report_callback(data, data_length);
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
      ESP_ERROR_CHECK(hid_class_request_set_protocol(hid_device_handle, HID_REPORT_PROTOCOL_BOOT));
      if (HID_PROTOCOL_KEYBOARD == dev_params.proto) {
        ESP_ERROR_CHECK(hid_class_request_set_idle(hid_device_handle, 0, 0));
      }
    }
    ESP_ERROR_CHECK(hid_host_device_start(hid_device_handle));
    break;
  default:
    break;
  }
}

static void usb_lib_task(void *arg) {
  const usb_host_config_t host_config = {.skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1,};

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

  while (!user_shutdown) {
    if (xQueueReceive(hid_host_event_queue, &evt_queue, pdMS_TO_TICKS(50))) {
      hid_host_device_event(evt_queue.hid_device_handle, evt_queue.event, evt_queue.arg);
    }
  }
  xQueueReset(hid_host_event_queue);
  vQueueDelete(hid_host_event_queue);
  vTaskDelete(NULL);
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
  const hid_host_driver_config_t hid_host_driver_config = { .create_background_task = true, .task_priority = 5, .stack_size = 4096, .core_id = 0, .callback = hid_host_device_callback, .callback_arg = NULL};
  ESP_ERROR_CHECK(hid_host_install(&hid_host_driver_config));
  user_shutdown = false;
  task_created = xTaskCreate(&hid_host_task, "hid_task", 4 * 1024, NULL, 2, NULL);
  assert(task_created == pdTRUE);
}
// This interrupt is called when the C64 set to zero the voltage on the POTX
// The BJT transforms the signal in HIGH and the Interrupt is invoked
void IRAM_ATTR handleInterrupt() {
  // Workaround for a BUG! Sometime the interrupt is triggered twice but only once when
  // it is HIGH, so I check the level and if it is not HIGH I do not start the timers
  // This is a bug of the Arduino API, the ESP32 IDF should work fine
  if (!((GPIO.in >> C64_INT) & 1)) return;
  timerWrite(timerOnX, 0);                      // Reset the timer for POTX
  timerAlarm(timerOnX, delayOnX, false, 0);     // Set the delay when the POTX timer interrupt will trigger the start
  timerWrite(timerOnY, 0);                      // Reset the ON timer for POTY
  timerAlarm(timerOnY, delayOnY, false, 0);     // Set the delay when the POTY timer interrupt will trigger start
}

void IRAM_ATTR turnOnPotX() {
  GPIO.out_w1ts = (1 << C64_POTX);              // Turn on POTX after delayOnX
  timerWrite(timerOffX, 0);                     // Reset the OFF timer for POTX
  timerAlarm(timerOffX, delayOffX, false, 0);   // Set the delay when the POTX timer interrupt will trigger the end
}

void IRAM_ATTR turnOffPotX() {
  GPIO.out_w1tc = (1 << C64_POTX);              // After delayOffX this interrupt is called and the GPIO of POTX is turned OFF
}

void IRAM_ATTR turnOnPotY() {
  GPIO.out_w1ts = (1 << C64_POTY);              // Turn on POTY after delayOnY
  timerWrite(timerOffY, 0);                     // Reset the OFF timer for POTY
  timerAlarm(timerOffY, delayOffY, false, 0);   // Set the delay when the POTY timer interrupt will trigger the end
}

void IRAM_ATTR turnOffPotY() {
  GPIO.out_w1tc = (1 << C64_POTY);              // After delayOffY this interrupt is called and the GPIO of POTY is turned OFF
}
// This timer is used to turn off the joystick pins when the mouse is in joystick mode, for the horizontal direction
void IRAM_ATTR turnOffJoyX() {
  digitalWrite(C64_LEFT, LOW);
  digitalWrite(C64_RIGHT, LOW);
  pinMode(C64_LEFT, INPUT);
  pinMode(C64_RIGHT, INPUT);
}
// This timer is used to turn off the joystick pins when the mouse is in joystick mode, for the vertical direction
void IRAM_ATTR turnOffJoyY() {
  digitalWrite(C64_UP, LOW);
  digitalWrite(C64_DOWN, LOW);
  pinMode(C64_UP, INPUT);
  pinMode(C64_DOWN, INPUT);
}
// Function of mouse in mouse mode
void c64_mouse_m(hid_mouse_input_report_boot_t *mouse_report) {
  if (mouse_report->buttons.button1) {  // Left button is wired to C64 FIRE
    pinMode(C64_FIRE, OUTPUT);
    digitalWrite(C64_FIRE, LOW);
  }
  else {
    digitalWrite(C64_FIRE, LOW);
    pinMode(C64_FIRE, INPUT);
  }
  if (mouse_report->buttons.button2) {  // Right button is wired to C64 UP
    pinMode(C64_UP, OUTPUT);
    digitalWrite(C64_UP, LOW);
  }
  else {
    digitalWrite(C64_UP, LOW);
    pinMode(C64_UP, INPUT);
  }
  delayOnX += STEPdelayOnX*mouse_report->x_displacement;   // Define the moment when the POTX has to be turned on after the interrupt
  if (delayOnX > MAXdelayOnX) {                            // If the value is over the limit, it returns to the zero
    delayOnX = MINdelayOnX;
  }
  if (delayOnX < MINdelayOnX) {                            // If the value is below the zero limit, it returns to the maximum
    delayOnX = MAXdelayOnX;
  }

  delayOnY -= STEPdelayOnX*mouse_report->y_displacement;   // Define the moment when the POTY has to be turned on after the interrupt
  if (delayOnY > MAXdelayOnY) {                            // If the value is over the limit, it returns to the zero
    delayOnY = MINdelayOnY;
  }
  if (delayOnY < MINdelayOnY) {                            // If the value is below the zero limit, it returns to the maximum
    delayOnY = MAXdelayOnY;
  }
}

// Function of mouse in joystick mode
void c64_mouse_j(hid_mouse_input_report_boot_t *mouse_report) {
  if (mouse_report->buttons.button1) {        // Map left button to C64 FIRE
    pinMode(C64_FIRE, OUTPUT);
    digitalWrite(C64_FIRE, LOW);
  }
  else {
    digitalWrite(C64_FIRE, LOW);
    pinMode(C64_FIRE, INPUT);
  }
  if (mouse_report->x_displacement>0) {       // If the motion is in the X direction move the joystick right or left
    pinMode(C64_RIGHT, OUTPUT);
    digitalWrite(C64_RIGHT, LOW);
  }
  else {
    pinMode(C64_LEFT, OUTPUT);
    digitalWrite(C64_LEFT, LOW);
  }
  if (mouse_report->y_displacement>0) {       // If the motion is in the Y direction move the joystick up or down
    pinMode(C64_DOWN, OUTPUT);
    digitalWrite(C64_DOWN, LOW);
  }
  else {
    pinMode(C64_UP, OUTPUT);
    digitalWrite(C64_UP, LOW);
  }
  timerWrite(timerOffX, 0);   // Reset the timer for the X direction
  timerAlarm(timerOffX, abs(mouse_report->x_displacement)*M2JCalib, false, 0);  // Define the interrupt that will turn off the X direction after a delay proportional to the motion
  timerWrite(timerOffY, 0);   // Reset the timer for the Y direction
  timerAlarm(timerOffY, abs(mouse_report->y_displacement)*M2JCalib, false, 0);  // Define the interrupt that will turn off the Y direction after a delay proportional to the motion
}

// Function of joystick in joystick mode
// It is almost self explanatory, the function receive an array of bytes and depending on the byte
// A C64 GPIO is called. This function was tested with Joypad for SNES, it is very possible that another model of
// joypad or joystick needs to change the bytes called here.
void c64_joystick_j(const uint8_t *const data, const int length) {
  switch (data[1]) {
    case 0:
      pinMode(C64_UP, OUTPUT);
      digitalWrite(C64_UP, LOW);
      break;
    case 255:
      pinMode(C64_DOWN, OUTPUT);
      digitalWrite(C64_DOWN, LOW);
      break;
    default:
      digitalWrite(C64_UP, LOW);
      digitalWrite(C64_DOWN, LOW);
      pinMode(C64_UP, INPUT);
      pinMode(C64_DOWN, INPUT);
      break;
  }
  switch (data[0]) {
    case 0:
      pinMode(C64_LEFT, OUTPUT);
      digitalWrite(C64_LEFT, LOW);
      break;
    case 255:
      pinMode(C64_RIGHT, OUTPUT);
      digitalWrite(C64_RIGHT, LOW);
      break;
    default:
      digitalWrite(C64_LEFT, LOW);
      digitalWrite(C64_RIGHT, LOW);
      pinMode(C64_LEFT, INPUT);
      pinMode(C64_RIGHT, INPUT);
      break;
  }
  switch (data[5]) {
    case 47:
      pinMode(C64_FIRE, OUTPUT);
      digitalWrite(C64_FIRE, LOW);
      break;
    case 79:
      //printf("B\n");
      break;
    case 31:
      //printf("X\n");
      break;
    case 143:
      //printf("Y\n");
      break;
    default:
      digitalWrite(C64_FIRE, LOW);
      pinMode(C64_FIRE, INPUT);
      break;
  }
  switch (data[6]) {
    case 1:
      pinMode(C64_FIRE, OUTPUT);
      digitalWrite(C64_FIRE, LOW);
    break;
    case 2:
      pinMode(C64_FIRE, OUTPUT);
      digitalWrite(C64_FIRE, LOW);
      break;
    case 16:
      //printf("SELECT\n");
      break;
    case 32:
      //printf("START\n");
      break;
    default:
      if (data[5] == 15) {
        digitalWrite(C64_FIRE, LOW);
        pinMode(C64_FIRE, INPUT);
      }
      break;
  }
}
// Function of joystick in mouse mode
void c64_joystick_m(const uint8_t *const data, const int length) {
  float x = 0;
  float y = 0;
  switch (data[1]) {            // If the motion is in vertical
    case 0:
      y = 3*STEPdelayOnY;       // set the y motion as 3 steps of mouse in the positive or negative direction
      break;
    case 255:
      y = -3*STEPdelayOnY;
      break;
    default:
      break;
  }
  switch (data[0]) {            // If the motion is in horizontal
    case 0:
      x = -3*STEPdelayOnX;      // set the x motion as 3 steps of mouse in the negative or positive direction
      break;
    case 255:
      x = 3*STEPdelayOnX;
      break;
    default:
      break;
  }
  switch (data[5]) {
    case 47:
      pinMode(C64_FIRE, OUTPUT);
      digitalWrite(C64_FIRE, LOW);
      break;
    case 79:
      //printf("B\n");
      break;
    case 31:
      //printf("X\n");
      break;
    case 143:
      //printf("Y\n");
      break;
    default:
      digitalWrite(C64_FIRE, LOW);
      pinMode(C64_FIRE, INPUT);
      break;
  }
  switch (data[6]) {
    case 1:
      pinMode(C64_FIRE, OUTPUT);
      digitalWrite(C64_FIRE, LOW);
    break;
    case 2:
      pinMode(C64_UP, OUTPUT);
      digitalWrite(C64_UP, LOW);
      break;
    case 16:
      //printf("SELECT\n");
      break;
    case 32:
      //printf("START\n");
      break;
    default:
      if (data[5] == 15) {
        digitalWrite(C64_FIRE, LOW);
        pinMode(C64_FIRE, INPUT);
        digitalWrite(C64_UP, LOW);
        pinMode(C64_UP, INPUT);
      }
      break;
  }
  // Setup the delays of the mouse POTX and Y according to the displacement of the joystick
  delayOnX += x;
  if (delayOnX > MAXdelayOnX) {
    delayOnX = MINdelayOnX;
  }
  if (delayOnX < MINdelayOnX) {
    delayOnX = MAXdelayOnX;
  }

  delayOnY += y;
  if (delayOnY > MAXdelayOnY) {
    delayOnY = MINdelayOnY;
  }
  if (delayOnY < MINdelayOnY) {
    delayOnY = MAXdelayOnY;
  }
}
// This interrupt is called when the switch mouse/joystick is activated.
// The reason to reset the board is that too many things has to change, in particular the
// Clock frequency and the USB (and timers) would be in an unpredictable state, so it is
// safer simply to reboot the board
void IRAM_ATTR switchMJHandler() {
  esp_restart();
}

void setup() {  
  // Turn on the LED in RED color
  ws2812b.begin();
  ws2812b.clear();
  ws2812b.setPixelColor(0, ws2812b.Color(0, 255, 0));
  ws2812b.show();
  // Start the USB
  app_main();
  // Configure the GPIOs for the Joystick
  pinMode(C64_UP, OUTPUT);
  digitalWrite(C64_UP, LOW);
  pinMode(C64_UP, INPUT);
  pinMode(C64_DOWN, OUTPUT);
  digitalWrite(C64_DOWN, LOW);
  pinMode(C64_DOWN, INPUT);
  pinMode(C64_LEFT, OUTPUT);
  digitalWrite(C64_LEFT, LOW);
  pinMode(C64_LEFT, INPUT);
  pinMode(C64_RIGHT, OUTPUT);
  digitalWrite(C64_RIGHT, LOW);
  pinMode(C64_RIGHT, INPUT);
  pinMode(C64_FIRE, OUTPUT);
  digitalWrite(C64_FIRE, LOW);
  pinMode(C64_FIRE, INPUT);
  // Define the GPIO and Interrupt for the mouse/joystick switch
  pinMode(SWITCH_MJ, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(SWITCH_MJ), switchMJHandler, CHANGE);
  // Define the GPIOs for the mouse interrupt and the POTs
  pinMode(C64_INT, INPUT_PULLUP);
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

  if (digitalRead(SWITCH_MJ)) {                                               // If we are in mouse mode
    timerAttachInterrupt(timerOnX, &turnOnPotX);                              // Attach the timers to the POT interrupts
    timerAttachInterrupt(timerOffX, &turnOffPotX);
    timerAttachInterrupt(timerOnY, &turnOnPotY);
    timerAttachInterrupt(timerOffY, &turnOffPotY);
    attachInterrupt(digitalPinToInterrupt(C64_INT), handleInterrupt, RISING); // Attach the interrupt PIN to the handler
    ws2812b.setPixelColor(0, ws2812b.Color(0, 0, 25));                        // Set the LED BLU
    ws2812b.show();
  }
  else {                                                                      // If we are in joystick mode
    ws2812b.setPixelColor(0, ws2812b.Color(25, 0, 0));                        // Turn the LED green
    ws2812b.show();
    // Decrease the frequency of the CPU to 10 MHz to drop the current usage of the board from 90 to 20 mA, in this way two
    // Joystick can be safely used on the C64 with the 100 mA current supply of the control ports.
    // We cannot use this frequency in mouse mode because the hardware timers must be fast enough to trigger the interrupt
    // at the right moment. Also it is very uncommon that we will need 2 mouse at he same time on the C64.
    setCpuFrequencyMhz(10);
    timerAttachInterrupt(timerOffX, &turnOffJoyX);                            // Attach the timers for the interrupts of the mouse in joystick mode
    timerAttachInterrupt(timerOffY, &turnOffJoyY);
  }
}

void loop() {}