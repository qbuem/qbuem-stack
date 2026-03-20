// Forces the ELF PT_TLS segment p_align to 64 on ARM64 Bionic.
//
// Clang on Android defaults to emulated TLS (__emutls) which does NOT create
// a PT_TLS program header.  Bionic's dynamic linker enforces ≥ 64-byte
// alignment on any PT_TLS segment that does exist, but can't see emutls vars.
// When another object in the binary (e.g. a system libc++ internal) creates a
// native PT_TLS segment with only 8-byte alignment, Bionic rejects the binary.
//
// Forcing global-dynamic TLS model on an alignas(64) variable creates a real
// .tbss section with Addralign=64 → lld sets PT_TLS p_align=64 → accepted.
alignas(64) thread_local char qbuem_tls_align_sentinel_
    __attribute__((tls_model("global-dynamic"))) = 0;
