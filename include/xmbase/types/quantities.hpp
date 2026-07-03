/*
 * quantities.hpp
 *
 * OPT-IN strong-typed 3D quantities. The geometry.hpp aliases (Velocity3d,
 * Force3d, ...) are all Eigen::Vector3d, so the compiler cannot stop you passing
 * a torque where a velocity is expected — the classic robotics mix-up. These
 * tagged wrappers make distinct quantities distinct *types* while keeping
 * natural arithmetic; reach the underlying Eigen vector explicitly via .vec().
 *
 * This header is NOT pulled in by types.hpp / the legacy facades: adopt it where
 * the safety is worth the explicit .vec() at Eigen boundaries. Mixing tags is a
 * compile error; arithmetic is closed within a tag (Force + Force -> Force,
 * Force * scalar -> Force).
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <utility>

#include <Eigen/Dense>

namespace xmotion {

template <typename Tag>
class Vec3 {
 public:
  using EigenType = Eigen::Vector3d;

  Vec3() = default;
  explicit Vec3(EigenType v) : v_(std::move(v)) {}
  Vec3(double x, double y, double z) : v_(x, y, z) {}

  // Explicit escape hatch to Eigen math (kept explicit on purpose — that is
  // what preserves the type safety).
  EigenType& vec() { return v_; }
  const EigenType& vec() const { return v_; }

  double x() const { return v_.x(); }
  double y() const { return v_.y(); }
  double z() const { return v_.z(); }
  double norm() const { return v_.norm(); }

  // Arithmetic closed within the tag.
  Vec3 operator+(const Vec3& o) const { return Vec3(v_ + o.v_); }
  Vec3 operator-(const Vec3& o) const { return Vec3(v_ - o.v_); }
  Vec3 operator-() const { return Vec3(-v_); }
  Vec3 operator*(double s) const { return Vec3(v_ * s); }
  Vec3 operator/(double s) const { return Vec3(v_ / s); }
  Vec3& operator+=(const Vec3& o) { v_ += o.v_; return *this; }
  Vec3& operator-=(const Vec3& o) { v_ -= o.v_; return *this; }

  bool operator==(const Vec3& o) const { return v_ == o.v_; }
  bool operator!=(const Vec3& o) const { return v_ != o.v_; }

  static Vec3 Zero() { return Vec3(EigenType::Zero()); }

 private:
  EigenType v_ = EigenType::Zero();
};

template <typename Tag>
Vec3<Tag> operator*(double s, const Vec3<Tag>& v) {
  return v * s;
}

// Distinct quantity tags. Add more as needed; each new tag is a new, unmixable
// type.
namespace quantity_tag {
struct LinearVelocity {};
struct AngularVelocity {};
struct LinearAcceleration {};
struct Force {};
struct Torque {};
}  // namespace quantity_tag

using LinearVelocity = Vec3<quantity_tag::LinearVelocity>;
using AngularVelocity = Vec3<quantity_tag::AngularVelocity>;
using LinearAcceleration = Vec3<quantity_tag::LinearAcceleration>;
using Force = Vec3<quantity_tag::Force>;
using Torque = Vec3<quantity_tag::Torque>;

}  // namespace xmotion
