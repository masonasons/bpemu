APH Braille+ / LevelStar Icon emulator
======================================

This runs the actual firmware from the device on an emulated PXA270 board.
Nothing here needs installing. Windows 10 or 11, 64-bit.


Running it
----------

Double-click BraillePlus.bat.

Or, from PowerShell:

    .\Run-BraillePlus.ps1

It takes about half a minute to boot, then the device starts speaking.


What you will see
-----------------

A black window opens and stays black. That is correct. The Braille+ has no
screen, so its firmware never draws anything at all. That window exists to
capture your keystrokes and pass them to the emulated keypad, so click it to
give it focus when you want to type on the device.

Sound comes out of your speakers, including the Eloquence speech the device
uses for everything.

The terminal you launched from shows the Linux serial console. The device's
login prompt and the application's own output share that line, so they
interleave. Logging in there is not needed for normal use; root has an empty
password if you want a shell.

To shut down, close the black window or press Ctrl+C in the terminal.


The keyboard
------------

The device has a braille keyboard, and the emulator maps it to your PC
keyboard. The six braille dots are:

    dot 1 = F      dot 4 = J
    dot 2 = D      dot 5 = K
    dot 3 = S      dot 6 = L

Press them together to form a braille character, the way you would on the real
device: F alone is "a", S alone is an apostrophe, and so on. Space, shift and
control work as themselves.


Networking
----------

    .\Run-BraillePlus.ps1 -Net

The real hardware's WiFi and Bluetooth are a proprietary Stonestreet One combo
chip with a binary-only driver, and cannot be emulated. What this does instead
is attach a USB ethernet adapter that the firmware already has a driver for.

The firmware does not configure it by itself, because it never expected one.
From the device's shell:

    ifconfig eth0 10.0.2.15 netmask 255.255.255.0 up
    route add default gw 10.0.2.2

That puts it behind user-mode networking, so outbound connections work and the
device is not reachable from your LAN.


Other options
-------------

    .\Run-BraillePlus.ps1 -AudioDrv none     boot silently
    .\Run-BraillePlus.ps1 -AudioDrv sdl      alternative sound backend
    .\Run-BraillePlus.ps1 -Display none      headless; no keypad input
    .\Run-BraillePlus.ps1 -Ram 128           more RAM

The device's 60GB hard drive is created as firmware\hdd.qcow2 the first time
you run it. It starts blank, so the device offers to format it on first boot.
Deleting that file resets the drive. It grows only as the guest writes, so it
does not really occupy 60GB.


What is emulated, and what is not
---------------------------------

Working: the processor and board, the boot flash and its filesystem, the
braille keypad, sound through both of the codec's outputs (which is what
Eloquence speech comes out of), the hard drive, the battery gauge, and wired
networking as described above.

Not emulated: WiFi and Bluetooth (see above), the vibration motor, and audio
recording, which reads silence. The amount of RAM and the exact flash layout
are informed guesses -- the real bootloader supplies those, and we cannot read
it.

docs\everest-board.md has the full technical account of how the board was
reconstructed, if you are interested in that sort of thing.


Licensing
---------

The emulator itself is QEMU, which is free software under the GPL v2. The
source for the board and device models added to it ships alongside this
package.

The firmware images in the firmware\ folder are not ours and are not free
software. They are the copyrighted work of American Printing House for the
Blind / LevelStar, and they include a licensed copy of the Eloquence speech
synthesiser. They are here so the emulator has something to run. Whether you
may pass this folder on to anyone else depends on your rights to that
firmware -- being able to copy it does not make it yours to distribute.
