#!/bin/sh
# Add a hook to Circle's USB device factory so that the emulator can offer
# its own driver (src/circle/usbpowerkey.cpp) for HID interfaces Circle has
# no driver for.  Prints "patched" when it changed the file.
F="${1:?usage: circle-usb-hook.sh CIRCLEDIR}/lib/usb/usbdevicefactory.cpp"
grep -q macemu_usb_hid_hook "$F" && exit 0
python3 - "$F" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
anchor = '''	else if (   pName->Compare ("int3-0-0") == 0
		 || pName->Compare ("int3-0-2") == 0
		 || pName->Compare ("int3-1-0") == 0)'''
assert anchor in s, "Circle's usbdevicefactory.cpp has changed"
hook = '''	else if (   (pName->Compare ("int3-0-0") == 0 || pName->Compare ("int3-1-0") == 0)
		 && macemu_usb_hid_hook != 0
		 && (pResult = macemu_usb_hid_hook (pParent)) != 0)
	{
		// claimed by the application's driver
	}
'''
s = s.replace(anchor, hook + anchor, 1)
decl = '\nCUSBFunction *macemu_usb_hid_hook (CUSBFunction *pParent) __attribute__ ((weak));\n'
i = s.index('\nCUSBFunction *CUSBDeviceFactory::GetDevice')
s = s[:i] + decl + s[i:]
open(p, 'w').write(s)
PY
echo patched
