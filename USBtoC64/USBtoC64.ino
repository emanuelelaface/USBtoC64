/*
USB to Commodore 64 and AMIGA adaptor V 4.1 by Emanuele Laface

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
#include "esp_timer.h"
#include "Adafruit_NeoPixel.h"
#include "EEPROM.h"

#define PIN_WS2812B      21 // Pin for RGB LED
#define NUM_PIXELS        1 // 1 LED
Adafruit_NeoPixel ws2812b(NUM_PIXELS, PIN_WS2812B, NEO_RGB + NEO_KHZ800); // Initialize LED Some board is NEO_RGB

// Define GPIOs for C64
#define C64_FIRE          7
#define C64_UP            8
#define C64_DOWN          9 
#define C64_LEFT         10
#define C64_RIGHT        11
#define C64_POTX          4
#define C64_POTY          6
// Define GPIO for interrupt from C64
#define C64_INT           1
// Define GPIO for switch Mouse - Joystick
#define SWITCH_MJ        13 // HIGH = mouse, LOW = Joystick

// Define the default timers for the mouse delay, all empirical for PAL version
#define PAL               1 // select if it is PAL or NTSC and adjust the timings

#if PAL
  #define MINdelayOnX    2450
  #define MAXdelayOnX    5040
  #define MINdelayOnY    2440
  #define MAXdelayOnY    5100
  #define STEPdelayOnX   10.16689245
  #define STEPdelayOnY   10.14384171
  // Define the timing for mouse used as Joystick
  #define M2JCalib      833 // 20000 at 240 MHz
#else
  #define MINdelayOnX    2360
  #define MAXdelayOnX    4855
  #define MINdelayOnY    2351
  #define MAXdelayOnY    4913
  #define STEPdelayOnX   9.794315054
  #define STEPdelayOnY   9.772109035
  // Define the timing for mouse used as Joystick
  #define M2JCalib      802 // 20000 at 240 MHz
#endif

// Amiga SETTINGS
#define AMIGA_FIRE        7
#define AMIGA_UP          8
#define AMIGA_DOWN        9 
#define AMIGA_LEFT       10
#define AMIGA_RIGHT      11
#define AMIGA_BUTTON2     5
#define AMIGA_BUTTON3     3
#define PULSE_LENGTH    150 // lenght of pusle for Amiga mouse

#define CONFIG            0 // set the configuration switch to the "Boot" button
#define JOYBUTTONS        7 // 4 directions and 3 fire
#define EEPROM_SIZE JOYBUTTONS*2 // define the size of the EEPROM we will need to save joystick data

int ISC64 = 0;  // Commodore 64 / Amiga detection flag

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

// From now till further comment is all the USB stuff that is copyed from the example fo the module
static const char *TAG = "USB MESSAGE";
QueueHandle_t hid_host_event_queue;

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
    if (ISC64 > 0) { // if it is a Commodore 64
      c64_mouse_m(mouse_report);        // The mouse function in mouse mode is called
    }
    else {
      amiga_mouse_m(mouse_report);
    }
  }
  else {                        // If we are in joystick mode
    if (ISC64 > 0) {
      c64_mouse_j(mouse_report);        // The mouse function in joystick mode is called
    }
    else {
      amiga_mouse_j(mouse_report);
    }
  }
}

static void hid_host_generic_report_callback(const uint8_t *const data, const int length) {
  // Here is where the USB joystick receive the report from USB.
  if (configMode) { // if the controller is in configuration mode, it calls a function that simply reads the joystick values
    c64_joystick_config(data, length);
  }
  else {
    if (digitalRead(SWITCH_MJ)) {     // If we are in Mouse mode
      if (ISC64>0) {  
        c64_joystick_m(data, length);   // The joystick function in mouse mode is called
      }
      else {
        amiga_joystick_m(data, length);
      }
    }
    else {   // If we are in joystick mode
      if (ISC64 > 0) {                   // If it is a Commodore 64       
        c64_joystick_j(data, length);   // The joystick function in joystick mode is called
      }
      else {
        amiga_joystick_j(data, length);
      }
    }
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

  while (true) {
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
// A C64 GPIO is called.
// The Joystick positions are recoded as UP, DOWN, LEFT, RIGHT, FIRE and compared here to see wich GPIO enable.
void c64_joystick_j(const uint8_t *const data, const int length) {
  if (data[joyPos[0]] == joyVal[0]) {
      pinMode(C64_UP, OUTPUT);
      digitalWrite(C64_UP, LOW);
  }
  else {
    digitalWrite(C64_UP, LOW);
    pinMode(C64_UP, INPUT);
  }
  if (data[joyPos[1]] == joyVal[1]) {
      pinMode(C64_DOWN, OUTPUT);
      digitalWrite(C64_DOWN, LOW);
  }
  else {
    digitalWrite(C64_DOWN, LOW);
    pinMode(C64_DOWN, INPUT);    
  }
  if (data[joyPos[2]] == joyVal[2]) {
      pinMode(C64_LEFT, OUTPUT);
      digitalWrite(C64_LEFT, LOW);
  }
  else {
    digitalWrite(C64_LEFT, LOW);
    pinMode(C64_LEFT, INPUT);
  }
  if (data[joyPos[3]] == joyVal[3]) {
      pinMode(C64_RIGHT, OUTPUT);
      digitalWrite(C64_RIGHT, LOW);
  }
  else {
    digitalWrite(C64_RIGHT, LOW);
    pinMode(C64_RIGHT, INPUT);
  }
  if ((data[joyPos[4]] == joyVal[4]) | (data[joyPos[5]] == joyVal[5]) | (data[joyPos[6]] == joyVal[6])) {
      pinMode(C64_FIRE, OUTPUT);
      digitalWrite(C64_FIRE, LOW);
  }
  else {
    digitalWrite(C64_FIRE, LOW);
    pinMode(C64_FIRE, INPUT);
  }
}
// Function of joystick in mouse mode
void c64_joystick_m(const uint8_t *const data, const int length) {
  float x = 0;
  float y = 0;
  if (data[joyPos[0]] == joyVal[0]) {            // If the motion is in vertical
      y = 3*STEPdelayOnY;       // set the y motion as 3 steps of mouse in the positive or negative direction
  }
  if (data[joyPos[1]] == joyVal[1]) {
      y = -3*STEPdelayOnY;
  }
  if (data[joyPos[2]] == joyVal[2]) {            // If the motion is in horizontal
      x = -3*STEPdelayOnX;      // set the x motion as 3 steps of mouse in the negative or positive direction
  }
  if (data[joyPos[3]] == joyVal[3]) {
      x = 3*STEPdelayOnX;
  }
  if (data[joyPos[4]] == joyVal[4]) {
      pinMode(C64_FIRE, OUTPUT);
      digitalWrite(C64_FIRE, LOW);
  }
  else {
    digitalWrite(C64_FIRE, LOW);
    pinMode(C64_FIRE, INPUT);
  }
  if (data[joyPos[5]] == joyVal[5]) {
      pinMode(C64_UP, OUTPUT);
      digitalWrite(C64_UP, LOW);
  }
  else {
    digitalWrite(C64_UP, LOW);
    pinMode(C64_UP, INPUT);
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

void AMIGAHorizontalMove() {
    digitalWrite(AMIGA_DOWN, H[QX]);
    digitalWrite(AMIGA_RIGHT, HQ[QX]);
    delayMicroseconds(PULSE_LENGTH);
}

void AMIGAVerticalMove() {
    digitalWrite(AMIGA_UP, H[QY]);
    digitalWrite(AMIGA_LEFT, HQ[QY]);
    delayMicroseconds(PULSE_LENGTH);
}

void AMIGA_Left() {
    AMIGAHorizontalMove();
    QX = (QX >= 3) ? 0 : ++QX;    
}

void AMIGA_Right() {
    AMIGAHorizontalMove();
    QX = (QX <= 0) ? 3 : --QX;
}

void AMIGA_Down() {
    AMIGAVerticalMove();
    QY = QY <= 0 ? 3 : --QY;
}

void AMIGA_Up() {
    AMIGAVerticalMove();
    QY = QY >= 3 ? 0 : ++QY;
}

void amiga_mouse_m(hid_mouse_input_report_boot_t *mouse_report) {
  int xsteps = abs(mouse_report->x_displacement);
  int ysteps = abs(mouse_report->y_displacement);
  int xsign = (mouse_report->x_displacement > 0 ? 1 : 0) ;
  int ysign = (mouse_report->y_displacement > 0 ? 1 : 0) ;

  if (mouse_report->buttons.button1) {  // Left button is wired to C64 FIRE
    pinMode(AMIGA_FIRE, OUTPUT);
    digitalWrite(AMIGA_FIRE, LOW);
  }
  else {
    digitalWrite(AMIGA_FIRE, LOW);
    pinMode(AMIGA_FIRE, INPUT);
  }
  if (mouse_report->buttons.button2) {  // Right button is wired to C64 UP
    pinMode(AMIGA_BUTTON2, OUTPUT);
    digitalWrite(AMIGA_BUTTON2, LOW);
  }
  else {
    digitalWrite(AMIGA_BUTTON2, LOW);
    pinMode(AMIGA_BUTTON2, INPUT);
  }
  if (mouse_report->buttons.button3) {  // Right button is wired to C64 UP
    pinMode(AMIGA_BUTTON3, OUTPUT);
    digitalWrite(AMIGA_BUTTON3, LOW);
  }
  else {
    digitalWrite(AMIGA_BUTTON3, LOW);
    pinMode(AMIGA_BUTTON3, INPUT);
  }
  while ((xsteps | ysteps) != 0) {
    if (xsteps != 0) {
        if (xsign)
            AMIGA_Right();
        else
            AMIGA_Left(); 
        xsteps--;
    }
    if (ysteps != 0) {
        if (ysign)
            AMIGA_Down();
        else
            AMIGA_Up(); 
        ysteps--;
    }
  }  
}

void amiga_mouse_j(hid_mouse_input_report_boot_t *mouse_report) {
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

void amiga_joystick_j(const uint8_t *const data, const int length) {
  if (data[joyPos[0]] == joyVal[0]) {
      pinMode(AMIGA_UP, OUTPUT);
      digitalWrite(AMIGA_UP, LOW);
  }
  else {
    digitalWrite(AMIGA_UP, LOW);
    pinMode(AMIGA_UP, INPUT);
  }
  if (data[joyPos[1]] == joyVal[1]) {
      pinMode(AMIGA_DOWN, OUTPUT);
      digitalWrite(AMIGA_DOWN, LOW);
  }
  else {
    digitalWrite(AMIGA_DOWN, LOW);
    pinMode(AMIGA_DOWN, INPUT);    
  }
  if (data[joyPos[2]] == joyVal[2]) {
      pinMode(AMIGA_LEFT, OUTPUT);
      digitalWrite(AMIGA_LEFT, LOW);
  }
  else {
    digitalWrite(AMIGA_LEFT, LOW);
    pinMode(AMIGA_LEFT, INPUT);
  }
  if (data[joyPos[3]] == joyVal[3]) {
      pinMode(AMIGA_RIGHT, OUTPUT);
      digitalWrite(AMIGA_RIGHT, LOW);
  }
  else {
    digitalWrite(AMIGA_RIGHT, LOW);
    pinMode(AMIGA_RIGHT, INPUT);
  }
  if ((data[joyPos[4]] == joyVal[4]) | (data[joyPos[5]] == joyVal[5]) | (data[joyPos[6]] == joyVal[6])) {
      pinMode(AMIGA_FIRE, OUTPUT);
      digitalWrite(AMIGA_FIRE, LOW);
  }
  else {
    digitalWrite(AMIGA_FIRE, LOW);
    pinMode(AMIGA_FIRE, INPUT);
  }
}

void amiga_joystick_m(const uint8_t *const data, const int length) {
  if (data[joyPos[0]] == joyVal[0]) {
    AMIGA_Up();
  }
  if (data[joyPos[1]] == joyVal[1]) {
    AMIGA_Down();
  }
  if (data[joyPos[2]] == joyVal[2]) {
    AMIGA_Left();
  }
  if (data[joyPos[3]] == joyVal[3]) {
    AMIGA_Right();
  }
  if ((data[joyPos[4]] == joyVal[4]) | (data[joyPos[5]] == joyVal[5]) | (data[joyPos[6]] == joyVal[6])) {
      pinMode(AMIGA_FIRE, OUTPUT);
      digitalWrite(AMIGA_FIRE, LOW);
  }
  else {
    digitalWrite(AMIGA_FIRE, LOW);
    pinMode(AMIGA_FIRE, INPUT);
  }
}


// Function to configure the Joystick
// This function is dummy, it simply records values for from the joystick for the configuration
void c64_joystick_config(const uint8_t *const data, const int length){
  for (int i=0; i<length; i++) {
    defaultConfigData[i] = data[i];
    defaultConfigLength = length;
  }
}
// This interrupt is called when the switch mouse/joystick is activated.
// The reason to reset the board is that too many things has to change, in particular the
// Clock frequency and the USB (and timers) would be in an unpredictable state, so it is
// safer simply to reboot the board
void IRAM_ATTR switchMJHandler() {
    esp_restart();
}

void configurator() {  // When the board is in configuration mode
  uint8_t tmpData[100];

  for (int i=0; i<10; i++) {  // blink red led fast 10 times
    ws2812b.clear();
    ws2812b.show();
    delay(50);
    ws2812b.setPixelColor(0, ws2812b.Color(0, 25, 0));
    ws2812b.show();
    delay(50);
  }
  delay(100);
  ws2812b.setPixelColor(0, ws2812b.Color(25, 0, 0)); // set green led
  ws2812b.show();
  for (int i=0; i<100; i++){
    defaultConfigData[i]=0;
    tmpData[i] = 0;
  }
  configMode = true;  // tells the USB that we are in configMode
  while(defaultConfigLength == 0){ // Wait for the USB controller connected that produces some data
    delay(100);
  }
  while(memcmp( tmpData, defaultConfigData, sizeof(uint8_t)*defaultConfigLength) != 0) { // if the data do not match the temporary data match them
    for (int i=0; i<defaultConfigLength; i++) {
      tmpData[i] = defaultConfigData[i];
    }
  }
  delay(300);
  ws2812b.clear();  // turn off the LED
  ws2812b.show();
  for (int joy = 0; joy<JOYBUTTONS; joy++){  // repeat for the 4 directions and the fire
    while(digitalRead(CONFIG)==HIGH) {  // Wait for Boot button pressed
      delay(100);
    }
    ws2812b.setPixelColor(0, ws2812b.Color(0, 0, 25)); // Set the blue led on
    ws2812b.show();
    for (int i=0; i<defaultConfigLength; i++) {  // sync the data with the temporary data
      tmpData[i] = defaultConfigData[i];
    }
    while(memcmp( tmpData, defaultConfigData, sizeof(uint8_t)*defaultConfigLength) == 0) { // wait for a difference between the temporary data and the joystick
      delay(100);
    }
    for (int i=0; i<defaultConfigLength; i++) {  // storey wich part of the array and wich value corresponds to the data
      if (tmpData[i] != defaultConfigData[i]) {
        joyVal[joy] = defaultConfigData[i];
        joyPos[joy] = i;
        break;
      }
    }
    ws2812b.clear();
    ws2812b.show();
  }
  for (int i=0; i<10; i++) {  // once finished, blink blue light fast 10 times
    ws2812b.clear();
    ws2812b.show();
    delay(50);
    ws2812b.setPixelColor(0, ws2812b.Color(25, 0, 0));
    ws2812b.show();
    delay(50);
  }
  for (int i=0; i<JOYBUTTONS; i++) { // Write the values in the EEPROM for the permanent storage
    EEPROM.write(2*i, joyPos[i]);
    EEPROM.write(2*i+1, joyVal[i]);
  }
  EEPROM.commit();
  ws2812b.clear();
  ws2812b.show();
  esp_restart();
}

void setup() {  
  // Turn on the LED in RED color
  //Serial.begin(115200);
  ws2812b.begin();
  ws2812b.clear();
  ws2812b.setPixelColor(0, ws2812b.Color(0, 255, 0));
  ws2812b.show();
  // start the EEPROM
  EEPROM.begin(EEPROM_SIZE);
  for (int i=0; i<JOYBUTTONS; i++) {
    joyPos[i] = EEPROM.read(2*i);
    joyVal[i] = EEPROM.read(2*i+1);
  }
  // Start the USB
  app_main();
  // Configure the GPIOs for the Joystick
  pinMode(CONFIG, INPUT_PULLUP);  // Set the BOOT button in input mode
  delay(1000); // Wait for 1 second
  if (digitalRead(CONFIG) == LOW) { // If the user holds the BOOT button it will set the board in configuration mode
    configurator();
  }
  else {
    // Define the GPIO and Interrupt for the mouse/joystick switch
    pinMode(SWITCH_MJ, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(SWITCH_MJ), switchMJHandler, CHANGE);
    // Define the GPIOs for the mouse interrupt and the POTs
    pinMode(C64_INT, INPUT_PULLUP);
    // Check if it is a C64 listening to the interrupt pin
    for (int i=0; i<1000; i++){
      if (((GPIO.in >> C64_INT) & 1)){
        ISC64++;
      }
      delayMicroseconds(256);
    }
    // If it is a C64
    if (ISC64 > 0) {
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
      pinMode(AMIGA_BUTTON2, OUTPUT); // Set the AMIGA BUTTON2 to OPEN CIRCUIT
      digitalWrite(AMIGA_BUTTON2, LOW);
      pinMode(AMIGA_BUTTON2, INPUT);
      pinMode(AMIGA_BUTTON3, OUTPUT); // Set the AMIGA BUTTON2 to OPEN CIRCUIT
      digitalWrite(AMIGA_BUTTON3, LOW);
      pinMode(AMIGA_BUTTON3, INPUT);
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
    // If is AMIGA
    else{
      if (digitalRead(SWITCH_MJ)) {  
        pinMode(AMIGA_UP, OUTPUT);
        pinMode(AMIGA_DOWN, OUTPUT);
        pinMode(AMIGA_LEFT, OUTPUT);
        pinMode(AMIGA_RIGHT, OUTPUT);
        pinMode(AMIGA_FIRE, OUTPUT);
        digitalWrite(AMIGA_FIRE, LOW);
        pinMode(AMIGA_FIRE, INPUT);
        pinMode(AMIGA_BUTTON2, OUTPUT);
        digitalWrite(AMIGA_BUTTON2, LOW);
        pinMode(AMIGA_BUTTON2, INPUT);
        pinMode(AMIGA_BUTTON3, OUTPUT);
        digitalWrite(AMIGA_BUTTON3, LOW);
        pinMode(AMIGA_BUTTON3, INPUT);                                               // If we are in mouse mode
        ws2812b.setPixelColor(0, ws2812b.Color(0, 0, 25));                           // Set the LED BLU
        ws2812b.show();
      }
      else {           
        pinMode(AMIGA_UP, OUTPUT);
        digitalWrite(AMIGA_UP, LOW);
        pinMode(AMIGA_UP, INPUT);
        pinMode(AMIGA_DOWN, OUTPUT);
        digitalWrite(AMIGA_DOWN, LOW);
        pinMode(AMIGA_DOWN, INPUT);
        pinMode(AMIGA_LEFT, OUTPUT);
        digitalWrite(AMIGA_LEFT, LOW);
        pinMode(AMIGA_LEFT, INPUT);
        pinMode(AMIGA_RIGHT, OUTPUT);
        digitalWrite(AMIGA_RIGHT, LOW);
        pinMode(AMIGA_RIGHT, INPUT);
        pinMode(AMIGA_FIRE, OUTPUT);
        digitalWrite(AMIGA_FIRE, LOW);
        pinMode(AMIGA_FIRE, INPUT);
        pinMode(AMIGA_BUTTON2, OUTPUT);
        digitalWrite(AMIGA_BUTTON2, LOW);
        pinMode(AMIGA_BUTTON2, INPUT);
        pinMode(AMIGA_BUTTON3, OUTPUT);
        digitalWrite(AMIGA_BUTTON3, LOW);
        pinMode(AMIGA_BUTTON3, INPUT);                                                             // If we are in joystick mode
        ws2812b.setPixelColor(0, ws2812b.Color(25, 0, 0));                        // Turn the LED green
        ws2812b.show();
        // Decrease the frequency of the CPU to 10 MHz to drop the current usage of the board from 90 to 20 mA, in this way two
        // Joystick can be safely used on the C64 with the 100 mA current supply of the control ports.
        // We cannot use this frequency in mouse mode because the hardware timers must be fast enough to trigger the interrupt
        // at the right moment. Also it is very uncommon that we will need 2 mouse at he same time on the C64.
        setCpuFrequencyMhz(10);
      }
    }
  }
}

void loop() {}