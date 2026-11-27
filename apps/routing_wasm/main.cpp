// satsim-routing: the routing program compiled to WebAssembly for the dashboard
// (dashboard/src/engine/run.ts). The same satsim_routing_app library as the native program;
// only the streams differ, because a page has no files and no terminal:
//
//  - the arguments are the routing program's, with --out - (the routes CSV to "standard
//    output") and the inputs and --drops-out in Emscripten's in-memory file system;
//  - the routes CSV goes to /run/routes.csv through a buffer that counts its lines and hands
//    each count to Module.onRoutesLine (when the caller set one), so a page can show progress;
//  - the summary goes to /run/summary.txt and every other message to /run/messages.txt
//    (run_main writes both to its error stream when the CSV goes to its output stream: the
//    summary is everything up to and including its "  checks: " line).
//
// The exit status is run_main's (0, 1 if a check failed, 2 for bad arguments or inputs).

#include <emscripten/em_js.h>

#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <ostream>
#include <span>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "routing/run.hpp"

// A JavaScript function the C++ calls: the body is JavaScript, so clang-format keeps out.
// clang-format off
EM_JS(void, satsim_routes_line, (double lines), {
  if (typeof Module["onRoutesLine"] === "function") Module["onRoutesLine"](lines);
});
// clang-format on

namespace {

constexpr std::string_view kRunDir = "/run";
constexpr std::string_view kChecksLine = "\n  checks: ";

// Passes every character on to `target` and reports each completed line.
class LineCountingBuffer : public std::streambuf {
 public:
  explicit LineCountingBuffer(std::streambuf& target) : target_(target) {}

 protected:
  int_type overflow(int_type ch) override {
    if (traits_type::eq_int_type(ch, traits_type::eof())) {
      return traits_type::not_eof(ch);
    }
    const auto c = traits_type::to_char_type(ch);
    if (traits_type::eq_int_type(target_.sputc(c), traits_type::eof())) {
      return traits_type::eof();
    }
    if (c == '\n') {
      completed_line();
    }
    return ch;
  }

  std::streamsize xsputn(const char* s, std::streamsize n) override {
    const std::streamsize written = target_.sputn(s, n);
    for (const char c : std::string_view(s, static_cast<std::size_t>(written))) {
      if (c == '\n') {
        completed_line();
      }
    }
    return written;
  }

  int sync() override { return target_.pubsync(); }

 private:
  void completed_line() {
    ++lines_;
    satsim_routes_line(static_cast<double>(lines_));
  }

  std::streambuf& target_;
  std::size_t lines_ = 0;
};

bool write_text(const std::filesystem::path& path, std::string_view text) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
  return static_cast<bool>(file);
}

int run(std::span<const std::string_view> args) {
  const std::filesystem::path dir(kRunDir);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::ofstream routes(dir / "routes.csv", std::ios::binary | std::ios::trunc);
  if (!routes.is_open()) {
    write_text(dir / "messages.txt", "satsim-routing: cannot open /run/routes.csv\n");
    return 2;
  }
  LineCountingBuffer counting(*routes.rdbuf());
  std::ostream out(&counting);
  std::ostringstream err;
  const int status = satsim::routing_app::run_main(args, out, err);
  out.flush();
  routes.close();

  const std::string text = err.str();
  std::string_view summary;
  std::string_view messages = text;
  if (const std::size_t at = text.find(kChecksLine); at != std::string::npos) {
    const std::size_t end = text.find('\n', at + kChecksLine.size());
    const std::size_t cut = end == std::string::npos ? text.size() : end + 1;
    summary = std::string_view(text).substr(0, cut);
    messages = std::string_view(text).substr(cut);
  }
  if (!write_text(dir / "summary.txt", summary) || !write_text(dir / "messages.txt", messages)) {
    return 2;
  }
  return status;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const std::span<char*> raw(argv, static_cast<std::size_t>(argc));
    const std::span<char*> given = raw.subspan(raw.empty() ? 0 : 1);  // Without the name.
    const std::vector<std::string_view> args(given.begin(), given.end());
    return run(args);
  } catch (const std::exception& e) {
    write_text("/run/messages.txt", std::string("satsim-routing: ") + e.what() + '\n');
    return 2;
  }
}
