#pragma once

class ReadingLogger {
public:
    static void logPageTurn();
    // Path to the historical CSV log (rows are written only when battery% or
    // charging state actually changes, not on every page turn).
    static const char* logPath();
};
