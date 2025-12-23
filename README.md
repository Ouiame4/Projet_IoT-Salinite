# 🌱 Système IoT de Surveillance de Salinité & Aide à la Décision (Maroc)

Ce projet est une solution IoT complète ("End-to-End") pour surveiller la salinité des sols agricoles dans les zones côtières marocaines (ex: Saïdia). Il combine l'acquisition de données physiques, le traitement local (Edge Computing) et une intelligence agronomique pour fournir des recommandations actionnables via Telegram.

## Fonctionnalités Clés

- **Surveillance Temps Réel :** Mesure continue de la Conductivité Électrique (EC) et conversion en TDS (ppm).

- **Edge Computing (ESP32) :**

  - Filtrage Numérique : Filtre médian et lissage exponentiel pour éliminer le bruit des capteurs low-cost.
  
  - Machine à États : Gestion intelligente des notifications pour éviter le "spam" d'alertes.
  
  - Intelligence Contextuelle : Adaptation des seuils et conseils selon la région géographique configurée.
  
  - Système Expert Embarqué : Génération de conseils précis (Irrigation, Lessivage, Amendement) sans dépendre du Cloud.

- **Dashboard Cloud :** Visualisation historique et temps réel sur ThingsBoard.

- **Alertes Mobiles :** Notifications riches via Telegram avec émojis et plans d'action.

## Architecture Matérielle

- **Microcontrôleur :** ESP32 DevKit V1 (Wi-Fi intégré).

- **Capteur :** Sonde TDS analogique (Total Dissolved Solids).

- **Alimentation :** 5V / 3.3V via Micro-USB.

## Installation & Configuration

### 1. Prérequis

Arduino IDE avec le support ESP32 installé.

Bibliothèques nécessaires (à installer via le Gestionnaire de bibliothèques) :
```bash
PubSubClient (Client MQTT)

WiFi (Standard ESP32)

HTTPClient & WiFiClientSecure (Standard ESP32)

Preferences (Standard ESP32)
```
### 2. Configuration du Firmware

Ouvrez le fichier source et modifiez la section CONFIGURATION UTILISATEUR avec vos propres identifiants :

```bash
// --- 1. WiFi & Cloud ---
const char* ssid        = "VOTRE_WIFI";
const char* password    = "VOTRE_MOT_DE_PASSE";
const char* token       = "VOTRE_TOKEN_THINGSBOARD"; 

// --- 2. Telegram ---
const char* bot_token   = "VOTRE_BOT_TOKEN";
const char* chat_id     = "VOTRE_CHAT_ID";

// --- 3. Géographie ---
String REGION_CIBLE = "SAIDIA"; // Choix : SAIDIA, AGADIR, DAKHLA...
```

### 3. Branchement

Sonde TDS (Signal) -> Broche 34 (Analog Input) de l'ESP32.

VCC -> 3.3V

GND -> GND

Format des Données (JSON)

Le système publie les données sur le topic MQTT : v1/devices/me/telemetry

```bash
{
  "tds": 845.2,
  "etat": "ALERTE",
  "region": "Nord / Oriental",
  "tendance": 12.5,
  "conseil": "LESSIVAGE IMMÉDIAT"
}
```

Auteur

Ouiame Makhoukh Élève Ingénieure en Data Science & Cloud Computing à l'ENSAO.

Projet réalisé dans le cadre du module IoT - Encadré par Prof. Kamal AZGHIOU.
