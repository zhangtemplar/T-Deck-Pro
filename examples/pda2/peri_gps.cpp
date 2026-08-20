
#include "utilities.h"
#include "peripheral.h"
#include <TinyGPS++.h>

/* clang-format off */

TinyGPSPlus gps;
static bool GPS_Recovery();
bool setupGPS();
void displayInfo();

static TaskHandle_t gps_handle;
static double gps_lat=0, gps_lng=0, gps_altitude=0, gps_speed=0;
static uint16_t gps_year=0;
static uint8_t gps_month=0, gps_day=0;
static uint8_t gps_hour=0, gps_minute=0, gps_second=0;
static uint32_t gps_vsat=0;

uint8_t buffer[256];

/* Negotiate the UART link and put the receiver into a known configuration.
 *
 * This has to run every time the module's supply is switched on, not just at
 * boot: a power cycle returns the receiver to its power-up defaults, so the
 * baud rate it talks at and the UBX settings applied here are both lost. Skip
 * it and the parser task sees nothing usable and the app reports no satellites
 * indefinitely — which looks exactly like a very slow fix. */
static bool gps_link_setup(void)
{
    bool result = false;
    // L76K GPS USE 9600 BAUDRATE
    // result = setupGPS();
    if(!result) {
        // Set u-blox m10q gps baudrate 38400
        SerialGPS.begin(38400, SERIAL_8N1, BOARD_GPS_RXD, BOARD_GPS_TXD);
        result = GPS_Recovery();
        if (!result) {
            SerialGPS.updateBaudRate(9600);
            result = GPS_Recovery();
            if (!result) {
                Serial.println("GPS Connect failed~!");
                result = false;
            }
            SerialGPS.updateBaudRate(38400);
        }
    }
    return result;
}

bool gps_init(void)
{   
    bool result = gps_link_setup();
    if(result) {
        Serial.println("GPS Task Create...!");
        gps_task_create();
    }
    return result;
}

/* Is the receiver already streaming NMEA at the expected rate? */
static bool gps_link_probe(uint32_t ms)
{
    SerialGPS.begin(38400, SERIAL_8N1, BOARD_GPS_RXD, BOARD_GPS_TXD);
    uint32_t t0 = millis();
    int starts = 0;
    while (millis() - t0 < ms) {
        while (SerialGPS.available()) {
            if (SerialGPS.read() == '$' && ++starts >= 2) return true;
        }
        delay(10);
    }
    return false;
}

/* Re-run the link setup after the receiver has been power cycled.
 *
 * Probe before negotiating. gps_link_setup() sends UBX-CFG-CFG clear/load,
 * which resets the receiver's configuration to defaults, and its four ack
 * waits cost up to ~3 s per baud attempt. Doing that on every power-on is both
 * slow and needlessly destructive when the receiver is already talking. */
bool gps_reinit(void)
{
    if (gps_link_probe(1500)) {
        Serial.println("[GPS] link alive at 38400, no re-negotiation needed");
        return true;
    }
    bool ok = gps_link_setup();
    Serial.printf("[GPS] re-init after power-on: %s\n", ok ? "ok" : "FAILED");
    return ok;
}

void gps_task(void *param)
{
    vTaskSuspend(gps_handle);
    while(1)
    {
        while (Serial.available()) {
            SerialGPS.write(Serial.read());
        }

        while (SerialGPS.available()) {
            int c = SerialGPS.read();
            // Serial.write(c);
            if (gps.encode(c)) {
                displayInfo();
            }
        }

        if (millis() > 30000 && gps.charsProcessed() < 10) {
            Serial.println(F("No GPS detected: check wiring."));
            delay(1000);
        }
        delay(1);
    }
}

void gps_task_create(void)
{
    xTaskCreate(gps_task, "gps_task", 1024 * 3, NULL, GPS_PRIORITY, &gps_handle);
    // vTaskSuspend(gps_handle);
}

void gps_task_suspend(void)
{
    vTaskSuspend(gps_handle);
}

void gps_task_resume(void)
{
    vTaskResume(gps_handle);
}

void gps_get_coord(double *lat, double *lng)
{
    *lat = gps_lat;
    *lng = gps_lng;
}

void gps_get_data(uint16_t *year, uint8_t *month, uint8_t *day)
{
    *year = gps_year;
    *month = gps_month;
    *day = gps_day;
}

void gps_get_time(uint8_t *hour, uint8_t *minute, uint8_t *second)
{
    *hour = gps_hour;
    *minute = gps_minute;
    *second = gps_second;
}

void gps_get_satellites(uint32_t *vsat)
{
    *vsat = gps_vsat;   // Visible Satellites
}

/* Characters the parser has consumed from the receiver. Zero means the UART
 * link is dead (wrong baud, unpowered, miswired) — a genuine cold start still
 * shows this climbing while the fix count stays at zero. */
uint32_t gps_chars_processed(void)
{
    return (uint32_t)gps.charsProcessed();
}

void gps_get_speed(double *speed)
{
    *speed = gps_speed;
}

/* clang-format on */
/* Cache the latest values, and log only sparingly.
 *
 * This used to print every field on every parsed sentence — dozens of
 * Serial.print calls several times a second. That was tolerable when the task
 * only ran inside the GPS screen, but the receiver is now also powered during
 * the boot time-sync window and by the weather app, so it drowned out the log
 * of whatever app was actually in use. At 115200 baud it also costs real time
 * in a task that runs continuously. Report fix transitions, then throttle. */
#define GPS_LOG_INTERVAL_MS 30000

void displayInfo()
{
    static bool last_valid = false;
    static uint32_t last_log = 0;

    bool valid = gps.location.isValid();

    if (valid) {
        gps_lat = gps.location.lat();
        gps_lng = gps.location.lng();
    }
    if (gps.date.isValid()) {
        gps_year  = gps.date.year();
        gps_month = gps.date.month();
        gps_day   = gps.date.day();
    }
    if (gps.time.isValid()) {
        gps_hour   = gps.time.hour();
        gps_minute = gps.time.minute();
        gps_second = gps.time.second();
    }
    if (gps.satellites.isValid()) gps_vsat  = gps.satellites.value();
    if (gps.speed.isValid())      gps_speed = gps.speed.kmph();

    uint32_t now = millis();
    bool changed = (valid != last_valid);
    if (!changed && (now - last_log) < GPS_LOG_INTERVAL_MS) return;
    last_valid = valid;
    last_log = now;

    if (valid) {
        Serial.printf("[GPS] %s %.6f,%.6f  %lu sats  %.1f km/h  %04u-%02u-%02u %02u:%02u:%02uZ\n",
                      changed ? "fix" : "...", gps_lat, gps_lng,
                      (unsigned long)gps_vsat, gps_speed,
                      gps_year, gps_month, gps_day, gps_hour, gps_minute, gps_second);
    } else {
        Serial.printf("[GPS] %s (%lu sats visible, %lu bytes from receiver)\n",
                      changed ? "fix lost" : "searching",
                      (unsigned long)gps_vsat, (unsigned long)gps.charsProcessed());
    }
}
/* clang-format off */

bool setupGPS()
{
    // L76K GPS USE 9600 BAUDRATE
    SerialGPS.begin(9600, SERIAL_8N1, BOARD_GPS_RXD, BOARD_GPS_TXD);
    bool result = false;
    uint32_t startTimeout ;
    for (int i = 0; i < 3; ++i) {
        SerialGPS.write("$PCAS03,0,0,0,0,0,0,0,0,0,0,,,0,0*02\r\n");
        delay(5);
        // Get version information
        startTimeout = millis() + 3000;
        Serial.print("Try to init L76K . Wait stop .");
        while (SerialGPS.available()) {
            Serial.print(".");
            SerialGPS.readString();
            if (millis() > startTimeout) {
                Serial.println("Wait L76K stop NMEA timeout!");
                return false;
            }
        };
        Serial.println();
        SerialGPS.flush();
        delay(200);

        SerialGPS.write("$PCAS06,0*1B\r\n");
        startTimeout = millis() + 500;
        String ver = "";
        while (!SerialGPS.available()) {
            if (millis() > startTimeout) {
                Serial.println("Get L76K timeout!");
                return false;
            }
        }
        SerialGPS.setTimeout(10);
        ver = SerialGPS.readStringUntil('\n');
        if (ver.startsWith("$GPTXT,01,01,02")) {
            Serial.println("L76K GNSS init succeeded, using L76K GNSS Module\n");
            result = true;
            break;
        }
        delay(500);
    }
    // Initialize the L76K Chip, use GPS + GLONASS
    SerialGPS.write("$PCAS04,5*1C\r\n");
    delay(250);
    SerialGPS.write("$PCAS03,1,1,1,1,1,1,1,1,1,1,,,0,0*26\r\n");
    delay(250);
    // Switch to Vehicle Mode, since SoftRF enables Aviation < 2g
    SerialGPS.write("$PCAS11,3*1E\r\n");
    return result;
}


static int getAck(uint8_t *buffer, uint16_t size, uint8_t requestedClass, uint8_t requestedID)
{
    uint16_t    ubxFrameCounter = 0;
    bool        ubxFrame = 0;
    uint32_t    startTime = millis();
    uint16_t    needRead;

    while (millis() - startTime < 800) {
        while (SerialGPS.available()) {
            int c = SerialGPS.read();
            switch (ubxFrameCounter) {
            case 0:
                if (c == 0xB5) {
                    ubxFrameCounter++;
                }
                break;
            case 1:
                if (c == 0x62) {
                    ubxFrameCounter++;
                } else {
                    ubxFrameCounter = 0;
                }
                break;
            case 2:
                if (c == requestedClass) {
                    ubxFrameCounter++;
                } else {
                    ubxFrameCounter = 0;
                }
                break;
            case 3:
                if (c == requestedID) {
                    ubxFrameCounter++;
                } else {
                    ubxFrameCounter = 0;
                }
                break;
            case 4:
                needRead = c;
                ubxFrameCounter++;
                break;
            case 5:
                needRead |=  (c << 8);
                ubxFrameCounter++;
                break;
            case 6:
                if (needRead >= size) {
                    ubxFrameCounter = 0;
                    break;
                }
                if (SerialGPS.readBytes(buffer, needRead) != needRead) {
                    ubxFrameCounter = 0;
                } else {
                    return needRead;
                }
                break;

            default:
                break;
            }
        }
    }
    return 0;
}

static bool GPS_Recovery()
{
    uint8_t cfg_clear1[] = {0xB5, 0x62, 0x06, 0x09, 0x0D, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x1C, 0xA2};
    uint8_t cfg_clear2[] = {0xB5, 0x62, 0x06, 0x09, 0x0D, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x1B, 0xA1};
    uint8_t cfg_clear3[] = {0xB5, 0x62, 0x06, 0x09, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x03, 0x1D, 0xB3};
    SerialGPS.write(cfg_clear1, sizeof(cfg_clear1));

    if (getAck(buffer, 256, 0x05, 0x01)) {
        Serial.println("Get ack successes!");
    }
    SerialGPS.write(cfg_clear2, sizeof(cfg_clear2));
    if (getAck(buffer, 256, 0x05, 0x01)) {
        Serial.println("Get ack successes!");
    }
    SerialGPS.write(cfg_clear3, sizeof(cfg_clear3));
    if (getAck(buffer, 256, 0x05, 0x01)) {
        Serial.println("Get ack successes!");
    }

    // UBX-CFG-RATE, Size 8, 'Navigation/measurement rate settings'
    uint8_t cfg_rate[] = {0xB5, 0x62, 0x06, 0x08, 0x00, 0x00, 0x0E, 0x30};
    SerialGPS.write(cfg_rate, sizeof(cfg_rate));
    if (getAck(buffer, 256, 0x06, 0x08)) {
        Serial.println("Get ack successes!");
    } else {
        return false;
    }
    return true;
}
