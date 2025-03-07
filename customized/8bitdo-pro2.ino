/*
CUSTOMIZATION FOR 8BitDo Pro 2, USB 11720:24582

USB to Commodore 64 adaptor V 1.1 by Emanuele Laface

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

#define PIN_WS2812B       21 // Pin for RGB LED
#define NUM_PIXELS         1 // 1 LED
Adafruit_NeoPixel ws2812b(NUM_PIXELS, PIN_WS2812B, NEO_RGB + NEO_KHZ800); // Initialize LED Some board is NEO_RGB

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
#define SWITCH_MJ         13 // HIGH = mouse, LOW = Joystick

// Define the default timers for the mouse delay, all empirical for PAL version
#define PAL                1 // select if it is PAL or NTSC and adjust the timings

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
#define PULSE_LENGTH    150 // lenght of pusle for Amiga mouse

int ISC64 = 0;  // Commodore 64 / Amiga/Atari detection flag
int ISAMIGA = 1;  // Switch to select AMIGA or ATARI

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

bool autofire = false;
bool autoleftright = false;

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
      a_mouse_m(mouse_report);
    }
  }
  else {                        // If we are in joystick mode
    if (ISC64 > 0) {
      c64_mouse_j(mouse_report);        // The mouse function in joystick mode is called
    }
    else {
      a_mouse_j(mouse_report);
    }
  }
}

static void hid_host_generic_report_callback(const uint8_t *const data, const int length) {
  // Here is where the USB joystick receive the report from USB.
  if (digitalRead(SWITCH_MJ)) {     // If we are in Mouse mode
    if (ISC64>0) {  
      c64_joystick_m(data, length);   // The joystick function in mouse mode is called
    }
    else {
      a_joystick_m(data, length);
    }
  }
  else {   // If we are in joystick mode
    if (ISC64 > 0) {                   // If it is a Commodore 64       
      c64_joystick_j(data, length);   // The joystick function in joystick mode is called
    }
    else {
      a_joystick_j(data, length);
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

  delayOnY -= STEPdelayOnY*mouse_report->y_displacement;   // Define the moment when the POTY has to be turned on after the interrupt
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
  bool isup = false;
  bool isdown = false;
  bool isleft = false;
  bool isright = false;
  bool isfire = false;

  if ((data[1] == 0) | (data[8] == 8)) {
    isup = true;
  }
  if (data[1] == 1) {
    isup = true;
    isright = true;
  }
  if (data[1] == 2) {
    isright = true;
  }
  if (data[1] == 3) {
    isdown = true;
    isright = true;
  }
  if (data[1] == 4) {
    isdown = true;
  }
  if (data[1] == 5) {
    isdown = true;
    isleft = true;
  }
  if (data[1] == 6) {
    isleft = true;
  }
  if (data[1] == 7) {
    isup = true;
    isleft = true;
  }
  if ((data[8] == 64) | (data[8] == 128) | (data[8] == 1)) {
      isfire = true;
  }
  if (data[8] == 16) {
    autofire = true;
  }
  if (data[8] == 2) {
    autofire = false;
  }
  if (data[8] == 32) {
    autoleftright = true;
  }
  if (data[8] == 4) {
    autoleftright = false;
  }
  if (autofire) {
    pinMode(C64_FIRE, OUTPUT);
    digitalWrite(C64_FIRE, LOW);
    delay(5);
  }
  if (autoleftright) {
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
  if (isup) {
    pinMode(C64_UP, OUTPUT);
    digitalWrite(C64_UP, LOW);
  }
  else {
    digitalWrite(C64_UP, LOW);
    pinMode(C64_UP, INPUT);
  }
  if (isdown) {
    pinMode(C64_DOWN, OUTPUT);
    digitalWrite(C64_DOWN, LOW);
  }
  else {
    digitalWrite(C64_DOWN, LOW);
    pinMode(C64_DOWN, INPUT);
  }
  if (isleft) {
    pinMode(C64_LEFT, OUTPUT);
    digitalWrite(C64_LEFT, LOW);
  }
  else {
    digitalWrite(C64_LEFT, LOW);
    pinMode(C64_LEFT, INPUT);
  }
  if (isright) {
    pinMode(C64_RIGHT, OUTPUT);
    digitalWrite(C64_RIGHT, LOW);
  }
  else {
    digitalWrite(C64_RIGHT, LOW);
    pinMode(C64_RIGHT, INPUT);
  }
  if (isfire) {
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
  if ((data[8] == 64) | (data[8] == 1)) {
      pinMode(C64_FIRE, OUTPUT);
      digitalWrite(C64_FIRE, LOW);
  }
  else {
    digitalWrite(C64_FIRE, LOW);
    pinMode(C64_FIRE, INPUT);
  }
  if (data[8] == 128) {
      pinMode(C64_UP, OUTPUT);
      digitalWrite(C64_UP, LOW);
  }
  else {
    digitalWrite(C64_UP, LOW);
    pinMode(C64_UP, INPUT);
  }
  if ((data[2] < 64) | (data[2] > 190)){
    delayOnX += (data[2]-127)/3;
  }
  if ((data[4] < 64) | (data[4] > 190)){
    delayOnX += (data[4]-127)/3;
  }
  if (delayOnX > MAXdelayOnX) {
    delayOnX = MINdelayOnX;
  }
  if (delayOnX < MINdelayOnX) {
    delayOnX = MAXdelayOnX;
  }
  if ((data[3] < 64) | (data[3] > 190)){
    delayOnY += (127-data[3])/3;
  }
  if ((data[5] < 64) | (data[5] > 190)){
    delayOnY += (127-data[5])/3;
  }
  if (delayOnY > MAXdelayOnY) {
    delayOnY = MINdelayOnY;
  }
  if (delayOnY < MINdelayOnY) {
    delayOnY = MAXdelayOnY;
  }
}

void AHorizontalMove(int pulse) {
  digitalWrite(A_DOWN, H[QX]);
  if (ISAMIGA == 1) {
    digitalWrite(A_RIGHT, HQ[QX]);
  }
  else {
    digitalWrite(A_UP, HQ[QX]);
  }
  delayMicroseconds(pulse);
}

void AVerticalMove(int pulse) {
  if (ISAMIGA == 1) {
    digitalWrite(A_UP, H[QY]);
  }
  else {
    digitalWrite(A_RIGHT, H[QY]);
  }
  digitalWrite(A_LEFT, HQ[QY]);
  delayMicroseconds(pulse);
}

void A_Left(int pulse) {
  AHorizontalMove(pulse);
  QX = (QX >= 3) ? 0 : ++QX;    
}

void A_Right(int pulse) {
  AHorizontalMove(pulse);
  QX = (QX <= 0) ? 3 : --QX;
}

void A_Down(int pulse) {
  AVerticalMove(pulse);
  QY = QY <= 0 ? 3 : --QY;
}

void A_Up(int pulse) {
  AVerticalMove(pulse);
  QY = QY >= 3 ? 0 : ++QY;
}

void a_mouse_m(hid_mouse_input_report_boot_t *mouse_report) {
  int xsteps = abs(mouse_report->x_displacement);
  int ysteps = abs(mouse_report->y_displacement);
  int xsign = (mouse_report->x_displacement > 0 ? 1 : 0) ;
  int ysign = (mouse_report->y_displacement > 0 ? 1 : 0) ;
  int xpulse = 0;
  int ypulse = 0;
  if (ISAMIGA == 1) {
    xpulse = PULSE_LENGTH;
    ypulse = PULSE_LENGTH;
  }
  else {
    xpulse = 18.6*PULSE_LENGTH/xsteps;
    ypulse = 18.6*PULSE_LENGTH/ysteps;
  }

  if (mouse_report->buttons.button1) {  // Left button is wired to C64 FIRE
    pinMode(A_FIRE, OUTPUT);
    digitalWrite(A_FIRE, LOW);
  }
  else {
    digitalWrite(A_FIRE, LOW);
    pinMode(A_FIRE, INPUT);
  }
  if (mouse_report->buttons.button2) {  // Right button is wired to C64 UP
    pinMode(A_BUTTON2, OUTPUT);
    digitalWrite(A_BUTTON2, LOW);
  }
  else {
    digitalWrite(A_BUTTON2, LOW);
    pinMode(A_BUTTON2, INPUT);
  }
  if (mouse_report->buttons.button3) {  // Right button is wired to C64 UP
    pinMode(A_BUTTON3, OUTPUT);
    digitalWrite(A_BUTTON3, LOW);
  }
  else {
    digitalWrite(A_BUTTON3, LOW);
    pinMode(A_BUTTON3, INPUT);
  }
  while ((xsteps | ysteps) != 0) {
    if (xsteps != 0) {
        if (xsign)
            A_Right(xpulse);
        else
            A_Left(xpulse); 
        xsteps--;
    }
    if (ysteps != 0) {
        if (ysign)
            A_Down(ypulse);
        else
            A_Up(ypulse); 
        ysteps--;
    }
  }  
}

void a_mouse_j(hid_mouse_input_report_boot_t *mouse_report) {
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

void a_joystick_j(const uint8_t *const data, const int length) {
  bool isup = false;
  bool isdown = false;
  bool isleft = false;
  bool isright = false;
  bool isfire = false;

  if ((data[1] == 0) | (data[8] == 8)) {
    isup = true;
  }
  if (data[1] == 1) {
    isup = true;
    isright = true;
  }
  if (data[1] == 2) {
    isright = true;
  }
  if (data[1] == 3) {
    isdown = true;
    isright = true;
  }
  if (data[1] == 4) {
    isdown = true;
  }
  if (data[1] == 5) {
    isdown = true;
    isleft = true;
  }
  if (data[1] == 6) {
    isleft = true;
  }
  if (data[1] == 7) {
    isup = true;
    isleft = true;
  }
  if ((data[8] == 64) | (data[8] == 128) | (data[8] == 1)) {
      isfire = true;
  }
  if (data[8] == 16) {
    autofire = true;
  }
  if (data[8] == 2) {
    autofire = false;
  }
  if (data[8] == 32) {
    autoleftright = true;
  }
  if (data[8] == 4) {
    autoleftright = false;
  }
  if (autofire) {
    pinMode(A_FIRE, OUTPUT);
    digitalWrite(A_FIRE, LOW);
    delay(5);
  }
  if (autoleftright) {
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
  if (isup) {
    pinMode(A_UP, OUTPUT);
    digitalWrite(A_UP, LOW);
  }
  else {
    digitalWrite(A_UP, LOW);
    pinMode(A_UP, INPUT);
  }
  if (isdown) {
    pinMode(A_DOWN, OUTPUT);
    digitalWrite(A_DOWN, LOW);
  }
  else {
    digitalWrite(A_DOWN, LOW);
    pinMode(A_DOWN, INPUT);
  }
  if (isleft) {
    pinMode(A_LEFT, OUTPUT);
    digitalWrite(A_LEFT, LOW);
  }
  else {
    digitalWrite(A_LEFT, LOW);
    pinMode(A_LEFT, INPUT);
  }
  if (isright) {
    pinMode(A_RIGHT, OUTPUT);
    digitalWrite(A_RIGHT, LOW);
  }
  else {
    digitalWrite(A_RIGHT, LOW);
    pinMode(A_RIGHT, INPUT);
  }
  if (isfire) {
    pinMode(A_FIRE, OUTPUT);
    digitalWrite(A_FIRE, LOW);
  }
  else {
    digitalWrite(A_FIRE, LOW);
    pinMode(A_FIRE, INPUT);
  }
}

void a_joystick_m(const uint8_t *const data, const int length) {
  int xsteps = 0;
  int ysteps = 0;
  int xsign = 0;
  int ysign = 0;
  int xpulse = 0;
  int ypulse = 0;
  if (ISAMIGA == 1) {
    xpulse = PULSE_LENGTH;
    ypulse = PULSE_LENGTH;
  }
  else {
    xpulse = 18.6*PULSE_LENGTH/xsteps;
    ypulse = 18.6*PULSE_LENGTH/ysteps;
  }

  if ((data[8] == 64) | (data[8] == 1)) {
      pinMode(A_FIRE, OUTPUT);
      digitalWrite(A_FIRE, LOW);
  }
  else {
    digitalWrite(A_FIRE, LOW);
    pinMode(A_FIRE, INPUT);
  }
  if (data[8] == 128) {
      pinMode(A_BUTTON2, OUTPUT);
      digitalWrite(A_BUTTON2, LOW);
  }
  else {
    digitalWrite(A_BUTTON2, LOW);
    pinMode(A_BUTTON2, INPUT);
  }

  if ((data[2] < 64) | (data[2] > 190)){
    xsteps += (data[2]-127)/40;
  }
  if ((data[4] < 64) | (data[4] > 190)){
    xsteps += (data[4]-127)/40;
  }
  if ((data[3] < 64) | (data[3] > 190)){
    ysteps += (data[3]-127)/40;
  }
  if ((data[5] < 64) | (data[5] > 190)){
    ysteps += (data[5]-127)/40;
  }

  xsign = (xsteps > 0 ? 1 : 0) ;
  ysign = (ysteps > 0 ? 1 : 0) ;
  xsteps = abs(xsteps);
  ysteps = abs(ysteps);

  while ((xsteps | ysteps) != 0) {
    if (xsteps != 0) {
        if (xsign)
            A_Right(xpulse);
        else
            A_Left(xpulse); 
        xsteps--;
    }
    if (ysteps != 0) {
        if (ysign)
            A_Down(ypulse);
        else
            A_Up(ypulse); 
        ysteps--;
    }
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

  delay(1000);
  // Start the USB
  app_main();
  pinMode(SWITCH_MJ, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(SWITCH_MJ), switchMJHandler, CHANGE);
  // Define the GPIOs for the mouse interrupt and the POTs
  pinMode(C64_INT, INPUT_PULLUP);
  // Check if it is a C64 listening to the interrupt pin

  for (int i=0; i<20; i++){
    if (((GPIO.in >> C64_INT) & 1)){
      ISC64++;
    }
    delayMicroseconds(256);
  }
  if (ISC64 < 5) {
    pinMode(A_BUTTON3, INPUT_PULLDOWN);
    pinMode(C64_POTX, INPUT_PULLDOWN);
    if (digitalRead(A_BUTTON3) == HIGH) {
      ISAMIGA = 1;
    }
    else {
      ISAMIGA = 0;
    }
  }

  // If it is a C64
  if (ISC64 >= 5 && ISC64 <= 15) {
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
  // If is AMIGA/ATARI
  else{
    if (digitalRead(SWITCH_MJ)) {  
      pinMode(A_UP, OUTPUT);
      pinMode(A_DOWN, OUTPUT);
      pinMode(A_LEFT, OUTPUT);
      pinMode(A_RIGHT, OUTPUT);
      pinMode(A_FIRE, OUTPUT);
      digitalWrite(A_FIRE, LOW);
      pinMode(A_FIRE, INPUT);
      pinMode(A_BUTTON2, OUTPUT);
      digitalWrite(A_BUTTON2, LOW);
      pinMode(A_BUTTON2, INPUT);
      pinMode(A_BUTTON3, OUTPUT);
      digitalWrite(A_BUTTON3, LOW);
      pinMode(A_BUTTON3, INPUT);                                               // If we are in mouse mode
      ws2812b.setPixelColor(0, ws2812b.Color(0, 0, 25));                           // Set the LED BLU
      ws2812b.show();
    }
    else {           
      pinMode(A_UP, OUTPUT);
      digitalWrite(A_UP, LOW);
      pinMode(A_UP, INPUT);
      pinMode(A_DOWN, OUTPUT);
      digitalWrite(A_DOWN, LOW);
      pinMode(A_DOWN, INPUT);
      pinMode(A_LEFT, OUTPUT);
      digitalWrite(A_LEFT, LOW);
      pinMode(A_LEFT, INPUT);
      pinMode(A_RIGHT, OUTPUT);
      digitalWrite(A_RIGHT, LOW);
      pinMode(A_RIGHT, INPUT);
      pinMode(A_FIRE, OUTPUT);
      digitalWrite(A_FIRE, LOW);
      pinMode(A_FIRE, INPUT);
      pinMode(A_BUTTON2, OUTPUT);
      digitalWrite(A_BUTTON2, HIGH);
      pinMode(A_BUTTON2, INPUT);
      pinMode(A_BUTTON3, OUTPUT);
      digitalWrite(A_BUTTON3, HIGH);
      pinMode(A_BUTTON3, INPUT);                          // If we are in joystick mode
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

void loop() {}
