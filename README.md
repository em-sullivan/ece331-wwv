# ECE331 - WWV Kernel Module
WWV kernel module for ECE331 - Introduction to Unix Systems Administration.

The wwv kernel driver takes the current date and time (sent from user space) and, encodes it into WWV format and drives a GPIO pin on the raspberry pi. This is then decoded by the ECE331 expansion board, which was provided by the class. I would post the project handout but I can't find it.

This kernel module was written for the Raspberry 3, kernel vesrion: 4.19.97-v7+

This class was done by May of 2020, but I may come back this and clean up the code.
