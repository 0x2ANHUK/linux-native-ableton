// Driver types in Live's audio settings (audiodrivers.cpp).
#pragma once

enum class AudioBackend { PipeWire = 0, Alsa = 1, Jack = 2 };

// The backend whose architecture is listing devices on this thread right now
// (PipeWire outside a scan).
AudioBackend scanBackend();

// The three driver types are in place (hooks installed for this Live build).
bool driverTypesActive();
