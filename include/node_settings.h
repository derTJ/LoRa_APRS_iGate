// DL6TJ: Einstellungen fuer die Node-Meldungen (Briefkasten, Auto) -> Telegram
// Zugangsdaten (Bot-Token, Chat-IDs) stehen NICHT hier, sondern werden im iGate ueber die Webseite /telegram eingetragen.
#ifndef NODE_SETTINGS_H_
#define NODE_SETTINGS_H_

#define NODE_ENABLE             true

// Erlaubte Absender. role: 0 = Briefkasten, 1 = Auto
struct NodeDef { const char *call; const char *name; int role; };
static const NodeDef NODE_LIST[] = {
    { "DL6TJ-7", "Briefkasten", 0 },
    { "DL6TJ-6", "Auto",        1 },
};

#define NODE_BRIEF_LOW_V        3.3f    // Briefkasten: Warnung bei Akku unter diesem Wert
#define NODE_BRIEF_STATUS_TG    true    // "Status"-Meldungen des Briefkastens an Telegram (nur Chat 1)
// Zeitzone fuer den Zeitabgleich der Nodes (Briefkasten): Stunden gegenueber UTC (1 = MEZ), EU-Sommerzeit automatisch
#define NODE_TZ_BASE_H          1
#define NODE_TZ_EU_DST          true
#define NODE_DEDUP_S            600     // gleiche Meldung (Absender + Text + Nachrichtennummer) innerhalb dieser Zeit nur einmal weiterleiten

#endif
