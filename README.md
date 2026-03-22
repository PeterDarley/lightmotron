# Think Tank

Very eary work on writing a controller for a combat robot.  I'd like the bot to be semi-autonomous in the following ways:
* Use an accelerometer to detect and react to suffering a hit
* Use a magnetometer to understand what direction the bot is posting
* Use either treads or Mecanum wheels to allow for absolute steering, meaning tell the bot to move in a certain cardinal direction (forward, backward, strafe left, strafe right) instead of normal steering (forward, backward, turn left, turn right)

If cloning this repo, you need to know that there are submodules for external libraries.  When cloning you'll need to do: `git submodule init`, then after instalation, and every time there is an update, do: `git submodule update`

Currently useing an ESP32 - Wroom module and developing in MicroPython

The current state of the Think Tank hardware:
![Alt text](https://github.com/PeterDarley/ThinkTank/blob/main/docs/ThinkTank.jpg)

I chose all the hardware out of ignorance because it was cheap, with the thought that once I had a better idea of what I was doing I could upgrade to what made sense.  So far I'm happy with all of it.

Hardware used:
* ESP-WROOM-32 development board - Only has 512k of memory.  I suspect this will need to be replaced.
* GY-273 QMC5883L 3 axis compass module
* GY-521 MPU-6050 3 axis Accelerometer/Gyroscope module