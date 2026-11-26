#pragma once
#include <stddef.h>
#include <stdint.h>

// Asks the human: blocks until the BOOT button is pressed (true) or 30 s pass (false).
bool waitForApproval(const uint8_t* message, size_t len);
