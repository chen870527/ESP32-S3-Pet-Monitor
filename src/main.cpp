/**
 * ESP32-S3 Pet Monitor — v2.0 System Integration & Verification Build
 * =====================================================================
 * Modules:
 *  - MJPEG Streaming: /stream  (iOS Safari compatible via HTML wrapper)
 *  - Web Dashboard:   /        (HTML page with live feed + status)
 *  - Snapshot:        /snapshot (single JPEG download)
 *  - Health Status:   /status  (JSON: heap, psram, pir count, uptime)
 *  - PIR Motion:      FreeRTOS Task on Core 0, full 4-stage event log
 *  - SD Card:         Photo storage w/ timestamp filename
 *  - Telegram Bot:    Photo push with event metadata
 *  - Health Monitor:  FreeRTOS Task — logs memory every 30s, warns if low
 */

#include "FS.h"
#include "SD_MMC.h"
#include "esp_camera.h"
#include "esp_http_server.h"
#include <Arduino.h>
#include <UniversalTelegramBot.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

// ==========================================
// Wi-Fi & Telegram credentials
// ==========================================
const char *ssid = "YOUR_WIFI_SSID";
const char *password = "YOUR_WIFI_PASSWORD";

#define BOT_TOKEN "YOUR_BOT_TOKEN"
#define CHAT_ID "YOUR_CHAT_ID"

WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);

// ==========================================
// GOOUUU ESP32-S3-CAM v1.3 Pin Definitions
// ==========================================
#define PWDN_GPIO_NUM -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 15
#define SIOD_GPIO_NUM 4
#define SIOC_GPIO_NUM 5
#define Y9_GPIO_NUM 16
#define Y8_GPIO_NUM 17
#define Y7_GPIO_NUM 18
#define Y6_GPIO_NUM 12
#define Y5_GPIO_NUM 10
#define Y4_GPIO_NUM 8
#define Y3_GPIO_NUM 9
#define Y2_GPIO_NUM 11
#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM 7
#define PCLK_GPIO_NUM 13

// ==========================================
// PIR Sensor
// ==========================================
#define PIR_SENSOR_PIN 1        // GPIO 1: confirmed free on GOOUUU v1.3
#define PIR_COOLDOWN_MS 5000    // Minimum ms between triggered events
#define PIR_NOISE_FILTER_MS 500 // Wait before confirming motion signal

volatile bool motionDetected = false;
volatile uint32_t pirEventCount = 0; // Total confirmed PIR events
unsigned long lastMotionTime = 0;

void IRAM_ATTR handleMotion() { motionDetected = true; }

// ==========================================
// System state
// ==========================================
unsigned long bootTime = 0; // Set after WiFi connects (millis() reference)

// ==========================================
// Async Telegram Upload — FreeRTOS Queue
// pirTask enqueues a job (<50 ms), telegramTask
// uploads it in the background on Core 1.
// This completely decouples TLS from the camera.
// ==========================================
typedef struct {
  uint8_t  *buf;      // malloc'd copy of JPEG data (freed after upload)
  size_t    len;
  uint32_t  eventNum;
  char      uptime[16];
  uint32_t  heapKB;
} TelegramJob;

#define TELEGRAM_QUEUE_LEN 3   // buffer up to 3 simultaneous events
QueueHandle_t telegramQueue = NULL;

// Forward declarations
void uploadTelegramJob(TelegramJob *job);
String getUptimeString();

// ==========================================
// Health Monitor Task (Core 1 — 30s interval)
// Logs heap & PSRAM usage; warns if memory is low
// ==========================================
void healthMonitorTask(void *pvParameters) {
  Serial.println("[Health] Memory monitor task started. Interval: 30s");
  while (true) {
    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t minHeap = ESP.getMinFreeHeap();
    uint32_t freePSRAM = ESP.getFreePsram();

    Serial.println("--------- [Health Report] ---------");
    Serial.printf("  Uptime    : %s\n", getUptimeString().c_str());
    Serial.printf("  Free Heap : %u KB  (min ever: %u KB)\n", freeHeap / 1024,
                  minHeap / 1024);
    Serial.printf("  Free PSRAM: %u KB\n", freePSRAM / 1024);
    Serial.printf("  PIR Events: %u\n", pirEventCount);
    Serial.printf("  WiFi RSSI : %d dBm\n", WiFi.RSSI());
    Serial.println("-----------------------------------");

    // Memory low-water warning
    if (freeHeap < 50000) {
      Serial.println(
          "[Health] [WARN]: Free heap below 50KB! Potential memory leak.");
    }
    if (freePSRAM < 512000) {
      Serial.println("[Health] [WARN]: Free PSRAM below 512KB!");
    }

    vTaskDelay(30000 / portTICK_PERIOD_MS);
  }
}

// ==========================================
// Uptime helper
// ==========================================
String getUptimeString() {
  unsigned long sec = millis() / 1000;
  unsigned long h = sec / 3600;
  unsigned long m = (sec % 3600) / 60;
  unsigned long s = sec % 60;
  char buf[32];
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", h, m, s);
  return String(buf);
}

// ==========================================
// uploadTelegramJob: blocking TLS upload
// Called only from telegramTask (Core 1, low priority)
// so it NEVER blocks the camera or stream handler.
// ==========================================
void uploadTelegramJob(TelegramJob *job) {
  Serial.printf("[Telegram #%u] Uploading %u bytes...\n", job->eventNum, job->len);

  String caption = "Motion Alert #" + String(job->eventNum) +
                   "\nUptime: " + String(job->uptime) +
                   "\nHeap: " + String(job->heapKB) + " KB free";

  const String boundary = "PetMonBoundary";
  String head = "--" + boundary + "\r\n"
                "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n" +
                String(CHAT_ID) + "\r\n"
                "--" + boundary + "\r\n"
                "Content-Disposition: form-data; name=\"caption\"\r\n\r\n" +
                caption + "\r\n"
                "--" + boundary + "\r\n"
                "Content-Disposition: form-data; name=\"photo\";"
                " filename=\"event.jpg\"\r\n"
                "Content-Type: image/jpeg\r\n\r\n";
  String tail = "\r\n--" + boundary + "--\r\n";

  WiFiClientSecure tgClient;
  tgClient.setInsecure();
  if (!tgClient.connect("api.telegram.org", 443)) {
    Serial.printf("[Telegram #%u] [ERROR] TLS connect failed\n", job->eventNum);
    return;
  }

  size_t totalLen = head.length() + job->len + tail.length();
  tgClient.println("POST /bot" + String(BOT_TOKEN) + "/sendPhoto HTTP/1.1");
  tgClient.println("Host: api.telegram.org");
  tgClient.println("Content-Type: multipart/form-data; boundary=" + boundary);
  tgClient.println("Content-Length: " + String(totalLen));
  tgClient.println("Connection: close");
  tgClient.println();
  tgClient.print(head);

  size_t w = 0;
  while (w < job->len) {
    size_t chunk = min((size_t)1024, job->len - w);
    tgClient.write(job->buf + w, chunk);
    w += chunk;
  }
  tgClient.print(tail);
  tgClient.flush();

  String resp = "";
  unsigned long deadline = millis() + 10000;
  while (millis() < deadline) {
    while (tgClient.available()) { resp += (char)tgClient.read(); deadline = millis() + 3000; }
    if (!tgClient.connected()) break;
    delay(10);
  }
  tgClient.stop();

  bool ok = resp.indexOf("\"ok\":true") >= 0;
  Serial.printf("[Telegram #%u] %s\n", job->eventNum, ok ? "[OK] Delivered" : "[ERROR] Failed");
}

// ==========================================
// capturePhoto: FAST path (<50 ms)
// Stage 1: Acquire frame   Stage 2: Save SD
// Stage 3: Enqueue job →   telegramTask uploads async
// ==========================================
void capturePhoto() {
  uint32_t eventNum = pirEventCount;
  Serial.printf("\n[PIR Event #%u] ===== Photo Pipeline Start =====\n", eventNum);

  // --- Stage 1: Acquire camera frame ---
  Serial.printf("[PIR Event #%u] Stage 1: Acquiring camera frame...\n", eventNum);
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.printf("[PIR Event #%u] [ERROR] Stage 1 FAILED\n", eventNum);
    return;
  }
  Serial.printf("[PIR Event #%u] [OK] Stage 1: %u bytes\n", eventNum, fb->len);

  // --- Stage 2: Save to SD Card ---
  Serial.printf("[PIR Event #%u] Stage 2: Writing to SD...\n", eventNum);
  String path = "/event_" + String(eventNum) + "_" + String(millis()) + ".jpg";
  File file = SD_MMC.open(path.c_str(), FILE_WRITE);
  if (file) {
    file.write(fb->buf, fb->len);
    file.close();
    Serial.printf("[PIR Event #%u] [OK] Stage 2: Saved %s\n", eventNum, path.c_str());
  } else {
    Serial.printf("[PIR Event #%u] [WARN] Stage 2: SD write failed\n", eventNum);
  }

  // --- Stage 3: Copy frame to PSRAM buffer → enqueue for async upload ---
  Serial.printf("[PIR Event #%u] Stage 3: Enqueueing Telegram job (async)...\n", eventNum);
  TelegramJob *job = (TelegramJob *)malloc(sizeof(TelegramJob));
  if (job) {
    job->buf = (uint8_t *)ps_malloc(fb->len); // allocate from PSRAM
    if (job->buf) {
      memcpy(job->buf, fb->buf, fb->len);
      job->len      = fb->len;
      job->eventNum = eventNum;
      job->heapKB   = ESP.getFreeHeap() / 1024;
      strlcpy(job->uptime, getUptimeString().c_str(), sizeof(job->uptime));

      if (xQueueSend(telegramQueue, &job, 0) == pdTRUE) {
        Serial.printf("[PIR Event #%u] [OK] Stage 3: Job queued\n", eventNum);
      } else {
        Serial.printf("[PIR Event #%u] [WARN] Stage 3: Queue full, dropping job\n", eventNum);
        free(job->buf); free(job);
      }
    } else {
      Serial.printf("[PIR Event #%u] ⚠️  Stage 3 WARN: ps_malloc failed\n", eventNum);
      free(job);
    }
  }
  // Return camera frame buffer IMMEDIATELY — stream can resume at once
  esp_camera_fb_return(fb);
  Serial.printf("[PIR Event #%u] [OK] Stage 4: Frame returned to pool. Heap: %u KB\n\n",
                eventNum, ESP.getFreeHeap() / 1024);
}

// ==========================================
// telegramTask: low-priority upload worker
// Core 1, Priority 2 — never competes with camera
// ==========================================
void telegramTask(void *pvParameters) {
  Serial.println("[System] Telegram upload task started on Core 1.");
  TelegramJob *job = NULL;
  while (true) {
    // Block indefinitely until a job is available
    if (xQueueReceive(telegramQueue, &job, portMAX_DELAY) == pdTRUE) {
      uploadTelegramJob(job);
      free(job->buf);
      free(job);
      job = NULL;
    }
  }
}

// ==========================================
// AVI MJPEG Video Recorder
// Records FRAMESIZE_VGA MJPEG frames to SD as
// a playable .avi file (VLC / Windows Media Player)
// ==========================================
#define AVI_FPS        10
#define AVI_DURATION_MS 5000
#define AVI_MAX_FRAMES  120   // 10fps * 12s safety headroom

// Byte layout constants (pre-computed from AVI RIFF spec)
#define AVI_OFF_RIFF_SIZE    4   // Total RIFF size (fixup)
#define AVI_OFF_AVIH_FRAMES  48  // avih.dwTotalFrames (fixup)
#define AVI_OFF_STRH_FRAMES  140 // strh.dwLength (fixup)
#define AVI_OFF_MOVI_SIZE    216 // movi LIST size (fixup)
#define AVI_OFF_MOVI_DATA    224 // first frame byte offset

typedef struct { uint32_t offset; uint32_t size; } AviIdx;
static AviIdx aviIdx[AVI_MAX_FRAMES];

static void aviU32(File &f, uint32_t v) { f.write((uint8_t *)&v, 4); }
static void aviFCC(File &f, const char *s) { f.write((uint8_t *)s, 4); }
static void aviU16(File &f, uint16_t v) { f.write((uint8_t *)&v, 2); }

static void aviWriteHeader(File &f) {
  const uint32_t W = 320, H = 240; // QVGA Resolution
  const uint32_t usPerFrame = 1000000 / AVI_FPS;

  aviFCC(f, "RIFF"); aviU32(f, 0);           // [4]  RIFF size FIXUP
  aviFCC(f, "AVI ");

  aviFCC(f, "LIST"); aviU32(f, 0xC0);        // hdrl LIST (192 bytes)
  aviFCC(f, "hdrl");

  // avih (Main AVI Header, 56 bytes)
  aviFCC(f, "avih"); aviU32(f, 56);
  aviU32(f, usPerFrame);  // usPerFrame
  aviU32(f, 0);           // maxBytesPerSec
  aviU32(f, 0);           // paddingGranularity
  aviU32(f, 0x10);        // flags: AVIF_HASINDEX
  aviU32(f, 0);           // [48] totalFrames FIXUP
  aviU32(f, 0);           // initialFrames
  aviU32(f, 1);           // streams
  aviU32(f, 0);           // suggestedBufferSize
  aviU32(f, W);           // width
  aviU32(f, H);           // height
  aviU32(f, 0); aviU32(f, 0); aviU32(f, 0); aviU32(f, 0); // reserved

  aviFCC(f, "LIST"); aviU32(f, 0x74);        // strl LIST (116 bytes)
  aviFCC(f, "strl");

  // strh (Stream Header, 56 bytes)
  aviFCC(f, "strh"); aviU32(f, 56);
  aviFCC(f, "vids");  // fccType: video
  aviFCC(f, "MJPG");  // fccHandler: Motion JPEG
  aviU32(f, 0);       // flags
  aviU32(f, 0);       // priority
  aviU32(f, 0);       // initialFrames
  aviU32(f, 1);       // scale
  aviU32(f, AVI_FPS); // rate (fps)
  aviU32(f, 0);       // start
  aviU32(f, 0);       // [140] length (frames) FIXUP
  aviU32(f, 0);       // suggestedBufferSize
  aviU32(f, 0xFFFFFFFF); // quality
  aviU32(f, 0);          // sampleSize
  aviU16(f, 0); aviU16(f, 0); aviU16(f, W); aviU16(f, H); // rcFrame

  // strf (BITMAPINFOHEADER, 40 bytes)
  aviFCC(f, "strf"); aviU32(f, 40);
  aviU32(f, 40);      // biSize
  aviU32(f, W);       // biWidth
  aviU32(f, H);       // biHeight
  aviU16(f, 1);       // biPlanes
  aviU16(f, 24);      // biBitCount
  aviFCC(f, "MJPG");  // biCompression
  aviU32(f, W*H*3);   // biSizeImage
  aviU32(f, 0); aviU32(f, 0); aviU32(f, 0); aviU32(f, 0);

  // movi LIST
  aviFCC(f, "LIST"); aviU32(f, 0);           // [216] movi size FIXUP
  aviFCC(f, "movi");                          // [220] frame data starts at [224]
}

void recordVideo(uint32_t eventNum) {
  String path = "/video_" + String(eventNum) + "_" + String(millis()) + ".avi";
  Serial.printf("[Video #%u] Starting %d ms recording @ %d fps → %s\n",
                eventNum, AVI_DURATION_MS, AVI_FPS, path.c_str());

  File f = SD_MMC.open(path.c_str(), FILE_WRITE);
  if (!f) {
    Serial.printf("[Video #%u] [ERROR] Could not open file for writing\n", eventNum);
    return;
  }

  aviWriteHeader(f);

  uint32_t frameCount = 0;
  uint32_t msPerFrame = 1000 / AVI_FPS;
  unsigned long start = millis();
  unsigned long nextFrame = start;

  while (millis() - start < AVI_DURATION_MS && frameCount < AVI_MAX_FRAMES) {
    if (millis() >= nextFrame) {
      camera_fb_t *fb = esp_camera_fb_get();
      if (fb) {
        // frame offset = position from start of movi data section
        aviIdx[frameCount].offset = f.position() - AVI_OFF_MOVI_DATA;
        aviIdx[frameCount].size   = fb->len;

        aviFCC(f, "00dc");       // video chunk for stream 0
        aviU32(f, fb->len);
        f.write(fb->buf, fb->len);
        if (fb->len & 1) f.write((uint8_t)0); // WORD-align padding

        esp_camera_fb_return(fb);
        frameCount++;
      }
      nextFrame += msPerFrame;
    }
    vTaskDelay(5 / portTICK_PERIOD_MS); // yield briefly
  }

  uint32_t moviEnd = f.position();

  // Write idx1 index chunk
  aviFCC(f, "idx1");
  aviU32(f, frameCount * 16);
  for (uint32_t i = 0; i < frameCount; i++) {
    aviFCC(f, "00dc");
    aviU32(f, 0x10);                   // AVIIF_KEYFRAME
    aviU32(f, aviIdx[i].offset + 4);   // offset from movi start
    aviU32(f, aviIdx[i].size);
  }
  uint32_t totalEnd = f.position();

  // Fix up header placeholders by seeking back
  f.seek(AVI_OFF_RIFF_SIZE);   aviU32(f, totalEnd - 8);
  f.seek(AVI_OFF_AVIH_FRAMES); aviU32(f, frameCount);
  f.seek(AVI_OFF_STRH_FRAMES); aviU32(f, frameCount);
  f.seek(AVI_OFF_MOVI_SIZE);   aviU32(f, moviEnd - (AVI_OFF_MOVI_SIZE + 4));
  f.close();

  float actualFps = (float)frameCount * 1000.0f / AVI_DURATION_MS;
  Serial.printf("[Video #%u] Done: %u frames @ %.1f fps (%lu KB) — %s\n",
                eventNum, frameCount, actualFps,
                (unsigned long)(totalEnd / 1024), path.c_str());
}

// ==========================================
// PIR Background Task (Core 0, Priority 5)
// ==========================================
void pirTask(void *pvParameters) {
  Serial.println("[System] PIR task started on Core 0.");
  while (true) {
    if (motionDetected) {
      motionDetected = false;

      // Noise filter: wait 500ms then re-check signal level
      vTaskDelay(PIR_NOISE_FILTER_MS / portTICK_PERIOD_MS);
      if (digitalRead(PIR_SENSOR_PIN) == HIGH) {
        unsigned long now = millis();
        if (now - lastMotionTime > PIR_COOLDOWN_MS) {
          lastMotionTime = now;
          pirEventCount++;
          Serial.printf("[PIR] Motion confirmed! Event #%u\n", pirEventCount);
          capturePhoto();          // Instant photo → Telegram
          recordVideo(pirEventCount); // 5s AVI → SD card
        } else {
          Serial.printf(
              "[PIR] Suppressed by cooldown (%lu ms remaining)\n",
              PIR_COOLDOWN_MS - (now - lastMotionTime));
        }
      } else {
        Serial.println("[PIR] Signal dropped — noise filtered.");
      }
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

// ==========================================
// HTTP Handlers
// ==========================================
httpd_handle_t stream_httpd = NULL;

// --- GET / : HTML dashboard (iOS Safari compatible) ---
static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  // Minimal HTML: embed the MJPEG stream in an <img> tag.
  // Safari supports MJPEG inside <img> but not as a raw top-level page.
  const char *html =
      "<!DOCTYPE html><html><head>"
      "<meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width, initial-scale=1'>"
      "<title> Pet Monitor</title>"
      "<style>"
      "  "
      "body{margin:0;background:#111;color:#eee;font-family:sans-serif;text-"
      "align:center;}"
      "  h1{padding:16px;font-size:1.2rem;letter-spacing:2px;color:#80cbc4;}"
      "  img{width:100%;max-width:640px;border-radius:8px;border:2px solid "
      "#333;}"
      "  .status{padding:12px;font-size:0.85rem;color:#aaa;}"
      "  a{display:inline-block;margin:8px;padding:10px "
      "20px;background:#37474f;"
      "    "
      "border-radius:6px;color:#80cbc4;text-decoration:none;font-size:0.9rem;}"
      "  a:hover{background:#546e7a;}"
      "</style></head><body>"
      "<h1> ESP32-S3 Pet Monitor</h1>"
      "<img src='/stream' alt='Live Feed'><br>"
      "<div class='status'>Live MJPEG stream · <a href='/snapshot'> "
      "Snapshot</a>"
      " <a href='/status'> Status</a></div>"
      "</body></html>";

  return httpd_resp_send(req, html, strlen(html));
}

// --- GET /stream : MJPEG stream ---
static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  char part_buf[128];
  bool first_logged = false;

  httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control",
                     "no-cache, no-store, must-revalidate");
  httpd_resp_set_hdr(req, "Pragma", "no-cache");

  unsigned long lastFrameTime = 0;
  const uint32_t interval = 1000 / 15; // Target 15 FPS

  while (true) {
    // --- FPS Capping Logic ---
    unsigned long now = millis();
    if (now - lastFrameTime < interval) {
        vTaskDelay(1); // yield to other tasks
        continue;
    }
    lastFrameTime = now;

    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("[Stream] Frame capture failed, retrying...");
      continue; // Don't break connection, just wait for next interval
    } else {
      if (!first_logged) {
        Serial.printf("[Stream] First frame: %u bytes @ %ux%u\n", fb->len, fb->width, fb->height);
        first_logged = true;
      }
    }

    esp_err_t send_res = ESP_OK;
    size_t hlen = snprintf(part_buf, sizeof(part_buf),
                           "\r\n--frame\r\nContent-Type: image/jpeg\r\n"
                           "Content-Length: %u\r\n\r\n", fb->len);
    send_res = httpd_resp_send_chunk(req, part_buf, hlen);

    if (send_res == ESP_OK) {
      send_res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    }

    esp_camera_fb_return(fb);
    fb = NULL;

    if (send_res != ESP_OK) {
      Serial.println("[Stream] [ERROR] Socket send failed, closing connection.");
      break; // Network error, stop streaming
    }
  }
  return ESP_OK;
}

// --- GET /snapshot : single JPEG download ---
static esp_err_t snapshot_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition",
                     "attachment; filename=snapshot.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  Serial.printf("[Snapshot] Manual snapshot served (%u bytes)\n", fb->len);
  return res;
}

// --- GET /status : HTML visual dashboard ---
static esp_err_t status_handler(httpd_req_t *req) {
  uint32_t start_ms = millis();
  Serial.println("[HTTP] /status request received.");

  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Refresh", "10");

  uint32_t f_heap  = ESP.getFreeHeap() / 1024;
  uint32_t m_heap  = ESP.getMinFreeHeap() / 1024;
  uint32_t t_heap  = 320;
  uint32_t u_heap  = (t_heap > f_heap) ? (t_heap - f_heap) : 0;
  uint32_t h_pct   = (u_heap * 100) / t_heap;

  uint32_t f_psram = ESP.getFreePsram() / 1024;
  uint32_t t_psram = 8192;
  uint32_t u_psram = (t_psram > f_psram) ? (t_psram - f_psram) : 0;
  uint32_t p_pct   = (u_psram * 100) / t_psram;

  int rssi = WiFi.RSSI();
  int r_pct = constrain(map(rssi, -100, -30, 0, 100), 0, 100);
  String up_str = getUptimeString();

  static char html_buf[4096]; // Increased to 4KB
  int len = snprintf(html_buf, sizeof(html_buf),
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Status</title>"
    "<style>"
    "body{margin:0;padding:16px;background:#111;color:#eee;font-family:sans-serif;}"
    "h1{color:#80cbc4;font-size:1.1rem;margin-bottom:16px;}"
    ".card{background:#1e2a30;border-radius:10px;padding:14px;margin-bottom:12px;}"
    ".label{font-size:.75rem;color:#90a4ae;text-transform:uppercase;letter-spacing:1px;}"
    ".value{font-size:1.3rem;font-weight:bold;margin:4px 0 8px;}"
    ".bar-bg{background:#263238;border-radius:6px;height:12px;overflow:hidden;}"
    ".bar{height:12px;border-radius:6px;transition:width .4s;}"
    ".bar-heap{background:linear-gradient(90deg,#4db6ac,#26c6da);}"
    ".bar-psram{background:linear-gradient(90deg,#7986cb,#64b5f6);}"
    ".bar-wifi{background:linear-gradient(90deg,#a5d6a7,#66bb6a);}"
    ".sub{font-size:.72rem;color:#78909c;margin-top:6px;}"
    ".grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;}"
    "a{color:#80cbc4;font-size:.85rem;}"
    "</style></head><body>"
    "<h1>ESP32-S3 System Status</h1>"
    "<div class='card'>"
    "<div class='label'>Internal RAM (Heap)</div>"
    "<div class='value'>%u%% used &nbsp;<span style='font-size:.9rem;color:#90a4ae;'>%u / %u KB</span></div>"
    "<div class='bar-bg'><div class='bar bar-heap' style='width:%u%%'></div></div>"
    "<div class='sub'>Min ever free: %u KB &nbsp;|&nbsp; Currently free: %u KB</div>"
    "</div>"
    "<div class='card'>"
    "<div class='label'>External PSRAM (8 MB OPI)</div>"
    "<div class='value'>%u%% used &nbsp;<span style='font-size:.9rem;color:#90a4ae;'>%u / %u KB</span></div>"
    "<div class='bar-bg'><div class='bar bar-psram' style='width:%u%%'></div></div>"
    "<div class='sub'>Free: %u KB &nbsp;(Camera frame buffers live here)</div>"
    "</div>"
    "<div class='grid'>"
    "<div class='card'>"
    "<div class='label'>Wi-Fi Signal</div>"
    "<div class='value'>%d%%</div>"
    "<div class='bar-bg'><div class='bar bar-wifi' style='width:%d%%'></div></div>"
    "<div class='sub'>%d dBm &nbsp;|&nbsp; %s</div>"
    "</div>"
    "<div class='card'>"
    "<div class='label'>PIR Events</div>"
    "<div class='value'>%u</div>"
    "<div class='sub'>Uptime: %s</div>"
    "</div>"
    "</div>"
    "<div style='text-align:center;margin-top:8px;'>"
    "<a href='/'>Dashboard</a> &nbsp;&nbsp; <a href='/snapshot'>Snapshot</a>"
    "</div>"
    "</body></html>",
    h_pct, u_heap, t_heap, h_pct, m_heap, f_heap,
    p_pct, u_psram, t_psram, p_pct, f_psram,
    r_pct, r_pct, rssi, ssid,
    pirEventCount, up_str.c_str()
  );

  Serial.printf("[HTTP] Status page built: %d bytes (took %ld ms)\n", len, millis() - start_ms);
  return httpd_resp_send(req, html_buf, len);
}

// ==========================================
// Start HTTP Server (all routes)
// ==========================================
void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_uri_handlers = 8;

  httpd_uri_t index_uri = {.uri = "/",
                           .method = HTTP_GET,
                           .handler = index_handler,
                           .user_ctx = NULL};
  httpd_uri_t stream_uri = {.uri = "/stream",
                            .method = HTTP_GET,
                            .handler = stream_handler,
                            .user_ctx = NULL};
  httpd_uri_t snapshot_uri = {.uri = "/snapshot",
                              .method = HTTP_GET,
                              .handler = snapshot_handler,
                              .user_ctx = NULL};
  httpd_uri_t status_uri = {.uri = "/status",
                            .method = HTTP_GET,
                            .handler = status_handler,
                            .user_ctx = NULL};

  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &index_uri);
    httpd_register_uri_handler(stream_httpd, &stream_uri);
    httpd_register_uri_handler(stream_httpd, &snapshot_uri);
    httpd_register_uri_handler(stream_httpd, &status_uri);
    Serial.println(
        "[Server] Registered routes: / | /stream | /snapshot | /status");
  } else {
    Serial.println("[Server] [ERROR] httpd_start failed!");
  }
}

// ==========================================
// setup()
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[System] ===== Pet Monitor v2.0 Boot =====");

  // --- Camera pin mapping ---
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 10000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size   = FRAMESIZE_QVGA; // Standard 4:3 Low-Lat Mode
    config.jpeg_quality = 10;
    config.fb_count     = 3;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
    Serial.println("[Camera] PSRAM found → QVGA triple-buffer (Maximum Compatibility)");
  } else {
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;
    Serial.println("[Camera] No PSRAM → QVGA single-buffer");
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[Fatal] Camera init failed: 0x%x\n", err);
    return;
  }

  // Wake OV2640 sensor from default black-frame state
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_brightness(s, 1);
    s->set_contrast(s, 1);
    s->set_saturation(s, 0);
    s->set_gainceiling(s, (gainceiling_t)6);
    s->set_exposure_ctrl(s, 1);
    s->set_gain_ctrl(s, 1);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_aec2(s, 1);
    s->set_lenc(s, 1);
    Serial.println("[Camera] OV2640 sensor parameters configured.");
  }

  // --- Wi-Fi ---
  WiFi.begin(ssid, password);
  Serial.print("[Network] Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[Network] [OK] Connected!");
  Serial.printf("[Network] IP: %s\n", WiFi.localIP().toString().c_str());
  secured_client.setInsecure();

  // --- HTTP Server ---
  startCameraServer();

  // --- SD Card ---
  Serial.print("[SD] Initializing...");
  SD_MMC.setPins(39, 38, 40); // CLK=39, CMD=38, D0=40 (1-bit mode)
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println(" [ERROR] Failed to mount SD");
  } else {
    uint64_t cardMB = SD_MMC.cardSize() / (1024 * 1024);
    Serial.printf(" [OK] Success — %llu MB\n", cardMB);
  }

  // --- PIR ---
  pinMode(PIR_SENSOR_PIN, INPUT_PULLDOWN);
  attachInterrupt(digitalPinToInterrupt(PIR_SENSOR_PIN), handleMotion, RISING);
  Serial.printf("[PIR] Interrupt attached → GPIO %d (cooldown: %dms)\n",
                PIR_SENSOR_PIN, PIR_COOLDOWN_MS);

  // --- FreeRTOS Queue & Tasks ---
  // Queue depth=3: drops oldest if burst of 3+ events before upload completes
  telegramQueue = xQueueCreate(TELEGRAM_QUEUE_LEN, sizeof(TelegramJob *));

  //  Task            | Core | Priority | Role
  //  --------------- | ---- | -------- | -----------------------------------
  //  pirTask         |   0  |    3     | Fast: detect motion, capture, enqueue
  //  telegramTask    |   1  |    2     | Async: TLS upload (slow, isolated)
  //  healthMonitor   |   1  |    1     | Periodic memory logging
  //  esp_httpd       |   0  |    5*    | MJPEG stream (system-managed)
  //  (*) esp_httpd default priority is 5; pirTask now at 3, won't starve it
  xTaskCreatePinnedToCore(pirTask,           "PIR_Task",    8192, NULL, 3, NULL, 0);
  xTaskCreatePinnedToCore(telegramTask,      "TG_Upload",   8192, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(healthMonitorTask, "Health_Task", 4096, NULL, 1, NULL, 1);

  Serial.println("\n============================================");
  Serial.println("   Pet Monitor v2.0 — System Ready");
  Serial.printf("  Dashboard : http://%s/\n",
                WiFi.localIP().toString().c_str());
  Serial.printf("  Stream    : http://%s/stream\n",
                WiFi.localIP().toString().c_str());
  Serial.printf("  Snapshot  : http://%s/snapshot\n",
                WiFi.localIP().toString().c_str());
  Serial.printf("  Status    : http://%s/status\n",
                WiFi.localIP().toString().c_str());
  Serial.println("============================================\n");
}

// ==========================================
// loop() — idle; all logic in tasks
// ==========================================
void loop() { vTaskDelay(1000 / portTICK_PERIOD_MS); }
