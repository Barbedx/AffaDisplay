// net.h — WiFi, mDNS, PsychicHttp and OTA, once instead of three times.
//
// Every example here brings up the same stack, and before this file they each carried their
// own copy. The copies had already drifted: `stack_size` was 8192 in one and 10240 in the
// others, `max_uri_handlers` 32 in one and 64 in the others. Nothing chose those numbers
// differently on purpose — they were edited in the file somebody happened to be in.
//
// THIS IS NOT LINE-COUNT TIDYING. Two of the four PsychicHttp knobs below are one-way doors
// that cost real hardware time to find, and knowledge that lives in three places is
// knowledge that is wrong in two of them.
#pragma once

#include <Arduino.h>
#include <ElegantOTA.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <PsychicHttp.h>
#include <WiFi.h>

namespace affanet {

// NVS namespace holding `ssid` / `pass`. Shared with MegaOpen deliberately: a board flashed
// with either firmware keeps its credentials.
constexpr const char* kWifiNamespace = "megaopen";
constexpr uint32_t    kStaJoinMs     = 15000;

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------
// STA if NVS has an SSID, otherwise our own AP. THE AP IS THE RECOVERY PATH, not a failure
// mode: a board with no credentials, or with the wrong password, still answers OTA at
// http://192.168.4.1/update. Without it, moving networks means a cable.
//
// Returns true for STA. `apSsid`/`apPass` are used only when that returns false.
inline bool startWifi(const char* apSsid, const char* apPass, const char* mdnsName,
                      const char* tag) {
  Preferences p;
  String ssid, pass;
  if (p.begin(kWifiNamespace, true)) {
    ssid = p.getString("ssid", "");
    pass = p.getString("pass", "");
    p.end();
  }

  WiFi.persistent(false);   // do not write the join to flash on every boot
  WiFi.setSleep(true);
  bool sta = false;

  if (ssid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    // BOUNDED, and the bound matters. An unbounded join blocks setup() for as long as the
    // router is unreachable; a 258 s block once banked 372 000 bogus bus errors before the
    // CAN driver had started, and read as a dead controller.
    const uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < kStaJoinMs) delay(100);
    sta = (WiFi.status() == WL_CONNECTED);
  }
  if (!sta) { WiFi.mode(WIFI_AP); WiFi.softAP(apSsid, apPass); }

  if (mdnsName && MDNS.begin(mdnsName)) MDNS.addService("http", "tcp", 80);

  const String ip = sta ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  Serial.printf("\n[%s] %s ip=%s  http://%s/  OTA http://%s/update\n",
                tag, sta ? "STA" : "AP", ip.c_str(), ip.c_str(), ip.c_str());
  return sta;
}

// ---------------------------------------------------------------------------
// HTTP + OTA
// ---------------------------------------------------------------------------
// Configure, listen, then register OTA BEFORE the caller's own routes.
//
// `onOtaStart` / `onOtaEnd` exist because a flash write must not race the CAN transmitter:
// every example passes setTxGate(false)/(true) here. onOtaEnd receives the success flag.
//
// FOUR KNOBS, AND TWO OF THEM ARE ONE-WAY DOORS:
//
//   lru_purge_enable = true   PsychicHttp only sets this itself under ENABLE_ASYNC, which
//                             these builds do not define. WITHOUT IT, esp_http_server keeps
//                             every socket until the client closes, the 7-slot table fills,
//                             and the board stops answering HTTP — including /update —
//                             while ping and mDNS still reply. It looks exactly like a
//                             crash and is not one. It cost a cable to find.
//
//   max_uri_handlers = 64     ElegantOTA registers several routes of its own. Overflow is
//                             SILENT: esp_http_server refuses the registration and the
//                             route simply does not exist. One extra route in a console
//                             once unregistered /ota/upload with no error anywhere, which
//                             is why OTA is registered FIRST, below.
//
//   max_open_sockets = 7      the esp_http_server ceiling on this target
//   stack_size = 10240        the httpd default is 4096 and these handlers build JSON on it
inline void startHttp(PsychicHttpServer& server,
                      void (*onOtaStart)(), void (*onOtaEnd)(bool),
                      void (*routes)()) {
  server.config.lru_purge_enable  = true;
  server.config.max_open_sockets  = 7;
  server.config.recv_wait_timeout = 3;
  server.config.send_wait_timeout = 3;
  server.config.max_uri_handlers  = 64;
  server.config.stack_size        = 10240;
  server.listen(80);

  // OTA FIRST — the only way back into a board with no cable, so it gets the URI slots
  // before anything of ours can exhaust them.
  if (onOtaStart) ElegantOTA.onStart(onOtaStart);
  if (onOtaEnd)   ElegantOTA.onEnd(onOtaEnd);
  ElegantOTA.begin(&server);

  if (routes) routes();
}

// Read/write the stored credentials. Exposed so an example can offer `?op=wifi`: before that
// existed, moving the board to another network meant a reflash.
inline bool storedSsid(String& out) {
  Preferences p;
  if (!p.begin(kWifiNamespace, true)) return false;
  out = p.getString("ssid", "");
  p.end();
  return true;
}

inline bool storeWifi(const char* ssid, const char* pass) {
  Preferences p;
  if (!p.begin(kWifiNamespace, false)) return false;
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.end();
  return true;
}

}  // namespace affanet
