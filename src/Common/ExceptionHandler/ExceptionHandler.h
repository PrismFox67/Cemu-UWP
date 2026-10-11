#pragma once

void ExceptionHandler_Init();

bool CrashLog_Create();
void CrashLog_SetOutputChannels(bool writeToStdErr, bool writeToLogTxt);
void CrashLog_WriteLine(std::string_view text, bool newLine = true);
void CrashLog_WriteHeader(const char* header);

void ExceptionHandler_LogGeneralInfo();

// called first when the process crashes, on the crashing thread (which may be out of stack: keep it small, no allocations)
void ExceptionHandler_SetCrashCallback(void (*callback)());
void ExceptionHandler_RunCrashCallback();
