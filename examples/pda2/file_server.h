/**
 * @file      file_server.h
 * @brief     HTTP file server for browsing / downloading / uploading SD content.
 *
 * Uses the built-in synchronous Arduino WebServer. handleClient() must be pumped
 * from the main loop via file_server_loop(); all SD access happens on that single
 * thread under the shared SPI lock, so it never races the display flush task.
 */
#ifndef __FILE_SERVER_H__
#define __FILE_SERVER_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Start the server on port 80. Requires WiFi (STA) to be connected; returns
 * false if it is not. Safe to call when already running (returns true). */
bool file_server_start(void);

/* Stop the server and free the listening socket. */
void file_server_stop(void);

/* True while the server is accepting connections. */
bool file_server_is_running(void);

/* Pump one round of client handling — call this from loop(). No-op when stopped. */
void file_server_loop(void);

/* "http://<ip>/" while running, or "" when stopped. */
const char *file_server_url(void);

#ifdef __cplusplus
}
#endif

#endif /* __FILE_SERVER_H__ */
