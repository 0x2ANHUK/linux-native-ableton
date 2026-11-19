// PipeWire devices listed in Live's audio device menus (pwdevices.cpp).
#pragma once
#include <string>

constexpr int kFirstVirtualCard = 100;
constexpr int kJackCard = 200; // driver type JACK: its single device (audiodrivers.cpp)

struct VirtualDevice {
    std::string node; // PipeWire node name; empty: the desktop's default devices
    std::string name;
    bool playback, capture;
    unsigned playbackChannels, captureChannels;
};

bool virtualDevicesEnabled();
bool virtualDevice(int card, VirtualDevice &device);

// Channels of the JACK device: the server's physical playback/capture ports (jackaudio.cpp; 2 without a server).
unsigned jackChannels(bool playback);

// PipeWire's clock.force-quantum and clock.min-quantum (frames at the graph's rate, 0 = not set) and the
// graph's rate (pwgraph.cpp). false: PipeWire did not answer.
bool pipewireClock(unsigned &force, unsigned &minimum, unsigned &rate);
