// BatteryHub's test signature covers these exact bytes, including the original file names.
#include "../src/update_signature.h"

#include "check.h"
#include <cstdio>
#include <string>

using namespace updates;

int main() {
    const std::string key = "RUNTMSAAAAAXg7njAQm2KvYEjKiVPaIEfV2WCDfTUDss+EegIuyB0N3musI8uy6MWetKMc9YxiHaGtBCkuE9oU0M7pNnojeR";
    const std::string otherKey = "RUNTMSAAAABZ7G3LXrKOdli89SBd+tu4Sknb1Fmner6WWKLzVqER9beOAN/Iy3Y5c9e4/au3sylRb/t2Wl3yVxuijCYv5zEP";
    const std::string setupHash = "abababababababababababababababababababababababababababababababab";
    const std::string sums = setupHash + "  WirelessBattery-Setup-9.9.9.exe\n"
                             "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd  WirelessBattery-9.9.9-portable.zip\n";
    const std::string signature = "QzIFxH0pL6rNBWejKwS7WDHz8gnieXMHSqXexlUyHQ/cLXhk3/WFN3WpzdkyR3pb8BPFPkyEExSQ4zr6y1x3Vw==\n";

    // The signature made by the PowerShell script verifies with its key, and with nothing else.
    CHECK(VerifySignature(sums, signature, key));
    CHECK(!VerifySignature(sums, signature, otherKey));
    // Any change to the list breaks it: one hash digit, the file name, an extra line.
    std::string tampered = sums;
    tampered[0] = 'b';
    CHECK(!VerifySignature(tampered, signature, key));
    CHECK(!VerifySignature(sums + "ef  evil.exe\n", signature, key));
    std::string renamed = sums;
    renamed.replace(renamed.find("9.9.9.exe"), 5, "9.9.8");
    CHECK(!VerifySignature(renamed, signature, key));
    // A changed or malformed signature or key.
    std::string flipped = signature;
    flipped[10] = flipped[10] == 'A' ? 'B' : 'A';
    CHECK(!VerifySignature(sums, flipped, key));
    CHECK(!VerifySignature(sums, "", key));
    CHECK(!VerifySignature(sums, "not base64!", key));
    CHECK(!VerifySignature(sums, signature.substr(0, 40), key));
    CHECK(!VerifySignature(sums, signature, ""));
    CHECK(!VerifySignature(sums, signature, key.substr(0, 40)));
    CHECK(!VerifySignature("", signature, key));
    CHECK(!VerifySignature(std::string(MaxSumsBytes + 1, 'a'), signature, key));
    CHECK(!VerifySignature(sums, std::string(MaxSignatureBytes + 1, 'A'), key));

    // The release key is configured, and a signature by any other key (this test key) is not a release.
    CHECK(ReleaseKeyConfigured());
    CHECK(!VerifyReleaseSignature(sums, signature));

    // Hash lookup in the list.
    std::string hex;
    CHECK(SignedHash(sums, "WirelessBattery-Setup-9.9.9.exe", hex) && hex == setupHash);
    CHECK(SignedHash(sums, "WirelessBattery-9.9.9-portable.zip", hex) && hex[0] == 'c');
    CHECK(SignedHash(std::string(64, 'F') + " *x.exe\r\n", "x.exe", hex) && hex == std::string(64, 'f'));
    CHECK(!SignedHash(sums, "WirelessBattery-Setup-9.9.8.exe", hex) && hex.empty());
    CHECK(!SignedHash(sums, "WirelessBattery-Setup-9.9.9.ex", hex));   // whole name only
    CHECK(!SignedHash(sums + sums, "WirelessBattery-Setup-9.9.9.exe", hex));  // listed twice
    CHECK(!SignedHash(std::string(63, 'a') + "  x.exe\n", "x.exe", hex));  // short hash
    CHECK(!SignedHash(std::string(64, 'g') + "  x.exe\n", "x.exe", hex));  // not hex
    CHECK(!SignedHash(std::string(64, 'a') + "\tx.exe\n", "x.exe", hex));  // wrong separator
    CHECK(!SignedHash("", "x.exe", hex));

    std::string decoded;
    CHECK(Base64Decode("TWE=", decoded) && decoded == "Ma");
    CHECK(Base64Decode("TWFu", decoded) && decoded == "Man");
    CHECK(!Base64Decode("TW=u", decoded) && !Base64Decode("TWF", decoded) && !Base64Decode("TW\nu", decoded));
    std::puts("Update signature tests passed (PowerShell-made signature, tampering, wrong key, checksum list).");
}
