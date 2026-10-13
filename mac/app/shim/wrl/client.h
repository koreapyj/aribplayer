#pragma once
// NOT WRL: the part of Microsoft::WRL::ComPtr that OpenFlowsMac.mm uses with the COM shim of shobjidl.h.
// Owns one reference; operator& releases and yields T**.
#include <utility>

namespace Microsoft::WRL {

template <class T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(const ComPtr& o) : p_(o.p_) { if (p_) p_->AddRef(); }
    ComPtr(ComPtr&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}
    ~ComPtr() { Reset(); }
    ComPtr& operator=(ComPtr o) noexcept { std::swap(p_, o.p_); return *this; }
    T* operator->() const { return p_; }
    T* Get() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    T** operator&() { Reset(); return &p_; }
    void Reset() { if (p_) { p_->Release(); p_ = nullptr; } }
private:
    T* p_ = nullptr;
};

}
