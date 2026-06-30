/*
 * geometry.hpp
 *
 * Eigen-backed geometry vocabulary for the motion / control layer.
 *
 * Conventions (documented, not enforced — keep them):
 *   - Units are SI: metres, m/s, m/s^2, radians, rad/s, newtons, newton-metres.
 *   - Angles are radians.
 *   - Euler angles are intrinsic Z-Y-X (yaw, pitch, roll).
 *   - Quaternions are unit quaternions, Hamilton convention (Eigen's default).
 *   - Frames are not encoded in the type system; name them at API boundaries
 *     (e.g. Odometry carries frame_id / child_frame_id). Be explicit about the
 *     frame a value lives in — mixing frames is the classic silent-failure bug.
 *
 * Note: Pose/Twist/Wrench embed fixed-size vectorizable Eigen types
 * (Quaterniond). This is safe under C++17 (over-aligned new is the default, so
 * std::vector<Pose> etc. work); revisit the Eigen alignment rules if this code
 * is ever built as C++14 or for a 32-bit target.
 *
 * Copyright (c) 2024-2026 Ruixiang Du (rdu)
 */

#pragma once

#include <string>

#include <Eigen/Dense>
#include <Eigen/Geometry>

#include "xmsigma/types/time.hpp"

namespace xmotion {

// --- raw geometric quantities (aliases of Eigen vectors) --------------------
// These are deliberately ergonomic aliases (full Eigen math, implicit interop).
// They carry semantic intent by name only; for compile-time distinctness of
// dangerous quantities (force vs velocity, ...) see quantities.hpp.

// workspace Cartesian coordinates
using Position3d = Eigen::Vector3d;
using Quaterniond = Eigen::Quaterniond;
using RotMatrix3d = Eigen::Matrix<double, 3, 3>;
using HomoMatrix3d = Eigen::Matrix<double, 4, 4>;

using Velocity3d = Eigen::Vector3d;
using Acceleration3d = Eigen::Vector3d;

// joint space coordinates
using JointPosition3d = Eigen::Vector3d;
using JointVelocity3d = Eigen::Vector3d;

// force / torque
using Force3d = Eigen::Vector3d;
using Torque3d = Eigen::Vector3d;

// --- composite types --------------------------------------------------------
// All members are default-initialized to a meaningful identity/zero so a
// default-constructed value is well-defined (a default Pose is identity, not
// garbage — Eigen does NOT zero/identity-init quaternions on its own).

struct Twist {
  Velocity3d linear = Velocity3d::Zero();
  Velocity3d angular = Velocity3d::Zero();
};

struct Wrench {
  Force3d force = Force3d::Zero();
  Torque3d torque = Torque3d::Zero();
};

struct Pose {
  Position3d position = Position3d::Zero();
  Quaterniond orientation = Quaterniond::Identity();
};

struct Euler {
  float roll{};
  float pitch{};
  float yaw{};
};

// Pose of child_frame_id expressed in frame_id, with its velocity (twist) and
// the time the estimate is valid for.
struct Odometry {
  std::string child_frame_id;        // the body this odometry describes
  Pose pose;                         // pose of child_frame_id in frame_id
  Twist twist;                       // velocity of child_frame_id
  std::string frame_id;              // reference frame the pose is expressed in
  Timestamp stamp{};                 // time the estimate is valid for
};

}  // namespace xmotion
