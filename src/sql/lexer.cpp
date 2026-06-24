#include "sql/lexer.h"

#include <cctype>
#include <stdexcept>

namespace heapdb {

Lexer::Lexer(std::string src) : src_(std::move(src)) {}

bool Lexer::more() const { return pos_ < src_.size(); }
char Lexer::peek() const { return pos_ < src_.size() ? src_[pos_] : '\0'; }
char Lexer::take() { return src_[pos_++]; }

std::vector<Token> Lexer::run() {
  std::vector<Token> out;
  while (more()) {
    char c = peek();
    if (std::isspace(static_cast<unsigned char>(c))) {
      take();
      continue;
    }
    // SQL line comment: "--" runs to end of line.
    if (c == '-' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '-') {
      while (more() && peek() != '\n') take();
      continue;
    }
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      std::string w;
      while (more() && (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_'))
        w.push_back(take());
      out.push_back({Lexeme::Word, w, 0});
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c)) ||
        (c == '-' && pos_ + 1 < src_.size() && std::isdigit((unsigned char)src_[pos_ + 1]))) {
      std::string n;
      n.push_back(take());  // sign or first digit
      while (more() && std::isdigit(static_cast<unsigned char>(peek()))) n.push_back(take());
      out.push_back({Lexeme::Number, n, std::stoll(n)});
      continue;
    }
    if (c == '\'') {
      take();  // opening quote
      std::string s;
      while (more() && peek() != '\'') s.push_back(take());
      if (!more()) throw std::runtime_error("unterminated string literal");
      take();  // closing quote
      out.push_back({Lexeme::Quoted, s, 0});
      continue;
    }
    // Multi-character symbols first, then single chars.
    if (c == '<' || c == '>' || c == '!' || c == '=') {
      std::string sym;
      sym.push_back(take());
      if (more() && peek() == '=') sym.push_back(take());
      out.push_back({Lexeme::Symbol, sym, 0});
      continue;
    }
    std::string sym(1, take());
    out.push_back({Lexeme::Symbol, sym, 0});
  }
  out.push_back({Lexeme::End, "", 0});
  return out;
}

}  // namespace heapdb
