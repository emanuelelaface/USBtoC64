# USB Adapter for Commodore 64 Joystick and Mouse Port

This adapter interfaces a USB device with the CONTROL Port of the C64, allowing it to be used as a mouse or joystick.

The joystick connects via pins 1, 2, 3, 4, and 6 of the CONTROL port, with the GPIOs simply set as open circuits or shorted to ground when a joystick direction is pressed.

The mouse uses the analog part of the port (Pot X and Pot Y). The Commodore 64 has these two pins for evaluating an analog resistor that charges an internal capacitor. The charging time is decoded as a resistor value with a digital value from 0 to 255. The trick used by Commodore engineers to use it as a mouse was to send a pulse at the right moment, making the C64 believe that the capacitor is fully charged. To establish the right moment, the C64 goes through 512 cycles (almost 512 microseconds, since the clock frequencies of PAL and NTSC are not exactly 1 MHz: PAL is 0.985248 MHz, NTSC is 1.022727 MHz). During the first 256 cycles, the potentiometer is set to ground, then it charges the capacitor.

The idea is to use an ESP32 board to wait for the discharge drop and then an additional 256 cycles, finally sending the proper value to the C64 at the right moment. The ESP32 allows interrupts when an input signal is falling; however, the voltage from the C64 is a bit too low for the ESP32 interrupt because the board requires at least 3 volts, while the C64 provides around 1.2 volts. Therefore, a BJT is used to amplify the signal (and invert it, so it is HIGH when the C64 is LOW, which reduces noise in detecting the status).

The initial variable values are the timing for a PAL C64 and are obtained empirically. The NTSC version of the timings are calculated scaling for the ratio of the frequencies NTSC/PAL. It is possible that another C64 may use slightly different timing, though it should be quite stable since Commodore sold the same mouse to everyone. When I will have one NTSC Commodore, I will test it and adjust the timings if needed.

There is an additional switch to make the board work in mouse mode or joystick mode. In mouse mode, any connected device will use the analog mouse, so a program like GEOS can be controlled with a USB mouse or a gamepad. In joystick mode, the board uses the joystick pins for any kind of device. This means that some games, like graphic adventure games (e.g., Maniac Mansion), can be played with a mouse even if they were originally designed for a game controller.

The two releases (2.1 and 3.0) of the PCB differs only for the kind of connector that can be mounted. The 90 degrees connectors are more difficult to find and needs more work to be cut and adapted for the board, so the version 3.0 is for the normal connectors that are easier to find and adapt.

<div style="display: flex; justify-content: space-between;">
  <img src="https://github.com/emanuelelaface/USBtoC64/blob/main/images/schematic.jpeg" alt="Schematic" style="width: 39%;">
  <img src="https://github.com/emanuelelaface/USBtoC64/blob/main/images/pic1-rev2.1.jpg" alt="Pic 1" style="width: 29%;">
  <img src="https://github.com/emanuelelaface/USBtoC64/blob/main/images/pic1-rev3.0.jpg" alt="Pic 1" style="width: 29%;">
</div>
<div style="display: flex; justify-content: space-between;">
  <img src="https://github.com/emanuelelaface/USBtoC64/blob/main/images/pic2-rev2.1.jpg" alt="Pic 2" style="width: 32%;">
  <img src="https://github.com/emanuelelaface/USBtoC64/blob/main/images/pic2-rev3.0.jpg" alt="Pic 3" style="width: 32%;">
  <img src="https://github.com/emanuelelaface/USBtoC64/blob/main/images/pic3-rev3.0.jpg" alt="Pic 3" style="width: 32%;">
</div>

## Pre-assembled and Tested Board
<div style="display: flex; justify-content: space-between;">
  <a href="https://www.tindie.com/products/burglar_ot/usbtoc64/"><img src="https://github.com/emanuelelaface/USBtoC64/blob/main/images/tindie-logo.png" alt="Tindie Logo Link" width="150" height="78"></a>
</div>

If you like this project and want a fully assembled and tested board, you can purchase it on [Tindie](https://www.tindie.com/products/burglar_ot/usbtoc64/). By doing so, you can also benefit from a customized configuration and support the future development of the project.

## Components
- **ESP32 S3 Mini**: I use the [Waveshare](https://www.waveshare.com/esp32-s3-zero.htm) version. There are other boards with a similar form factor, but the pinout may be different, which would require redesigning the board.
- **2N3904 NPN transistor**.
- **PCB Slide Switch, 3-pin**.
- **Two 1 kOhm resistors, 1% tolerance**.
- **Two 150 Ohm resistors, 1% tolerance**.
- **Two BAT 43 Schottky Diodes**.
- **DE-9 (also known as D-SUB 9 or DB 9) female connector**: I use the 90-degree version to solder it onto the PCB. Regardless of the connector type, it's good practice to remove the metallic enclosure because it can easily short the +5V line of the C64 when inserted, potentially damaging your computer. I also cut the lateral plastic to make it more compact.


---
## Installation From Arduino IDE

To install the code from the source file **USBtoC64.ino**, you will need the Arduino IDE. Ensure that the ESP32 board is installed, specifically the ESP32S3 Dev Module.

Additionally, the ESP32 USB HID HOST library is required. This library is not available in the official repository. You can download the ZIP file of the repository from [ESP32_USB_Host_HID](https://github.com/esp32beans/ESP32_USB_Host_HID). To install it, go to Sketch->Include Library->Add .ZIP Library in the Arduino IDE.

To set the board in upload mode, hold the **BOOT** button while the board is disconnected from the USB port. Then, connect the board to the USB port and after one second, the USB port should appear in the list of ports in the Arduino IDE. You can then upload the code.

To select PAL or NTSC timing the #define PAL line has to be set as ture or false.

## Installation From the Binary File

Alternatively, you can load the binary file **USBtoC64.bin**, which is located in the BIN folder. Follow these steps:

1. From Python, run `pip install esptool`.
2. Once esptool is installed, press and hold the **BOOT** button before connecting the board to the USB cable on the computer. Then, connect the board, wait a second, and release the button.
3. On the computer, run `esptool.py -b 921600 -c esp32s3 -p <PORT> write_flash --flash_freq 80m 0x00000 USBtoC64-<PAL|NTSC>.bin`, where <PORT> is the USB port created once the board is connected. On Windows, it is probably COM3 or something similar. On Linux and Mac, it will be /dev/tty.USBsomething or /dev/cu.usbsomething. <PAL|NTSC> is the version with the timings for PAL or for NTSC version of the Commodore 64.

---

## Joystick Configuration

Each joystick or gamepad presents data to the USB in a different way. The library used for ESP32 receives an array of uint8 where each element of the array is connected to a button or an axis, and it is not possible to predict in advance how the joystick will associate this data to the buttons. Therefore, the user has to configure the USBtoC64 manually. To do this, follow the procedure below:

- Identify the **boot** button on the board. It is the left button when the USB port is oriented towards the top.
- Disconnect the joystick from the controller.
- When the controller is connected to the Commodore 64, it will boot with a ![](https://placehold.co/15x15/f03c15/f03c15.png) `RED` LED for one second, then it will change to green (for Joystick) or blue (for mouse) depending on the position of the switch mode.
- To set the controller in configuration mode, the **boot** button must be pressed during the red light. This can be done by rebooting the board (pressing the other button on the board, which is the reset button) and then holding the **boot** button during the red LED.
- If pressed correctly, the red light will flash 10 times quickly, and then a ![](https://placehold.co/15x15/c5f015/c5f015.png) `GREEN` LED will indicate that the board is in configuration mode.
- Insert the USB controller to configure; the green light should turn off, and the controller is ready to be programmed.
- The procedure now requires the insertion of 7 controls: the 4 directions in the exact sequence UP, DOWN, LEFT, RIGHT, and then 3 buttons for FIRE. (3 because most controllers have many buttons and it can be useful to map more than one to fire. Additionally, when the joystick is used in mouse mode, the first 2 fire buttons are assigned as left and right buttons.)
- To associate the controls, press the **boot** button each time and then the controller direction.
- Press the **boot** button; a ![](https://placehold.co/15x15/1589F0/1589F0.png) `BLUE` LED should appear, and the controller will wait for the UP direction. Once the UP direction is pressed on the controller, the blue LED will turn off.
- Repeat the previous step for DOWN, LEFT, RIGHT, FIRE1, FIRE2, FIRE3.
- After the last button, the controller will flash a ![](https://placehold.co/15x15/c5f015/c5f015.png) `GREEN` LED and will be set to work with that controller.
- It is now possible to reboot the controller and use it with the configured joystick.

If your controller is advanced, with analog joystick and you want to map specifically as mouse or you want some more advanced customization you can discover how your values
are mapped following this [Configurator](https://raw.githack.com/emanuelelaface/USBtoC64/main/configurator/config.html) and once you download the CSV file you can contact me for a specific configuration, or if you know your business you can code your controls directly in the firmware source and upload to your controller. You can look at the customized folder where I will collect specific configuration for commercial controllers.

---

WARNING: Some controllers may use the USB port to charge a battery (especially if they are also Bluetooth), and this could draw more than 100 mA from the C64, potentially shutting down the Commodore (and possibly damaging it). If you use a controller with a battery, you should remove the battery before connecting it or disable the charging functionality if possible.

---

WARNING: DON'T CONNECT THE COMMODORE 64 AND THE USB PORT TO A SOURCE OF POWER AT THE SAME TIME.
THE POWER WILL ARRIVE DIRECTLY TO THE SID OF THE COMMODORE AND MAY DESTROY IT.

---

THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
