#pragma once
#include "Arduino.h"
// In-memory File with the same read/peek/print/write/seek/position/size semantics the parser uses.
class File {
public:
    File() {}
    explicit File(std::string *buf) : buf_(buf) {}
    explicit operator bool() const { return buf_ != nullptr; }
    int read() { if (!buf_ || pos_ >= buf_->size()) return -1; return (unsigned char)(*buf_)[pos_++]; }
    int peek() { if (!buf_ || pos_ >= buf_->size()) return -1; return (unsigned char)(*buf_)[pos_]; }
    size_t print(const char *s) { size_t n = strlen(s); if (failAfter_ < n) n = failAfter_; buf_->append(s, n); failAfter_ -= n; return n; }
    size_t write(uint8_t c) { if (failAfter_ == 0) return 0; buf_->push_back((char)c); failAfter_--; return 1; }
    size_t position() const { return pos_; }
    bool seek(size_t p) { if (!buf_ || p > buf_->size()) return false; pos_ = p; return true; }
    size_t size() const { return buf_ ? buf_->size() : 0; }
    void close() {}
    void failAfter(size_t n) { failAfter_ = n; } // simulate a full card after n bytes
private:
    std::string *buf_ = nullptr; size_t pos_ = 0; size_t failAfter_ = (size_t)-1;
};
