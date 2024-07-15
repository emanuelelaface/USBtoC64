/*
USB to Commodore 64 adaptor by Emanuele Laface

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

#define PIN_WS2812B 21
#define NUM_PIXELS 1
Adafruit_NeoPixel ws2812b(NUM_PIXELS, PIN_WS2812B, NEO_GRB + NEO_KHZ800);

#define C64_FIRE          7
#define C64_UP            8
#define C64_DOWN          9 
#define C64_LEFT         10
#define C64_RIGHT        11

#define C64_POTX          4
#define C64_POTY          6

#define C64_INT           1

#define SWITCH_MJ         13 //2 // HIGH = mouse, LOW = Joystick

#define MINdelayOnX    2450
#define MAXdelayOnX    5040
#define MINdelayOnY    2440
#define MAXdelayOnY    5100
#define STEPdelayOnX   10.16689245
#define STEPdelayOnY   10.14384171

#define M2JCalib      20000

volatile uint64_t delayOnX = MINdelayOnX;
volatile uint64_t delayOnY = MINdelayOnY;
volatile uint64_t delayOffX = 10;
volatile uint64_t delayOffY = 10;

hw_timer_t *timerOnX = NULL;
hw_timer_t *timerOnY = NULL;
hw_timer_t *timerOffX = NULL;
hw_timer_t *timerOffY = NULL;

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
  if (digitalRead(SWITCH_MJ)) {
    c64_mouse_m(mouse_report);
  }
  else {
    c64_mouse_j(mouse_report);
  }
}

static void hid_host_generic_report_callback(const uint8_t *const data, const int length) {
  if (digitalRead(SWITCH_MJ)) {
    c64_joystick_m(data, length);
  }
  else {
    c64_joystick_j(data, length);
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

void IRAM_ATTR handleInterrupt() {
  if (!((GPIO.in >> C64_INT) & 1)) return; // Workaround for a BUG!
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

void IRAM_ATTR turnOffPotX() {
  GPIO.out_w1tc = (1 << C64_POTX);
}

void IRAM_ATTR turnOnPotY() {
  GPIO.out_w1ts = (1 << C64_POTY);
  timerWrite(timerOffY, 0);
  timerAlarm(timerOffY, delayOffY, false, 0);
}

void IRAM_ATTR turnOffPotY() {
  GPIO.out_w1tc = (1 << C64_POTY);
}

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

void c64_mouse_m(hid_mouse_input_report_boot_t *mouse_report) {
  if (mouse_report->buttons.button1) {
    pinMode(C64_FIRE, OUTPUT);
    digitalWrite(C64_FIRE, LOW);
  }
  else {
    digitalWrite(C64_FIRE, LOW);
    pinMode(C64_FIRE, INPUT);
  }
  if (mouse_report->buttons.button2) {
    pinMode(C64_UP, OUTPUT);
    digitalWrite(C64_UP, LOW);
  }
  else {
    digitalWrite(C64_UP, LOW);
    pinMode(C64_UP, INPUT);
  }
  delayOnX += STEPdelayOnX*mouse_report->x_displacement;
  if (delayOnX > MAXdelayOnX) {
    delayOnX = MINdelayOnX;
  }
  if (delayOnX < MINdelayOnX) {
    delayOnX = MAXdelayOnX;
  }

  delayOnY -= STEPdelayOnX*mouse_report->y_displacement;
  if (delayOnY > MAXdelayOnY) {
    delayOnY = MINdelayOnY;
  }
  if (delayOnY < MINdelayOnY) {
    delayOnY = MAXdelayOnY;
  }
}

void c64_mouse_j(hid_mouse_input_report_boot_t *mouse_report) {
  if (mouse_report->buttons.button1) {
    pinMode(C64_FIRE, OUTPUT);
    digitalWrite(C64_FIRE, LOW);
  }
  else {
    digitalWrite(C64_FIRE, LOW);
    pinMode(C64_FIRE, INPUT);
  }
  if (mouse_report->x_displacement>0) {
    pinMode(C64_RIGHT, OUTPUT);
    digitalWrite(C64_RIGHT, LOW);
  }
  else {
    pinMode(C64_LEFT, OUTPUT);
    digitalWrite(C64_LEFT, LOW);
  }
  if (mouse_report->y_displacement>0) {
    pinMode(C64_DOWN, OUTPUT);
    digitalWrite(C64_DOWN, LOW);
  }
  else {
    pinMode(C64_UP, OUTPUT);
    digitalWrite(C64_UP, LOW);
  }
  timerWrite(timerOffX, 0);
  timerAlarm(timerOffX, abs(mouse_report->x_displacement)*M2JCalib, false, 0);
  timerWrite(timerOffY, 0);
  timerAlarm(timerOffY, abs(mouse_report->y_displacement)*M2JCalib, false, 0);
}

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

void c64_joystick_m(const uint8_t *const data, const int length) {
  float x = 0;
  float y = 0;
  switch (data[1]) {
    case 0:
      y = 3*STEPdelayOnY;
      break;
    case 255:
      y = -3*STEPdelayOnY;
      break;
    default:
      break;
  }
  switch (data[0]) {
    case 0:
      x = -3*STEPdelayOnX;
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

void IRAM_ATTR switchMJHandler() {
  esp_restart();
}

void setup() {
  
  ws2812b.begin();
  ws2812b.clear();
  ws2812b.setPixelColor(0, ws2812b.Color(0, 255, 0));
  ws2812b.show();
  
  app_main();
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

  pinMode(SWITCH_MJ, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(SWITCH_MJ), switchMJHandler, CHANGE);

  pinMode(C64_INT, INPUT_PULLUP);
  pinMode(C64_POTX, OUTPUT);
  pinMode(C64_POTY, OUTPUT);

  timerOnX = timerBegin(10000000);
  timerAlarm(timerOnX, delayOnX, false, 0);
  timerOffX = timerBegin(10000000);
  timerAlarm(timerOffX, delayOffX, false, 0);
  timerOnY = timerBegin(10000000);
  timerAlarm(timerOnY, delayOnY, false, 0);
  timerOffY = timerBegin(10000000);
  timerAlarm(timerOffY, delayOffY, false, 0);
  
  ws2812b.clear();
  ws2812b.show();

  if (digitalRead(SWITCH_MJ)) {
    timerAttachInterrupt(timerOnX, &turnOnPotX);
    timerAttachInterrupt(timerOffX, &turnOffPotX);
    timerAttachInterrupt(timerOnY, &turnOnPotY);
    timerAttachInterrupt(timerOffY, &turnOffPotY);
    attachInterrupt(digitalPinToInterrupt(C64_INT), handleInterrupt, RISING);
    ws2812b.setPixelColor(0, ws2812b.Color(0, 0, 25));
    ws2812b.show();
  }
  else {
    timerAttachInterrupt(timerOffX, &turnOffJoyX);
    timerAttachInterrupt(timerOffY, &turnOffJoyY);
    ws2812b.setPixelColor(0, ws2812b.Color(25, 0, 0));
    ws2812b.show();
  }
}

void loop() {}