// DL6TJ: Meldungen von eigenen Nodes (Briefkasten, Auto) per APRS-Nachricht empfangen,
// per ACK bestaetigen und an Telegram weiterleiten.
//
// Die Nodes senden APRS-Nachrichten an den iGate, Pfad NOGATE (damit weder Upload ins APRS-IS
// noch Digipeating stattfindet):   DL6TJ-7>APLAIX,NOGATE::DL6TJ-10 :Post ist da! | Batt: 3.92V{1234
// Der iGate antwortet mit "ackNNNN". Telegram-Versand laeuft in einer eigenen Task, damit der LoRa-Empfang nicht blockiert.

#include <APRSPacketLib.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include "configuration.h"
#include "station_utils.h"
#include "node_settings.h"
#include "node_utils.h"
#include "utils.h"

extern Configuration    Config;
extern APRSPacket       lastAprsPacket;

namespace NODE_Utils {

    struct TgItem {
        char    text[220];
        uint8_t chats;      // Bit0 = Chat 1, Bit1 = Chat 2
    };

    static QueueHandle_t    tgQueue     = nullptr;
    static String           tgToken, tgChat1, tgChat2;

    static struct { uint32_t hash; uint32_t ms; } seen[16];

    static void loadCredentials() {
        Preferences p;
        p.begin("nodetg", true);
        tgToken = p.getString("tok", "");
        tgChat1 = p.getString("c1", "");
        tgChat2 = p.getString("c2", "");
        p.end();
    }

    static String urlEncode(const String &s) {
        String out;
        char buf[4];
        for (unsigned int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (isalnum((unsigned char)c)) out += c;
            else if (c == ' ') out += "%20";
            else { snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c); out += buf; }
        }
        return out;
    }

    static bool tgSendOne(const String &chat, const String &text) {
        if (tgToken == "" || chat == "") return true;       // nicht eingerichtet: nichts zu tun (kein Wiederholen)
        String url = "https://api.telegram.org/bot" + tgToken + "/sendMessage?chat_id=" + chat + "&text=" + urlEncode(text);
        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient https;
        https.setTimeout(8000);
        bool ok = false;
        if (https.begin(client, url)) {
            int code = https.GET();
            ok = (code == 200);
            if (!ok) Utils::println("Node-Telegram: HTTP " + String(code));
            https.end();
        }
        return ok;
    }

    static void tgTask(void *) {
        TgItem item;
        for (;;) {
            if (xQueueReceive(tgQueue, &item, portMAX_DELAY) != pdTRUE) continue;
            String text = String(item.text);
            bool d1 = !(item.chats & 1), d2 = !(item.chats & 2);
            for (int tries = 0; tries < 4 && !(d1 && d2); tries++) {
                if (tries > 0) vTaskDelay(pdMS_TO_TICKS(15000));
                if (WiFi.status() != WL_CONNECTED) continue;
                if (ESP.getMaxAllocHeap() < 50000) { Utils::println("Node-Telegram: zu wenig Speicher, spaeter erneut"); continue; }
                if (!d1) d1 = tgSendOne(tgChat1, text);
                if (!d2) d2 = tgSendOne(tgChat2, text);
            }
            if (!(d1 && d2)) Utils::println("Node-Telegram: Meldung verworfen");
        }
    }

    static void tgQueueText(const String &text, uint8_t chats) {
        if (!tgQueue) return;
        TgItem item;
        memset(&item, 0, sizeof(item));
        text.substring(0, sizeof(item.text) - 1).toCharArray(item.text, sizeof(item.text));
        item.chats = chats;
        if (xQueueSend(tgQueue, &item, 0) != pdTRUE) Utils::println("Node-Telegram: Warteschlange voll");
    }

    static bool alreadySeen(const String &key) {
        uint32_t h = 2166136261UL;
        for (unsigned int i = 0; i < key.length(); i++) { h ^= (uint8_t)key.charAt(i); h *= 16777619UL; }
        uint32_t now = millis();
        int free = -1;
        for (int i = 0; i < 16; i++) {
            if (seen[i].hash == h && seen[i].ms != 0 && now - seen[i].ms < (uint32_t)NODE_DEDUP_S * 1000UL) return true;
            if (free < 0 && (seen[i].ms == 0 || now - seen[i].ms >= (uint32_t)NODE_DEDUP_S * 1000UL)) free = i;
        }
        if (free < 0) free = 0;
        seen[free].hash = h;
        seen[free].ms = now ? now : 1;
        return false;
    }

    static float parseBatt(const String &t) {
        int i = t.indexOf("Batterie:");
        if (i >= 0) return t.substring(i + 9).toFloat();
        i = t.indexOf("Batt:");
        if (i >= 0) return t.substring(i + 5).toFloat();
        return 0.0f;
    }

    void processLoRaPacket(const String& packet) {
        if (!NODE_ENABLE) return;
        if (lastAprsPacket.header != "" || lastAprsPacket.type != 1) return;                 // nur direkte APRS-Nachrichten
        String myCall = (Config.tacticalCallsign == "") ? Config.callsign : Config.tacticalCallsign;
        if (lastAprsPacket.addressee != myCall) return;                                       // nicht an uns

        const NodeDef *node = nullptr;
        for (const NodeDef &n : NODE_LIST) if (lastAprsPacket.sender == n.call) { node = &n; break; }
        if (!node) return;

        String payload = lastAprsPacket.payload;                                              // "Text{Nr"
        String text = payload;
        int brace = payload.lastIndexOf('{');
        if (brace >= 0) text = payload.substring(0, brace);
        text.trim();

        // 1) ACK senden (bei jedem empfangenen Exemplar, auch bei Wiederholungen: der erste ACK kann verloren gegangen sein)
        if (brace >= 0) {
            String ack = APRSPacketLib::generateAckPacket(myCall, "APLRG1", "RFONLY", lastAprsPacket.sender, payload);
            if (ack != "") STATION_Utils::addToOutputPacketBuffer(ack);
        }

        // 2) nur einmal weiterleiten
        if (alreadySeen(lastAprsPacket.sender + "|" + payload)) return;
        Utils::println(String("Node ") + node->name + ": " + text);

        // 3) Regeln je Node
        float batt = parseBatt(text);
        if (node->role == 0) {                                                                // Briefkasten
            if (text.indexOf("Post") >= 0) {
                tgQueueText(String(node->name) + ": " + text, 3);                             // beide Chats; darf auch mal verloren gehen
            } else if (text.indexOf("Status") >= 0) {
                if (NODE_BRIEF_STATUS_TG) tgQueueText(String(node->name) + ": " + text, 1);
            }
            if (batt > 0.0f && batt < NODE_BRIEF_LOW_V) tgQueueText("Batterie Briefkasten niedrig (" + String(batt, 2) + " V)", 1);
        } else if (node->role == 1) {                                                         // Auto
            if (text.startsWith("WARN") || text.startsWith("KRITISCH") || text.indexOf("Spannungen") >= 0) {
                tgQueueText(String(node->name) + ": " + text, 1);
            }
        }
    }

    // ---- Webseite /telegram: Zugangsdaten setzen (werden im Flash des iGates gespeichert, nicht im Programmtext) ----
    static void handleTelegram(AsyncWebServerRequest *request) {
        if (Config.webadmin.active && !request->authenticate(Config.webadmin.username.c_str(), Config.webadmin.password.c_str()))
            return request->requestAuthentication();

        String msg = "";
        if (request->method() == HTTP_POST) {
            Preferences p;
            p.begin("nodetg", false);
            if (request->hasParam("tok", true) && request->getParam("tok", true)->value() != "") p.putString("tok", request->getParam("tok", true)->value());
            if (request->hasParam("c1", true) && request->getParam("c1", true)->value() != "")   p.putString("c1",  request->getParam("c1", true)->value());
            if (request->hasParam("c2", true) && request->getParam("c2", true)->value() != "")   p.putString("c2",  request->getParam("c2", true)->value());
            if (request->hasParam("clear", true)) { p.remove("tok"); p.remove("c1"); p.remove("c2"); }
            p.end();
            loadCredentials();
            if (request->hasParam("test", true)) { tgQueueText("Test vom iGate " + Config.callsign, 3); msg = "Testnachricht in der Warteschlange."; }
            else msg = "Gespeichert.";
        }
        String h = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>Telegram</title></head><body style='font-family:sans-serif;max-width:480px;margin:20px auto'>";
        h += "<h3>Telegram f&uuml;r Node-Meldungen</h3>";
        if (msg != "") h += "<p><b>" + msg + "</b></p>";
        h += "<p>Bot-Token: " + String(tgToken != "" ? "gesetzt" : "<i>nicht gesetzt</i>") + "<br>Chat 1: " + (tgChat1 != "" ? tgChat1 : String("<i>nicht gesetzt</i>")) + "<br>Chat 2: " + (tgChat2 != "" ? tgChat2 : String("<i>nicht gesetzt</i>")) + "</p>";
        h += "<form method='POST' action='/telegram'>";
        h += "Bot-Token (leer = unver&auml;ndert)<br><input name='tok' type='password' style='width:100%'><br><br>";
        h += "Chat 1 &ndash; alle Meldungen (leer = unver&auml;ndert)<br><input name='c1' style='width:100%'><br><br>";
        h += "Chat 2 &ndash; nur &bdquo;Post ist da&ldquo; vom Briefkasten (leer = unver&auml;ndert)<br><input name='c2' style='width:100%'><br><br>";
        h += "<button type='submit'>Speichern</button> ";
        h += "<button type='submit' name='test' value='1'>Speichern + Test senden</button> ";
        h += "<button type='submit' name='clear' value='1'>Alles l&ouml;schen</button></form></body></html>";
        request->send(200, "text/html", h);
    }

    void registerWeb(AsyncWebServer *server) {
        server->on("/telegram", HTTP_GET, handleTelegram);
        server->on("/telegram", HTTP_POST, handleTelegram);
    }

    void setup() {
        loadCredentials();
        tgQueue = xQueueCreate(6, sizeof(TgItem));
        xTaskCreate(tgTask, "nodetg", 12288, nullptr, 1, nullptr);
    }

}
