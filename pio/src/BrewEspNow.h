#pragma once

#include <ESP8266WiFi.h>
#include <string.h>
#include "BrewProtocol.h"

extern "C" {
  #include <espnow.h>
  #include <user_interface.h>
}

// MAC do seu ESP32 receptor.
static uint8_t brewReceiverMac[6] = {
  0xF0, 0x08, 0xD1, 0xC8, 0x32, 0x08
};

// Para o primeiro teste, não presumimos a unidade do polinômio.
static constexpr uint8_t brewDensityUnit = BREW_UNIT_UNKNOWN;

static volatile bool brewAckReceived = false;
static volatile bool brewSendFinished = false;

static uint32_t brewBootId = 0;
static uint32_t brewSequence = 0;

static void brewOnSent(uint8_t *mac, uint8_t status) {
  (void)status;

  if (mac && memcmp(mac, brewReceiverMac, 6) == 0) {
    brewSendFinished = true;
  }
}

static void brewOnReceive(uint8_t *mac,
                          uint8_t *bytes,
                          uint8_t length) {
  if (!mac || !bytes ||
      memcmp(mac, brewReceiverMac, 6) != 0 ||
      length != sizeof(BrewAck)) {
    return;
  }

  BrewAck ack;
  memcpy(&ack, bytes, sizeof(ack));

  if (brewHeaderValid(ack, BREW_ACK) &&
      ack.sensorId == ESP.getChipId() &&
      ack.bootId == brewBootId &&
      ack.sequence == brewSequence) {
    brewAckReceived = true;
  }
}

static bool brewSendReading(float temperatureC,
                            float tiltDegrees,
                            float gravity,
                            float batteryV) {
  WiFi.persistent(false);
  WiFi.setAutoConnect(false);
  WiFi.setAutoReconnect(false);

  if (!WiFi.mode(WIFI_STA)) {
    Serial.println("Falha ao ativar STA.");
    return false;
  }

  WiFi.disconnect();
  delay(20);

  if (!wifi_set_channel(BREW_CHANNEL)) {
    Serial.println("Falha ao configurar canal.");
    return false;
  }

  if (esp_now_init() != 0) {
    Serial.println("Falha ao iniciar ESP-NOW.");
    return false;
  }

  if (esp_now_set_self_role(ESP_NOW_ROLE_COMBO) != 0 ||
      esp_now_register_send_cb(brewOnSent) != 0 ||
      esp_now_register_recv_cb(brewOnReceive) != 0 ||
      esp_now_add_peer(
        brewReceiverMac,
        ESP_NOW_ROLE_COMBO,
        BREW_CHANNEL,
        nullptr,
        0
      ) != 0) {
    Serial.println("Falha ao configurar ESP-NOW.");
    esp_now_deinit();
    return false;
  }

  if (brewBootId == 0) {
    brewBootId = os_random();
    if (brewBootId == 0) brewBootId = 1;
  }

  ++brewSequence;

  BrewTelemetry packet = {};

  packet.header.magic = BREW_MAGIC;
  packet.header.version = BREW_VERSION;
  packet.header.type = BREW_TELEMETRY;
  packet.header.gravityUnit = brewDensityUnit;
  packet.header.sensorId = ESP.getChipId();
  packet.header.bootId = brewBootId;
  packet.header.sequence = brewSequence;

  packet.temperatureC = temperatureC;
  packet.tiltDegrees = tiltDegrees;
  packet.gravity = gravity;
  packet.batteryV = batteryV;

  brewAckReceived = false;

  for (uint8_t attempt = 1;
       attempt <= 3 && !brewAckReceived;
       ++attempt) {
    brewSendFinished = false;

    Serial.printf("ESP-NOW: tentativa %u\n", attempt);

    int result = esp_now_send(
      brewReceiverMac,
      reinterpret_cast<uint8_t *>(&packet),
      sizeof(packet)
    );

    if (result != 0) {
      Serial.printf("Falha ao enfileirar envio: %d\n", result);
      delay(50);
      continue;
    }

    uint32_t startedAt = millis();

    while (millis() - startedAt < 800UL) {
      if (brewSendFinished && brewAckReceived) break;
      delay(1);
    }

    if (!brewSendFinished) {
      Serial.println("Timeout aguardando conclusao do envio.");
      break;
    }
  }

  bool confirmed = brewAckReceived;

  esp_now_deinit();

  Serial.println(
    confirmed
      ? "ACK recebido do ESP32."
      : "Sem ACK; encerrando este ciclo."
  );

  return confirmed;
}