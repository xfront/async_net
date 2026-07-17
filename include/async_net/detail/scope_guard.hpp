#pragma once

#include <utility>

namespace async_net {

template<typename Func>
class scope_guard {
public:
    explicit scope_guard(Func&& func) noexcept
        : func_(std::move(func)), active_(true) {}

    ~scope_guard() {
        if (active_) {
            func_();
        }
    }

    scope_guard(const scope_guard&) = delete;
    scope_guard& operator=(const scope_guard&) = delete;

    scope_guard(scope_guard&& other) noexcept
        : func_(std::move(other.func_)), active_(other.active_) {
        other.active_ = false;
    }

    void dismiss() { active_ = false; }

private:
    Func func_;
    bool active_;
};

template<typename Func>
scope_guard<Func> make_scope_guard(Func&& func) {
    return scope_guard<Func>(std::move(func));
}

} // namespace async_net
