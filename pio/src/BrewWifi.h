#pragma once

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include <string.h>
#include "BrewProtocol.h"

extern "C" {
  #include <user_interface.h>
}

// Usados somente se nao houver SSID salvo no portal. Devem coincidir com
// BEER_AP_SSID / BEER_AP_PASSWORD do ESP32 (nunca com a rede externa).
static constexpr char brewDefaultSsid[] = "BeerMonitor";
static constexpr char brewDefaultPassword[] = "beer-monitor";
static constexpr uint8_t brewDensityUnit = BREW_UNIT_UNKNOWN;
static uint32_t brewBootId = 0;
static uint32_t brewSequence = 0;

static bool brewReadConfirmation(HTTPClient &http, const BrewTelemetry &packet) {
  const int size = http.getSize();
  if (size <= 0 || size > 256) return false;
  const String response = http.getString();
  if (response.length() != static_cast<size_t>(size)) return false;
  StaticJsonDocument<256> ack;
  if (deserializeJson(ack, response, DeserializationOption::NestingLimit(2)) ||
      !ack.is<JsonObject>()) return false;
  return ack["accepted"].is<bool>() && ack["accepted"].as<bool>() &&
         ack["version"].is<uint32_t>() && ack["version"].as<uint32_t>() == BREW_VERSION &&
         ack["sensor_id"].is<uint32_t>() && ack["sensor_id"].as<uint32_t>() == packet.header.sensorId &&
         ack["boot_id"].is<uint32_t>() && ack["boot_id"].as<uint32_t>() == packet.header.bootId &&
         ack["sequence"].is<uint32_t>() && ack["sequence"].as<uint32_t>() == packet.header.sequence;
}

static bool brewSendReading(float temperatureC, float tiltDegrees,
                            float gravity, float batteryV,
                            const char *savedSsid, const char *savedPassword) {
  if (!isfinite(temperatureC) || !isfinite(tiltDegrees) ||
      !isfinite(gravity) || !isfinite(batteryV)) {
    Serial.println("Leitura nao finita; envio HTTP cancelado.");
    return false;
  }
  const bool haveSavedNetwork = savedSsid && savedSsid[0];
  const char *ssid = haveSavedNetwork ? savedSsid : brewDefaultSsid;
  const char *password = haveSavedNetwork ? savedPassword : brewDefaultPassword;
  if (!password || strlen(ssid) > 32 ||
      strlen(password) < 8 || strlen(password) > 63) {
    Serial.println("Configure SSID e senha WPA2 do ESP32 no portal do iSpindel.");
    return false;
  }
  WiFi.persistent(false);
  WiFi.setAutoConnect(false);
  WiFi.setAutoReconnect(false);
  if (!WiFi.mode(WIFI_STA) ||
      !WiFi.config(IPAddress(0U), IPAddress(0U), IPAddress(0U))) {
    Serial.println("Falha ao ativar Wi-Fi/DHCP.");
    return false;
  }

  if (!brewBootId) {
    brewBootId = os_random();
    if (!brewBootId) brewBootId = 1;
  }
  BrewTelemetry packet = {};
  packet.header.magic = BREW_MAGIC;
  packet.header.version = BREW_VERSION;
  packet.header.type = BREW_TELEMETRY;
  packet.header.gravityUnit = brewDensityUnit;
  packet.header.sensorId = ESP.getChipId();
  packet.header.bootId = brewBootId;
  if (++brewSequence == 0) ++brewSequence;
  packet.header.sequence = brewSequence;
  packet.temperatureC = temperatureC;
  packet.tiltDegrees = tiltDegrees;
  packet.gravity = gravity;
  packet.batteryV = batteryV;

  StaticJsonDocument<384> doc;
  doc["version"] = packet.header.version;
  doc["sensor_id"] = packet.header.sensorId;
  doc["boot_id"] = packet.header.bootId;
  doc["sequence"] = packet.header.sequence;
  doc["gravity_unit"] = packet.header.gravityUnit;
  doc["temperature_c"] = temperatureC;
  doc["tilt_degrees"] = tiltDegrees;
  doc["gravity"] = gravity;
  doc["battery_v"] = batteryV;
  char body[BREW_HTTP_BODY_MAX];
  if (doc.overflowed() || measureJson(doc) >= sizeof(body)) return false;
  const size_t bodySize = serializeJson(doc, body, sizeof(body));

  // Duas associacoes limitadas: uma mudanca de canal durante o envio provoca
  // nova busca do SSID, sempre repetindo a mesma leitura/sequencia.
  for (uint8_t connection = 1; connection <= 2; ++connection) {
    WiFi.disconnect();
    delay(20);
    WiFi.begin(ssid, password);
    Serial.printf("Wi-Fi: procurando %s (tentativa %u).\n", ssid, connection);
    const uint32_t startedAt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 15000UL) {
      delay(10);
    }
    if (WiFi.status() != WL_CONNECTED) continue;

    const IPAddress receiver = WiFi.gatewayIP();
    if (receiver == IPAddress(0U)) continue;
    Serial.printf("Wi-Fi conectado; canal %d; receptor %s.\n",
                  WiFi.channel(), receiver.toString().c_str());
    for (uint8_t attempt = 1; attempt <= 3; ++attempt) {
      WiFiClient client;
      HTTPClient http;
      http.setTimeout(2000); // Inclui a conexao TCP no core ESP8266 utilizado.
      http.setReuse(false);
      if (!http.begin(client, receiver.toString(), BREW_HTTP_PORT, BREW_HTTP_PATH)) {
        http.end();
        break;
      }
      http.addHeader("Content-Type", "application/json");
      Serial.printf("HTTP: enviando leitura, tentativa %u.\n", attempt);
      const int status = http.POST(reinterpret_cast<const uint8_t *>(body), bodySize);
      const bool accepted = status == HTTP_CODE_OK && brewReadConfirmation(http, packet);
      http.end();
      if (accepted) {
        Serial.println("Leitura confirmada pelo ESP32 via HTTP/JSON.");
        WiFi.disconnect();
        return true;
      }
      Serial.printf("HTTP sem confirmacao valida (status %d).\n", status);
      // Erros de dados/configuracao nao melhoram repetindo o mesmo corpo.
      if (status == 400 || status == 403 || status == 409 ||
          status == 413 || status == 415 || status == 422) {
        WiFi.disconnect();
        return false;
      }
      if (WiFi.status() != WL_CONNECTED) break;
      delay(200UL * attempt);
    }
  }
  WiFi.disconnect();
  Serial.println("Sem confirmacao HTTP do ESP32; nova tentativa no proximo ciclo de sono.");
  return false;
}
