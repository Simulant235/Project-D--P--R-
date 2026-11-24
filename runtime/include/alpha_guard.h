// Alpha-Ablauf, von Launcher und Spiel-.exe genutzt
#pragma once
#include <cstdint>

namespace alpha {

enum class State { NoLimit, Ok, Expired, Tampered };

struct Result {
    State state = State::NoLimit;
    bool ntp_ok = false;        // echte Zeit vom Zeitserver erhalten
    long long real = 0;         // echte Zeit (Unix-Sekunden, UTC), 0 = unbekannt
    long long local = 0;        // PC-Uhr (Unix-Sekunden, UTC)
    int expires = 0;            // letzter gültiger Tag JJJJMMTT (0 = kein Ablauf)
    int release = 0;            // Datum des Alpha-Builds JJJJMMTT
};

// Prüft die Testphase. test: nullptr/"" = echt, "expired" = Fall A erzwingen, "tamper" = Fall B erzwingen.
// Fragt dafür kurz (timeout_ms) per NTP die echte Zeit ab; ohne Netz gilt die PC-Uhr.
Result check(const char* test, int timeout_ms = 1200);

} // namespace alpha
