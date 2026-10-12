// PC simulation of the sketch: drives pins/time/cards and checks the behaviour.
#include "Arduino.h"
#include "SPI.h"
#include "EEPROM.h"
#include "Wire.h"
#include "LiquidCrystal_I2C.h"
#include "MFRC522.h"

unsigned long g_millis = 0;
int g_pin[32]; int g_mode[32];
std::deque<char> g_serial_in;
SerialMock Serial; SPIMock SPI;
uint8_t g_eeprom[1024]; EEPROMMock EEPROM;
char g_lcd[2][17]; int g_cur_row = 0;
std::vector<std::vector<uint8_t>> g_cards_queue;

#include "../rfid_prepaid_card/rfid_prepaid_card.ino"

static int fails = 0, checks = 0;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { fails++; printf("  FAIL: %s\n", msg); } else printf("  ok:   %s\n", msg); } while (0)

void run(unsigned ms) { for (unsigned i = 0; i < ms; i++) { g_millis++; loop(); } }
void swipe(std::vector<uint8_t> uid) { g_cards_queue.push_back(uid); run(5); }
void press(int pin, unsigned hold) { g_pin[pin] = HIGH; run(hold); g_pin[pin] = LOW; run(60); }
bool lcd_has(const char *s) { return strstr(g_lcd[0], s) || strstr(g_lcd[1], s); }
void load_eeprom() { FILE *f = fopen("/tmp/sim_eeprom.bin", "rb"); if (f) { fread(g_eeprom, 1, 1024, f); fclose(f); } else memset(g_eeprom, 0xFF, 1024); }
void save_eeprom() { FILE *f = fopen("/tmp/sim_eeprom.bin", "wb"); fwrite(g_eeprom, 1, 1024, f); fclose(f); }
uint16_t stored_balance(int slot) { return loadBalance(slot); }

int main(int argc, char **argv) {
  int phase = argc > 1 ? atoi(argv[1]) : 1;
  load_eeprom();
  setup();
  std::vector<uint8_t> cardA = {0x04, 0xA1, 0xB2, 0xC3};
  std::vector<uint8_t> cardB = {0x04, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6};   // 7-byte UID

  if (phase == 1) {
    printf("== Phase 1: fresh EEPROM ==\n");
    CHECK(lcd_has("Swipe Card"), "idle screen shows 'Swipe Card'");

    printf("-- inputs are ignored before a card is swiped\n");
    press(7, 400);
    CHECK(g_pin[3] == LOW, "no service output without a card");

    printf("-- new card gets 1000 DA\n");
    swipe(cardA); run(10);
    CHECK(lcd_has("New card") && lcd_has("1000"), "new card message with 1000 DA");
    CHECK(stored_balance(0) == 1000, "balance written to EEPROM");
    run(2100);
    CHECK(lcd_has("Press Button"), "message expires by itself (non-blocking), then 'Press Button'");

    printf("-- 100 DA press: deduct once, output only while held\n");
    g_pin[7] = HIGH; run(300);
    CHECK(currentBalance == 900, "balance 900 after 100 DA press held 300 ms");
    CHECK(g_pin[3] == HIGH, "service output ON while held");
    run(2000);
    CHECK(currentBalance == 900, "holding the button does NOT deduct again");
    g_pin[7] = LOW; run(100);
    CHECK(g_pin[3] == LOW, "service output OFF after release");

    printf("-- contact bounce counts as ONE press\n");
    for (int i = 0; i < 6; i++) { g_pin[7] = HIGH; run(4); g_pin[7] = LOW; run(4); }
    g_pin[7] = HIGH; run(400); g_pin[7] = LOW; run(100);
    CHECK(currentBalance == 800, "bouncing press deducted exactly 100 DA (800)");

    printf("-- 200 DA, then both pins (300 DA) even if 40 ms apart\n");
    press(8, 300);
    CHECK(currentBalance == 600, "200 DA press -> 600");
    g_pin[7] = HIGH; run(40); g_pin[8] = HIGH; run(400); g_pin[7] = LOW; g_pin[8] = LOW; run(100);
    CHECK(currentBalance == 300, "both pins (staggered 40 ms) -> exactly 300 DA (balance 300)");

    printf("-- very short pulse (60 ms) still pays and gives service\n");
    g_pin[7] = HIGH; run(60); g_pin[7] = LOW; run(60);
    CHECK(currentBalance == 200, "60 ms pulse -> 100 DA (balance 200)");
    CHECK(g_pin[3] == HIGH, "service stays on for the minimum time");
    run(700);
    CHECK(g_pin[3] == LOW, "then turns off");

    printf("-- re-swiping the card must NOT reset the balance (old bug)\n");
    run(2100);
    swipe(cardA); run(10);
    CHECK(currentBalance == 200, "balance still 200 after re-swipe");

    printf("-- insufficient balance: no service, no deduction\n");
    press(8, 300);                    // 200 -> 0 : exactly enough
    CHECK(currentBalance == 0, "200 DA press with 200 DA -> 0");
    CHECK(lcd_has("empty") || lcd_has("Flexi"), "empty message after use");
    run(3100);
    CHECK(lcd_has("Swipe Card"), "session ended, back to 'Swipe Card'");
    swipe(cardA); run(2100);
    press(7, 300);
    CHECK(g_pin[3] == LOW, "NO service when balance is 0 (old bug: service ran for free)");
    CHECK(currentBalance == 0, "balance stays 0");
    save_eeprom();
  } else if (phase == 2) {
    printf("== Phase 2: after a power cycle ==\n");
    swipe(cardA); run(10);
    CHECK(currentBalance == 0, "balance 0 was remembered after power cut");
    printf("-- serial recharge\n");
    g_serial_in.push_back('r'); run(5);
    CHECK(currentBalance == 1000, "'r' recharges +1000 DA");
    run(2100);
    press(7, 300);
    CHECK(currentBalance == 900, "service works again after recharge");
    run(2100);

    printf("-- partial balance: enough for 100 but not 300\n");
    currentBalance = 250; saveBalance(currentSlot, 250);
    g_pin[7] = HIGH; g_pin[8] = HIGH; run(400); g_pin[7] = LOW; g_pin[8] = LOW; run(100);
    CHECK(currentBalance == 250 && lcd_has("Insufficient"), "300 DA request denied, balance unchanged");
    CHECK(g_pin[3] == LOW, "no service when denied");

    printf("-- 7-byte UID card is a different card from the 4-byte one\n");
    run(2100); swipe(cardB); run(10);
    CHECK(currentSlot == 1 && currentBalance == 1000, "7-byte UID stored in its own slot with 1000 DA");

    printf("-- unsupported 10-byte UID\n");
    swipe({1,2,3,4,5,6,7,8,9,10}); run(10);
    CHECK(lcd_has("unsupported"), "10-byte UID rejected with a message");

    printf("-- session timeout\n");
    run(2100); run(31000);
    CHECK(lcd_has("Swipe Card"), "idle session locks again after 30 s");

    printf("-- memory full\n");
    for (int i = 0; i < 12; i++) { std::vector<uint8_t> c = {0x10, (uint8_t)i, 0, 0}; swipe(c); run(10); }
    CHECK(lcd_has("Memory full"), "11th card gets 'Memory full'");

    printf("-- erase all\n");
    g_serial_in.push_back('X'); run(5);
    CHECK(g_eeprom[0] == 0xFF && g_eeprom[10] == 0xFF, "'X' erases the EEPROM");
    save_eeprom();
  }
  printf("\n%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
