#pragma once

#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include "drake/common/symbolic.h"
#include "drake/systems/framework/event_collection.h"

namespace drake {
namespace systems {

template <class T>
class System;

enum class WitnessFunctionDirection {
  /// This witness function will never be triggered.
  kNone,

  /// Witness function triggers when the function crosses or touches zero
  /// after an initial positive evaluation.
  kPositiveThenNonPositive,

  /// Witness function triggers when the function crosses or touches zero
  /// after an initial negative evaluation.
  kNegativeThenNonNegative,

  /// Witness function triggers *any time* the function crosses/touches zero,
  /// *except* when the witness function evaluates to zero at the beginning
  /// of the interval. Conceptually equivalent to kPositiveThenNonNegative OR
  /// kNegativeThenNonNegative.
  kCrossesZero,
};

/// Abstract class that describes a function that is able to help determine
/// the time and state at which a simulation should be halted, which may be
/// done for any number of purposes, including publishing or state
/// reinitialization (i.e., event handling).
///
/// For the ensuing discussion, consider two times (`t₀` and `t₁ > t₀`) and
/// states corresponding to those times (`x(t₀)` and `x(t₁)`). A
/// witness function, `w(t, x)`, "triggers" only when it crosses zero at a time
/// `t*` where `t₀ < t* ≤ t₁`. Note the half-open interval. For an example of a
/// witness function, consider the "signed distance" (i.e., Euclidean distance
/// when bodies are disjoint and minimum translational distance when bodies
/// intersect) between two rigid bodies; this witness function can be used to
/// determine both the time of impact for rigid bodies and their states at that
/// time of impact.
///
/// Precision in the definition of the witness function is necessary, because we
/// want the witness function to trigger only once if, for example,
/// `w(t₀, x(t₀)) ≠ 0`, `w(t₁, x(t₁)) = 0`, and `w(t₂, x(t₂)) ≠ 0`, for some
/// t₂ > t₁. In other words, if the witness function is evaluated over the
/// intervals [t₀, t₁] and [t₁, t₂], meaning that the zero occurs precisely at
/// an interval endpoint, the witness function should trigger once. Similarly,
/// the witness function should trigger exactly once if `w(t₀, x(t₀)) ≠ 0`,
/// `w(t*, x(t*)) = 0`, and `w(t₁, x(t₁)) = 0`, for `t* ∈ (t₀, t₁)`. We can
/// define the trigger condition formally over interval `[t₀, t₁]` using the
/// function:<pre>
/// T(w, t₀, x(t₀), t₁) =   1   if w(t₀, x(t₀)) ≠ 0 and
///                                w(t₀, x(t₀))⋅w(t₁, x(t₁)) ≤ 0
///                         0   if w(t₀, x(t₀)) = 0 or
///                                w(t₀, x(t₀))⋅w(t₁, x(t₁)) > 0
/// </pre>
/// We wish for the witness function to trigger if the trigger function
/// evaluates to one. The trigger function can be further modified, if
/// desired, to incorporate the constraint that the witness function should
/// trigger only when crossing from positive values to negative values, or vice
/// versa.
///
/// A good witness function should not cross zero repeatedly over a small
/// interval of time (relative to the maximum designated integration step size)
/// or over small changes in state; when a witness function has
/// been "bracketed" over an interval of time (i.e., it changes sign), that
/// witness function will ideally cross zero only once in that interval.
///
/// A witness function trigger time is isolated only to a small interval of
/// time (as described in Simulator). The disadvantage of this scheme is that it
/// always requires the length of the interval to be reduced to the requisite
/// length *and that each function evaluation (which requires numerical
/// integration) is extraordinarily expensive*. If, for example, the (slow)
/// bisection algorithm were used to isolate the time interval, the number of
/// integrations necessary to cut the interval from a length of ℓ to a length of
/// ε will be log₂(ℓ / ε). Bisection is just one of several possible algorithms
/// for isolating the time interval, though it's a reliable choice and always
/// converges linearly.
template <class T>
class WitnessFunction {
 public:
  DRAKE_NO_COPY_NO_MOVE_NO_ASSIGN(WitnessFunction)

  virtual ~WitnessFunction() {}

  /// Constructs the witness function with the pointer to the given non-null
  /// System, with the given direction type, and with no event type.
  /// @note Constructing a witness function with no corresponding event forces
  ///       Simulator's integration of an ODE to halt at the witness isolation
  ///       time. For example, isolating a function's minimum or maximum values
  ///       can be realized with a witness that triggers on a sign change of
  ///       the function's time derivative, ensuring that the actual extreme
  ///       value is present in the discretized trajectory.
  /// @warning the pointer to the System must be valid as long or longer than
  /// the lifetime of the witness function.
  WitnessFunction(const System<T>* system,
                  const WitnessFunctionDirection& dtype) :
                  system_(system), dir_type_(dtype) { DRAKE_DEMAND(system); }

  /// Constructs the witness function with the pointer to the given non-null
  /// System, with the given direction type, and with a unique pointer to the
  /// event that is to be dispatched when this witness function triggers.
  /// Example events are publish, discrete variable update, unrestricted update
  /// events.
  /// @tparam EventType a class derived from Event<T>
  /// @warning the pointer to the System must be valid as long or longer than
  /// the lifetime of the witness function.
  template <class EventType>
  WitnessFunction(const System<T>* system,
                  const WitnessFunctionDirection& dtype,
                  std::unique_ptr<EventType> e) :
                  system_(system), dir_type_(dtype) {
    static_assert(std::is_base_of<Event<T>, EventType>::value,
        "EventType must be a descendant of Event");
    DRAKE_DEMAND(system);
    event_ = std::move(e);
  }

  /// Gets the name of this witness function (used primarily for logging and
  /// debugging).
  const std::string& get_name() const { return name_; }

  /// Sets the name of this witness function.
  void set_name(const std::string& name) { name_ = name; }

  /// Gets the direction(s) under which this witness function triggers.
  WitnessFunctionDirection get_dir_type() const { return dir_type_; }

  /// Evaluates the witness function at the given context.
  T CalcWitnessValue(const Context <T>& context) const;

  /// Gets a reference to the System used by this witness function.
  const System<T>& get_system() const { return *system_; }

  /// Checks whether the witness function should trigger using given
  /// values at w0 and wf. Note that this function is not specific to a
  /// particular witness function.
  decltype(T() < T()) should_trigger(const T& w0, const T& wf) const {
    WitnessFunctionDirection dtype = get_dir_type();

    const T zero(0);
    switch (dtype) {
      case WitnessFunctionDirection::kNone:
        return (T(0) > T(0));

      case WitnessFunctionDirection::kPositiveThenNonPositive:
        return (w0 > zero && wf <= zero);

      case WitnessFunctionDirection::kNegativeThenNonNegative:
        return (w0 < zero && wf >= zero);

      case WitnessFunctionDirection::kCrossesZero:
        return ((w0 > zero && wf <= zero) ||
                (w0 < zero && wf >= zero));

      default:
        DRAKE_ABORT();
    }
  }

  /// Sets the event that will be dispatched when the witness function
  /// triggers. If @p e is null, no event will be dispatched.
  template <class EventType>
  void set_event(std::unique_ptr<EventType> e) {
    event_ = std::move(e);
  }

  /// Gets the event that will be dispatched when the witness function
  /// triggers. A null pointer indicates that no event will be dispatched.
  const Event<T>* get_event() const { return event_.get(); }

  /// Gets a mutable pointer to the event that will occur when the witness
  /// function triggers.
  Event<T>* get_mutable_event() { return event_.get(); }

 protected:
  /// Derived classes will implement this function to evaluate the witness
  /// function at the given context.
  /// @param context an already-validated Context
  virtual T DoCalcWitnessValue(const Context <T>& context) const = 0;

  // The name of this witness function.
  std::string name_;

 private:
  // A reference to the system.
  const System<T>* system_{nullptr};

  // Direction(s) under which this witness function triggers.
  WitnessFunctionDirection dir_type_;

  // Unique pointer to the event.
  std::unique_ptr<Event<T>> event_;
};

}  // namespace systems
}  // namespace drake
