/*
 * RFID prepaid card reader (Arduino Uno / Nano)
 * ---------------------------------------------------------------------------
 * - A MIFARE card is swiped on an MFRC522 reader.
 * - Each card has a prepaid balance (in DA) that is stored in the EEPROM,
 *   so it survives card re-swipes and power cuts.
 * - Two inputs request a service that costs 100 DA (pin 7) or 200 DA (pin 8);
 *   both active together cost 300 DA. The price is deducted ONCE per press,
 *   and the service output is enabled only if the balance covers the price.
 * - A 16x2 I2C LCD shows the card, balance and messages.
 *
 * The sketch never blocks with delay(): everything runs on millis() timers,
 * so no input pulse is missed while a message is displayed.
 *
 * Libraries: MFRC522 (miguelbalboa), LiquidCrystal_I2C.
 *
 * SECURITY NOTE: the balance is stored on the Arduino, keyed by the card UID.
 * UIDs of standard MIFARE cards can be cloned, so this is a learning /
 * prototype project, NOT a payment system.
 */

#include <SPI.h>
#include <MFRC522.h>
#include <EEPROM.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ------------------------------- Pins ---------------------------------------
const uint8_t RFID_SS_PIN  = 10;   // MFRC522 SDA/SS
const uint8_t RFID_RST_PIN = 9;    // MFRC522 RST
const uint8_t PIN_100      = 7;    // request: 100 DA
const uint8_t PIN_200      = 8;    // request: 200 DA
const uint8_t SERVICE_PIN  = 3;    // output: enabled while a paid request is held
const uint8_t SIGNAL_PIN   = 4;    // output: short beep / LED after a deduction

const uint8_t LCD_ADDRESS  = 0x27; // try 0x3F if the screen stays blank

// ------------------------------ Settings ------------------------------------
// Level on PIN_100 / PIN_200 that means "requested".
//   HIGH -> use an external 10k pull-down resistor on each pin (default).
//   LOW  -> the sketch uses INPUT_PULLUP; connect the pin to GND to request.
#define INPUT_ACTIVE_LEVEL HIGH

const uint16_t INITIAL_BALANCE = 1000;  // DA given to a new card / per recharge
const uint16_t PRICE_100  = 100;
const uint16_t PRICE_200  = 200;
const uint16_t PRICE_BOTH = 300;
const uint16_t PRICE_MIN  = PRICE_100;  // below this a card is considered empty

const unsigned long DEBOUNCE_MS        = 30;     // input must be stable this long
const unsigned long COMBINE_MS         = 150;    // window to detect "both pins" (300 DA)
const unsigned long SERVICE_MIN_MS     = 500;    // minimum service time per payment
const unsigned long BEEP_MS            = 150;
const unsigned long MESSAGE_MS         = 2000;
const unsigned long SESSION_TIMEOUT_MS = 30000;  // back to "Swipe Card" when unused

// ----------------------------- EEPROM layout --------------------------------
// One 10-byte block per card:
//   [0]     UID length (4 or 7); 0xFF = empty slot
//   [1..7]  UID bytes
//   [8..9]  balance (uint16_t)
const uint8_t MAX_CARDS       = 10;
const uint8_t BLOCK_SIZE      = 10;
const uint8_t MAX_UID_BYTES   = 7;
const uint8_t EMPTY_MARK      = 0xFF;
const uint8_t BALANCE_OFFSET  = 8;

// ------------------------------ Globals -------------------------------------
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
LiquidCrystal_I2C lcd(LCD_ADDRESS, 16, 2);

enum Mode : uint8_t { MODE_IDLE, MODE_ACTIVE };
Mode mode = MODE_IDLE;

int8_t   currentSlot    = -1;
uint16_t currentBalance = 0;

struct DebouncedInput {
  uint8_t pin;
  bool stable;
  bool lastRaw;
  unsigned long changedAt;
};
DebouncedInput in100 = { PIN_100, false, false, 0 };
DebouncedInput in200 = { PIN_200, false, false, 0 };

bool windowOpen    = false;   // collecting inputs to tell 100 / 200 / 300 apart
uint8_t pendingMask = 0;      // bit0 = pin 7 seen, bit1 = pin 8 seen
unsigned long windowStart = 0;
bool waitRelease   = false;   // one charge per press: wait until released
bool serviceActive = false;
unsigned long serviceOffAt = 0;

unsigned long messageUntil = 0;
unsigned long beepUntil    = 0;
unsigned long lastActivity = 0;

// ------------------------------- LCD helpers --------------------------------
// Writes one full 16-character row (pads with spaces, so no lcd.clear() flicker).
void lcdLine(uint8_t row, const char *text) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%-16s", text);
  lcd.setCursor(0, row);
  lcd.print(buf);
}

void drawBalanceLine(uint8_t row) {
  char buf[17];
  snprintf(buf, sizeof(buf), "Bal: %u DA", currentBalance);
  lcdLine(row, buf);
}

void drawIdle() {
  lcdLine(0, "Swipe Card");
  lcdLine(1, "");
}

void drawActive() {
  lcdLine(0, "Press Button");
  drawBalanceLine(1);
}

void redrawScreen() {
  if (mode == MODE_ACTIVE) drawActive(); else drawIdle();
}

// Shows a message for `ms` milliseconds without blocking; the normal screen
// comes back automatically (see loop()).
void showMessage(const char *line1, const char *line2, unsigned long ms) {
  lcdLine(0, line1);
  lcdLine(1, line2);
  messageUntil = millis() + ms;
}

void beep() {
  digitalWrite(SIGNAL_PIN, HIGH);
  beepUntil = millis() + BEEP_MS;
}

// ------------------------------ EEPROM --------------------------------------
int slotAddr(uint8_t slot) { return slot * BLOCK_SIZE; }

int8_t findCard(const byte *uid, byte size) {
  for (uint8_t i = 0; i < MAX_CARDS; i++) {
    int a = slotAddr(i);
    if (EEPROM.read(a) != size) continue;          // also skips empty slots
    bool match = true;
    for (byte j = 0; j < size; j++) {
      if (EEPROM.read(a + 1 + j) != uid[j]) { match = false; break; }
    }
    if (match) return i;
  }
  return -1;
}

uint16_t loadBalance(uint8_t slot) {
  uint16_t b;
  EEPROM.get(slotAddr(slot) + BALANCE_OFFSET, b);
  return b;
}

void saveBalance(uint8_t slot, uint16_t balance) {
  EEPROM.put(slotAddr(slot) + BALANCE_OFFSET, balance);   // writes only changed bytes
}

// Stores a new card in the first free slot. Returns the slot, or -1 if full.
int8_t createCard(const byte *uid, byte size) {
  for (uint8_t i = 0; i < MAX_CARDS; i++) {
    int a = slotAddr(i);
    if (EEPROM.read(a) == EMPTY_MARK) {
      EEPROM.update(a, size);
      for (byte j = 0; j < size; j++) EEPROM.update(a + 1 + j, uid[j]);
      saveBalance(i, INITIAL_BALANCE);
      return i;
    }
  }
  return -1;
}

// ------------------------------- Sessions -----------------------------------
void endSession() {
  mode = MODE_IDLE;
  currentSlot = -1;
  windowOpen = false;
  pendingMask = 0;
  redrawScreen();
}

void startSession(const byte *uid, byte size) {
  if (size > MAX_UID_BYTES) {
    showMessage("Card unsupported", "UID too long", MESSAGE_MS);
    return;
  }

  char hex[2 * MAX_UID_BYTES + 1];
  for (byte i = 0; i < size; i++) snprintf(hex + 2 * i, 3, "%02X", uid[i]);

  bool isNew = false;
  int8_t slot = findCard(uid, size);
  if (slot < 0) {
    slot = createCard(uid, size);
    if (slot < 0) {
      showMessage("Memory full", "Card not saved", MESSAGE_MS);
      return;
    }
    isNew = true;
  }

  currentSlot = slot;
  currentBalance = loadBalance(slot);          // keep the stored balance
  mode = MODE_ACTIVE;
  lastActivity = millis();
  windowOpen = false;
  pendingMask = 0;
  waitRelease = (in100.stable || in200.stable); // ignore a button already held

  char bal[17];
  snprintf(bal, sizeof(bal), "Bal: %u DA", currentBalance);
  showMessage(isNew ? "New card" : hex, bal, MESSAGE_MS);

  Serial.print(isNew ? F("New card ") : F("Card "));
  Serial.print(hex);
  Serial.print(F(" slot "));
  Serial.print(slot);
  Serial.print(F(" balance "));
  Serial.println(currentBalance);
}

void handleCard() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  byte uid[10];
  byte size = rfid.uid.size;
  if (size > sizeof(uid)) size = sizeof(uid);
  memcpy(uid, rfid.uid.uidByte, size);

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  startSession(uid, size);
}

// ------------------------------- Payments -----------------------------------
void chargeFor(uint8_t mask) {
  uint16_t price = (mask == 3) ? PRICE_BOTH : (mask == 2) ? PRICE_200 : PRICE_100;
  lastActivity = millis();

  if (currentBalance >= price) {
    currentBalance -= price;
    saveBalance(currentSlot, currentBalance);

    serviceActive = true;
    serviceOffAt = millis() + SERVICE_MIN_MS;
    digitalWrite(SERVICE_PIN, HIGH);
    beep();

    char l1[17], l2[17];
    snprintf(l1, sizeof(l1), "Deducted: %uDA", price);
    snprintf(l2, sizeof(l2), "Bal: %u DA", currentBalance);
    showMessage(l1, l2, MESSAGE_MS);
    Serial.print(F("Deducted "));
    Serial.print(price);
    Serial.print(F(" -> balance "));
    Serial.println(currentBalance);
  } else {
    // Not enough balance: no service, no deduction.
    char l2[17];
    snprintf(l2, sizeof(l2), "Bal: %u DA", currentBalance);
    showMessage("Insufficient", l2, MESSAGE_MS);
    Serial.println(F("Insufficient balance"));
  }
}

void updateInput(DebouncedInput &in) {
  bool raw = (digitalRead(in.pin) == INPUT_ACTIVE_LEVEL);
  if (raw != in.lastRaw) {
    in.lastRaw = raw;
    in.changedAt = millis();
  } else if (raw != in.stable && millis() - in.changedAt >= DEBOUNCE_MS) {
    in.stable = raw;
  }
}

void handleInputs() {
  bool a = in100.stable;
  bool b = in200.stable;

  if (a || b) {
    if (!waitRelease) {
      if (!windowOpen) { windowOpen = true; windowStart = millis(); }
      pendingMask |= (a ? 1 : 0) | (b ? 2 : 0);
    }
  } else {
    waitRelease = false;      // everything released: ready for the next press
  }

  // After the short window, charge once for whatever was requested.
  if (windowOpen && millis() - windowStart >= COMBINE_MS) {
    windowOpen = false;
    chargeFor(pendingMask);
    pendingMask = 0;
    waitRelease = (a || b);
  }

  // Service output follows the request, but stays on for at least SERVICE_MIN_MS.
  if (serviceActive && !a && !b && millis() >= serviceOffAt) {
    serviceActive = false;
    digitalWrite(SERVICE_PIN, LOW);

    if (currentBalance < PRICE_MIN) {
      showMessage("Balance empty", "Recharge: Flexi", 3000);
      endSession();
    }
  }
}

// --------------------------- Serial admin (optional) ------------------------
// Open the Serial Monitor at 9600 baud and send:
//   l  list stored cards and balances
//   r  recharge the last swiped card (+INITIAL_BALANCE)
//   X  erase ALL cards (capital X)
void listCards() {
  for (uint8_t i = 0; i < MAX_CARDS; i++) {
    int a = slotAddr(i);
    byte size = EEPROM.read(a);
    if (size == EMPTY_MARK) continue;
    Serial.print(i);
    Serial.print(F(": "));
    for (byte j = 0; j < size && j < MAX_UID_BYTES; j++) {
      byte v = EEPROM.read(a + 1 + j);
      if (v < 0x10) Serial.print('0');
      Serial.print(v, HEX);
    }
    Serial.print(F("  balance "));
    Serial.println(loadBalance(i));
  }
}

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'l') {
      listCards();
    } else if (c == 'r') {
      if (currentSlot < 0) { Serial.println(F("Swipe a card first.")); continue; }
      uint32_t sum = (uint32_t)currentBalance + INITIAL_BALANCE;
      currentBalance = (sum > 65535UL) ? 65535 : (uint16_t)sum;
      saveBalance(currentSlot, currentBalance);
      Serial.print(F("Recharged -> "));
      Serial.println(currentBalance);
      redrawScreen();
    } else if (c == 'X') {
      for (int i = 0; i < MAX_CARDS * BLOCK_SIZE; i++) EEPROM.update(i, 0xFF);
      endSession();
      Serial.println(F("All cards erased."));
    }
  }
}

// ------------------------------ Arduino entry -------------------------------
void setup() {
  Serial.begin(9600);
  SPI.begin();
  rfid.PCD_Init();

  lcd.init();
  lcd.backlight();

  pinMode(PIN_100, INPUT_ACTIVE_LEVEL == HIGH ? INPUT : INPUT_PULLUP);
  pinMode(PIN_200, INPUT_ACTIVE_LEVEL == HIGH ? INPUT : INPUT_PULLUP);
  pinMode(SERVICE_PIN, OUTPUT);
  pinMode(SIGNAL_PIN, OUTPUT);
  digitalWrite(SERVICE_PIN, LOW);
  digitalWrite(SIGNAL_PIN, LOW);

  drawIdle();
  Serial.println(F("Ready. Commands: l (list), r (recharge last card), X (erase all)"));
}

void loop() {
  unsigned long now = millis();

  updateInput(in100);
  updateInput(in200);
  handleSerial();

  // Housekeeping timers (non-blocking replacements for delay()).
  if (beepUntil && (long)(now - beepUntil) >= 0) {
    beepUntil = 0;
    digitalWrite(SIGNAL_PIN, LOW);
  }
  if (messageUntil && (long)(now - messageUntil) >= 0) {
    messageUntil = 0;
    redrawScreen();
  }

  // Cards are only read while no paid service is running.
  if (!serviceActive) handleCard();

  if (mode == MODE_ACTIVE) {
    handleInputs();
    if (!serviceActive && !windowOpen && now - lastActivity >= SESSION_TIMEOUT_MS) {
      endSession();                      // nobody used the session: lock it again
    }
  }
}
