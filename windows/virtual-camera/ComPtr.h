#pragma once

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    explicit ComPtr(T* value) : value_(value) { if (value_) value_->AddRef(); }
    ComPtr(const ComPtr& other) : value_(other.value_) { if (value_) value_->AddRef(); }
    ComPtr(ComPtr&& other) noexcept : value_(other.value_) { other.value_ = nullptr; }
    ~ComPtr() { Reset(); }
    ComPtr& operator=(const ComPtr& other) {
        if (this != &other) { Reset(); value_ = other.value_; if (value_) value_->AddRef(); }
        return *this;
    }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) { Reset(); value_ = other.value_; other.value_ = nullptr; }
        return *this;
    }
    void Reset(T* value = nullptr) { if (value_) value_->Release(); value_ = value; }
    T* Get() const { return value_; }
    T** Put() { Reset(); return &value_; }
    T* operator->() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }
    T* Detach() { T* value = value_; value_ = nullptr; return value; }
private:
    T* value_{};
};
