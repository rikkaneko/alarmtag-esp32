/*
 * This file is part of alarmtag-esp32.
 * Copyright (c) 2023 Joe Ma <rikkaneko23@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <Preferences.h>

const char *CONTROL_SERVICE_UUID = "4ac6a418-d0eb-4016-a8a7-090467c9cf1c";
const char *ALERT_POLICY_CONFIG_CHARACTERISTIC_UUID = "154456ac-0b87-4c7e-a716-3ebf0055262d";
const char *MAKE_ALERT_CHARACTERISTIC_UUID = "e75b30ba-c8eb-4d65-9239-02a8a7877a1d";
const char *PIN_AUTH_CHARACTERISTIC_UUID = "d25822e9-eba6-4d27-8f03-179a03e588ab";
constexpr int RGB_PIN[] = {27, 25, 33};
constexpr int BUZZER_PIN = 32;

enum RGB {
  Red = 0, Green = 1, Blue = 2
};

BLEAdvertising *adverting;
hw_timer_t *alarm_timer = nullptr;
hw_timer_t *advising_timer = nullptr;
Preferences config;
// Bit 0: alarm-on-disconnect
uint64_t flags = 0b1;
bool is_alarm_enable = false;
bool is_auth = false;
std::string saved_pin;

void start_alarm() {
  timerAlarmEnable(alarm_timer);
  Serial.println("alarm: started");
  is_alarm_enable = true;
}

void end_alarm() {
  timerAlarmDisable(alarm_timer);
  digitalWrite(BUZZER_PIN, LOW);
  Serial.println("alarm: ended");
  is_alarm_enable = false;
}

void start_advising() {
  BLEDevice::startAdvertising();
  Serial.println("ble_server: AlarmTag started advising");
  timerAlarmEnable(advising_timer);
}

std::string flag_text(uint64_t flag) {
  std::string result;
  if (((flags >> 0) & 1U) == 0) result += "alarm-on-disconnect:0;";
  if (((flags >> 0) & 1U) == 1) result += "alarm-on-disconnect:1;";
  if (((flags >> 1) & 1U) == 0) result += "device-lock:0;";
  if (((flags >> 1) & 1U) == 1) result += "device-lock:1;";
  return result;
}

std::string get_client_address(esp_bd_addr_t address) {
  char remoteAddress[18];
  sprintf(remoteAddress, "%.2X:%.2X:%.2X:%.2X:%.2X:%.2X",
          address[0],
          address[1],
          address[2],
          address[3],
          address[4],
          address[5]);
  return remoteAddress;
}

class CustomBLEServerCallbacks : public BLEServerCallbacks {
public:
  void onConnect(BLEServer *pServer, esp_ble_gatts_cb_param_t *param) override {
    BLEServerCallbacks::onConnect(pServer, param);
    auto address = get_client_address(param->connect.remote_bda);
    Serial.printf("client: connected (%s)\n", address.c_str());
    timerAlarmDisable(advising_timer);
    digitalWrite(RGB_PIN[RGB::Blue], HIGH);
    // Check device-lock
    if (((flags >> 1) & 1U) == 0 || !config.isKey("device-pin")) {
      is_auth = true;
    }
  }

  void onDisconnect(BLEServer *pServer) override {
    BLEServerCallbacks::onDisconnect(pServer);
    Serial.println("client: disconnected");
    if (((flags >> 0) & 1U) == 1) { // alarm-on-disconnect is set
      Serial.println("policy: start alarm (alarm-on-disconnect)");
      start_alarm();
    }
    is_auth = false;
    start_advising();
  }
};

class AlertPolicyCallbacks : public BLECharacteristicCallbacks {
public:
  void onWrite(BLECharacteristic *pCharacteristic) override {
    BLECharacteristicCallbacks::onWrite(pCharacteristic);
    // Check auth state
    if (!is_auth) {
      Serial.println("auth: required pin");
      return;
    }
    auto data = pCharacteristic->getValue();
    // alarm-on-disconnect
    if (((flags >> 0) & 1U) == 1 && data.find("alarm-on-disconnect:0") != std::string::npos) {
      Serial.println("config: auto-alarm disabled");
      flags &= ~(1 << 0);
    } else if (((flags >> 0) & 1U) == 0 && data.find("alarm-on-disconnect:1") != std::string::npos) {
      Serial.println("config: alarm-on-disconnect enabled");
      flags |= 1 << 0;
    }
    // lock target device
    if (((flags >> 1) & 1U) == 1 && data.find("device-lock:0") != std::string::npos) {
      Serial.println("config: device-lock disabled");
      if (config.isKey("device-pin"))
        config.remove("device-pin");
      flags &= ~(1 << 1);
    } else if (((flags >> 1) & 1U) == 0 && data.find("device-lock:1") != std::string::npos) {
      if (saved_pin.empty()) {
        Serial.println("config: required pin before enabling the lock");
        return;
      }
      config.putString("device-pin", saved_pin.c_str());
      Serial.println("config: pin set");
      Serial.println("config: device-lock enabled");
      flags |= 1 << 1;
    }
    Serial.printf("config: new flag 0x%lx\n", flags);
    pCharacteristic->setValue(flag_text(flags));
    pCharacteristic->notify();
    delay(3); // add delay for bluetooth congestion
    config.putULong64("flags", flags);
  }

  void onRead(BLECharacteristic *pCharacteristic) override {
    BLECharacteristicCallbacks::onRead(pCharacteristic);
    pCharacteristic->setValue(flag_text(flags));
  }
};

class ToggleAlertCallbacks : public BLECharacteristicCallbacks {
public:
  void onWrite(BLECharacteristic *pCharacteristic) override {
    // Check auth state
    if (!is_auth) {
      Serial.println("auth: required pin");
      return;
    }
    BLECharacteristicCallbacks::onWrite(pCharacteristic);
    auto data = pCharacteristic->getValue();
    if (data.length() == 1 && (data[0] == '1' || data[0] == 1)) {
      if (!is_alarm_enable) {
        Serial.println("client: start alarm");
        start_alarm();
      } else {
        Serial.println("client: stop alarm");
        end_alarm();
      }
    }
  }
};

class PINAuthCallbacks : public BLECharacteristicCallbacks {
public:
  void onWrite(BLECharacteristic *pCharacteristic) override {
    BLECharacteristicCallbacks::onWrite(pCharacteristic);
    auto data = pCharacteristic->getValue();
    if (!is_auth) {
      if (config.getString("device-pin").equals(data.c_str())) {
        is_auth = true;
        Serial.println("auth: unlocked");
      } else {
        Serial.println("auth: wrong pin");
      }
    }
    if (!data.empty())
      saved_pin = data;
  }

};

void setup() {
  // Restore saved config
  config.begin("alarmtag");
  flags = config.getULong64("flags", 0b1);
  // Initialize serial console
  Serial.begin(115200);
  Serial.printf("cpu_freq = %uMHz, xtal_freq = %uMHz, apb_freq = %uHz\n",
                getCpuFrequencyMhz(), getXtalFrequencyMhz(), getApbFrequency());
  setCpuFrequencyMhz(80);
  Serial.println("core: using 80MHz CPU clock");
  Serial.printf("cpu_freq = %uMHz, xtal_freq = %uMHz, apb_freq = %uHz\n",
                getCpuFrequencyMhz(), getXtalFrequencyMhz(), getApbFrequency());
  Serial.printf("config: flag 0x%lx\n", flags);
  Serial.printf("config: flag %s\n", flag_text(flags).c_str());
  // Check device-lock
  if (((flags >> 1) & 1U) == 1 && config.isKey("device-pin")) {
    Serial.println("auth: device is locked");
  }
  // Initialize pin-outs
  pinMode(BUZZER_PIN, OUTPUT);
  for (auto p: RGB_PIN) {
    pinMode(p, OUTPUT);
    digitalWrite(p, HIGH);
  }
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(RGB_PIN[RGB::Red], LOW);
  // Initialize alarm_timer
  alarm_timer = timerBegin(0, 80, true);
  timerAlarmWrite(alarm_timer, 100000, true);
  timerAttachInterrupt(alarm_timer, []() {
    digitalWrite(BUZZER_PIN, !digitalRead(BUZZER_PIN));
  }, true);
  // Initialize advising_timer
  advising_timer = timerBegin(1, 80, true);
  timerAlarmWrite(advising_timer, 500000, true);
  timerAttachInterrupt(advising_timer, []() {
    digitalWrite(RGB_PIN[RGB::Blue], !digitalRead(RGB_PIN[RGB::Blue]));
  }, true);
  // Initialize BLE device
  BLEDevice::init("AlarmTag");
  auto *server = BLEDevice::createServer();
  server->setCallbacks(new CustomBLEServerCallbacks);
  auto *control_service = server->createService(CONTROL_SERVICE_UUID);
  // Control alert policy
  auto *alert_policy_config =
      control_service->createCharacteristic(ALERT_POLICY_CONFIG_CHARACTERISTIC_UUID,
                                            BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE |
                                            BLECharacteristic::PROPERTY_NOTIFY);
  alert_policy_config->setValue(flag_text(flags));
  alert_policy_config->setCallbacks(new AlertPolicyCallbacks);
  // Make alert
  auto *toggle_alert =
      control_service->createCharacteristic(MAKE_ALERT_CHARACTERISTIC_UUID,
                                            BLECharacteristic::PROPERTY_WRITE);
  toggle_alert->setCallbacks(new ToggleAlertCallbacks);
  // PIN Entry
  auto *pin_auth =
      control_service->createCharacteristic(PIN_AUTH_CHARACTERISTIC_UUID,
                                            BLECharacteristic::PROPERTY_WRITE);
  pin_auth->setCallbacks(new PINAuthCallbacks);
  // Start service
  control_service->start();
  adverting = BLEDevice::getAdvertising();
  adverting->addServiceUUID(CONTROL_SERVICE_UUID);
  adverting->setScanResponse(true);
  adverting->setMinPreferred(0x06);  // functions that help with iPhone connections issue
  adverting->setMinPreferred(0x12);
  start_advising();
  digitalWrite(RGB_PIN[RGB::Red], HIGH);
}

void loop() {}