/*
 * Copyright 2019-present Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <unifex/config.hpp>
#include <unifex/tag_invoke.hpp>
#include <unifex/receiver_concepts.hpp>
#include <unifex/sender_concepts.hpp>
#include <unifex/type_traits.hpp>
#include <unifex/type_list.hpp>
#include <unifex/manual_lifetime.hpp>
#include <unifex/manual_lifetime_union.hpp>
#include <unifex/async_trace.hpp>

#include <utility>
#include <cassert>
#include <exception>

namespace unifex {

namespace detail {

template<typename Source, typename Receiver>
class unstoppable_operation;

template<typename Source, typename Receiver>
class unstoppable_source_receiver;

template<typename Source, typename Receiver>
class unstoppable_source_receiver {
  using operation = unstoppable_operation<Source, Receiver>;
public:
  explicit unstoppable_source_receiver(operation* op) noexcept
  : op_(op) {}

  unstoppable_source_receiver(unstoppable_source_receiver&& other) noexcept
  : op_(std::exchange(other.op_, {}))
  {}
 
  void set_value() {
    assert(op_ != nullptr);
    unifex::set_value(std::move(op_->receiver_));
  }

  template<
    typename R = Receiver,
    std::enable_if_t<std::is_invocable_v<decltype(unifex::set_done), R>, int> = 0>
  void set_done() noexcept {
    assert(op_ != nullptr);
    unifex::set_value(std::move(op_->receiver_));
  }

  template<
    typename Error,
    std::enable_if_t<std::is_invocable_v<decltype(unifex::set_error), Receiver, Error>, int> = 0>
  void set_error(Error&& error) noexcept {
    assert(op_ != nullptr);
    unifex::set_error(std::move(op_->receiver_), (Error&&)error);
  }

private:
  template<typename CPO, typename... Args>
  friend auto tag_invoke(CPO cpo, const unstoppable_source_receiver& r, Args&&... args)
      noexcept(std::is_nothrow_invocable_v<CPO, const Receiver&, Args...>)
      -> std::invoke_result_t<CPO, const Receiver&, Args...> {
    return std::move(cpo)(r.get_receiver(), (Args&&)args...);
  }
  
  template <typename VisitFunc>
  friend void tag_invoke(
      tag_t<visit_continuations>,
      const unstoppable_source_receiver& r,
      VisitFunc&& func) noexcept(std::is_nothrow_invocable_v<
                                VisitFunc&,
                                const Receiver&>) {
    std::invoke(func, r.get_receiver());
  }

  const Receiver& get_receiver() const noexcept {
    assert(op_ != nullptr);   
    return op_->receiver_;
  }

  operation* op_;
};

template<typename Source, typename Receiver>
class unstoppable_operation {
  using source_receiver = detail::unstoppable_source_receiver<Source, Receiver>;

public:
  template<typename Source2, typename Receiver2>
  explicit unstoppable_operation(Source2&& source, Receiver2&& receiver)
      noexcept(std::is_nothrow_constructible_v<Source, Source2> &&
               std::is_nothrow_constructible_v<Receiver, Receiver2> &&
               is_nothrow_connectable_v<Source&, source_receiver>)
  : source_((Source2&&)source)
  , receiver_((Receiver&&)receiver)
  {
    sourceOp_.construct_from([&] {
        return unifex::connect(source_, source_receiver{this});
      });
  }

  ~unstoppable_operation() {
    if (isSourceOpConstructed_) {
      sourceOp_.destruct();
      isSourceOpConstructed_ = false;
    }
  }

  void start() & noexcept {
    unifex::start(sourceOp_.get());
  }

private:
  friend unstoppable_source_receiver<Source, Receiver>;

  using source_op_t = operation_t<Source&, unstoppable_source_receiver<Source, Receiver>>;

  UNIFEX_NO_UNIQUE_ADDRESS Source source_;
  UNIFEX_NO_UNIQUE_ADDRESS Receiver receiver_;
  bool isSourceOpConstructed_ = true;
  manual_lifetime<source_op_t> sourceOp_;
};

} // namespace detail

template<typename Source>
class unstoppable_sender {

public:
  template<template<typename...> class Variant,
           template<typename...> class Tuple>
  using value_types = Variant<Tuple<>>;

  template<template<typename...> class Variant>
  using error_types = typename Source::template error_types<Variant>;

  template<typename Source2>
  explicit unstoppable_sender(Source2&& source)
    noexcept(std::is_nothrow_constructible_v<Source, Source2>)
  : source_((Source2&&)source)
  {}

  template<
    typename Receiver,
    typename Op = detail::unstoppable_operation<Source, std::remove_cvref_t<Receiver>>>
  Op connect(Receiver&& r) &&
      noexcept(std::is_nothrow_constructible_v<Op, Source, Receiver>) 
    {
    return Op{(Source&&)source_, (Receiver&&)r};
  }

  template<
    typename Receiver,
    typename Op = detail::unstoppable_operation<Source, std::remove_cvref_t<Receiver>>>
  Op connect(Receiver&& r) const &
      noexcept(std::is_nothrow_constructible_v<Op, const Source&, Receiver>) 
    {
      return Op{source_, (Receiver&&)r};
  }

private:
  Source source_;
};

inline constexpr struct unstoppable_cpo {
  template<typename Source>
  auto operator()(Source&& source) const
      noexcept(is_nothrow_tag_invocable_v<unstoppable_cpo, Source>)
      -> tag_invoke_result_t<unstoppable_cpo, Source> {
    return tag_invoke(*this, (Source&&)source);
  }

  template<
    typename Source,
    std::enable_if_t<
        !is_tag_invocable_v<unstoppable_cpo, Source> &&
        std::is_constructible_v<std::remove_cvref_t<Source>, Source>, int> = 0>
  auto operator()(Source&& source) const
      noexcept(std::is_nothrow_constructible_v<
                   unstoppable_sender<std::remove_cvref_t<Source>>,
                   Source>)
      -> unstoppable_sender<std::remove_cvref_t<Source>> {
    return unstoppable_sender<std::remove_cvref_t<Source>>{
        (Source&&)source};
  }
} unstoppable{};

} // namespace unifex