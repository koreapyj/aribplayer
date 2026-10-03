#pragma once
// Small multicast event helper. Thread-safe add/remove/raise; handlers are invoked outside the lock
// on a snapshot, so a handler may add/remove subscriptions (including its own) while being raised.
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace wvp {

template <class... Args>
class Event {
public:
    using Handler = std::function<void(Args...)>;
    using Token = std::uint64_t;          // 0 is never a valid token

    Event() = default;
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;

    Token add(Handler h) {
        std::lock_guard<std::mutex> lk(mu_);
        Token t = ++next_;
        auto list = std::make_shared<List>(list_ ? *list_ : List{});
        list->emplace_back(t, std::make_shared<Handler>(std::move(h)));
        list_ = std::move(list);
        return t;
    }

    void remove(Token t) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!list_) return;
        auto list = std::make_shared<List>();
        for (auto& e : *list_) if (e.first != t) list->push_back(e);
        list_ = std::move(list);
    }

    void clear() { std::lock_guard<std::mutex> lk(mu_); list_.reset(); }

    bool empty() const { std::lock_guard<std::mutex> lk(mu_); return !list_ || list_->empty(); }

    void raise(Args... args) const {
        std::shared_ptr<List> snap;
        { std::lock_guard<std::mutex> lk(mu_); snap = list_; }
        if (!snap) return;
        for (auto& e : *snap) (*e.second)(args...);
    }
    void operator()(Args... args) const { raise(args...); }

private:
    using List = std::vector<std::pair<Token, std::shared_ptr<Handler>>>;
    mutable std::mutex mu_;
    std::shared_ptr<List> list_;
    Token next_ = 0;
};

template <class... Args>
class Subscription {
public:
    Subscription() = default;
    Subscription(Event<Args...>& ev, typename Event<Args...>::Handler h) : ev_(&ev), tok_(ev.add(std::move(h))) {}
    Subscription(Subscription&& o) noexcept : ev_(o.ev_), tok_(o.tok_) { o.ev_ = nullptr; }
    Subscription& operator=(Subscription&& o) noexcept {
        if (this != &o) { reset(); ev_ = o.ev_; tok_ = o.tok_; o.ev_ = nullptr; }
        return *this;
    }
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    ~Subscription() { reset(); }
    void reset() { if (ev_) { ev_->remove(tok_); ev_ = nullptr; } }
private:
    Event<Args...>* ev_ = nullptr;
    typename Event<Args...>::Token tok_ = 0;
};

}
