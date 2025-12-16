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
const float SEUIL_NORMAL        = 400.0;
const float SEUIL_ATTENTION     = 700.0;
const float SEUIL_IMMERSION_MIN = 150.0; 

// ============================================================
//                    VARIABLES GLOBALES
// ============================================================
const long intervalEnvoi = 5000; 
unsigned long dernierEnvoi = 0;
const unsigned long ALERT_REPEAT_INTERVAL = 10UL * 60UL * 1000UL; // 10 min

// Analyse de tendance
const int TREND_WINDOW = 6;
float trendWindow[TREND_WINDOW];
int trendIndex = 0;
bool trendFilled = false;

// Objets
WiFiClient espClient;
PubSubClient mqttClient(espClient);
Preferences prefs;

// États & Mémoire
String lastState = "";
unsigned long lastAlertTime = 0;

// ============================================================
//                    STRUCTURES DE DONNÉES
// ============================================================

struct RegionInfo {
  String region;
  String climat;
  String culturesAdaptées;
  String conseilsIrrigation;
};

struct DecisionAgronomique {
  // DIAGNOSTIC
  String emoji_etat;
  String titre_etat;
  float tds;
  String niveau_gravite;
  
  // ACTIONS IMMÉDIATES
  String action_prioritaire;
  String quantite_eau;
  String frequence;
  String moment_optimal;
  
  // CULTURES
  String cultures_ok;
  String cultures_danger;
  String cultures_prochaine_saison;
  
  // TIMELINE
  String urgence;
  String delai_action;
  String duree_traitement;
  
  // SURVEILLANCE
  String symptomes_plantes;
  String quoi_verifier;
};

// ============================================================
//                    FONCTIONS UTILITAIRES
// ============================================================

String urlEncode(String s) {
  String out = "";
  for (size_t i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (isalnum(c)) out += c;
    else if (c == ' ') out += '+';
    else {
      const char* hex = "0123456789ABCDEF";
      out += '%';
      out += hex[(c >> 4) & 0xF];
      out += hex[c & 0xF];
    }
  }
  return out;
}

// ============================================================
//               INTELLIGENCE GÉOGRAPHIQUE (MAROC)
// ============================================================

RegionInfo getRegionInfo(String region) {
  RegionInfo info;
  region.toUpperCase();  

  if (region == "TANGER" || region == "TETOUAN" || region == "MDIQ" || 
      region == "FNIDEQ" || region == "AL HOCEIMA" || region == "NADOR" || 
      region == "BERKANE" || region == "SAIDIA") 
  {
    info.region = "Nord / Oriental";
    info.climat = "🌦️ Humide";
    info.culturesAdaptées = "🍊 Agrumes, 🍇 Vigne";
    info.conseilsIrrigation = "Attention aux remontées salines l'été";
  }
  else if (region == "KENITRA" || region == "RABAT" || region == "SALE" || 
           region == "MOHAMMEDIA" || region == "CASABLANCA") 
  {
    info.region = "Côte Nord (Gharb)";
    info.climat = "🌤️ Tempéré";
    info.culturesAdaptées = "🍓 Fruits rouges, 🥑 Avocat";
    info.conseilsIrrigation = "Drainage important requis";
  }
  else if (region == "EL JADIDA" || region == "SAFI" || region == "ESSAOUIRA") 
  {
    info.region = "Doukkala / Abda";
    info.climat = "🌤️ Venté";
    info.culturesAdaptées = "🍅 Tomate champ, 🍈 Melon";
    info.conseilsIrrigation = "Eau rare, économisez-la";
  }
  else if (region == "AGADIR" || region == "TIZNIT" || region == "CHTOUKA") 
  {
    info.region = "Souss-Massa";
    info.climat = "🔥 Chaud";
    info.culturesAdaptées = "🍅 Tomate serre, 🍌 Banane";
    info.conseilsIrrigation = "Eau saumâtre fréquente";
  }
  else if (region == "LAAYOUNE" || region == "DAKHLA" || region == "BOUJDOUR") 
  {
    info.region = "Sahara (Dakhla)";
    info.climat = "🔥🌬️ Désert";
    info.culturesAdaptées = "🍅 Tomate Cherry, 🍈 Melon";
    info.conseilsIrrigation = "Irrigation de précision vitale";
  }
  else {
    info.region = "Maroc (Général)";
    info.climat = "🌍 Variable";
    info.culturesAdaptées = "Céréales, Olivier";
    info.conseilsIrrigation = "Adapter selon la saison";
  }
  return info;
}

// ============================================================
//         GÉNÉRATION DES RECOMMANDATIONS DÉTAILLÉES
// ============================================================

DecisionAgronomique genererDecision(float tds, String region) {
  DecisionAgronomique dec;
  dec.tds = tds;
  
  // Cas 1 : Normal (< 400)
  if (tds < 400.0) {
    dec.emoji_etat = "🟢";
    dec.titre_etat = "SOL SAIN - Conditions Optimales";
    dec.niveau_gravite = "Normal";
    dec.action_prioritaire = "Continue ton irrigation habituelle";
    dec.quantite_eau = "Dose normale (selon ta culture)";
    dec.frequence = "Selon besoins de la plante";
    dec.moment_optimal = "Matin (7h-9h) ou soir (18h-20h)";
    dec.cultures_ok = "🥕 TOUT ! Carotte, Fraise, Oignon, Laitue, Haricot, Concombre";
    dec.cultures_danger = "Aucune restriction";
    dec.cultures_prochaine_saison = "Profite pour planter des cultures délicates (Fraise, Laitue)";
    dec.urgence = "Situation stable";
    dec.delai_action = "Pas d'urgence";
    dec.duree_traitement = "Continue normalement";
    dec.symptomes_plantes = "Aucun symptôme attendu - plantes saines";
    dec.quoi_verifier = "Rien de spécial, juste l'entretien habituel";
  }
  // Cas 2 : Attention (400 - 700)
  else if (tds < 700.0) {
    dec.emoji_etat = "🟡";
    dec.titre_etat = "ATTENTION - Début de Stress Salin";
    dec.niveau_gravite = "Préoccupant - Surveiller";
    dec.action_prioritaire = "Augmente légèrement l'irrigation pour diluer le sel";
    dec.quantite_eau = "+10% d'eau par rapport à d'habitude";
    dec.frequence = "Arrose tous les 2 jours (au lieu de 3)";
    dec.moment_optimal = "Matin (6h-8h) - JAMAIS en plein soleil";
    dec.cultures_ok = "🍅 Tomate, 🌽 Maïs, 🥬 Chou, 🥔 Pomme de terre";
    dec.cultures_danger = "🛑 Évite : Fraise, Haricot vert, Laitue (trop sensibles)";
    dec.cultures_prochaine_saison = "Prépare un lessivage pour la saison prochaine";
    dec.urgence = "Cette semaine";
    dec.delai_action = "Commence dès demain matin";
    dec.duree_traitement = "Continue 2 semaines, puis réévalue";
    dec.symptomes_plantes = "Feuilles avec bords secs/jaunâtres";
    dec.quoi_verifier = "Vérifie le BOUT des feuilles chaque matin (premiers signes)";
  }
  // Cas 3 : Alerte (700 - 1000)
  else if (tds < 1000.0) {
    dec.emoji_etat = "🟠";
    dec.titre_etat = "ALERTE - Sel Élevé, Agis Vite !";
    dec.niveau_gravite = "Critique - Action Urgente";
    dec.action_prioritaire = "LESSIVAGE IMMÉDIAT : Inonde le sol pour chasser le sel";
    dec.quantite_eau = "+30% d'eau (300L/m² minimum)";
    dec.frequence = "2 fois par jour pendant 3 jours, puis 1x/jour pendant 4 jours";
    dec.moment_optimal = "Matin (6h) ET soir (19h) - Évite 10h-16h (évaporation)";
    dec.cultures_ok = "🍠 Betterave, 🌾 Orge, 🥬 Épinard (tolérantes au sel)";
    dec.cultures_danger = "🛑 STOP TOUT : Salades, Carottes, Oignons, Fraises (vont mourir)";
    dec.cultures_prochaine_saison = "Attends que le TDS descende < 500 ppm avant de replanter";
    dec.urgence = "🚨 IMMÉDIAT - Dans les 24h";
    dec.delai_action = "Agis AUJOURD'HUI même";
    dec.duree_traitement = "Lessivage intensif : 7 jours minimum";
    dec.symptomes_plantes = "Feuilles brûlées, croissance arrêtée, flétrissement";
    dec.quoi_verifier = "Mesure le TDS tous les 2 jours pour voir si ça descend";
  }
  // Cas 4 : Danger (> 1000)
  else {
    dec.emoji_etat = "🔴";
    dec.titre_etat = "DANGER - Sol Toxique, Culture Impossible";
    dec.niveau_gravite = "Catastrophique - Intervention d'Expert";
    dec.action_prioritaire = "DRAINAGE + AMENDEMENT : Pose des drains ET ajoute du Gypse";
    dec.quantite_eau = "Inondation massive (500L/m²) APRÈS avoir posé les drains";
    dec.frequence = "Drainage continu pendant 2 semaines";
    dec.moment_optimal = "Travaux de jour (8h-17h) - contacte un agronome";
    dec.cultures_ok = "🌴 Seulement Palmier dattier (ultra-tolérant)";
    dec.cultures_danger = "🚫 AUCUNE culture maraîchère possible - sol toxique";
    dec.cultures_prochaine_saison = "Réhabilitation du sol : 3-6 mois minimum";
    dec.urgence = "🚑 URGENCE ABSOLUE";
    dec.delai_action = "Appelle un expert MAINTENANT (contacte l'ORMVA)";
    dec.duree_traitement = "Réhabilitation : 3 à 6 mois";
    dec.symptomes_plantes = "Plantes mortes ou mourantes - croûte de sel visible";
    dec.quoi_verifier = "Ne plante RIEN avant que TDS < 700 ppm";
  }
  
  return dec;
}

// ============================================================
//         CONSTRUCTION DU MESSAGE TELEGRAM COMPLET
// ============================================================

String construireMessageDecision(float tds, String region, float percentChange = 0.0) {
  DecisionAgronomique dec = genererDecision(tds, region);
  RegionInfo ri = getRegionInfo(region);
  
  String msg = "";
  
  // EN-TÊTE
  msg += dec.emoji_etat + " *" + dec.titre_etat + "*\n";
  msg += "📍 " + ri.region + " • " + ri.climat + "\n";
  msg += "📊 Salinité : *" + String(tds, 0) + " ppm* (" + dec.niveau_gravite + ")\n";
  
  // Tendance si disponible
  if (percentChange != 0.0) {
    if (percentChange > 0) {
      msg += "📈 Tendance : +" + String(percentChange, 1) + "% (hausse)\n";
    } else {
      msg += "📉 Tendance : " + String(percentChange, 1) + "% (baisse)\n";
    }
  }
  
  msg += "━━━━━━━━━━━━━━━━━━━━\n\n";
  
  // SECTION 1 : ACTION IMMÉDIATE
  msg += "🎯 *QUE FAIRE MAINTENANT ?*\n";
  msg += "➤ " + dec.action_prioritaire + "\n\n";
  
  msg += "💧 *Quantité d'eau :*\n";
  msg += "   " + dec.quantite_eau + "\n\n";
  
  msg += "📅 *Fréquence :*\n";
  msg += "   " + dec.frequence + "\n\n";
  
  msg += "🕐 *Meilleur moment :*\n";
  msg += "   " + dec.moment_optimal + "\n\n";
  
  msg += "⏰ *URGENCE :* " + dec.urgence + "\n";
  msg += "⏳ *Délai :* " + dec.delai_action + "\n";
  msg += "📆 *Durée :* " + dec.duree_traitement + "\n";
  msg += "━━━━━━━━━━━━━━━━━━━━\n\n";
  
  // SECTION 2 : CULTURES
  msg += "🌱 *CULTURES - Quoi Planter ?*\n\n";
  
  msg += "✅ *Cultures possibles :*\n";
  msg += "   " + dec.cultures_ok + "\n\n";
  
  msg += "❌ *À ÉVITER absolument :*\n";
  msg += "   " + dec.cultures_danger + "\n\n";
  
  msg += "🔮 *Prochaine saison :*\n";
  msg += "   " + dec.cultures_prochaine_saison + "\n";
  msg += "━━━━━━━━━━━━━━━━━━━━\n\n";
  
  // SECTION 3 : SURVEILLANCE
  msg += "👁️ *SURVEILLANCE*\n\n";
  
  msg += "🔍 *Symptômes à observer :*\n";
  msg += "   " + dec.symptomes_plantes + "\n\n";
  
  msg += "✔️ *Action de suivi :*\n";
  msg += "   " + dec.quoi_verifier + "\n";
  msg += "━━━━━━━━━━━━━━━━━━━━\n\n";
  
  // AIDE si critique
  if (tds >= 1000.0) {
    msg += "📞 *BESOIN D'AIDE ?*\n";
    msg += "   • ORMVA Oriental\n";
    msg += "   • Centre Conseil Agricole\n";
    msg += "   • Agronome de proximité\n";
    msg += "━━━━━━━━━━━━━━━━━━━━\n\n";
  }
  
  // Note régionale si nécessaire
  if (tds > 400.0) {
    msg += "📍 *Note " + ri.region + " :*\n";
    msg += "   " + ri.conseilsIrrigation + "\n\n";
  }
  
  // Pied de page
  msg += "━━━━━━━━━━━━━━━━━━━━\n";
  msg += "🤖 Système IoT Salinité ";
  
  return msg;
}

// ============================================================
//                  FILTRAGE & LECTURE
// ============================================================

float medianOfArray(float *a, int n) {
  for (int i = 0; i < n - 1; ++i) {
    for (int j = i + 1; j < n; ++j) {
      if (a[j] < a[i]) {
        float tmp = a[i]; a[i] = a[j]; a[j] = tmp;
      }
    }
  }
  return a[n/2];
}

float lireTDSFiltered() {
  float mesures[NUM_LECTURES];
  float minVal = 10000.0; 
  float maxVal = -10000.0;

  for (int i = 0; i < NUM_LECTURES; ++i) {
    int raw = analogRead(TDS_PIN);
    float tension = (raw * VREF) / ADC_RESOLUTION;
    mesures[i] = tension * 1000.0 * FACTEUR_CONVERSION;
    
    if (mesures[i] < minVal) minVal = mesures[i];
    if (mesures[i] > maxVal) maxVal = mesures[i];
    delay(20);
  }
  
  if ((maxVal - minVal) > 300.0) return 0.0;

  float med = medianOfArray(mesures, NUM_LECTURES);
  if (med < SEUIL_IMMERSION_MIN) return 0.0;
  
  if (med > MAX_TDS_CAPTEUR) med = MAX_TDS_CAPTEUR;
  
  static float smoothed = 0.0;
  smoothed = 0.7 * smoothed + 0.3 * med;
  return smoothed;
}

// ============================================================
//                  WIFI & MQTT
// ============================================================

void connecterWiFi() {
  Serial.print("Connexion WiFi");
  WiFi.begin(ssid, password);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    Serial.print("."); delay(500); tries++;
  }
  if (WiFi.status() == WL_CONNECTED) Serial.println(" ✅");
  else Serial.println(" ❌ WiFi Erreur");
}

void connecterMQTT() {
  if (mqttClient.connected()) return;
  mqttClient.setServer(mqtt_server, mqtt_port);
  while (!mqttClient.connected()) {
    Serial.print("Connexion MQTT...");
    if (mqttClient.connect("ESP32_Projet_S3", token, NULL)) {
      Serial.println(" ✅ Connecté");
    } else {
      Serial.print(" ❌ Code: "); Serial.println(mqttClient.state());
      delay(2000);
    }
  }
}

// ============================================================
//     FONCTION MQTT (SIMPLIFIÉE - SANS GPS)
// ============================================================

void envoyerDonneesMQTT(float tds, String etat, float tendance) {
  RegionInfo ri = getRegionInfo(REGION_CIBLE);
  DecisionAgronomique dec = genererDecision(tds, REGION_CIBLE);

  // Construction JSON sans coordonnées GPS
  String payload = "{";
  payload += "\"tds\":" + String(tds, 1);
  payload += ",\"etat\":\"" + etat + "\"";
  payload += ",\"region\":\"" + ri.region + "\""; 
  payload += ",\"tendance\":" + String(tendance, 1);
  payload += ",\"conseil\":\"" + dec.action_prioritaire + "\""; 
  payload += "}";
  
  mqttClient.publish("v1/devices/me/telemetry", payload.c_str());
}

// ============================================================
//                  TELEGRAM
// ============================================================

void envoyerTelegramRaw(String message) {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  
  String url = "https://api.telegram.org/bot" + String(bot_token) + 
               "/sendMessage?chat_id=" + String(chat_id) + 
               "&text=" + urlEncode(message) + 
               "&parse_mode=Markdown";
               
  http.begin(client, url);
  int httpCode = http.GET();
  
  if (httpCode > 0) {
    Serial.println("✅ Telegram envoyé");
  } else {
    Serial.println("❌ Erreur Telegram");
  }
  
  http.end();
}

void envoyerAlerteTelegramAmelioree(float tds, float percentChange) {
  String msg = "🚨 *NOUVELLE ALERTE*\n\n";
  msg += construireMessageDecision(tds, REGION_CIBLE, percentChange);
  envoyerTelegramRaw(msg);
}

void envoyerConseilNormalAmelioree(float tds, float percentChange, String titre = "ℹ️ *BULLETIN QUOTIDIEN*") {
  String msg = titre + "\n\n";
  msg += construireMessageDecision(tds, REGION_CIBLE, percentChange);
  envoyerTelegramRaw(msg);
}

// ============================================================
//                  GESTION TENDANCE
// ============================================================

void pushTrendWindow(float val) {
  trendWindow[trendIndex] = val;
  trendIndex = (trendIndex + 1) % TREND_WINDOW;
  if (!trendFilled && trendIndex == 0) trendFilled = true;
}

float calculerTendance() {
  if (!trendFilled) return 0.0;
  
  float sum = 0;
  for(int i = 0; i < TREND_WINDOW; i++) {
    sum += trendWindow[i];
  }
  float moyenne = sum / TREND_WINDOW;
  
  float derniereMesure = trendWindow[(trendIndex - 1 + TREND_WINDOW) % TREND_WINDOW];
  
  if (moyenne > 0) {
    return ((derniereMesure - moyenne) / moyenne) * 100.0;
  }
  return 0.0;
}

// ============================================================
//                  SETUP & LOOP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║  Système Salinité Maroc                  ║");
  Serial.println("║  Version Finale                          ║");
  Serial.println("╚════════════════════════════════════════╝");
  Serial.print("📍 Région : "); Serial.println(REGION_CIBLE);

  pinMode(TDS_PIN, INPUT);
  
  // Charger dernière valeur
  prefs.begin("salinite", true);
  float lastTDS = prefs.getFloat("lastTDS", 0.0);
  Serial.print("💾 Dernière mesure : "); Serial.print(lastTDS); Serial.println(" ppm");
  prefs.end();

  connecterWiFi();
  connecterMQTT();
  
  // Message de démarrage
  String startMsg = "🌍 *Système Activé*\n\n";
  startMsg += "📍 Région : " + REGION_CIBLE + "\n";
  startMsg += "Prêt à surveiller votre sol !";
  envoyerTelegramRaw(startMsg);
  
  Serial.println("Système prêt !\n");
}

void loop() {
  if (!mqttClient.connected()) connecterMQTT();
  mqttClient.loop();

  unsigned long now = millis();
  if (now - dernierEnvoi >= intervalEnvoi) {
    dernierEnvoi = now;

    // 1. Lecture brute
    float tds = lireTDSFiltered(); 
    
    // 2. Détermination de l'état INSTANTANÉ (Candidat)
    String etatInstantan = "";
    if (tds == 0.0) etatInstantan = "NON_IMMERGEE";
    else if (tds < SEUIL_NORMAL) etatInstantan = "NORMAL";
    else if (tds < SEUIL_ATTENTION) etatInstantan = "ATTENTION";
    else etatInstantan = "ALERTE";

    // 3. Calcul de la tendance (sur les valeurs brutes)
    if (tds > 0) pushTrendWindow(tds);
    float percentChange = calculerTendance();

    // 4. Envoi MQTT (On envoie toujours la donnée brute au Dashboard pour voir ce qui se passe)
    // Note : On envoie l'état instantané pour le temps réel
    Serial.print("📊 TDS: "); Serial.print(tds, 1); 
    Serial.print(" | Brut: "); Serial.print(etatInstantan);
    
    envoyerDonneesMQTT(tds, etatInstantan, percentChange);

    // ═══════════════════════════════════════════════════════
    //        FILTRE DE STABILITÉ (ANTI-FAUX POSITIFS)
    // ═══════════════════════════════════════════════════════
    static String etatCandidat = "";
    static int compteurStabilite = 0;
    
    // Si l'état change par rapport à la dernière lecture (ex: bruit dans l'air)
    if (etatInstantan != etatCandidat) {
        Serial.println(" -> ⏳ Instable (Attente confirmation...)");
        etatCandidat = etatInstantan; // On mémorise ce nouvel état potentiel
        compteurStabilite = 0;        // On reset le compteur
        return;                       // ON SORT : Pas de Telegram tant que ce n'est pas stable !
    } else {
        compteurStabilite++; // L'état se maintient
    }

    // Il faut que l'état soit identique au moins 1 fois de suite (Confirmation)
    if (compteurStabilite < 1) return; 

    // SI ON ARRIVE ICI, L'ÉTAT EST CONFIRMÉ ET STABLE
    String etatStable = etatInstantan;
    Serial.println(" -> ✅ Confirmé");

    // ═══════════════════════════════════════════════════════
    //          LOGIQUE DE NOTIFICATION TELEGRAM
    // ═══════════════════════════════════════════════════════

    // 1. Détection de changement d'état (sur l'état STABLE uniquement)
    if (etatStable != lastState) {
       
       // On ignore le passage à "NON_IMMERGEE" pour les alertes (quand on sort la sonde)
       if (etatStable != "NON_IMMERGEE") {
           Serial.println("🔔 Changement d'état validé ! Envoi Telegram...");
           
           if (etatStable == "ALERTE") {
              envoyerAlerteTelegramAmelioree(tds, percentChange);
           } else {
              if (lastState != "" && lastState != "NON_IMMERGEE") {
                  envoyerConseilNormalAmelioree(tds, percentChange, "📢 *RETOUR À LA NORMALE*");
              }
           }
           lastAlertTime = now;
       }
       
       lastState = etatStable; // Mise à jour de la mémoire
    }

    // 2. Rappels périodiques (Seulement si ALERTE confirmée)
    if (etatStable == "ALERTE") {
       if (now - lastAlertTime >= ALERT_REPEAT_INTERVAL) {
          Serial.println("⏰ Rappel d'alerte envoyé");
          envoyerAlerteTelegramAmelioree(tds, percentChange);
          lastAlertTime = now;
       }
    } 
    // Bulletin périodique pour les autres états (Optionnel, ici désactivé ou long)
    else if (etatStable == "NORMAL" || etatStable == "ATTENTION") {
       static unsigned long lastAdvice = 0;
       // Toutes les 30 min (30*60*1000)
       if (now - lastAdvice > 1800000UL) { 
          envoyerConseilNormalAmelioree(tds, percentChange, "ℹ️ *BULLETIN PÉRIODIQUE*");
          lastAdvice = now;
       }
    }
    
    // Sauvegarde Persistence
    static unsigned long lastSave = 0;
    if (now - lastSave > 60000 && tds > 0) {
       prefs.begin("salinite", false);
       prefs.putFloat("lastTDS", tds);
       prefs.end();
       lastSave = now;
    }
  }
}