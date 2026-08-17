/**
 * @file      file_server.cpp
 * @brief     Synchronous HTTP file server over the SD card.
 *
 * Endpoints:
 *   GET  /                 -> redirect to /list?path=/
 *   GET  /list?path=/dir   -> HTML directory listing (browse)
 *   GET  /download?path=f  -> stream a file as an attachment
 *   GET  /delete?path=f    -> delete a file (or empty dir), redirect back
 *   GET  /mkdir?path=d&name=n -> create a folder, redirect back
 *   POST /upload?path=/dir -> multipart upload into that directory
 *
 * Every SD operation is wrapped in shared_spi_lock()/shared_spi_prepare_device()
 * so it can't collide with the display flush on the shared SPI bus. handleClient()
 * is single-threaded (pumped from loop()), so the recursive lock is only ever held
 * within one request at a time.
 */
#include "file_server.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SD.h>
#include "utilities.h"      /* BOARD_SD_CS */

extern void shared_spi_lock(void);
extern void shared_spi_unlock(void);
extern void shared_spi_prepare_device(int cs_pin);

static WebServer s_server(80);
static bool s_running = false;
static char s_url[48] = "";

/* Upload state (valid only across one POST /upload request). */
static File   s_upload_file;
static String s_upload_dir;

/* ------------------------------------------------------------------ helpers */

static String html_escape(const String &in)
{
    String out;
    out.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); i++) {
        char c = in[i];
        switch (c) {
            case '&': out += "&amp;";  break;
            case '<': out += "&lt;";   break;
            case '>': out += "&gt;";   break;
            case '"': out += "&quot;"; break;
            case '\'':out += "&#39;";  break;
            default:  out += c;        break;
        }
    }
    return out;
}

static String url_encode(const String &in)
{
    static const char *hex = "0123456789ABCDEF";
    String out;
    out.reserve(in.length() * 3);
    for (size_t i = 0; i < in.length(); i++) {
        char c = in[i];
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' ||
            c == '~' || c == '/') {
            out += c;
        } else {
            out += '%';
            out += hex[(c >> 4) & 0xF];
            out += hex[c & 0xF];
        }
    }
    return out;
}

/* basename of a '/'-separated path. */
static String base_name(const String &path)
{
    int slash = path.lastIndexOf('/');
    return (slash < 0) ? path : path.substring(slash + 1);
}

/* parent directory of a path ("/a/b.txt" -> "/a", "/a" -> "/"). */
static String parent_dir(const String &path)
{
    int slash = path.lastIndexOf('/');
    if (slash <= 0) return "/";
    return path.substring(0, slash);
}

/* Join a directory and a leaf into a clean absolute path. */
static String path_join(const String &dir, const String &leaf)
{
    if (dir.length() == 0 || dir == "/") return "/" + leaf;
    if (dir.endsWith("/")) return dir + leaf;
    return dir + "/" + leaf;
}

static String human_size(uint32_t bytes)
{
    char buf[24];
    if (bytes < 1024)
        snprintf(buf, sizeof(buf), "%u B", bytes);
    else if (bytes < 1024UL * 1024)
        snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    else
        snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
    return String(buf);
}

static String arg_path(void)
{
    String p = s_server.hasArg("path") ? s_server.arg("path") : String("/");
    if (p.length() == 0) p = "/";
    if (p[0] != '/') p = "/" + p;
    /* strip a trailing slash except for root */
    while (p.length() > 1 && p.endsWith("/")) p.remove(p.length() - 1);
    return p;
}

static void redirect_to(const String &path)
{
    s_server.sendHeader("Location", "/list?path=" + url_encode(path));
    s_server.send(303, "text/plain", "");
}

/* ----------------------------------------------------------------- handlers */

static void handle_list(void)
{
    String path = arg_path();

    String page;
    page.reserve(4096);
    page += F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
              "<meta name='viewport' content='width=device-width,initial-scale=1'>"
              "<title>T-Deck-Pro Files</title><style>"
              "body{font-family:sans-serif;margin:14px;color:#222;}"
              "h2{font-size:17px;word-break:break-all;}"
              "table{border-collapse:collapse;width:100%;font-size:15px;}"
              "td,th{padding:7px 8px;border-bottom:1px solid #ddd;text-align:left;}"
              ".r{text-align:right;white-space:nowrap;}"
              "a{text-decoration:none;color:#06c;}"
              "form{margin:10px 0;}"
              ".del{color:#c00;}"
              "</style></head><body>");
    page += "<h2>SD: " + html_escape(path) + "</h2>";

    page += "<form method='POST' action='/upload?path=" + url_encode(path) +
            "' enctype='multipart/form-data'>"
            "<input type='file' name='f' required> "
            "<input type='submit' value='Upload'></form>";
    page += "<form method='GET' action='/mkdir'>"
            "<input type='hidden' name='path' value='" + html_escape(path) + "'>"
            "<input name='name' placeholder='new folder' required> "
            "<input type='submit' value='Create folder'></form>";

    page += F("<table><tr><th>Name</th><th class='r'>Size</th><th class='r'></th></tr>");

    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    File dir = SD.open(path);
    bool is_dir = dir && dir.isDirectory();

    if (!is_dir) {
        if (dir) dir.close();
        shared_spi_unlock();
        s_server.send(404, "text/plain", "Not a directory: " + path);
        return;
    }

    if (path != "/") {
        String up = parent_dir(path);
        page += "<tr><td><a href='/list?path=" + url_encode(up) +
                "'>&#128193; ..</a></td><td class='r'></td><td></td></tr>";
    }

    File e = dir.openNextFile();
    while (e) {
        String name = base_name(String(e.name()));
        String child = path_join(path, name);
        bool ed = e.isDirectory();
        uint32_t sz = ed ? 0 : (uint32_t)e.size();
        e.close();

        page += "<tr><td>";
        if (ed) {
            page += "<a href='/list?path=" + url_encode(child) + "'>&#128193; " +
                    html_escape(name) + "/</a>";
        } else {
            page += "<a href='/download?path=" + url_encode(child) + "'>&#128196; " +
                    html_escape(name) + "</a>";
        }
        page += "</td><td class='r'>" + (ed ? String("") : human_size(sz)) + "</td>";
        page += "<td class='r'><a class='del' href='/delete?path=" + url_encode(child) +
                "' onclick=\"return confirm('Delete " + html_escape(name) + "?')\">delete</a></td></tr>";

        e = dir.openNextFile();
    }
    dir.close();
    shared_spi_unlock();

    page += F("</table></body></html>");
    s_server.send(200, "text/html", page);
}

static void handle_download(void)
{
    if (!s_server.hasArg("path")) { s_server.send(400, "text/plain", "missing path"); return; }
    String path = s_server.arg("path");

    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    File f = SD.open(path, FILE_READ);
    if (!f || f.isDirectory()) {
        if (f) f.close();
        shared_spi_unlock();
        s_server.send(404, "text/plain", "Not found: " + path);
        return;
    }

    s_server.sendHeader("Content-Disposition",
                        "attachment; filename=\"" + base_name(path) + "\"");
    s_server.streamFile(f, "application/octet-stream");
    f.close();
    shared_spi_unlock();
}

static void handle_delete(void)
{
    if (!s_server.hasArg("path")) { s_server.send(400, "text/plain", "missing path"); return; }
    String path = s_server.arg("path");
    String parent = parent_dir(path);

    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    File f = SD.open(path);
    bool is_dir = f && f.isDirectory();
    if (f) f.close();
    bool ok = is_dir ? SD.rmdir(path) : SD.remove(path);
    shared_spi_unlock();

    Serial.printf("[FILESRV] delete %s -> %s\n", path.c_str(), ok ? "ok" : "FAILED");
    redirect_to(parent);
}

static void handle_mkdir(void)
{
    String path = arg_path();
    String name = s_server.hasArg("name") ? s_server.arg("name") : String("");
    name.trim();
    if (name.length() == 0) { redirect_to(path); return; }
    /* keep it a single-level leaf name */
    name = base_name(name);
    String full = path_join(path, name);

    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    bool ok = SD.mkdir(full);
    shared_spi_unlock();

    Serial.printf("[FILESRV] mkdir %s -> %s\n", full.c_str(), ok ? "ok" : "FAILED");
    redirect_to(path);
}

static void handle_upload(void)
{
    HTTPUpload &up = s_server.upload();

    if (up.status == UPLOAD_FILE_START) {
        s_upload_dir = arg_path();
        String full = path_join(s_upload_dir, base_name(String(up.filename)));
        shared_spi_lock();
        shared_spi_prepare_device(BOARD_SD_CS);
        s_upload_file = SD.open(full, FILE_WRITE);
        Serial.printf("[FILESRV] upload start %s (%s)\n", full.c_str(),
                      s_upload_file ? "opened" : "OPEN FAILED");
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (s_upload_file) s_upload_file.write(up.buf, up.currentSize);
    } else if (up.status == UPLOAD_FILE_END || up.status == UPLOAD_FILE_ABORTED) {
        if (s_upload_file) {
            s_upload_file.close();
            Serial.printf("[FILESRV] upload %s: %u bytes\n",
                          up.status == UPLOAD_FILE_END ? "done" : "aborted",
                          (unsigned)up.totalSize);
        }
        /* balance the lock taken in UPLOAD_FILE_START */
        shared_spi_unlock();
    }
}

static void handle_upload_done(void)
{
    redirect_to(s_upload_dir.length() ? s_upload_dir : String("/"));
}

/* -------------------------------------------------------------------- public */

static bool s_routes_installed = false;

static void install_routes(void)
{
    if (s_routes_installed) return;
    s_server.on("/", HTTP_GET, [](){ redirect_to("/"); });
    s_server.on("/list", HTTP_GET, handle_list);
    s_server.on("/download", HTTP_GET, handle_download);
    s_server.on("/delete", HTTP_GET, handle_delete);
    s_server.on("/mkdir", HTTP_GET, handle_mkdir);
    s_server.on("/upload", HTTP_POST, handle_upload_done, handle_upload);
    s_server.onNotFound(handle_list);
    s_routes_installed = true;
}

bool file_server_start(void)
{
    if (s_running) return true;
    if (WiFi.status() != WL_CONNECTED) return false;

    install_routes();
    s_server.begin();
    s_running = true;
    snprintf(s_url, sizeof(s_url), "http://%s/", WiFi.localIP().toString().c_str());
    Serial.printf("[FILESRV] started at %s\n", s_url);
    return true;
}

void file_server_stop(void)
{
    if (!s_running) return;
    s_server.stop();
    s_running = false;
    s_url[0] = '\0';
    Serial.println("[FILESRV] stopped");
}

bool file_server_is_running(void)
{
    return s_running;
}

void file_server_loop(void)
{
    if (s_running) s_server.handleClient();
}

const char *file_server_url(void)
{
    return s_url;
}
