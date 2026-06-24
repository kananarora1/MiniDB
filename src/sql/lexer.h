#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace heapdb {

// Token categories. Keywords are not distinguished here - they come through as
// Word and the parser matches them case-insensitively, which keeps the lexer
// tiny and free of a keyword table.
enum class Lexeme { Word, Number, Quoted, Symbol, End };

struct Token {
  Lexeme kind;
  std::string text;   // identifier / symbol / string body
  int64_t number = 0; // valid when kind == Number
};

// A hand-written scanner. SQL's lexical grammar is small enough that splitting
// the input into tokens with a single pass over the characters is clearer than
// pulling in a generator.
class Lexer {
 public:
  explicit Lexer(std::string src);
  std::vector<Token> run();

 private:
  char peek() const;
  char take();
  bool more() const;

  std::string src_;
  std::size_t pos_ = 0;
};

}  // namespace heapdb
