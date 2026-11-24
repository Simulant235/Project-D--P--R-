// Alpha-Ablauf. Echte Zeit per NTP (pool.ntp.org), ohne Netz gilt die PC-Uhr.
// Daten liegen XOR-maskiert im Programm (cmake/alpha.cmake).
//   Fall A: Zeit nach Ablaufdatum -> Expired
//   Fall B: PC-Uhr weicht stark von NTP ab oder liegt vor dem Build-Datum -> Tampered
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstring>
#include <ctime>

#include "alpha_guard.h"

#ifndef DIVA_ALPHA_EXPIRES_X
#define DIVA_ALPHA_EXPIRES_X 0
#define DIVA_ALPHA_RELEASE_X 0
#endif

namespace alpha {
namespace {

int unmask(long long x) {
    if (!x) return 0;
    volatile unsigned key = 0x5A5A5A5Au;   // = DIVA_ALPHA_KEY in cmake/alpha.cmake
    return (int)((unsigned)x ^ key);
}

int ymd(long long unix_seconds) {   // JJJJMMTT (UTC)
    const time_t t = (time_t)unix_seconds;
    tm g;
    gmtime_s(&g, &t);
    return (g.tm_year + 1900) * 10000 + (g.tm_mon + 1) * 100 + g.tm_mday;
}

long long days_from_ymd(int d) {   // Tage seit 1970 (für Vergleiche)
    tm g = {};
    g.tm_year = d / 10000 - 1900;
    g.tm_mon = d / 100 % 100 - 1;
    g.tm_mday = d % 100;
    return (long long)_mkgmtime(&g) / 86400;
}

// SNTP (RFC 4330): 48 Byte raus, Sendezeit des Servers lesen
long long ntp_now(int timeout_ms) {
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return 0;
    long long result = 0;
    addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo("pool.ntp.org", "123", &hints, &res) == 0 && res) {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s != INVALID_SOCKET) {
            DWORD to = (DWORD)timeout_ms;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof to);
            unsigned char pkt[48] = {0x1B};   // LI 0, Version 3, Modus 3 (Client)
            if (sendto(s, (const char*)pkt, 48, 0, res->ai_addr, (int)res->ai_addrlen) == 48 &&
                recv(s, (char*)pkt, 48, 0) == 48) {
                const unsigned secs = ((unsigned)pkt[40] << 24) | ((unsigned)pkt[41] << 16) | ((unsigned)pkt[42] << 8) | pkt[43];
                if (secs) result = (long long)secs - 2208988800LL;   // NTP-Epoche 1900 -> Unix 1970
            }
            closesocket(s);
        }
        freeaddrinfo(res);
    }
    WSACleanup();
    return result;
}

} // namespace

Result check(const char* test, int timeout_ms) {
    Result r;
    r.expires = unmask(DIVA_ALPHA_EXPIRES_X);
    r.release = unmask(DIVA_ALPHA_RELEASE_X);
    r.local = (long long)time(nullptr);
    if (test && strcmp(test, "expired") == 0) { r.state = State::Expired; return r; }
    if (test && strcmp(test, "tamper") == 0) { r.state = State::Tampered; return r; }
    if (!r.expires) return r;   // kein Ablauf (kein Alpha-Build)
    r.real = ntp_now(timeout_ms);
    r.ntp_ok = r.real != 0;
    const long long day_local = r.local / 86400;
    // Fall B: Uhr > 24 h daneben oder vor dem Build-Datum
    if ((r.ntp_ok && (r.local - r.real > 86400 || r.real - r.local > 86400)) ||
        (r.release && day_local < days_from_ymd(r.release) - 1)) {
        r.state = State::Tampered;
        return r;
    }
    // Fall A: abgelaufen
    r.state = ymd(r.ntp_ok ? r.real : r.local) > r.expires ? State::Expired : State::Ok;
    return r;
}

} // namespace alpha
