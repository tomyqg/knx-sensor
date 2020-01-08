#define SEALEVELPREASURE_HPA (1013.25)

#ifndef __linux__
// #include <Adafruit_BME280.h>
// #include <Adafruit_Sensor.h>
// #include <ClosedCube_HDC1080.h>
// #include <SparkFun_SCD30_Arduino_Library.h>
#include "SensorHDC1080.h"
#include "SensorBME280.h"
#include "SensorBME680.h"
#include "SensorSCD30.h"
#endif

#include "../../knx-logic/src/Board.h"
// Reihenfolge beachten damit die Definitionen von Sensormodul.h ...
#include "Sensormodul.h"
// ... auf jeden Fall Vorrang haben (beeinflussen auch die Logik)
#include "../../knx-logic/src/LogikmodulCore.h"

// Achtung: Bitfelder in der ETS haben eine gewöhnungswürdige
// Semantik: ein 1 Bit-Feld mit einem BitOffset=0 wird in Bit 7(!) geschrieben
#define BIT_1WIRE 1
#define BIT_Temp 2
#define BIT_Hum 4
#define BIT_Pre 8
#define BIT_Voc 16
#define BIT_Co2 32
#define BIT_RESERVE 64
#define BIT_LOGIC 128

#define SENSOR_HDC1080 0x06
#define SENSOR_BME280 0x0E
#define SENSOR_BME680 0x1E
#define SENSOR_CO2 0x26
#define SENSOR_CO2_BME280 0x2E
#define SENSOR_CO2_BME680 0x3E
#define SENSOR_FILTER_INT 0x7E

#ifdef __linux__
extern KnxFacade<LinuxPlatform, Bau57B0> knx;
#endif

// runtime information for the whole logik module
struct sSensorInfo
{
    // double currentValue;
    unsigned long sendDelay;
    unsigned long readDelay;
};

struct sRuntimeInfo
{
    uint16_t currentPipeline;
    sSensorInfo temp;
    sSensorInfo hum;
    sSensorInfo pre;
    sSensorInfo voc;
    sSensorInfo co2;
    sSensorInfo co2b;
    sSensorInfo dew;
    sSensorInfo wire[8];
    unsigned long startupDelay;
    unsigned long heartbeatDelay;
    uint16_t countSaveInterrupt = 0;
    uint16_t countSaveOld = 999;
    uint32_t saveInterruptTimestamp = 0;
};

sRuntimeInfo gRuntimeData;
uint8_t gSensor = 0;

typedef bool (*getSensorValue)(MeasureType, double&);

uint16_t getError() {
    return (uint16_t)knx.getGroupObject(LOG_KoError).value(getDPT(VAL_DPT_7));
}

void setError(uint16_t iValue) {
    knx.getGroupObject(LOG_KoError).valueNoSend(iValue, getDPT(VAL_DPT_7));
}

void sendError() {
    knx.getGroupObject(LOG_KoError).objectWritten();
}

void ProcessHeartbeat()
{
    // the first heartbeat is send directly after startup delay of the device
    if (gRuntimeData.heartbeatDelay == 0 || delayCheck(gRuntimeData.heartbeatDelay, knx.paramInt(LOG_Heartbeat) * 1000))
    {
        // we waited enough, let's send a heartbeat signal
        knx.getGroupObject(LOG_KoHeartbeat).value(true, getDPT(VAL_DPT_1));
        // if there is an error, we send it with heartbeat, too
        if (knx.paramByte(LOG_Error) & 128) {
            if (getError()) sendError();
            // write diagnose object
            if (gRuntimeData.countSaveInterrupt != gRuntimeData.countSaveOld) {
                char buffer[15];
                sprintf(buffer, "SAVE %d", gRuntimeData.countSaveInterrupt);
                knx.getGroupObject(LOG_KoDiagnose).value(buffer, getDPT(VAL_DPT_16));
                gRuntimeData.countSaveOld = gRuntimeData.countSaveInterrupt;
            }
        }
        gRuntimeData.heartbeatDelay = millis();
        // debug-helper for logic module
        // print("ParDewpoint: ");
        // println(knx.paramByte(LOG_Dewpoint));
        logikDebug();
    }
}

void ProcessReadRequests() {
    // we evaluate only Bit 2 here, which holds the information about read external values on startup
    if (knx.paramByte(LOG_TempExtRead) & 4) {
        knx.getGroupObject(LOG_KoExt1Temp).requestObjectRead();
        knx.getGroupObject(LOG_KoExt2Temp).requestObjectRead();
    }
    if (knx.paramByte(LOG_HumExtRead) & 4) {
        knx.getGroupObject(LOG_KoExt1Hum).requestObjectRead();
        knx.getGroupObject(LOG_KoExt2Hum).requestObjectRead();
    }
    if (knx.paramByte(LOG_PreExtRead) & 4) {
        knx.getGroupObject(LOG_KoExt1Pre).requestObjectRead();
        knx.getGroupObject(LOG_KoExt2Pre).requestObjectRead();
    }
    if (knx.paramByte(LOG_VocExtRead) & 4) {
        knx.getGroupObject(LOG_KoExt1VOC).requestObjectRead();
        knx.getGroupObject(LOG_KoExt2VOC).requestObjectRead();
    }
    if (knx.paramByte(LOG_Co2ExtRead) & 4) {
        knx.getGroupObject(LOG_KoExt1Co2).requestObjectRead();
        knx.getGroupObject(LOG_KoExt2Co2).requestObjectRead();
    }
}

// this callback is used by BME680 during delays while mesauring
// we implement this delay, but keep normal loop processing alive
void sensorDelayCallback(uint32_t iMillis) {
    printf("sensorDelayCallback: Called with a delay of %lu ms", iMillis);
    uint32_t lMillis = millis();
    while (millis() - lMillis < iMillis) {
        knx.loop();
        ProcessHeartbeat();
        if (gSensor & BIT_LOGIC)
            logikLoop();
    } 
    printf("sensorDelayCallback: Left after %lu ms", millis() - lMillis);
}

// Starting all required sensors, this call may be blocking (with delay)
void StartSensor()
{
#ifdef __linux__
    return true;
#else
    Sensor* lSensor;
    // bool lResult = true;
    uint8_t lMeasureTypes;
    // uint16_t lError = (uint16_t)knx.getGroupObject(LOG_KoError).value(getDPT(VAL_DPT_7));
    uint8_t lSensorFlags = gSensor & SENSOR_FILTER_INT;
    if (lSensorFlags == SENSOR_HDC1080)
    {
        lMeasureTypes = static_cast<MeasureType>(Temperature | Humidity);
        lSensor = new SensorHDC1080(lMeasureTypes, 0x40);
        lSensor->begin();
    }
    if (lSensorFlags == SENSOR_BME280 || lSensorFlags == SENSOR_CO2_BME280)
    {
        lMeasureTypes = static_cast<MeasureType>(Temperature | Humidity | Pressure);
        lSensor = new SensorBME280(lMeasureTypes, 0x76);
        lSensor->begin();
    }
    if (lSensorFlags == SENSOR_BME680 || lSensorFlags == SENSOR_CO2_BME680)
    {
        lMeasureTypes = static_cast<MeasureType>(Temperature | Humidity | Pressure | Voc | Accuracy | Co2);
        lSensor = new SensorBME680(lMeasureTypes, 0x76, sensorDelayCallback);
        lSensor->begin();
        gSensor |= BIT_Co2;
    }
    if (lSensorFlags == SENSOR_CO2 || lSensorFlags == SENSOR_CO2_BME280)
    {
        lMeasureTypes = static_cast<MeasureType>(Temperature | Humidity | Co2);
        lSensor = new SensorSCD30(lMeasureTypes, 0x61);
        lSensor->begin();
    }
    // Tempoary for compare both co2 values
    if (lSensorFlags == SENSOR_CO2_BME680) 
    {
        lMeasureTypes = static_cast<MeasureType>(Temperature | Humidity | Reserved);
        lSensor = new SensorSCD30(lMeasureTypes, 0x61);
        lSensor->begin();
        gSensor |= BIT_RESERVE;
    }
#endif
}
bool ReadSensorValue(MeasureType iMeasureType, double& eValue) {
    return Sensor::measureValue(iMeasureType, eValue);
}

// the entries have the same order as the KOs starting with "Ext"
uint8_t gIsExternalValueValid[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

// generic sensor processing
void ProcessSensor(sSensorInfo *cData, getSensorValue fGetSensorValue, MeasureType iMeasureType, double iOffsetFactor, double iValueFactor, uint16_t iParamIndex, uint16_t iKoNumber, bool iForce = false)
{
    bool lSend = iForce;
    // process send cycle
    uint32_t lCycle = knx.paramInt(iParamIndex + 1) * 1000;

    // we waited enough, let's send the value
    if (lCycle && delayCheck(cData->sendDelay, lCycle))
        lSend = true;

    // process read cycle
    if (iForce || delayCheck(cData->readDelay, 3100))
    {
        // we waited enough, let's read the sensor
        int32_t lOffset = (int8_t)knx.paramByte(iParamIndex);
        double lValue;
        bool lValid = fGetSensorValue(iMeasureType, lValue);
        if (lValid) {
            // we have now the internal sensor value, we correct it now
            lValue += (lOffset / iOffsetFactor);
            lValue = lValue / iValueFactor;
            // if there are external values to take into account, we do it here
            uint8_t lNumExternalValues = knx.paramByte(iParamIndex + 9) & 3;
            double lDivisor = 0.0;
            double lDivident = 0.0;
            double lFactor = 0.0;
            switch (lNumExternalValues)
            {
                case 2:
                    lFactor = knx.paramByte(iParamIndex + 12) * gIsExternalValueValid[(iKoNumber - LOG_KoTemp) * 2 + 1]; // factor for external value 2
                    lDivident = (double)knx.getGroupObject((iKoNumber - LOG_KoTemp) * 2 + LOG_KoExt2Temp).value(getDPT(VAL_DPT_9)) * lFactor;
                    lDivisor = lFactor;
                case 1:
                    lFactor = knx.paramByte(iParamIndex + 11) * gIsExternalValueValid[(iKoNumber - LOG_KoTemp) * 2]; // factor for external value 1
                    lDivident += (double)knx.getGroupObject((iKoNumber - LOG_KoTemp) * 2 + LOG_KoExt1Temp).value(getDPT(VAL_DPT_9)) * lFactor;
                    lDivisor += lFactor;
                    lFactor = knx.paramByte(iParamIndex + 10); // factor for internal value
                    lDivident += lValue * lFactor;
                    lDivisor += lFactor;
                    if (lDivisor > 0.0) lValue = lDivident / lDivisor;
                    break;
                default:
                    lDivisor = 1;
                    break;
            }
            if (lDivisor > 0.1) {
                // smoothing (? glätten ?) of the new value
                lValue = (double)knx.getGroupObject(iKoNumber).value(getDPT(VAL_DPT_9)) + (lValue - (double)knx.getGroupObject(iKoNumber).value(getDPT(VAL_DPT_9))) / knx.paramByte(iParamIndex + 8);
                // evaluate sending conditions (relative delta / absolute delta)
                double lDelta = 100.0 - lValue / (double)knx.getGroupObject(iKoNumber).value(getDPT(VAL_DPT_9)) * 100.0;
                uint32_t lPercent = knx.paramByte(iParamIndex + 7);
                if (lPercent && (uint32_t)abs(lDelta) >= lPercent)
                    lSend = true;
                float lAbsolute = knx.paramWord(iParamIndex + 5) / iOffsetFactor;
                if (lAbsolute > 0.0 && abs(lValue - (float)knx.getGroupObject(iKoNumber).value(getDPT(VAL_DPT_9))) >= lAbsolute)
                    lSend = true;
                // we always store the new value in KO, even it it is not sent (to satisfy potential read request)
                knx.getGroupObject(iKoNumber).valueNoSend(lValue, getDPT(VAL_DPT_9));
            }
        }
        cData->readDelay = millis();
    }
    if (lSend)
    {
        if ((getError() & iMeasureType) == 0) knx.getGroupObject(iKoNumber).objectWritten();
        cData->sendDelay = millis();
    }
}

struct sPoint
{
    double x;
    double y;
};

sPoint comfort1[8] = {{17, 88.8}, {21.4, 84.1}, {25, 60}, {27.1, 30.5}, {25.9, 29.5}, {20, 29.5}, {17.1, 48.8}, {15.9, 78.8}};
sPoint comfort2[4] = {{17.5, 74.7}, {22, 72.9}, {24.3, 37.6}, {18.9, 41.8}};

bool InPolygon(sPoint *iPoly, uint8_t iLen, double iX, double iY)
{

    int j = iLen - 1;
    bool lResult = false;
    for (int i = 0; i < iLen; i++)
    {
        if (((iPoly[i].y > iY) != (iPoly[j].y > iY)) && (iX < (iPoly[j].x - iPoly[i].x) * (iY - iPoly[i].y) / (iPoly[j].y - iPoly[i].y) + iPoly[i].x))
        {
            lResult = !lResult;
        }
        j = i;
    }
    return lResult;
}

// Dewpoint is a vitual sensor and might be implemented on sensor class level, but we implement it here (easier and shorter)
bool CalculateDewValue(MeasureType iMeasureType, double& eValue) {
    double lTemp = knx.getGroupObject(LOG_KoTemp).value(getDPT(VAL_DPT_9)); //gRuntimeData.temp.currentValue;
    double lHum = knx.getGroupObject(LOG_KoHum).value(getDPT(VAL_DPT_9));   //gRuntimeData.hum.currentValue;
    double lLogHum = log(lHum / 100.0);
    eValue = 243.12 * ((17.62 * lTemp) / (243.12 + lTemp) + lLogHum) / ((17.62 * 243.12) / (243.12 + lTemp) - lLogHum);
    return true;
}

void CalculateComfort(bool iForce = false)
{
    static uint32_t sMillis = 0;
    bool lSend = iForce;
    if (iForce || millis() - sMillis > 1000)
    {
        sMillis = millis();
        // do not calculate if underlying measures are corrupt
        if (getError() & (Temperature | Humidity)) return;

        double lTemp = knx.getGroupObject(LOG_KoTemp).value(getDPT(VAL_DPT_9)); //gRuntimeData.temp.currentValue;
        double lHum = knx.getGroupObject(LOG_KoHum).value(getDPT(VAL_DPT_9));   //gRuntimeData.hum.currentValue;
        if (knx.paramByte(LOG_Comfort) & 32)
        {
            // comfortzone
            uint8_t lComfort = 0;
            if (InPolygon(comfort2, 4, lTemp, lHum))
            {
                lComfort = 2;
            }
            else if (InPolygon(comfort1, 8, lTemp, lHum))
            {
                lComfort = 1;
            }
            if ((uint8_t)knx.getGroupObject(LOG_KoComfort).value(getDPT(VAL_DPT_5)) != lComfort)
                lSend = true;
            if (lSend)
                knx.getGroupObject(LOG_KoComfort).value(lComfort, getDPT(VAL_DPT_5));
        }
    }
}

void CalculateAccuracy(bool iForce = false)
{
    static uint32_t sMillis = 0;
    bool lSend = iForce;
    if (iForce || delayCheck(sMillis, 60000))
    {
        sMillis = millis();
        // do not calculate if underlying measures are corrupt
        if (getError() & Accuracy) return;

        if (knx.paramByte(LOG_Accuracy) & 8)
        {
            // get accuracy
            double lAccuracyMeasure;
            bool lSuccess = Sensor::measureValue(Accuracy, lAccuracyMeasure);
            if (lSuccess) {
                uint8_t lAccuracy = (uint8_t)lAccuracyMeasure;
                uint8_t lOldAccuracy = (uint8_t)knx.getGroupObject(LOG_KoSensorAccuracy).value(getDPT(VAL_DPT_5001));
                if (lOldAccuracy != lAccuracy)
                    lSend = true;
                if (lSend)
                    knx.getGroupObject(LOG_KoSensorAccuracy).value(lAccuracy, getDPT(VAL_DPT_5001));
            }
        }
    }
}

uint8_t getAirquality(double iCurrent, double* iLimits) {
    uint8_t lResult = 6;
    for (uint8_t i = 0; i < 5; i++)
    {
        if (iCurrent < iLimits[i]) {
            lResult = i + 1;
            break;
        }
    }
    return lResult;
}

void CalculateAirquality(bool iForce = false)
{
    static uint32_t sMillis = 0;
    static double sVocLimits[5] = {51,101,151,201,301};
    static double sCo2Limits[5] = {401,701,1001,1401,2001};

    bool lSend = iForce;
    if (iForce || delayCheck(sMillis, 5000))
    {
        sMillis = millis();
        // do not calculate if underlying measures are corrupt
        if (getError() & (Voc | Co2)) return;

        if (knx.paramByte(LOG_Airquality) & 16)
        {
            // get airquality
            uint8_t lAirquality = 6;
            if (gSensor & BIT_Voc) {
                double lVoc = knx.getGroupObject(LOG_KoVOC).value(getDPT(VAL_DPT_9));
                lAirquality = getAirquality(lVoc, sVocLimits);
            } else if (gSensor & BIT_Co2) {
                double lCo2 = knx.getGroupObject(LOG_KoCo2).value(getDPT(VAL_DPT_9));
                lAirquality = getAirquality(lCo2, sCo2Limits);
            }
            if ((uint8_t)knx.getGroupObject(LOG_KoAirquality).value(getDPT(VAL_DPT_5)) != lAirquality)
                lSend = true;
            if (lSend)
                knx.getGroupObject(LOG_KoAirquality).value(lAirquality, getDPT(VAL_DPT_5));
        }
    }
}

// true solgange der Start des gesamten Moduls verzögert werden soll
bool startupDelay()
{
    return !delayCheck(gRuntimeData.startupDelay, knx.paramInt(LOG_StartupDelay) * 1000);
}

void ProcessSensors(bool iForce = false)
{
    if (gSensor & BIT_Temp)
        ProcessSensor(&gRuntimeData.temp, ReadSensorValue, Temperature, 10.0, 1.0, LOG_TempOffset, LOG_KoTemp, iForce);
    if (gSensor & BIT_Hum)
        ProcessSensor(&gRuntimeData.hum, ReadSensorValue, Humidity, 1.0, 1.0, LOG_HumOffset, LOG_KoHum, iForce);
    if (gSensor & BIT_Pre)
        ProcessSensor(&gRuntimeData.pre, ReadSensorValue, Pressure, 1.0, 1.0, LOG_PreOffset, LOG_KoPre, iForce);
    if (gSensor & BIT_Voc)
        ProcessSensor(&gRuntimeData.voc, ReadSensorValue, Voc, 1.0, 1.0, LOG_VocOffset, LOG_KoVOC, iForce);
    if (gSensor & BIT_Co2)
        ProcessSensor(&gRuntimeData.co2, ReadSensorValue, Co2, 1.0, 1.0, LOG_Co2Offset, LOG_KoCo2, iForce);
    if (gSensor & BIT_RESERVE)
        ProcessSensor(&gRuntimeData.co2b, ReadSensorValue, Reserved, 1.0, 1.0, LOG_Co2Offset, LOG_KoCo2b, iForce);

    if ((gSensor & (BIT_Temp | BIT_Hum)) == (BIT_Temp | BIT_Hum)) {
        ProcessSensor(&gRuntimeData.dew, CalculateDewValue, static_cast<MeasureType>(Temperature | Humidity), 10.0, 1.0, LOG_DewOffset, LOG_KoDewpoint, iForce);
        CalculateComfort(iForce);
    };
    if (gSensor & (BIT_Voc | BIT_Co2)) CalculateAirquality(iForce);
    if (gSensor & BIT_Voc) CalculateAccuracy(iForce);
    
    // error processing
    uint8_t lError = Sensor::getError();
    if (lError != getError()) {
        setError(lError);
        if (knx.paramByte(LOG_Error) & 128)
            sendError();
    }
}

void ProcessKoCallback(GroupObject &iKo)
{
    // check if we evaluate own KO
    if (iKo.asap() == LOG_KoRequestValues) {
        println("Request values called");
        print("KO-Value is ");
        println((bool)iKo.value(getDPT(VAL_DPT_1)));
        if ((bool)iKo.value(getDPT(VAL_DPT_1)))
            ProcessSensors(true);
    } else if (iKo.asap() >= LOG_KoExt1Temp && iKo.asap() <= LOG_KoExt2Co2) {
        // as soon as we receive any external sensor value, we mark this in our validity map
        gIsExternalValueValid[iKo.asap() - LOG_KoExt1Temp] = 1;
    } else {
        // else dispatch to logicmodule
        processInputKo(iKo);
    }
}

void ProcessInterrupt() {
    if (gRuntimeData.saveInterruptTimestamp) {
        printf("Sensormodul: SAVE-Interrupt processing started after %lu ms", millis() - gRuntimeData.saveInterruptTimestamp);
        gRuntimeData.saveInterruptTimestamp = millis();
        // for the moment, we send only en Info on error object in case of an save interrumpt
        uint16_t lError = getError();
        setError(lError | 128);
        sendError();
        // switch off all energy intensive hardware to gain time for EEPROM write
        savePower();
        // call according logic interrupt handler
        logicProcessInterrupt(true);
        Sensor::saveState();
        // in case, SaveInterrupt was a false positive
        Wire.end();
        // wait 1 Second in interrupt handler
        delay(1000);
        restorePower();
        delay(100);
        Wire.begin();
        // Sensor::restartSensors();
        printf("Sensormodul: SAVE-Interrupt processing duration was %lu ms", millis() - gRuntimeData.saveInterruptTimestamp);
        gRuntimeData.saveInterruptTimestamp = 0;
    }
}

void appLoop()
{
    static bool sForceAtStartup = true;

    if (!knx.configured())
        return;

    // handle KNX stuff
    if (startupDelay())
        return;

    ProcessInterrupt();

    // at this point startup-delay is done
    // we process heartbeat
    ProcessHeartbeat();
    if (sForceAtStartup) ProcessReadRequests();
    if (gSensor & BIT_LOGIC)
        logikLoop();

    // at Startup, we want to send all values immediately
    ProcessSensors(sForceAtStartup);
    sForceAtStartup = false;

    Sensor::sensorLoop();
}

// handle interrupt from save pin
void onSafePinInterruptHandler() {
    gRuntimeData.countSaveInterrupt += 1;
    gRuntimeData.saveInterruptTimestamp = millis();;
}

void beforeRestartHandler() {
    printf("before Restart called");
    Sensor::saveState();
    logicBeforeRestartHandler();
    // we try get a clean state on I2C bus
    Wire.end();
}

void beforeTableUnloadHandler(TableObject& iTableObject, LoadState& iNewState) {
    static uint32_t sLastCalled = 0;
    printf("Table changed called with state %d", iNewState);
    
    if (iNewState == 0) {
        printf("Table unload called");
        if (sLastCalled == 0 || delayCheck(sLastCalled, 10000)) {
            Sensor::saveState();
            logicBeforeTableUnloadHandler(iTableObject, iNewState);
            sLastCalled = millis();
        }
    }
}


void appSetup(uint8_t iBuzzerPin, uint8_t iSavePin)
{

    gSensor = (knx.paramByte(LOG_SensorDevice));

    if (knx.configured())
    {
        // check hardware availability
        boardCheck();
        // try to get rid of occasional I2C lock...
        savePower();
        delay(100);
        restorePower();
        gRuntimeData.startupDelay = millis();
        gRuntimeData.heartbeatDelay = 0;
        gRuntimeData.countSaveInterrupt = 0;
        // GroupObject &lKoRequestValues = knx.getGroupObject(LOG_KoRequestValues);
        if (GroupObject::classCallback() == 0) GroupObject::classCallback(ProcessKoCallback);
        if (knx.getBeforeRestartCallback() == 0) knx.addBeforeRestartCallback(beforeRestartHandler);
        if (TableObject::getBeforeTableUnloadCallback() == 0) TableObject::addBeforeTableUnloadCallback(beforeTableUnloadHandler);
        StartSensor();
        if (gSensor & BIT_LOGIC) {
            if (iSavePin) {
                logicAttachSaveInterrupt(digitalPinToInterrupt(iSavePin), onSafePinInterruptHandler, FALLING);
            }
            logikSetup(iBuzzerPin, false);
        }
    }
}