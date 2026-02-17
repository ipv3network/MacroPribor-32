#ifndef WIFICONFIG_H
#define WIFICONFIG_H

#include <Arduino.h>
#include <IPAddress.h>

struct WifiConfig {
  const char* ssid;
  const char* pass;
  bool useStaticIP;
  IPAddress local_ip;
  IPAddress gateway;
  IPAddress subnet;
};

const char* device_hostname = "macropribor-32";

// Настройки первой точки доступа
WifiConfig ap1 = {
  "SSID_1",          // Имя сети
  "PASSWORD_1",      // Пароль
  false,             // true = Статический IP, false = DHCP
  IPAddress(192, 168, 1, 100),
  IPAddress(192, 168, 1, 1),
  IPAddress(255, 255, 255, 0)
};

// Настройки второй точки доступа
WifiConfig ap2 = {
  "SSID_2",
  "PASSWORD_2",
  true,              // Используем статический IP
  IPAddress(192, 168, 0, 50),
  IPAddress(192, 168, 0, 1),
  IPAddress(255, 255, 255, 0)
};

// Настройки собственной точки доступа (Fallback AP)
const char* fallback_ssid = "MacroPribor-32";
const char* fallback_pass = "MacroPribor-32";

#endif
