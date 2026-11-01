#pragma once
// The game's own Keyblade in your VR hand: its model, as the game gave it to the DS in the
// model's own space, turned into a frame where it lies along the controller's blade axis.

#include <vector>

#include "khvr/math.h"

namespace khvr {

// Where a weapon model lies in its own space: its grip, the way its blade runs from there, the
// way its teeth stick out across the blade, and its length (grip to tip).
struct WeaponFrame {
    Vec3 grip;
    Vec3 blade;  // unit, grip to tip
    Vec3 side;   // unit, across the blade towards the teeth
    float length = 0.0f;
    bool valid = false;

    // A point of the model in the hand's frame, one blade length a unit: the blade along -Z
    // (the way BladeAxis points), the teeth towards +X, the grip at the origin.
    Vec3 ToHand(const Vec3 &p) const;
};

// Finds the frame of a weapon from its vertices alone (its model's origin may be anywhere): the
// blade between its two points farthest apart, the tip the end its teeth (the point farthest off
// that line) are at, the grip a little in from the other end.
WeaponFrame FindWeaponFrame(const std::vector<Vec3> &points);

// Where a hand model lies in its hand joint's own space (the joint's pivot, the wrist, at the
// origin): its middle, the way it runs from the wrist to the knuckles, the way its palm faces
// (its thinnest way across), and its length along the first.
struct HandFrame {
    Vec3 centre;
    Vec3 fingers;  // unit, wrist to knuckles
    Vec3 palm;     // unit, across the hand (which of its two faces the palm is is not known)
    float length = 0.0f;
    bool valid = false;

    // A point of the model in the hand's frame, one hand length a unit: the palm's way along +X,
    // wrist to knuckles along +Y, the middle at the origin.
    Vec3 ToHand(const Vec3 &p) const;
};

HandFrame FindHandFrame(const std::vector<Vec3> &points);

}  // namespace khvr
