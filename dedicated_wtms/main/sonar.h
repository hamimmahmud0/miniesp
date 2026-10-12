#pragma once
// HC-SR04 echo timing by GPIO interrupt (IRAM handler stamps both edges): the width does not depend on task scheduling. The interrupt is allocated on
// the core that sets the pins up, so sonar_service() must run on the application core: any other thread asking for new pins just requests it.
void sonar_service(void);                              // call regularly from the application core's task
int sonar_pulse(int trig, int echo, int timeout_us);   // echo width in us; -1 no echo, -2 echo stuck high (also -1 while the pins are being set up)
