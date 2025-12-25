/* Projet IoT - Surveillance Salinité & Aide à la décision (Maroc)
 * ==============================================================
 * Auteur      : Ouiame Makhoukh
 * Encadrant   : Prof. Kamal AZGHIOU
 * Plateforme  : ESP32 + Capteur TDS Analogique
 * Cloud       : ThingsBoard Community Edition
 * Alertes     : Telegram Bot API
 * * Description : 
 * Ce firmware acquiert les données de conductivité du sol, les filtre pour
 * éliminer le bruit, analyse la situation agronomique en fonction de la
 * région (ex: Saïdia) et envoie des alertes intelligentes. 

*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>

// ============================================================
//                    CONFIGURATION UTILISATEUR
// ============================================================

// --- 1. WiFi & Cloud ---
const char* ssid        = "VOTRE_WIFI";
const char* password    = "VOTRE_MOT_DE_PASSE";
const char* mqtt_server = "demo.thingsboard.io";
const int   mqtt_port   = 1883;
const char* token       = "VOTRE_TOKEN_THINGSBOARD"; 

// --- 2. Telegram ---
const char* bot_token   = "VOTRE_BOT_TOKEN";
const char* chat_id     = "VOTRE_CHAT_ID";

// --- 3. Géographie (CHOISIR VOTRE VILLE ICI) ---
String REGION_CIBLE = "SAIDIA"; 

// --- 4. Calibrage Capteur ---
#define TDS_PIN 34
#define NUM_LECTURES 15
const float VREF = 3.3;
const int ADC_RESOLUTION = 4095;
const float FACTEUR_CONVERSION = 0.5; 
const float MAX_TDS_CAPTEUR = 1000.0;

// --- 5. Seuils (ppm) ---
const float SEUIL_IMMERSION_MIN = 20.0;
const float SEUIL_NORMAL = 400.0;
const float SEUIL_ATTENTION = 700.0;

// Timers
const unsigned long INTERVAL_LECTURE = 5000;
const unsigned long ALERT_REPEAT_INTERVAL = 600000;

/* ==========================================================
                     OBJETS & VARIABLES
========================================================== */

WiFiClient espClient;
PubSubClient mqttClient(espClient);
Preferences prefs;

unsigned long lastSend = 0;
unsigned long lastAlertTime = 0;
String lastState = "";

/* ==========================================================
                     STRUCTURES
========================================================== */

struct RegionInfo {
  String region;
  String climat;
  String cultures;
  String conseil;
};

/* ==========================================================
                     OUTILS
========================================================== */

String urlEncode(String s) {
  String out = "";
  for (char c : s) {
    if (isalnum(c)) out += c;
    else if (c == ' ') out += '+';
    else {
      char buf[4];
      sprintf(buf, "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

/* ==========================================================
                RÉGIONS CÔTIÈRES DU MAROC
========================================================== */

RegionInfo getRegionInfo(String region) {
  region.toUpperCase();
  RegionInfo r;

  if (region == "TANGER" || region == "TETOUAN" || region == "MDIQ" ||
      region == "FNIDEQ" || region == "AL HOCEIMA" || region == "NADOR" ||
      region == "BERKANE" || region == "SAIDIA") {
    r.region = "Nord / Oriental";
    r.climat = "🌦️ Humide côtier";
    r.cultures = "🍊 Agrumes, 🍇 Vigne, 🥔 Pomme de terre";
    r.conseil = "Surveiller remontées salines estivales";
  }
  else if (region == "KENITRA" || region == "RABAT" || region == "SALE" ||
           region == "CASABLANCA" || region == "MOHAMMEDIA") {
    r.region = "Gharb – Atlantique Nord";
    r.climat = "🌤️ Tempéré océanique";
    r.cultures = "🥑 Avocat, 🍓 Fruits rouges";
    r.conseil = "Drainage essentiel en sols lourds";
  }
  else if (region == "EL JADIDA" || region == "SAFI" || region == "ESSAOUIRA") {
    r.region = "Doukkala – Abda";
    r.climat = "🌬️ Venté";
    r.cultures = "🍅 Tomate, 🍈 Melon";
    r.conseil = "Irrigation économe recommandée";
  }
  else if (region == "AGADIR" || region == "TIZNIT" || region == "CHTOUKA") {
    r.region = "Souss-Massa";
    r.climat = "🔥 Chaud";
    r.cultures = "🍅 Tomate serre, 🍌 Banane";
    r.conseil = "Eau saumâtre fréquente – surveiller TDS";
  }
  else if (region == "DAKHLA" || region == "LAAYOUNE" || region == "BOUJDOUR") {
    r.region = "Sahara Atlantique";
    r.climat = "🔥🌬️ Désert";
    r.cultures = "🍈 Melon, 🍅 Tomate cerise";
    r.conseil = "Irrigation de précision obligatoire";
  }
  else {
    r.region = "Maroc (Général)";
    r.climat = "🌍 Variable";
    r.cultures = "🌾 Céréales, 🫒 Olivier";
    r.conseil = "Adapter selon saison";
  }

  return r;
}

/* ==========================================================
                LECTURE TDS (CORRIGÉE)
========================================================== */

float lireTDSFiltered() {
  float samples[NUM_LECTURES];

  for (int i = 0; i < NUM_LECTURES; i++) {
    samples[i] = analogRead(TDS_PIN);
    delay(40);
  }

  // Tri pour médiane
  for (int i = 0; i < NUM_LECTURES - 1; i++) {
    for (int j = i + 1; j < NUM_LECTURES; j++) {
      if (samples[i] > samples[j]) {
        float t = samples[i];
        samples[i] = samples[j];
        samples[j] = t;
      }
    }
  }

  float raw = samples[NUM_LECTURES / 2];
  float voltage = raw * VREF / ADC_RESOLUTION;

  float tds = (133.42 * pow(voltage, 3)
              -255.86 * pow(voltage, 2)
              +857.39 * voltage) * 0.5;

  if (tds < SEUIL_IMMERSION_MIN || voltage < 0.05) return 0.0;

  static float smooth = 0;
  smooth = 0.7 * smooth + 0.3 * tds;

  return smooth;
}

/* ==========================================================
                ÉTAT DU SOL
========================================================== */

String determinerEtat(float tds) {
  if (tds == 0.0) return "NON_IMMERGEE";
  if (tds < SEUIL_NORMAL) return "NORMAL";
  if (tds < SEUIL_ATTENTION) return "ATTENTION";
  return "ALERTE";
}

/* ==========================================================
                CONNEXIONS
========================================================== */

void connecterWiFi() {
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("WiFi connecté");
}

void connecterMQTT() {
  mqttClient.setServer(mqtt_server, mqtt_port);
  while (!mqttClient.connected()) {
    if (mqttClient.connect("ESP32_SALINITE", token, NULL)) {
      Serial.println("MQTT connecté");
    } else {
      delay(2000);
    }
  }
}

/* ==========================================================
                TELEGRAM
========================================================== */

void envoyerTelegram(String message) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String url = "https://api.telegram.org/bot" + String(bot_token) +
               "/sendMessage?chat_id=" + String(chat_id) +
               "&parse_mode=Markdown&text=" + urlEncode(message);

  http.begin(client, url);
  http.GET();
  http.end();
}
String construireMessageAlerte(float tds, String region) {
  RegionInfo ri = getRegionInfo(region);

  String msg = "🚨 *NOUVELLE ALERTE*\n\n";

  msg += " *ALERTE - Sel Élevé, Agis Vite !*\n";
  msg += "📍 " + ri.region + " • " + ri.climat + "\n";
  msg += "📊 Salinité : *" + String(tds, 0) + " ppm* (Critique - Action Urgente)\n";
  msg += "━━━━━━━━━━━━━━━━━━━━\n\n";

  msg += "🎯 *QUE FAIRE MAINTENANT ?*\n";
  msg += "➤ *LESSIVAGE IMMÉDIAT* : Inonde le sol pour chasser le sel\n\n";

  msg += "💧 *Quantité d'eau :*\n";
  msg += "   +30% d'eau (300L/m² minimum)\n\n";

  msg += "📅 *Fréquence :*\n";
  msg += "   2 fois/jour pendant 3 jours,\n";
  msg += "   puis 1 fois/jour pendant 4 jours\n\n";

  msg += "🕐 *Meilleur moment :*\n";
  msg += "   Matin (6h) ET soir (19h)\n";
  msg += "   ⛔ Éviter 10h–16h (évaporation)\n\n";

  msg += "⏰ *URGENCE* : 🚨 IMMÉDIAT (24h)\n";
  msg += "📆 *Durée* : Lessivage intensif 7 jours minimum\n";
  msg += "━━━━━━━━━━━━━━━━━━━━\n\n";

  msg += "🌱 *CULTURES - Quoi Planter ?*\n\n";
  msg += "✅ *Cultures possibles :*\n";
  msg += "   🍠 Betterave, 🌾 Orge, 🥬 Épinard\n\n";

  msg += "❌ *À ÉVITER absolument :*\n";
  msg += "    Salades, Carottes, Oignons, Fraises\n\n";

  msg += "🔮 *Prochaine saison :*\n";
  msg += "   Attendre TDS < 500 ppm avant replantation\n";
  msg += "━━━━━━━━━━━━━━━━━━━━\n\n";

  msg += "👁️ *SURVEILLANCE*\n\n";
  msg += "🔍 Symptômes :\n";
  msg += "   Feuilles brûlées, croissance arrêtée\n\n";

  msg += "✔️ Suivi :\n";
  msg += "   Mesure TDS tous les 2 jours\n";
  msg += "━━━━━━━━━━━━━━━━━━━━\n\n";

  msg += "📍 *Note " + ri.region + " :*\n";
  msg += "   " + ri.conseil + "\n\n";

  msg += "━━━━━━━━━━━━━━━━━━━━\n";
  msg += "🤖 *Système IoT Salinité*";

  return msg;
}

/* ==========================================================
                SETUP
========================================================== */

void setup() {
  Serial.begin(115200);
  pinMode(TDS_PIN, INPUT);

  connecterWiFi();
  connecterMQTT();

  envoyerTelegram("🌍 *Système Salinité Activé*\n📍 Région : " + REGION_CIBLE);
}

/* ==========================================================
                LOOP PRINCIPALE
========================================================== */

void loop() {
  mqttClient.loop();

  if (millis() - lastSend < INTERVAL_LECTURE) return;
  lastSend = millis();

  float tds = lireTDSFiltered();
  String etat = determinerEtat(tds);
  RegionInfo ri = getRegionInfo(REGION_CIBLE);

  Serial.print("TDS: ");
  Serial.print(tds, 1);
  Serial.print(" ppm | État: ");
  Serial.println(etat);

  // MQTT
  String payload = "{";
  payload += "\"tds\":" + String(tds,1);
  payload += ",\"etat\":\"" + etat + "\"";
  payload += ",\"region\":\"" + ri.region + "\"";
  payload += "}";

  mqttClient.publish("v1/devices/me/telemetry", payload.c_str());

  // Telegram sur changement d’état
  if (etat != lastState && etat != "NON_IMMERGEE") {

  // ALERTE : message complet
  if (etat == "ALERTE") {
    String msg = construireMessageAlerte(tds, REGION_CIBLE);
    envoyerTelegram(msg);
    lastAlertTime = millis();
  }

  //  ATTENTION : message résumé
  else if (etat == "ATTENTION") {
    String msg = "⚠️ *ATTENTION – Début de stress salin*\n\n";
    msg += "📍 " + ri.region + "\n";
    msg += "📊 TDS : " + String(tds, 0) + " ppm\n";
    msg += "🧠 Conseil : Augmente légèrement l’irrigation";
    envoyerTelegram(msg);
  }

  lastState = etat;
}


}
