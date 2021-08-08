#!/usr/bin/env python

# pip3 install pyserial

import serial.tools.list_ports
import serial

a = serial.tools.list_ports.comports()
for w in a:
    # https://devicehunt.com/view/type/usb/vendor/1A86/device/7523
    # https://devicehunt.com/view/type/usb/vendor/10C4/device/EA60
    if "1a86" in w.hwid.lower() or "10c4" in w.hwid.lower():
        print(w.device)
