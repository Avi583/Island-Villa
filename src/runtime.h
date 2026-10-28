// runtime.h — freestanding runtime: memset/memcpy/memmove and the linear-memory bump allocator.
// Include from exactly one translation unit (render.cpp); it defines the libc symbols.
#pragma once
#include "scene.h"

using namespace rt;

#ifdef __wasm__
#define RT_EXPORT(name) extern "C" __attribute__((export_name(#name)))
extern "C" {
// The compiler may emit calls to these for struct copies; we have no libc.
void* memset(void* d, int c, unsigned long n) {
  volatile unsigned char* p = (volatile unsigned char*)d;
  while (n--) *p++ = (unsigned char)c;
  return d;
}
void* memcpy(void* d, const void* s, unsigned long n) {
  volatile unsigned char* p = (volatile unsigned char*)d;
  const unsigned char* q = (const unsigned char*)s;
  while (n--) *p++ = *q++;
  return d;
}
void* memmove(void* d, const void* s, unsigned long n) {
  volatile unsigned char* p = (volatile unsigned char*)d;
  const unsigned char* q = (const unsigned char*)s;
  if (p < q) { while (n--) *p++ = *q++; }
  else { p += n; q += n; while (n--) *--p = *--q; }
  return d;
}
extern unsigned char __heap_base;
}
// Bump allocator straight on top of linear memory: grow pages as needed.
static unsigned char* heapAlloc(u32 bytes) {
  u32 base = ((u32)(unsigned long)&__heap_base + 15u) & ~15u;
  u32 need = base + bytes;
  u32 have = (u32)__builtin_wasm_memory_size(0) * 65536u;
  if (need > have) {
    u32 pages = (need - have + 65535u) / 65536u;
    if (__builtin_wasm_memory_grow(0, pages) == (unsigned long)-1) return nullptr;
  }
  return (unsigned char*)(unsigned long)base;
}
#else
#define RT_EXPORT(name) extern "C"
extern "C" void* malloc(unsigned long);
static unsigned char* heapAlloc(u32 bytes) { return (unsigned char*)malloc(bytes); }
#endif
