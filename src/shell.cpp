#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "engine.h"
#include "session.h"

using namespace heapdb;

namespace {

// Split a blob of SQL into individual statements on ';' (no ';' inside our
// literals' grammar makes this safe enough for the MiniDB subset).
std::vector<std::string> splitStatements(const std::string& text) {
  std::vector<std::string> out;
  std::string cur;
  bool inStr = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    // Skip "--" line comments entirely (so a ';' inside a comment is ignored).
    if (!inStr && c == '-' && i + 1 < text.size() && text[i + 1] == '-') {
      while (i < text.size() && text[i] != '\n') ++i;
      continue;
    }
    if (c == '\'') inStr = !inStr;
    if (c == ';' && !inStr) {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  // trailing statement without ';'
  for (char c : cur)
    if (!std::isspace((unsigned char)c)) {
      out.push_back(cur);
      break;
    }
  return out;
}

bool blank(const std::string& s) {
  for (char c : s)
    if (!std::isspace((unsigned char)c)) return false;
  return true;
}

void runBatch(Session& sess, const std::string& text) {
  for (const std::string& stmt : splitStatements(text)) {
    if (blank(stmt)) continue;
    try {
      std::cout << sess.exec(stmt) << "\n";
    } catch (const std::exception& e) {
      std::cout << "ERROR: " << e.what() << "\n";
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: minidb <data-dir> [script.sql]\n";
    return 2;
  }
  std::string dir = argv[1];
  std::filesystem::create_directories(dir);

  Engine engine(dir);
  Session sess(engine);

  // Script mode: run a file of statements and exit.
  if (argc >= 3) {
    std::ifstream in(argv[2]);
    std::stringstream ss;
    ss << in.rdbuf();
    runBatch(sess, ss.str());
    return 0;
  }

  // Interactive REPL.
  std::cout << "MiniDB (HeapHackers) - Track B / MVCC. Type SQL ending in ';'.\n"
               "Meta: .tables  .crash  .exit\n";
  std::string buffer, line;
  while (true) {
    std::cout << (buffer.empty() ? (sess.inExplicitTxn() ? "minidb*> " : "minidb> ")
                                 : "    ...> ");
    if (!std::getline(std::cin, line)) break;

    std::string trimmed = line;
    if (buffer.empty() && !trimmed.empty() && trimmed[0] == '.') {
      if (trimmed.rfind(".exit", 0) == 0 || trimmed.rfind(".quit", 0) == 0) break;
      if (trimmed.rfind(".tables", 0) == 0) {
        for (const std::string& n : engine.catalog().names()) std::cout << n << "\n";
        continue;
      }
      if (trimmed.rfind(".crash", 0) == 0) {
        // Simulate a hard crash: leave the buffer pool unflushed so recovery
        // has work to do. Committed data must survive via the WAL.
        std::cout << "** simulating crash (no flush) **\n";
        std::_Exit(137);
      }
      std::cout << "unknown meta-command\n";
      continue;
    }

    buffer += line + "\n";
    if (buffer.find(';') == std::string::npos) continue;
    runBatch(sess, buffer);
    buffer.clear();
  }
  return 0;
}
