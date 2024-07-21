USB adapter for Commodore 64 Joystick and Mouse port.

This adapter interfaces a usb device to the CONTROL Port of the C64 using it as mouse or joystick.

The Joystick is via PIN 1,2,3,4,6 of the CONTROL port and the GPIO are simply turned as open circuit or short to ground when a direction of the joystic is pressed.

The mouse uses the analog part of the port (Pot X and Pot Y).
The Commodore 64 has the two pots for evaluating an analog resistor that charges an internal capacitor, the time of charging is decoded as value of the resistor with a digital value from 0 to 255. The trick used by Commodore engineers to use it as mouse was to send a pulse at the right moment that makes the C64 believing that the capacitor is fully charged.
To establish the right moment, the C64 does a 512 cycles (almost 512 usec, almost because the clock frequency of PAL and NTSC are not 1 MHz (PAL: 0.985248 MHz, NTSC: 1.022727 MHz) where in the first 256 cycles set the pot to ground, then leave it charging the capacitor.

The idea is to use a ESP32 board to wait for the discharge drop, wait 256 cycles and then send the proper value to the C64 at the right moment.
The ESP32 allows interrupts when an input signal is falling, unfortunately the voltage out of the C64 is a bit to low for the ESP32 interrupt because it wants at least 3 volts and the C64 is around 1.2 volts. So a BJT is used to amplify the signal (and inverting it, so it is HIGH when for the C64 is low, this reduces a bit the noise in detection of the status).

The values in the initial variables are the timing for a PAL C64 and are obtained purely empirically. It is very possible that another C64 uses a bit different timing, even if I believe that it should be quite stable considering that Commodore was selling the same mouse to everyone. For sure there can be difference with a NTSC version. When I will have one on my hands I will test it and adjust the timing according to it.

![](https://github.com/emanuelelaface/USBtoC64/blob/main/Schematic.jpeg)

WARNING: DON'T CONNECT THE COMMODORE 64 AND THE USB PORT TO A SOURCE OF POWER AT THE SAME TIME.
THE POWER WILL ARRIVE DIRECTLY TO THE SID OF THE COMMODORE AND MAY DESTROY IT.

---

THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
