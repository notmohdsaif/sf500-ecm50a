#pragma once
#include "globals.h"

// =====================================================
// SENSORS.H
// Sensor init/scan, RS485/Modbus read, EC averaging, auto-dosing
// =====================================================

void initSensors();
void readSensors();
void updateECAverage(float reading);
void checkAutoDosing();
void tickStabiliseSkip();
void loadSmartCalibration();
void saveSmartCalibration();
