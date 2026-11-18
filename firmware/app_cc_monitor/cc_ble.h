#pragma once
/*
 * Bluetooth build only (CC_MONITOR_BLE): the robot's link to cc-ble on the paired Mac.
 * One GATT characteristic carries JSON both ways, split into packets (frames.h): the Mac writes, the robot
 * notifies. Only a bonded, encrypted, passkey-authenticated link may write or receive anything.
 * A new Mac may pair only while frames::may_pair() holds (USB plugged into a computer, first 2 minutes).
 */
#include <cstdint>
#include <functional>
#include <string>

namespace cc_ble {

enum class State {
    NotPaired,   // no bond, pairing window closed
    Pairing,     // no bond, pairing window open
    Waiting,     // bonded, the Mac isn't connected
    Connected,   // the paired Mac is connected over a secure link
};

// All three run on the NimBLE host task: don't touch LVGL in them.
struct Events {
    std::function<void(std::string)> on_message;   // a whole JSON message from the Mac
    std::function<void(uint32_t)> on_passkey;      // code to show while pairing; 0 = pairing over
    std::function<void(State)> on_state;
};

void start(Events ev);                // once; starts Bluetooth and advertising
bool send(const std::string& json);   // false when the paired Mac isn't connected securely

}   // namespace cc_ble
