// DL6TJ: Meldungen von eigenen Nodes (Briefkasten, Auto) per APRS-Nachricht empfangen,
// nach Telegram weiterleiten und erst dann per ACK bestaetigen. Ausserdem: Befehle an die Nodes zustellen.
//
// Die Nodes senden APRS-Nachrichten an den iGate, Pfad NOGATE (kein Upload ins APRS-IS, kein Digipeating):
//     DL6TJ-7>APZTJN,NOGATE::DL6TJ-10 :Post ist da! #12 | Batt: 3.92V{1234
// Der iGate antwortet mit "ack1234" -- bei Meldungen, die nach Telegram gehen, erst NACHDEM Telegram sie angenommen hat.
// Der Telegram-Versand laeuft in einer eigenen Task, damit der LoRa-Empfang nicht blockiert.

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

    #define NODE_COUNT      (sizeof(NODE_LIST) / sizeof(NODE_LIST[0]))
    #define SLOT_NONE       255

    struct TgItem {
        char    text[220];
        uint8_t chats;      // Bit0 = Chat 1 (Pflicht), Bit1 = Chat 2 (optional)
        uint8_t slot;       // Eintrag in entries[] oder SLOT_NONE
    };

    enum { ST_FREE = 0, ST_PENDING = 1, ST_DONE = 2, ST_FAILED = 3 };

    struct Entry {
        uint32_t         hash;
        uint32_t         ms;
        volatile uint8_t state;
        bool             acked;
        bool             armAfterAck;
        uint8_t          node;
        char             sender[12];
        char             payload[100];      // zuletzt empfangenes Exemplar (enthaelt die aktuelle Nachrichtennummer)
        char             tgText[220];
    };

    struct Cmd {
        bool     active;
        uint8_t  stage;         // 0 = wartet auf das Befehlsfenster des Nodes, 1 = wird gesendet
        uint8_t  tries;
        uint16_t id;
        uint32_t nextMs;
        uint32_t untilMs;
        char     text[64];
    };

    static QueueHandle_t    tgQueue     = nullptr;
    static String           tgToken, tgChat1, tgChat2;
    static Entry            entries[16];
    static Cmd              cmds[NODE_COUNT];

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

    static bool tgSendOne(const String &chat, const String &text, bool required) {
        if (tgToken == "" || chat == "") return !required;      // nicht eingerichtet: Pflicht-Chat = Fehler, optionaler Chat = uebersprungen
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
            if (!d1 && (tgToken == "" || tgChat1 == "")) {                       // Pflicht-Chat fehlt: gar nicht erst versuchen
                Utils::println("Node-Telegram: nicht eingerichtet (Seite /telegram)");
                if (item.slot < 16) entries[item.slot].state = ST_FAILED;
                continue;
            }
            for (int tries = 0; tries < 3 && !(d1 && d2); tries++) {
                if (tries > 0) vTaskDelay(pdMS_TO_TICKS(8000));
                if (WiFi.status() != WL_CONNECTED) continue;
                if (ESP.getMaxAllocHeap() < 50000) { Utils::println("Node-Telegram: zu wenig Speicher, spaeter erneut"); continue; }
                if (!d1) d1 = tgSendOne(tgChat1, text, true);
                if (!d2) d2 = tgSendOne(tgChat2, text, false);
            }
            bool ok = d1 && d2;
            if (!ok) Utils::println("Node-Telegram: Meldung nicht zugestellt");
            if (item.slot < 16) entries[item.slot].state = ok ? ST_DONE : ST_FAILED;
        }
    }

    static void tgQueueText(const String &text, uint8_t chats, uint8_t slot = SLOT_NONE) {
        if (!tgQueue) return;
        TgItem item;
        memset(&item, 0, sizeof(item));
        text.substring(0, sizeof(item.text) - 1).toCharArray(item.text, sizeof(item.text));
        item.chats = chats;
        item.slot = slot;
        if (xQueueSend(tgQueue, &item, 0) != pdTRUE) {
            Utils::println("Node-Telegram: Warteschlange voll");
            if (slot < 16) entries[slot].state = ST_FAILED;
        }
    }

    static uint32_t fnv(const String &key) {
        uint32_t h = 2166136261UL;
        for (unsigned int i = 0; i < key.length(); i++) { h ^= (uint8_t)key.charAt(i); h *= 16777619UL; }
        return h;
    }

    static String myCall() { return (Config.tacticalCallsign == "") ? Config.callsign : Config.tacticalCallsign; }

    static void sendAck(const char *sender, const char *payload) {
        String ack = APRSPacketLib::generateAckPacket(myCall(), "APLRG1", "RFONLY", String(sender), String(payload));
        if (ack != "") STATION_Utils::addToOutputPacketBuffer(ack);
    }

    static int findEntry(uint32_t h) {
        uint32_t now = millis();
        for (int i = 0; i < 16; i++)
            if (entries[i].state != ST_FREE && entries[i].hash == h && now - entries[i].ms < (uint32_t)NODE_DEDUP_S * 1000UL) return i;
        return -1;
    }

    static int allocEntry() {
        uint32_t now = millis();
        int best = -1;
        for (int i = 0; i < 16; i++) {
            if (entries[i].state == ST_FREE || now - entries[i].ms >= (uint32_t)NODE_DEDUP_S * 1000UL) { best = i; break; }
        }
        if (best < 0) {                                                  // ältesten nicht-laufenden Eintrag nehmen
            uint32_t oldest = 0;
            for (int i = 0; i < 16; i++) {
                if (entries[i].state == ST_PENDING) continue;
                uint32_t age = now - entries[i].ms;
                if (best < 0 || age > oldest) { best = i; oldest = age; }
            }
        }
        if (best < 0) best = 0;
        memset((void *)&entries[best], 0, sizeof(Entry));
        return best;
    }

    static float parseBatt(const String &t) {
        int i = t.indexOf("Batterie:");
        if (i >= 0) return t.substring(i + 9).toFloat();
        i = t.indexOf("Batt:");
        if (i >= 0) return t.substring(i + 5).toFloat();
        return 0.0f;
    }

    // ---- Befehle an Nodes ----
    static void sendCmdNow(int idx) {
        Cmd &c = cmds[idx];
        c.id = 1000 + (esp_random() % 9000);
        String pkt = APRSPacketLib::generateMessagePacket(myCall(), "APLRG1", "RFONLY", String(NODE_LIST[idx].call), String(c.text)) + "{" + String(c.id);
        STATION_Utils::addToOutputPacketBuffer(pkt);
        c.tries++;
        c.nextMs = millis() + 10000UL;
        Utils::println(String("Node-Befehl an ") + NODE_LIST[idx].name + ": " + c.text);
    }

    static void armCmd(int idx) {                                       // der Node hat sein Befehlsfenster geoeffnet
        Cmd &c = cmds[idx];
        if (!c.active || c.stage != 0) return;
        c.stage = 1;
        c.tries = 0;
        c.untilMs = millis() + 70000UL;
        sendCmdNow(idx);
    }

    void processLoRaPacket(const String& packet) {
        if (!NODE_ENABLE) return;
        if (lastAprsPacket.header != "" || lastAprsPacket.type != 1) return;                 // nur direkte APRS-Nachrichten
        String me = myCall();
        if (lastAprsPacket.addressee != me) return;                                           // nicht an uns

        int idx = -1;
        for (size_t i = 0; i < NODE_COUNT; i++) if (lastAprsPacket.sender == NODE_LIST[i].call) { idx = (int)i; break; }
        if (idx < 0) return;
        const NodeDef *node = &NODE_LIST[idx];

        String payload = lastAprsPacket.payload;                                              // "Text{Nr"
        String text = payload;
        int brace = payload.lastIndexOf('{');
        bool wantAck = (brace >= 0);
        if (wantAck) text = payload.substring(0, brace);
        text.trim();

        // Bestaetigung eines Befehls durch den Node
        if (text.startsWith("ack")) {
            if (cmds[idx].active && cmds[idx].stage == 1 && text == "ack" + String(cmds[idx].id)) {
                cmds[idx].active = false;
                Utils::println(String("Node-Befehl bestaetigt von ") + node->name);
            }
            return;
        }

        // Befehlsfenster: der Node hat gerade gemeldet (Start/Status) und hoert jetzt eine Weile zu
        if (text.startsWith("Start") || text.startsWith("Status")) armCmd(idx);

        // Welche Meldung geht nach Telegram, braucht sie eine Bestaetigung erst NACH der Zustellung?
        bool needTg = false;                    // Telegram Pflicht -> ACK erst nach Zustellung
        uint8_t chats = 0;
        bool arm = false;
        if (node->role == 0) {                                                                // Briefkasten
            if (text.startsWith("Post"))        { needTg = true; chats = 3; arm = true; }     // beide Chats; Fenster nach dem ACK
            else if (text.startsWith("Status")) { if (NODE_BRIEF_STATUS_TG) chats = 1; }
            else if (text.startsWith("Start") || text.startsWith("OK") || text.startsWith("Test")) chats = 1;
        } else if (node->role == 1) {                                                         // Auto
            if (text.startsWith("WARN") || text.startsWith("KRITISCH")) { needTg = true; chats = 1; }
            else if (text.indexOf("Spannungen") >= 0) chats = 1;
        }
        String tgText = String(node->name) + ": " + text;

        // Duplikate / Wiederholungen: Schluessel ohne Nachrichtennummer
        uint32_t h = fnv(String(node->call) + "|" + text);
        int e = findEntry(h);
        if (e >= 0) {
            payload.toCharArray(entries[e].payload, sizeof(entries[e].payload));
            if (entries[e].state == ST_DONE) { if (wantAck) sendAck(entries[e].sender, entries[e].payload); }     // erneut bestaetigen (ACK kann verloren gegangen sein)
            else if (entries[e].state == ST_FAILED && needTg) { entries[e].state = ST_PENDING; entries[e].acked = false; tgQueueText(tgText, chats, e); }   // nochmal versuchen
            return;                                                                           // laeuft schon: nichts doppelt
        }

        Utils::println(String("Node ") + node->name + ": " + text);
        e = allocEntry();
        Entry &en = entries[e];
        en.hash = h;
        en.ms = millis() ? millis() : 1;
        en.node = idx;
        en.armAfterAck = arm;
        node->call ? strncpy(en.sender, node->call, sizeof(en.sender) - 1) : (char *)nullptr;
        payload.toCharArray(en.payload, sizeof(en.payload));
        tgText.toCharArray(en.tgText, sizeof(en.tgText));

        if (needTg) {
            en.state = ST_PENDING;                                                            // ACK folgt in loop(), sobald Telegram angenommen hat
            tgQueueText(tgText, chats, e);
        } else {
            en.state = ST_DONE;
            en.acked = true;
            if (wantAck) sendAck(en.sender, en.payload);
            if (chats) tgQueueText(tgText, chats);
        }

        // Akkuwarnung Briefkasten
        float batt = parseBatt(text);
        if (node->role == 0 && batt > 0.0f && batt < NODE_BRIEF_LOW_V) tgQueueText("Batterie Briefkasten niedrig (" + String(batt, 2) + " V)", 1);
    }

    void loop() {
        if (!NODE_ENABLE) return;
        uint32_t now = millis();
        for (int i = 0; i < 16; i++) {                                                        // zugestellt -> jetzt bestaetigen
            Entry &en = entries[i];
            if (en.state == ST_DONE && !en.acked) {
                en.acked = true;
                sendAck(en.sender, en.payload);
                if (en.armAfterAck) armCmd(en.node);                                          // der Node oeffnet nach dem ACK sein Befehlsfenster
            }
        }
        for (size_t i = 0; i < NODE_COUNT; i++) {                                             // Befehle zustellen / aufgeben
            Cmd &c = cmds[i];
            if (!c.active || c.stage != 1) continue;
            if (c.tries >= 4 && now >= c.nextMs) { c.active = false; tgQueueText(String("Befehl an ") + NODE_LIST[i].name + " nicht bestaetigt: " + c.text, 1); }
            else if (c.tries < 4 && now >= c.nextMs) sendCmdNow((int)i);
            else if (now > c.untilMs) { c.active = false; tgQueueText(String("Befehl an ") + NODE_LIST[i].name + " nicht bestaetigt: " + c.text, 1); }
        }
    }

    // ---- Webseite /telegram: Zugangsdaten und Befehle an Nodes ----
    static String esc(const String &s) {
        String o;
        for (unsigned int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '&') o += "&amp;"; else if (c == '"') o += "&quot;"; else o += c;
        }
        return o;
    }

    static void handleTelegram(AsyncWebServerRequest *request) {
        if (Config.webadmin.active && !request->authenticate(Config.webadmin.username.c_str(), Config.webadmin.password.c_str()))
            return request->requestAuthentication();

        String msg = "";
        if (request->method() == HTTP_POST) {
            if (request->hasParam("nodecmd", true)) {
                int n = request->hasParam("node", true) ? request->getParam("node", true)->value().toInt() : 0;
                String t = request->getParam("nodecmd", true)->value();
                t.trim();
                if (n >= 0 && n < (int)NODE_COUNT) {
                    if (t == "") { cmds[n].active = false; msg = "Befehl gelöscht."; }
                    else { memset(&cmds[n], 0, sizeof(Cmd)); t.substring(0, sizeof(cmds[n].text) - 1).toCharArray(cmds[n].text, sizeof(cmds[n].text)); cmds[n].active = true; msg = "Befehl vorgemerkt; wird nach der nächsten Meldung des Nodes zugestellt."; }
                }
            } else {
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
        }
        String h = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>Telegram</title></head><body style='font-family:sans-serif;max-width:520px;margin:20px auto;padding:0 10px'>";
        h += "<h3>Telegram f&uuml;r Node-Meldungen</h3>";
        if (msg != "") h += "<p><b>" + msg + "</b></p>";
        h += "<p>Bot-Token: " + String(tgToken != "" ? "gesetzt" : "<i>nicht gesetzt</i>") + "<br>Chat 1: " + (tgChat1 != "" ? esc(tgChat1) : String("<i>nicht gesetzt</i>")) + "<br>Chat 2: " + (tgChat2 != "" ? esc(tgChat2) : String("<i>nicht gesetzt</i>")) + "</p>";
        h += "<form method='POST' action='/telegram'>";
        h += "Bot-Token (leer = unver&auml;ndert)<br><input name='tok' type='password' style='width:100%'><br><br>";
        h += "Chat 1 &ndash; alle Meldungen (leer = unver&auml;ndert)<br><input name='c1' style='width:100%'><br><br>";
        h += "Chat 2 &ndash; nur &bdquo;Post ist da&ldquo; vom Briefkasten (leer = unver&auml;ndert)<br><input name='c2' style='width:100%'><br><br>";
        h += "<button type='submit'>Speichern</button> ";
        h += "<button type='submit' name='test' value='1'>Speichern + Test senden</button> ";
        h += "<button type='submit' name='clear' value='1'>Alles l&ouml;schen</button></form>";

        h += "<hr><h3>Befehl an einen Node</h3>";
        h += "<p style='font-size:13px'>Wird nach der n&auml;chsten Start-/Status-/Post-Meldung des Nodes in dessen Befehlsfenster gesendet. "
             "Briefkasten: <code>TX=6</code> <code>CHK=5</code> <code>ST=1440</code> <code>REM=0</code> <code>WIN=60</code> <code>TEST</code> <code>TEST=300</code> <code>DEF</code> <code>?</code> &ndash; mehrere mit Leerzeichen trennen.</p>";
        for (size_t i = 0; i < NODE_COUNT; i++) {
            if (cmds[i].active) h += String("<p>Vorgemerkt f&uuml;r ") + NODE_LIST[i].name + ": <code>" + esc(String(cmds[i].text)) + "</code>" + (cmds[i].stage == 1 ? " (wird gesendet)" : " (wartet)") + "</p>";
        }
        h += "<form method='POST' action='/telegram'><select name='node'>";
        for (size_t i = 0; i < NODE_COUNT; i++) h += String("<option value='") + String((int)i) + "'>" + NODE_LIST[i].name + " (" + NODE_LIST[i].call + ")</option>";
        h += "</select> <input name='nodecmd' placeholder='z. B. TX=6' style='width:55%'> <button type='submit'>Vormerken</button></form>";
        h += "<p style='font-size:13px'>Leeres Feld + Vormerken l&ouml;scht einen vorgemerkten Befehl. Die Antwort des Nodes kommt als Telegram-Nachricht.</p>";
        h += "</body></html>";
        request->send(200, "text/html", h);
    }

    void registerWeb(AsyncWebServer *server) {
        server->on("/telegram", HTTP_GET, handleTelegram);
        server->on("/telegram", HTTP_POST, handleTelegram);
    }

    void setup() {
        loadCredentials();
        memset((void *)entries, 0, sizeof(entries));
        memset((void *)cmds, 0, sizeof(cmds));
        tgQueue = xQueueCreate(6, sizeof(TgItem));
        xTaskCreate(tgTask, "nodetg", 12288, nullptr, 1, nullptr);
    }

}
