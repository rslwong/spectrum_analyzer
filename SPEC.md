create a microphone test / real-time spectrum analyzer. Captures the on-board mic and draws a
log-spaced bar-graph spectrum with peak-hold markers. A level meter and a `LISTENING`/`quiet` indicator light up when the mic detects sound, and the dominant frequency is shown.
- use file JC4880P443C_I_W.zip in this directory for reference code 
- use file JC4880P443C-BOARD.md for board specification
- create a Makefile for clean, compile and upload. The Makefile should also initialize the ESP-IDF environmnet, so user can just use "make" to compile the project without needing to source other setup files  
- create a README.md for this project
- create an ESP-IDF.md file if specifc version of ESP-IDF is used for this board. I would like to reuse this file for other future project so that we won't hit version incompatiblity issue in the future given the same board. 

 

