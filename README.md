## RPS Game For RP2350 (Tested on Raspberry Pi Pico-2)

This is project contains the program called "Rock Paper Showdown" Arcade Game from Contract Rush Dx game.
To make your own arcade cabinet, 3D models and schematic is available to this git.

How to compile ?

First step, you have to install Raspberry Pi Pico SDK C/C++, please follow the instructions on the link below.
https://www.raspberrypi.com/news/raspberry-pi-pico-windows-installer/

Second step, you will have to bring a folder containing all these source code files to your project.
(Please see Raspberry Pi Pico SDK documentation to import project on VSCode)

Project description :
All headers files contains sprites and sound (except for st7789_lcd.pio.h that contains screen's driver functions).
Rock_Paper_Shodown.c is the main file, you will have to compile with this file.


# Schematic

Parts needed :

Raspberry Pi Pico 2 (or equivalent board equipped with a RP2350 MCU) : https://shop.pimoroni.com/products/raspberry-pi-pico-2?variant=54906879181179
Xbox 360 joystick part (or equivalent sized 20.3mm x 17.9 mm) : https://www.amazon.com/Joystick-Wireless-Controller-Thumbstick-Replacement/dp/B0CL4GWQFH
Tactile push button 6 mm x 6 mm x 8 mm : https://www.amazon.com/Luftschloss-Heights-Tactile-Button-Momentary/dp/B0FW3GS6XC?crid=G4JQX4C5CGXV&dib=unde
ST7735 1.44" screen 128x128 pixels : https://www.amazon.com/128x128-Display-Module-ST7735-Arduino/dp/B0DN9VVNXC?crid=3J1L4UOF8YHII&dib
ST7735S 0.96" screen 80x160 pixels : https://www.amazon.com/80X160-Display-Module-ST7735-Interface/dp/B0DMVCHGX7?crid=2MI3LR05XMVNU&dib
28 mm diameter Speaker 1W : https://www.amazon.com/Speaker-Speakers-Compatible-Loudspeaker-Player/dp/B09YH5JZ1K?crid=3LCZ090KDDNEW&dib
2N2222 NPN Transistor : https://www.amazon.com/Eowpower-Transistors-2N2222A-Transistor-Through/dp/B09N1DZHQM?crid=3UK7M41WFFGER&dib 
20 AWG Electrical Wires : https://www.amazon.com/Fermerry-Stranded-Electronic-Electrical-Automotive/dp/B089CFST2X?crid=1CYKSMLSES0VX&dib
2.54 mm pin header : https://www.amazon.com/Jabinco-Breakable-Header-Connector-Arduino/dp/B0817JG3XN?crid=HIK23YK6AMT7&dib
2.54 mm connector header : https://www.amazon.com/Ferwooh-Single-Straight-Connector-Spacing/dp/B0CZ6X313F?crid=HIK23YK6AMT7&dib

Please follow this "non-extistent" schematic to mount the electronic circuit.

(Add schematic...)
