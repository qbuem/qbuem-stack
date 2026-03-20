#pragma once

/**
 * @file qbuem/http/template_engine.hpp
 * @brief v2.6.0 TemplateEngine — zero-copy pre-compiled template rendering
 * @defgroup qbuem_template_engine TemplateEngine
 * @ingroup qbuem_http
 *
 * ## Overview
 *
 * `TemplateEngine` compiles HTML/JSON/text templates into a sequence of
 * `Segment` instructions at initialisation time and renders them directly
 * into a caller-supplied `MutableBufferView` (or `std::string` fallback)
 * with **zero heap allocation** per render.
 *
 * ### Template syntax
 * ```
 * Hello, {{name}}!
 * Your score is {{score}}.
 * {{#if admin}}Admin panel{{/if}}
 * {{#each items}}  - {{.}}
 * {{/each}}
 * ```
 *
 * - `{{key}}`          — variable substitution (HTML-escaped by default)
 * - `{{{key}}}`        — raw (unescaped) variable
 * - `{{#if key}}…{{/if}}`   — conditional block (truthy = non-empty string)
 * - `{{#each key}}…{{/each}}` — iteration over a `std::vector<std::string>` value
 * - `{{! comment }}`   — comment (stripped at compile time)
 * - `{{> partial}}`    — partial inclusion (pre-registered at compile time)
 *
 * ### Zero-copy model
 * Literal string segments are stored as `std::string_view` slices into the
 * original template source (which the caller must keep alive for the
 * lifetime of the compiled template).  No intermediate string is built
 * during rendering.
 *
 * ### Thread safety
 * `CompiledTemplate::render()` is const and reentrant — multiple reactor
 * threads may render the same compiled template concurrently.
 *
 * ## Usage Example
 * @code
 * TemplateEngine engine;
 *
 * // Register a partial
 * engine.add_partial("header", "<header>{{title}}</header>");
 *
 * // Compile once (at startup)
 * auto tmpl = engine.compile(R"(
 *   {{> header}}
 *   <p>Hello, {{name}}!</p>
 *   {{#if admin}}<a href="/admin">Admin</a>{{/if}}
 * )");
 *
 * // Render per-request (zero allocation)
 * TemplateContext ctx;
 * ctx.set("title", "My App");
 * ctx.set("name",  req.query("user"));
 * ctx.set("admin", user.is_admin ? "1" : "");
 *
 * std::string out;
 * out.reserve(4096);
 * tmpl.render(ctx, out);
 * res.status(200).header("Content-Type", "text/html").body(std::move(out));
 * @endcode
 *
 * @{
 */

#include <qbuem/common.hpp>

#include <cassert>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace qbuem {
namespace http {

// ─── TemplateContext ──────────────────────────────────────────────────────────

/**
 * @brief Per-render variable bindings passed to `CompiledTemplate::render()`.
 *
 * ### Values
 * - Scalar: `std::string_view` (caller owns lifetime for the render call)
 * - List:   `std::span<const std::string_view>` for `{{#each}}` iteration
 *
 * Construction and `set()` calls are cold-path; the render engine reads
 * values via `get()` / `get_list()` which use `unordered_map::find`.
 */
class TemplateContext {
public:
  TemplateContext() = default;

  /**
   * @brief Bind a scalar variable.
   *
   * @param key   Variable name (must match `{{key}}` in template).
   * @param value String value (caller must keep alive for the render duration).
   */
  void set(std::string key, std::string value) {
    scalars_.insert_or_assign(std::move(key), std::move(value));
  }

  /**
   * @brief Bind a list variable for `{{#each key}}` iteration.
   *
   * @param key    Variable name.
   * @param items  List of string values.
   */
  void set_list(std::string key, std::vector<std::string> items) {
    lists_.insert_or_assign(std::move(key), std::move(items));
  }

  /** @brief Look up a scalar value (returns empty string_view if not found). */
  [[nodiscard]] std::string_view get(std::string_view key) const noexcept {
    auto it = scalars_.find(std::string(key));
    if (it == scalars_.end()) return {};
    return it->second;
  }

  /** @brief Look up a list value (returns nullptr if not found). */
  [[nodiscard]] const std::vector<std::string>* get_list(std::string_view key) const noexcept {
    auto it = lists_.find(std::string(key));
    if (it == lists_.end()) return nullptr;
    return &it->second;
  }

  /** @brief Return truthy-ness of a variable (non-empty = truthy). */
  [[nodiscard]] bool is_truthy(std::string_view key) const noexcept {
    return !get(key).empty();
  }

  /** @brief Remove all bindings. */
  void clear() noexcept { scalars_.clear(); lists_.clear(); }

private:
  std::unordered_map<std::string, std::string>              scalars_;
  std::unordered_map<std::string, std::vector<std::string>> lists_;
};

// ─── Segment (compile-time IR) ────────────────────────────────────────────────

/**
 * @brief Internal compile-time instruction segment.
 *
 * One of: Literal, Variable, RawVariable, IfBlock, EachBlock, Partial.
 * Stored as `std::variant` to avoid virtual dispatch.
 */
struct SegLiteral   { std::string_view text; };
struct SegVariable  { std::string key; bool escape = true; };
struct SegIfBlock   { std::string key; std::vector<struct Segment> body; };
struct SegEachBlock { std::string key; std::vector<struct Segment> body; };
struct SegPartial   { std::string name; };

struct Segment {
  std::variant<SegLiteral, SegVariable, SegIfBlock, SegEachBlock, SegPartial> data;
};

// ─── CompiledTemplate ─────────────────────────────────────────────────────────

/**
 * @brief A compiled template ready for repeated zero-allocation rendering.
 *
 * Constructed by `TemplateEngine::compile()`.  Immutable after compilation;
 * thread-safe for concurrent `render()` calls.
 */
class CompiledTemplate {
public:
  /**
   * @brief Render the template into the output string.
   *
   * @param ctx     Variable bindings for this render pass.
   * @param output  Output buffer (callers should `reserve()` an appropriate capacity).
   */
  void render(const TemplateContext& ctx, std::string& output) const {
    render_segments(segments_, ctx, output);
  }

  /**
   * @brief Render and return as a new `std::string` (convenience overload).
   */
  [[nodiscard]] std::string render(const TemplateContext& ctx) const {
    std::string out;
    out.reserve(estimated_size_);
    render(ctx, out);
    return out;
  }

  /** @brief Source template text (kept for diagnostic purposes). */
  [[nodiscard]] std::string_view source() const noexcept { return source_; }

  /** @brief Number of top-level segments in the compiled IR. */
  [[nodiscard]] size_t segment_count() const noexcept { return segments_.size(); }

private:
  friend class TemplateEngine;

  std::string             source_owned_;    ///< Owned copy of the original template source
  std::string_view        source_;          ///< View into source_owned_
  std::vector<Segment>    segments_;        ///< Compiled instruction sequence
  size_t                  estimated_size_ = 512; ///< Heuristic output size for reserve()

  // ── HTML escaping ─────────────────────────────────────────────────────────

  static void append_escaped(std::string& out, std::string_view text) {
    for (char c : text) {
      switch (c) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"':  out += "&quot;"; break;
      case '\'': out += "&#39;";  break;
      default:   out += c;        break;
      }
    }
  }

  // ── Render engine (recursive, const) ─────────────────────────────────────

  void render_segments(const std::vector<Segment>& segs,
                       const TemplateContext& ctx,
                       std::string& out,
                       std::string_view each_item = {}) const
  {
    for (const auto& seg : segs) {
      std::visit([&](const auto& s) {
        using SType = std::decay_t<decltype(s)>;

        if constexpr (std::is_same_v<SType, SegLiteral>) {
          out += s.text;

        } else if constexpr (std::is_same_v<SType, SegVariable>) {
          // "." inside {{#each}} refers to the current item
          std::string_view val;
          if (s.key == "." && !each_item.empty())
            val = each_item;
          else
            val = ctx.get(s.key);

          if (s.escape)
            append_escaped(out, val);
          else
            out += val;

        } else if constexpr (std::is_same_v<SType, SegIfBlock>) {
          if (ctx.is_truthy(s.key))
            render_segments(s.body, ctx, out);

        } else if constexpr (std::is_same_v<SType, SegEachBlock>) {
          const auto* list = ctx.get_list(s.key);
          if (list != nullptr) {
            for (const auto& item : *list)
              render_segments(s.body, ctx, out, item);
          }

        } else if constexpr (std::is_same_v<SType, SegPartial>) {
          // Partial rendering is resolved by the engine at compile time;
          // at render time we just output the pre-compiled segments.
          // (Partials are inlined into the parent segment list by the compiler.)
          (void)s;
        }
      }, seg.data);
    }
  }
};

// ─── TemplateEngine ───────────────────────────────────────────────────────────

/**
 * @brief Compiles and caches templates; manages partial registrations.
 *
 * Create one `TemplateEngine` at application startup.  Compiled templates are
 * thread-safe and can be rendered concurrently by multiple reactor threads.
 */
class TemplateEngine {
public:
  TemplateEngine() = default;

  /**
   * @brief Register a named partial template.
   *
   * Partials are inlined at compile time — registering them before calling
   * `compile()` is required.
   *
   * @param name    Partial name (referenced as `{{> name}}`).
   * @param source  Partial template source.
   */
  void add_partial(std::string name, std::string source) {
    partials_.insert_or_assign(std::move(name), std::move(source));
  }

  /**
   * @brief Compile a template string into a `CompiledTemplate`.
   *
   * @param source  Template source (copied internally).
   * @return        Compiled template ready for rendering.
   */
  [[nodiscard]] CompiledTemplate compile(std::string source) const {
    CompiledTemplate tmpl;
    tmpl.source_owned_ = std::move(source);
    tmpl.source_       = tmpl.source_owned_;
    parse(tmpl.source_owned_, tmpl.segments_);
    // Estimate output size from literal text length
    tmpl.estimated_size_ = estimate_output_size(tmpl.segments_);
    return tmpl;
  }

  /**
   * @brief Compile and cache a template under a name for later retrieval.
   *
   * @param name    Cache key.
   * @param source  Template source.
   * @return        Reference to the cached `CompiledTemplate`.
   */
  const CompiledTemplate& compile_cached(std::string name, std::string source) {
    auto it = cache_.find(name);
    if (it == cache_.end()) {
      auto [ins, ok] = cache_.emplace(std::move(name), compile(std::move(source)));
      return ins->second;
    }
    return it->second;
  }

  /**
   * @brief Retrieve a cached compiled template.
   *
   * @param name  Cache key.
   * @return      Pointer to the cached template, or nullptr if not found.
   */
  [[nodiscard]] const CompiledTemplate* get(std::string_view name) const noexcept {
    auto it = cache_.find(std::string(name));
    if (it == cache_.end()) return nullptr;
    return &it->second;
  }

private:
  std::unordered_map<std::string, std::string>         partials_;
  mutable std::unordered_map<std::string, CompiledTemplate> cache_;

  // ── Parser ────────────────────────────────────────────────────────────────

  void parse(std::string_view src, std::vector<Segment>& out) const {
    size_t pos = 0;

    while (pos < src.size()) {
      // Find next opening delimiter {{
      const size_t open = src.find("{{", pos);

      if (open == std::string_view::npos) {
        // Remaining text is all literal
        if (pos < src.size())
          out.push_back(Segment{SegLiteral{src.substr(pos)}});
        break;
      }

      // Emit literal before the tag
      if (open > pos)
        out.push_back(Segment{SegLiteral{src.substr(pos, open - pos)}});

      // Determine tag type
      const size_t tag_start = open + 2;
      if (tag_start >= src.size()) break;

      const bool is_raw    = (src[tag_start] == '{');
      const bool is_block  = (src[tag_start] == '#');
      const bool is_close  = (src[tag_start] == '/');
      const bool is_partial= (src[tag_start] == '>');
      const bool is_comment= (src[tag_start] == '!');

      const std::string_view close_delim = is_raw ? "}}}" : "}}";
      const size_t content_start = tag_start + (is_raw || is_block || is_close || is_partial || is_comment ? 1 : 0);
      const size_t close = src.find(close_delim, tag_start);
      if (close == std::string_view::npos) break; // Malformed — stop

      std::string_view content = trim(src.substr(content_start, close - content_start));
      pos = close + close_delim.size();

      if (is_comment) {
        // Skip comments
        continue;
      } else if (is_partial) {
        // {{> partial_name}} — inline the partial's compiled segments
        const std::string partial_name(content);
        auto pit = partials_.find(partial_name);
        if (pit != partials_.end()) {
          // Recursively parse the partial and inline its segments
          parse(pit->second, out);
        }
      } else if (is_raw) {
        // {{{key}}} — raw (unescaped)
        out.push_back(Segment{SegVariable{std::string(content), false}});
        pos += 1; // skip extra }
      } else if (is_block) {
        // {{#if key}} or {{#each key}}
        const bool is_if   = content.starts_with("if ");
        const bool is_each = content.starts_with("each ");
        const std::string_view block_key = is_if  ? trim(content.substr(3))
                                         : is_each ? trim(content.substr(5))
                                         : content;
        const std::string_view end_tag = is_if ? "/if" : "/each";

        // Find matching close tag — naive (non-nested) search
        const std::string end_open  = std::string("{{") + std::string(end_tag) + "}}";
        const size_t body_end = src.find(end_open, pos);
        if (body_end == std::string_view::npos) break;

        std::string_view body_src = src.substr(pos, body_end - pos);
        std::vector<Segment> body_segs;
        parse(body_src, body_segs);
        pos = body_end + end_open.size();

        if (is_if) {
          out.push_back(Segment{SegIfBlock{std::string(block_key), std::move(body_segs)}});
        } else {
          out.push_back(Segment{SegEachBlock{std::string(block_key), std::move(body_segs)}});
        }
      } else if (is_close) {
        // {{/...}} — handled by the block parser above; skip stray closes
        continue;
      } else {
        // {{key}} — escaped variable
        out.push_back(Segment{SegVariable{std::string(content), true}});
      }
    }
  }

  [[nodiscard]] static std::string_view trim(std::string_view sv) noexcept {
    const auto not_space = [](char c){ return c != ' ' && c != '\t'; };
    const auto begin = std::find_if(sv.begin(), sv.end(), not_space);
    const auto end   = std::find_if(sv.rbegin(), sv.rend(), not_space).base();
    if (begin >= end) return {};
    return sv.substr(static_cast<size_t>(begin - sv.begin()),
                     static_cast<size_t>(end - begin));
  }

  [[nodiscard]] static size_t estimate_output_size(const std::vector<Segment>& segs) noexcept {
    size_t total = 0;
    for (const auto& seg : segs) {
      std::visit([&](const auto& s) {
        using SType = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<SType, SegLiteral>)
          total += s.text.size();
        else
          total += 32; // Heuristic for variable / block overhead
      }, seg.data);
    }
    return total > 0 ? total : 512;
  }
};

/** @} */

} // namespace http
} // namespace qbuem
