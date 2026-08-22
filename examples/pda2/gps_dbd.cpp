/**
 * @file      gps_dbd.cpp
 * @brief     UBX-MGA-DBD navigation database save/restore. See gps_dbd.h.
 */
#include "gps_dbd.h"

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>
#include <time.h>

#include "utilities.h"
#include "peripheral.h"

/* UBX-MGA-DBD is class 0x13, id 0x80. Polling it (a zero-length message of
 * the same class/id) makes the receiver stream the database back as a run of
 * MGA-DBD messages, terminated by an MGA-ACK. */
#define UBX_CLS_MGA     0x13
#define UBX_ID_DBD      0x80
#define UBX_ID_MGA_ACK  0x60

/* A GPS+GLONASS dump runs to a few kilobytes. The cap is a backstop against a
 * receiver that never stops talking, not a real expectation. */
#define DBD_MAX_BYTES   (12 * 1024)

/* Ephemeris goes stale in hours and almanac in weeks, but the receiver checks
 * validity itself — this only avoids spending time pushing bytes that cannot
 * possibly help any more. */
#define DBD_MAX_AGE_S   (30L * 24 * 3600)

#define NVS_NS   "gpsdbd"
#define NVS_BLOB "db"
#define NVS_TIME "ts"

/* ---- UBX frame reader ----
 *
 * Deliberately hand-rolled rather than reusing a parser: we need the frames
 * back byte-for-byte to hand them to the receiver again, not decoded. */

static uint8_t ck_a, ck_b;
static void ck_reset(void) { ck_a = ck_b = 0; }
static void ck_add(uint8_t c) { ck_a = (uint8_t)(ck_a + c); ck_b = (uint8_t)(ck_b + ck_a); }

/* Collect UBX frames until the terminating MGA-ACK, the buffer fills, or the
 * receiver goes quiet. Only frames of class/id cls/id are kept; anything else
 * on the wire (NMEA, other UBX) is skipped. Returns bytes written. */
static size_t collect_frames(uint8_t *out, size_t cap, uint32_t quiet_ms, uint32_t total_ms)
{
    size_t   used = 0;
    uint32_t t0 = millis(), last = millis();
    int      state = 0;
    uint16_t len = 0, got = 0;
    uint8_t  cls = 0, id = 0;
    uint8_t  frame[512];
    size_t   flen = 0;

    while (millis() - t0 < total_ms && millis() - last < quiet_ms) {
        if (!SerialGPS.available()) { delay(2); continue; }
        int ci = SerialGPS.read();
        if (ci < 0) continue;
        uint8_t c = (uint8_t)ci;
        last = millis();

        switch (state) {
        case 0: if (c == 0xB5) { state = 1; } break;
        case 1: state = (c == 0x62) ? 2 : 0; break;
        case 2: cls = c; ck_reset(); ck_add(c); state = 3; break;
        case 3: id = c;  ck_add(c); state = 4; break;
        case 4: len = c; ck_add(c); state = 5; break;
        case 5:
            len |= (uint16_t)c << 8;
            ck_add(c);
            got = 0;
            flen = 0;
            if (len > sizeof(frame) - 8) { state = 0; break; }   /* not ours */
            frame[flen++] = 0xB5; frame[flen++] = 0x62;
            frame[flen++] = cls;  frame[flen++] = id;
            frame[flen++] = (uint8_t)(len & 0xFF);
            frame[flen++] = (uint8_t)(len >> 8);
            state = (len == 0) ? 7 : 6;
            break;
        case 6:
            ck_add(c);
            frame[flen++] = c;
            if (++got >= len) state = 7;
            break;
        case 7:                          /* checksum A */
            if (c != ck_a) { state = 0; break; }
            frame[flen++] = c;
            state = 8;
            break;
        case 8:                          /* checksum B */
            state = 0;
            if (c != ck_b) break;
            frame[flen++] = c;

            if (cls == UBX_CLS_MGA && id == UBX_ID_MGA_ACK) return used;  /* done */
            if (cls == UBX_CLS_MGA && id == UBX_ID_DBD) {
                if (used + flen > cap) return used;                       /* full */
                memcpy(out + used, frame, flen);
                used += flen;
            }
            break;
        }
    }
    return used;
}

static void send_poll(void)
{
    /* B5 62 13 80 00 00 <ck> — zero-length message of the class/id polls it. */
    uint8_t poll[8] = {0xB5, 0x62, UBX_CLS_MGA, UBX_ID_DBD, 0x00, 0x00, 0x00, 0x00};
    ck_reset();
    for (int i = 2; i < 6; i++) ck_add(poll[i]);
    poll[6] = ck_a;
    poll[7] = ck_b;

    while (SerialGPS.available()) SerialGPS.read();   /* drop pending NMEA */
    SerialGPS.write(poll, sizeof(poll));
    SerialGPS.flush();
}

bool gps_dbd_save(void)
{
    uint8_t *buf = (uint8_t *)ps_malloc(DBD_MAX_BYTES);
    if (!buf) {
        Serial.println("[DBD] no PSRAM for dump buffer");
        return false;
    }

    send_poll();
    size_t n = collect_frames(buf, DBD_MAX_BYTES, 400, 4000);

    if (n == 0) {
        Serial.println("[DBD] receiver returned no database");
        free(buf);
        return false;
    }

    time_t now;
    time(&now);

    Preferences p;
    p.begin(NVS_NS, false);
    size_t w = p.putBytes(NVS_BLOB, buf, n);
    p.putULong(NVS_TIME, (uint32_t)now);
    p.end();
    free(buf);

    Serial.printf("[DBD] saved %u bytes of nav database%s\n",
                  (unsigned)w, w == n ? "" : " (SHORT WRITE)");
    return w == n;
}

long gps_dbd_age_s(void)
{
    Preferences p;
    p.begin(NVS_NS, true);
    size_t   n  = p.getBytesLength(NVS_BLOB);
    uint32_t ts = p.getULong(NVS_TIME, 0);
    p.end();
    if (n == 0 || ts == 0) return -1;

    time_t now;
    time(&now);
    /* The clock is only trustworthy once it has been set; before that we
     * cannot date the dump, so treat it as fresh and let the receiver judge. */
    if ((long)now < (long)ts) return 0;
    return (long)now - (long)ts;
}

bool gps_dbd_restore(void)
{
    Preferences p;
    p.begin(NVS_NS, true);
    size_t n = p.getBytesLength(NVS_BLOB);
    p.end();

    if (n == 0) {
        Serial.println("[DBD] nothing stored yet (first run after a power cut)");
        return false;
    }

    long age = gps_dbd_age_s();
    if (age > DBD_MAX_AGE_S) {
        Serial.printf("[DBD] stored dump is %ld days old, skipping\n", age / 86400);
        return false;
    }

    uint8_t *buf = (uint8_t *)ps_malloc(n);
    if (!buf) {
        Serial.println("[DBD] no PSRAM to load dump");
        return false;
    }

    p.begin(NVS_NS, true);
    size_t got = p.getBytes(NVS_BLOB, buf, n);
    p.end();

    if (got != n) {
        Serial.println("[DBD] short read from NVS");
        free(buf);
        return false;
    }

    /* Push the frames back one at a time, pacing them so the receiver's input
     * buffer keeps up — it has to validate and file each record. */
    size_t sent = 0, i = 0, frames = 0;
    while (i + 8 <= n) {
        if (buf[i] != 0xB5 || buf[i + 1] != 0x62) break;      /* corrupt */
        uint16_t len = (uint16_t)buf[i + 4] | ((uint16_t)buf[i + 5] << 8);
        size_t   flen = 8 + len;
        if (i + flen > n) break;

        SerialGPS.write(buf + i, flen);
        SerialGPS.flush();
        delay(5);
        sent += flen;
        frames++;
        i += flen;
    }
    free(buf);

    Serial.printf("[DBD] restored %u bytes in %u frames (age %ld min)\n",
                  (unsigned)sent, (unsigned)frames, age < 0 ? 0 : age / 60);
    return frames > 0;
}
