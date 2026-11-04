#pragma once
#include <atomic>
#include <array>
#include <cstdlib>
#include <new>
std::atomic<bool> countAllocations{false};
std::atomic<std::size_t> allocationCalls{0},allocationBytes{0};
void* operator new(std::size_t n) {
    if(countAllocations.load(std::memory_order_relaxed)){allocationCalls.fetch_add(1,std::memory_order_relaxed);allocationBytes.fetch_add(n,std::memory_order_relaxed);}
    if(void* p=std::malloc(n?n:1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
