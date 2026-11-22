// Browser input for the engine: library_bo1.js installs the page's listeners and fills g_bo1Input (web_window.cpp
// turns it into window messages); gamepads go through XInput, which the engine's win_gamepad.cpp polls.
#include <windows.h>
#include <xinput.h>

#include <stddef.h>

#include <emscripten.h>

#include "web_platform.h"

extern "C" {
// library_bo1.js, run on the page's thread
void bo1_input_install(Bo1InputShared *shared, int eventsOffset, int eventStride, int cursorOffset, int keyStateOffset, int sizeOffset,
    int gamepadOffset, int inputTimeOffset);
}

// gamepad state the page copies from navigator.getGamepads() every animation frame, and the motors' speeds it plays
// on the gamepad (XInputSetState)
struct Bo1GamepadShared
{
    volatile int32_t connected[XUSER_MAX_COUNT];
    volatile uint32_t packet[XUSER_MAX_COUNT];
    XINPUT_GAMEPAD pad[XUSER_MAX_COUNT];
    volatile uint16_t motors[XUSER_MAX_COUNT][2]; // the left (low frequency) and right (high frequency) motor, of 65535
};
static Bo1GamepadShared s_gamepads;

void Bo1_InitInput()
{
    g_bo1Input.hasFocus = 1;
    g_bo1Input.cursorShown = 1; // as on Windows, until the engine hides it
    g_bo1Input.fullScreenSet = -1; // nothing set by the page; it puts the player's setting in fullScreen (below)
    bo1_input_install(&g_bo1Input, (int)offsetof(Bo1InputShared, events), (int)sizeof(Bo1InputEvent), (int)offsetof(Bo1InputShared, cursorX),
        (int)offsetof(Bo1InputShared, keyState), (int)offsetof(Bo1InputShared, width), (int)((char *)&s_gamepads - (char *)&g_bo1Input),
        (int)offsetof(Bo1InputShared, inputTime));
    Bo1_Log("input: canvas %dx%d", g_bo1Input.width, g_bo1Input.height);
}

extern "C" DWORD WINAPI XInputGetState(DWORD dwUserIndex, XINPUT_STATE *pState)
{
    if (dwUserIndex >= XUSER_MAX_COUNT || !s_gamepads.connected[dwUserIndex])
        return ERROR_DEVICE_NOT_CONNECTED;
    pState->dwPacketNumber = s_gamepads.packet[dwUserIndex];
    pState->Gamepad = s_gamepads.pad[dwUserIndex];
    return ERROR_SUCCESS;
}

// the gamepad's rumble (src/win32/win_gamepad.cpp: GPad_UpdateRumble), which the page plays (library_bo1.js: pollPads)
extern "C" DWORD WINAPI XInputSetState(DWORD dwUserIndex, XINPUT_VIBRATION *pVibration)
{
    if (dwUserIndex >= XUSER_MAX_COUNT)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (pVibration)
    {
        s_gamepads.motors[dwUserIndex][0] = pVibration->wLeftMotorSpeed;
        s_gamepads.motors[dwUserIndex][1] = pVibration->wRightMotorSpeed;
    }
    return s_gamepads.connected[dwUserIndex] ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" DWORD WINAPI XInputGetCapabilities(DWORD dwUserIndex, DWORD, XINPUT_CAPABILITIES *pCapabilities)
{
    if (dwUserIndex >= XUSER_MAX_COUNT || !s_gamepads.connected[dwUserIndex])
        return ERROR_DEVICE_NOT_CONNECTED;
    memset(pCapabilities, 0, sizeof(*pCapabilities));
    pCapabilities->Type = XINPUT_DEVTYPE_GAMEPAD;
    pCapabilities->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    return ERROR_SUCCESS;
}

extern "C" void WINAPI XInputEnable(BOOL) {}
