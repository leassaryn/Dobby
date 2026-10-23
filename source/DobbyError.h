#pragma once

#include "dobby.h"

void DobbySetError(DobbyError error, int system_error = 0);
int DobbyFailure(DobbyError error, int system_error = 0);
int DobbySuccess();

bool DobbyIsBtiGuarded(void *address);
