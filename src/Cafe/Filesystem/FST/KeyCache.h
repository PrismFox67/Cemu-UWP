#pragma once

void KeyCache_Prepare();
void KeyCache_Reload(); // reads keys.txt again after it was changed

uint8* KeyCache_GetAES128(sint32 index);