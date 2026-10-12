#pragma once
#include "Arduino.h"
#include <vector>
extern std::vector<std::vector<uint8_t>> g_cards_queue;   // cards "presented" to the reader
struct MFRC522 {
  struct Uid { byte size; byte uidByte[10]; } uid;
  MFRC522(uint8_t, uint8_t) {}
  void PCD_Init() {}
  bool PICC_IsNewCardPresent() { return !g_cards_queue.empty(); }
  bool PICC_ReadCardSerial() {
    auto c = g_cards_queue.front(); g_cards_queue.erase(g_cards_queue.begin());
    uid.size = (byte)c.size(); memcpy(uid.uidByte, c.data(), c.size()); return true;
  }
  void PICC_HaltA() {}
  void PCD_StopCrypto1() {}
};
