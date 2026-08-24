/**
 * @file      lowpower_mgr.h
 * @brief     Idle low-power mode — quiet, but always awake.
 *
 * After a period with no key or touch activity the device throttles back:
 * the CPU drops to a lower clock, lingering accessories (GPS in particular)
 * are switched off, and the main loop idles more slowly. The panel keeps its
 * image — e-ink needs no power to hold one — with a small indicator in the
 * top-right corner. Any keypress or touch restores full speed.
 *
 * This deliberately does NOT use esp_light_sleep_start(). Light sleep stops
 * the CPU entirely, and on this board it could not be woken again from the
 * keypad/touch interrupt lines — the device had to be reset. Staying in the
 * normal loop at a reduced clock keeps the keyboard polled and responsive,
 * which is worth far more than the extra microamps.
 */
#ifndef __LOWPOWER_MGR_H__
#define __LOWPOWER_MGR_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Idle timeout before throttling back. */
#define LOWPOWER_DEFAULT_TIMEOUT_MS (5u * 60u * 1000u)

/* CPU clocks. Idling parks every radio (power_suspend_all), so the 80 MHz
 * floor that WiFi would otherwise impose does not apply and we can drop
 * further. Raise LOWPOWER_IDLE_MHZ back to 80 if USB CDC misbehaves at 40. */
#define LOWPOWER_ACTIVE_MHZ 240
#define LOWPOWER_IDLE_MHZ   40
/* Floor while a radio an app holds is still powered; WiFi will not run below. */
#define LOWPOWER_RADIO_MHZ  80

void lowpower_init(void);
void lowpower_poll(void);            /* call from loop() */
void lowpower_note_activity(void);   /* key / touch */

/* True while throttled — loop() idles longer in this state. */
bool lowpower_is_idle(void);

/* 0 disables idling entirely. */
void     lowpower_set_timeout(uint32_t ms);
uint32_t lowpower_get_timeout(void);

/* Hold full power while something long-running is in progress. Reference
 * counted; every inhibit needs a matching allow. */
void lowpower_inhibit(void);
void lowpower_allow(void);

#ifdef __cplusplus
}
#endif

#endif /* __LOWPOWER_MGR_H__ */
