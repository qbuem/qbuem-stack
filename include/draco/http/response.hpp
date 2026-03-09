#include <beast_json/beast_json.hpp>
#include <draco/common.hpp>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace draco {

/**
 * @brief HTTP Response builder.
 */
class Response {
public:
  Response() = default;

  Response &status(int code);
  Response &header(std::string_view key, std::string_view value);
  Response &body(std::string_view b);
  Response &json(const beast::Value &v);

  std::string serialize() const;

private:
  std::string_view status_to_string(int code) const;

  int status_code_ = 200;
  std::unordered_map<std::string, std::string> headers_;
  std::string body_;
};

} // namespace draco
