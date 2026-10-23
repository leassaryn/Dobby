#pragma once

#include "dobby.h"

void DobbySetError(DobbyError error, int system_error = 0);
int DobbyFailure(DobbyError error, int system_error = 0);
int DobbySuccess();

bool DobbyIsBtiGuarded(void *address);

void DobbyRecordEvent(DobbyEventKind kind, DobbyError error, void *address = nullptr, size_t size = 0,
                      uint64_t detail = 0, int system_error = 0);
