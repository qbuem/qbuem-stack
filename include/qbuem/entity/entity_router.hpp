#pragma once

/**
 * @file qbuem/entity/entity_router.hpp
 * @brief Reusable, extensible entity framework — type-tagged serialization +
 *        dispatch for game / web-server entities, built on qbuem-json.
 * @ingroup qbuem_entity
 *
 * An **entity** here is just any plain struct registered with
 * `QBUEM_JSON_FIELDS(T, ...)` (so it already gets JSON via `qbuem::read/write`
 * and binary CBOR via `qbuem::cbor::encode/decode`). There is **no base class
 * and no vtable** — a game's `Weapon`/`Monster` or a web `User`/`Order` stays a
 * cache-friendly POD-ish struct. qbuem-json owns serialization; this header adds
 * only the cross-project **reuse layer** that both games and web servers want:
 *
 *  - **Extensible registry:** each project registers its own entity types by
 *    name with `on<T>(name, handler)` — adding a new entity type never touches a
 *    central enum/variant.
 *  - **Self-describing envelope:** `encode<T>()` wraps an entity as
 *    `[u16 name_len][name][CBOR payload]` — a heterogeneous, transport-friendly
 *    byte string you can put on a socket, an `SHMChannel` (as bytes), a save
 *    file, or a DB blob.
 *  - **Typed dispatch:** `dispatch(bytes)` reads the tag and routes to the
 *    correct typed handler.
 *
 * For human-readable / single-type I/O use qbuem-json directly
 * (`qbuem::write(e)` / `qbuem::read<T>(json)`); this layer is for tagged,
 * polymorphic entity *streams*.
 *
 * **Opt-in:** the whole header is active only when qbuem-json is available
 * (`__has_include`), so it never forces a third-party include into a
 * zero-dependency core build.
 *
 * @code
 * struct Weapon  { std::string name; int damage; float range; };
 * struct Monster { std::string id; int hp; std::vector<std::string> drops; };
 * QBUEM_JSON_FIELDS(Weapon,  name, damage, range)
 * QBUEM_JSON_FIELDS(Monster, id, hp, drops)
 *
 * qbuem::entity::EntityRouter r;
 * r.on<Weapon>("weapon",  [](const Weapon& w)  { equip(w); });
 * r.on<Monster>("monster",[](const Monster& m) { spawn(m); });
 *
 * std::string wire = r.encode(Weapon{"Excalibur", 120, 2.5f}); // send over socket/SHM
 * r.dispatch(wire);                                            // → calls equip(w)
 * @endcode
 */

#if __has_include(<qbuem_json/qbuem_json.hpp>)

#include <qbuem_json/qbuem_json.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>

namespace qbuem::entity {

/**
 * @brief Opaque 64-bit entity identity. A convenience for entities that key on a
 *        numeric id; SaaS entities may prefer a `std::string id` field instead.
 */
enum class EntityId : std::uint64_t {};

/**
 * @brief Type-tagged entity serializer + dispatcher (the extensibility layer).
 *
 * Register each entity type once with `on<T>(name, handler)`. `encode<T>()`
 * produces a self-describing envelope; `dispatch()` routes by tag to the handler.
 * Not thread-safe to mutate (register) concurrently with `dispatch`; register all
 * types at startup, then dispatch from worker threads (read-only) freely.
 */
class EntityRouter {
public:
  /**
   * @brief Register an entity type + its handler. `T` must have
   *        `QBUEM_JSON_FIELDS(T, ...)`. Re-registering a name replaces it.
   */
  template <typename T>
  void on(std::string type_name, std::function<void(const T&)> handler) {
    names_.insert_or_assign(std::type_index(typeid(T)), type_name);
    decoders_.insert_or_assign(
        std::move(type_name),
        [h = std::move(handler)](std::string_view payload) {
          h(qbuem::cbor::decode<T>(payload));
        });
  }

  /**
   * @brief Encode an entity into a self-describing envelope
   *        (`[u16 name_len][name][CBOR payload]`). Returns empty if `T` was never
   *        registered.
   */
  template <typename T>
  [[nodiscard]] std::string encode(const T& e) const {
    const auto it = names_.find(std::type_index(typeid(T)));
    if (it == names_.end()) return {};
    const std::string& name = it->second;
    std::string out;
    const auto nlen = static_cast<std::uint16_t>(name.size());
    out.push_back(static_cast<char>((nlen >> 8) & 0xFF));
    out.push_back(static_cast<char>(nlen & 0xFF));
    out.append(name);
    qbuem::cbor::encode_to(out, e); // appends the CBOR payload in place
    return out;
  }

  /**
   * @brief Decode an envelope and route it to the registered handler.
   * @returns true if a handler ran; false if the envelope is malformed, the type
   *          is unknown, or the CBOR payload failed to decode.
   *
   * Never throws: this is the trust boundary for untrusted bytes (socket / SHM /
   * file), so it contains the parse exceptions qbuem-json may raise.
   */
  bool dispatch(std::string_view envelope) const noexcept {
    if (envelope.size() < 2) return false;
    const std::size_t nlen =
        (static_cast<unsigned char>(envelope[0]) << 8) |
         static_cast<unsigned char>(envelope[1]);
    if (envelope.size() < 2 + nlen) return false;
    const std::string_view name    = envelope.substr(2, nlen);
    const std::string_view payload = envelope.substr(2 + nlen);
    const auto it = decoders_.find(std::string(name));
    if (it == decoders_.end()) return false;
    try {
      it->second(payload);
    } catch (...) {
      return false; // malformed CBOR / type mismatch — contained at the boundary
    }
    return true;
  }

  /** @brief The registered name for `T`, or empty if unregistered. */
  template <typename T>
  [[nodiscard]] std::string_view type_name() const {
    const auto it = names_.find(std::type_index(typeid(T)));
    return it == names_.end() ? std::string_view{}
                              : std::string_view{it->second};
  }

  /** @brief Number of registered entity types. */
  [[nodiscard]] std::size_t registered_count() const noexcept {
    return decoders_.size();
  }

private:
  std::unordered_map<std::type_index, std::string>                     names_;
  std::unordered_map<std::string, std::function<void(std::string_view)>> decoders_;
};

} // namespace qbuem::entity

#endif // __has_include(<qbuem_json/qbuem_json.hpp>)
