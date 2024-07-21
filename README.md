USB Adapter for Commodore 64 Joystick and Mouse Port

This adapter interfaces a USB device with the CONTROL Port of the C64, allowing it to be used as a mouse or joystick.

The joystick connects via pins 1, 2, 3, 4, and 6 of the CONTROL port, with the GPIOs simply set as open circuits or shorted to ground when a joystick direction is pressed.

The mouse uses the analog part of the port (Pot X and Pot Y). The Commodore 64 has these two pins for evaluating an analog resistor that charges an internal capacitor. The charging time is decoded as a resistor value with a digital value from 0 to 255. The trick used by Commodore engineers to use it as a mouse was to send a pulse at the right moment, making the C64 believe that the capacitor is fully charged. To establish the right moment, the C64 goes through 512 cycles (almost 512 microseconds, since the clock frequencies of PAL and NTSC are not exactly 1 MHz: PAL is 0.985248 MHz, NTSC is 1.022727 MHz). During the first 256 cycles, the potentiometer is set to ground, then it charges the capacitor.

The idea is to use an ESP32 board to wait for the discharge drop and then an additional 256 cycles, finally sending the proper value to the C64 at the right moment. The ESP32 allows interrupts when an input signal is falling; however, the voltage from the C64 is a bit too low for the ESP32 interrupt because the board requires at least 3 volts, while the C64 provides around 1.2 volts. Therefore, a BJT is used to amplify the signal (and invert it, so it is HIGH when the C64 is LOW, which reduces noise in detecting the status).

The initial variable values are the timing for a PAL C64 and are obtained empirically. It is possible that another C64 may use slightly different timing, though it should be quite stable since Commodore sold the same mouse to everyone. There can be differences with an NTSC version. When I have one, I will test it and adjust the timing accordingly.

There is an additional switch to make the board work in mouse mode or joystick mode. In mouse mode, any connected device will use the analog mouse, so a program like GEOS can be controlled with a USB mouse or a gamepad. In joystick mode, the board uses the joystick pins for any kind of device. This means that some games, like graphic adventure games (e.g., Maniac Mansion), can be played with a mouse even if they were originally designed for a game controller.

![](https://github.com/emanuelelaface/USBtoC64/blob/main/Schematic.jpeg)

WARNING: DON'T CONNECT THE COMMODORE 64 AND THE USB PORT TO A SOURCE OF POWER AT THE SAME TIME.
THE POWER WILL ARRIVE DIRECTLY TO THE SID OF THE COMMODORE AND MAY DESTROY IT.

---

THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
