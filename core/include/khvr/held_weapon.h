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

// Finds the frame of a weapon from its vertices. The model's origin is taken as its grip (where
// the game attaches it to the hand) when it lies near one end of the blade; otherwise the grip
// is a little in from the end nearest the origin. The blade runs to the vertex farthest from the
// grip, the teeth to the vertex farthest off the blade.
WeaponFrame FindWeaponFrame(const std::vector<Vec3> &points);

}  // namespace khvr
