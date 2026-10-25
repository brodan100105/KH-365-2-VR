// The ARM7, as far as the ARM9's code can tell: it answers PXI FIFO messages the way the
// NitroSDK's ARM7 component does, and keeps the system work area (0x027ffc00) up to date.
//
// A PXI word is: tag in bits 0-4, error flag bit 5, 26 bits of data. The ARM9's SDK code waits for
// these answers (docs/ARCHITECTURE.md lists the subsystems); anything this file does not know is
// logged once and bounced back with the error flag, which is also what the real ARM7 does for a
// tag nobody listens to.
#include <chrono>
#include <ctime>
#include <deque>
#include <mutex>

#include "guest.h"
#include "hle.h"

namespace khvr::hle {

namespace {

enum Tag : uint32_t {
    kTagRtc = 5,
    kTagTouch = 6,
    kTagSound = 7,
    kTagPower = 8,
    kTagWireless = 10,
    kTagFs = 11,     // card backup requests and their completion
    kTagOs = 12,
    kTagCartridge = 13,
    kTagCard = 14,
};

constexpr uint32_t kSpiEnd = 0x01000000;
constexpr uint32_t kSpiStart = 0x02000000;
constexpr uint32_t kSysWork = 0x027ffc00;
constexpr uint32_t kSndSharedWorkCommand = 0x1d;

std::deque<uint32_t> gReplies;
uint32_t gSyncNibble = 0;
uint32_t gSoundShared = 0;      // SNDSharedWork, once the sound driver tells us where it is
uint32_t gCardCmd = 0;          // CARDiCommandArg, from the backup INIT request
bool gTouchAuto = false;

uint32_t Word(uint32_t tag, uint32_t data, bool err = false) {
    return (tag & 0x1f) | (err ? 0x20u : 0u) | (data << 6);
}

void Reply(uint32_t tag, uint32_t data, bool err = false) { gReplies.push_back(Word(tag, data, err)); }

uint8_t Bcd(int v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }

// The RTC's raw date and time in the system work (the SDK converts from BCD).
void WriteClock() {
    std::time_t now = std::time(nullptr);
    std::tm t;
    localtime_r(&now, &t);
    uint32_t rtc = kSysWork + 0x1e8;
    Write8(rtc + 0, Bcd(t.tm_year % 100));
    Write8(rtc + 1, Bcd(t.tm_mon + 1));
    Write8(rtc + 2, Bcd(t.tm_mday));
    Write8(rtc + 3, static_cast<uint8_t>(t.tm_wday));
    Write8(rtc + 4, Bcd(t.tm_hour));
    Write8(rtc + 5, Bcd(t.tm_min));
    Write8(rtc + 6, Bcd(t.tm_sec));
}

// The touch screen sample the SDK copies out of the system work (HW_TOUCHPANEL_BUF): raw 12-bit
// coordinates with a "touched" bit, converted back to pixels by the firmware's calibration,
// which SetupSharedArea writes as an identity mapping.
void WriteTouch() {
    const InputState &in = Input();
    uint32_t x = in.touching ? in.touchX : 0, y = in.touching ? in.touchY : 0;
    uint16_t rawX = static_cast<uint16_t>(x * 16), rawY = static_cast<uint16_t>(y * 16);
    uint32_t a = 0x027fffaa;
    Write16(a, static_cast<uint16_t>((rawX & 0xfff) | ((rawY & 0xf) << 12)));
    Write16(a + 2, static_cast<uint16_t>((rawY >> 4) | (in.touching ? 0x0100 : 0)));
}

void OnSpi(uint32_t tag, uint32_t data) {
    // A request is one or more words: the first has the start bit and the command in bits 8-14,
    // the last the end bit (a one-word request has both). The reply names the command, so it
    // has to come from the start word: TP_RequestAutoSamplingStartAsync sends two words, and
    // answering the end word's bits left TP_WaitBusy spinning forever on the title screen.
    static uint32_t startCmd[32];
    if (data & kSpiStart) startCmd[tag & 0x1f] = (data >> 8) & 0x7f;
    if (!(data & kSpiEnd)) return;  // more parameter words follow
    uint32_t cmd = startCmd[tag & 0x1f];
    if (tag == kTagTouch) {
        WriteTouch();
        if (cmd == 1) gTouchAuto = true;
        if (cmd == 2) gTouchAuto = false;
    }
    uint32_t result = 0;  // SPI_PXI_RESULT_SUCCESS
    if (tag == kTagPower && cmd >= 0x70 && cmd <= 0x74) result = cmd == 0x70 ? 0x0d : 0;  // PMIC registers
    Reply(tag, kSpiEnd | (cmd << 8) | result);
}

void OnRtc(uint32_t data) {
    uint32_t cmd = (data >> 8) & 0x7f;
    WriteClock();
    Reply(kTagRtc, (cmd << 8) | 0);  // RTC_PXI_RESULT_SUCCESS
}

void OnSound(uint32_t data) {
    // The word is a pointer to a list of SNDCommand {next, id, arg[4]}; the ARM7's sound driver
    // runs it and advances finishCommandTag. No audio yet: only the commands that tell us where
    // things are matter.
    // Guest RAM ends below 0x04000000, so a pointer always fits the 26 data bits.
    uint32_t cmd = data;
    for (int guard = 0; cmd && guard < 4096; ++guard) {
        uint32_t id = Read32(cmd + 4);
        if (id == kSndSharedWorkCommand) {
            gSoundShared = Read32(cmd + 8);
            Log("arm7: sound shared work at 0x%08x", gSoundShared);
        }
        cmd = Read32(cmd);
    }
    if (gSoundShared) Write32(gSoundShared, Read32(gSoundShared) + 1);  // finishCommandTag
}

void OnCardRequest(uint32_t data, bool err) {
    (void)err;
    static bool expectCmdPointer = false;
    if (expectCmdPointer) {
        gCardCmd = data;
        expectCmdPointer = false;
        Log("arm7: card backup command block at 0x%08x", gCardCmd);
        Write32(gCardCmd, 0);  // CARD_RESULT_SUCCESS
        Reply(kTagFs, 0, true);
        return;
    }
    uint32_t req = data;
    if (req == 0) {  // CARD_REQ_INIT: the command block's address follows
        expectCmdPointer = true;
        return;
    }
    uint32_t result = 0;
    if (gCardCmd) {
        uint32_t src = Read32(gCardCmd + 12), dst = Read32(gCardCmd + 16), len = Read32(gCardCmd + 20);
        switch (req) {
        case 1: break;  // ACK
        case 2: break;  // IDENTIFY: any backup type is fine, the save file grows as needed
        case 3: Write32(gCardCmd + 8, 0x00c2ff01); break;  // READ_ID
        case 6: WatchHostWrite(dst, len, "save read"); SaveRead(src, GuestMem() + dst, len); break;                 // READ_BACKUP
        case 7: case 8: SaveWrite(dst, GuestMem() + src, len); SaveFlush(); break;  // WRITE/PROGRAM
        case 9: result = SaveVerify(dst, GuestMem() + src, len) ? 0 : 1; break;  // VERIFY
        case 10: case 11: case 15: SaveErase(dst, len); SaveFlush(); break;  // ERASE page/sector/sub
        case 12: SaveErase(0, 0x800000); SaveFlush(); break;                 // ERASE_CHIP
        default: LogOnce("card-req", "arm7: card request %u not handled", req); break;
        }
        Write32(gCardCmd, result);
    }
    Reply(kTagFs, 0, true);  // the SDK takes a bounced word as "request done"
}

}  // namespace

void Arm7Reset() {
    gReplies.clear();
    gSyncNibble = 0;
    gSoundShared = 0;
    gCardCmd = 0;
    gTouchAuto = false;
}

uint32_t Arm7SyncOut(uint32_t arm9Out) {
    // PXI_InitFifo echoes our nibble back and waits for us to move on; counting up reaches 0
    // again after 16 rounds, which ends its handshake.
    gSyncNibble = (arm9Out + 1) & 0xf;
    return gSyncNibble;
}

void Arm7OnSend(uint32_t word) {
    uint32_t tag = word & 0x1f;
    bool err = (word >> 5) & 1;
    uint32_t data = word >> 6;
    switch (tag) {
    case kTagTouch:
    case kTagPower: OnSpi(tag, data); break;
    case kTagRtc: OnRtc(data); break;
    case kTagSound: OnSound(data); break;
    case kTagFs: OnCardRequest(data, err); break;
    case kTagCartridge: Reply(tag, 1); break;            // CTRDGi_InitCallback: "no cartridge"
    case kTagOs: Reply(tag, 0x10 << 8); break;           // OSi_CommonCallback: reset handshake
    case kTagCard: Reply(tag, 0); break;
    default:
        LogOnce(("pxi-tag-" + std::to_string(tag)).c_str(), "arm7: PXI tag %u not handled (data 0x%07x)", tag, data);
        if (!err) Reply(tag, data, true);
        break;
    }
}

bool Arm7HasReply() { return !gReplies.empty(); }

uint32_t Arm7PopReply() {
    uint32_t w = gReplies.front();
    gReplies.pop_front();
    return w;
}

void Arm7VBlank() {
    WriteTouch();
    // The ARM7's key word, which Pad_Sample ORs over KEYINPUT: X in bit 10, Y in bit 11, debug in
    // bit 13 (active low), the hinge in bit 15 (clear = open). Bits 0-9 stay clear.
    uint16_t keys = Input().keys;
    uint16_t word = 0x2c00;
    if (keys & (1u << 10)) word &= ~0x0400u;
    if (keys & (1u << 11)) word &= ~0x0800u;
    Write16(0x027fffa8, word);
    if (gTouchAuto) Reply(kTagTouch, 0x10 << 8);
}

}  // namespace khvr::hle
