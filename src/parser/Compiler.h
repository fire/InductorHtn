#pragma once

#include "core/core_bind.h"
#include "core/object/reference.h"

#include <String>
#include <fstream>
#include <ios>
#include <iostream>
#include <istream>
#include <list>
#include <memory>


// TODO FIRE 2021-03-14 Investigate library https://github.com/yhirose/cpp-peglib

class LexerReader;
class Symbol;

// This is a PEG (Parsing Expression Grammar) parser
// The Lexer's job is to allow the parser to grab a character at a time
// from the stream that is opened via Open().  The parser then can combine
// them into larger symbols based on the parser's rules.
//
// The parser actually uses LexerReader (which wraps Lexer) because LexerReader
// automatically handles putting tokens back into the Lexer if they aren't
// consumed. See comments there for more details.
//
// Lexer also allows the parser to record rules that fail, and keeps the one
// that went deepest into the tree. That's because the deepest failure is,
// surprisingly often, the actual error in the document being parsed. Thus,
// that's the error returned.
class Lexer : public Reference {
  GDCLASS(Lexer, Reference);

public:
  friend LexerReader;

  Lexer() {}

  long consumedCharacters() { return m_consumedCharacters; }
  int DeepestFailure() { return (int)m_deepestFailure; }
  bool Eof();
  String ErrorMessage() { return m_errorMessage; }
  static void GetLineAndColumn(int charPosition,
                               std::shared_ptr<std::istream> stream,
                               int &lineCount, int &columnCount,
                               String *linestring = nullptr);
  void Open(std::shared_ptr<std::istream> stream);
  Ref<Symbol> Peek();
  void ReportFailure(const String &errorMessage);
  int TransactionDepth();

  std::shared_ptr<std::istream> stream() { return m_stream; }

private:
  // Used by LexerReader to create transactions when symbols are consumed
  // Aborting them puts the symbols back into the Lexer so the next rule can try
  // to consume them.
  void AbortTransaction(long position);
  long BeginTransaction();
  void CommitTransaction(long position);
  long Position() { return (long)m_stream->tellg(); }
  void Seek(long position);
  Ref<Symbol> Read();

  long m_consumedCharacters;
  long m_deepestFailure;
  String m_errorMessage;
  std::shared_ptr<std::istream> m_stream;
  int m_transactionDepth;
};

class Symbol;

// LexerReader is the primary interface between the parser and the Lexer
// Because the parser is exploring a tree of rules until it gets to an endpoint,
// it will often need to back up and try different branches. LexerReader
// represents a single transaction that allows the characters that a rule uses
// to get committed and consumed from the parser (if it succeeds) or aborted and
// put back into the Lexer for another rule to try (if it fails).
//
// It has correctness code (i.e. FailFast) that ensures that it doesn't get
// committed if there are children transactions still pending
class LexerReader {
public:
  LexerReader(Ref<Lexer> lexer);
  ~LexerReader();

  void Abort();
  void Begin();
  void Commit();
  Ref<Symbol> Peek();
  unsigned short Peek(Ref<Symbol> &symbol);
  Ref<Symbol> Read();
  unsigned short Read(Ref<Symbol> &symbol);

private:
  // This class should always be a local stack based class, (i.e. never created
  // with new()) because otherwise it could mess up the ordering of tokens when
  // it gets destructed
  void *operator new(size_t);
  void *operator new[](size_t);

  long m_originalPosition;
  int m_transactionDepth;
  Ref<Lexer> m_lexer;
};

LexerReader::LexerReader(Ref<Lexer> lexer)
    : m_originalPosition(-2), m_transactionDepth(-1) {
  m_lexer = lexer;
};

LexerReader::~LexerReader() {
  if (m_originalPosition != -2) {
    Abort();
  }
};

// Outer transactions can't commit or abort until nested ones complete because
// otherwise we will move the file pointer out from under it
// Really it is just a debugging guarantee since the calling code should never
// do this
void LexerReader::Abort() {
  FailFastAssert(m_originalPosition != -2);
  FailFastAssert(m_transactionDepth == m_lexer->TransactionDepth());
  m_lexer->AbortTransaction(m_originalPosition);
  m_originalPosition = -2;
}

void LexerReader::Begin() {
  FailFastAssert(m_originalPosition == -2);
  m_originalPosition = m_lexer->BeginTransaction();
  m_transactionDepth = m_lexer->TransactionDepth();
}

void LexerReader::Commit() {
  FailFastAssert(m_originalPosition != -2);
  FailFastAssert(m_transactionDepth == m_lexer->TransactionDepth());
  m_lexer->CommitTransaction(m_originalPosition);
  m_originalPosition = -2;
};

Ref<Symbol> LexerReader::Peek() { return m_lexer->Peek(); }

unsigned short LexerReader::Peek(Ref<Symbol> &symbol) {
  symbol = m_lexer->Peek();
  return symbol->symbolID();
}

Ref<Symbol> LexerReader::Read() {
  FailFastAssert(m_originalPosition != -2);
  return m_lexer->Read();
};

unsigned short LexerReader::Read(Ref<Symbol> &symbol) {
  FailFastAssert(m_originalPosition != -2);
  symbol = m_lexer->Read();
  return symbol->symbolID();
};

void Lexer::AbortTransaction(long position) {
  m_transactionDepth--;
  ERR_FAIL_COND(m_transactionDepth < 0);
  Seek(position);
}

long Lexer::BeginTransaction() {
  m_transactionDepth++;
  long position = Position();
  return position;
}

void Lexer::CommitTransaction(long position) {
  // Can't commit something that we haven't read yet
  ERR_FAIL_COND(position > m_consumedCharacters);

  // The Reader must detect if we are aborting out of order, but we can do a
  // simple check here
  m_transactionDepth--;
  ERR_FAIL_COND(m_transactionDepth < 0);
}

bool Lexer::Eof() { return Peek()->symbolID() == SymbolID::eof; }

// Used to report where an error occurred
void Lexer::GetLineAndColumn(int charPosition, shared_ptr<istream> stream,
                             int &lineCount, int &columnCount,
                             String *lineString) {
  if (!stream->good()) {
    stream->clear();
  }
  stream->seekg(0);

  char character;
  lineCount = 1;
  columnCount = 0;
  int lastLineStart = 0;
  int charCount = 0;
  bool foundCR = false;
  String result;

  String lastLine;
  String nextLine;
  while (charPosition > charCount && stream->good()) {
    character = stream->get();
    result.push_back(character);
    charCount++;

    if (character == '\r') {
      foundCR = true;
    } else if (character == '\n' && foundCR) {
      lastLine = result;

      lineCount++;
      foundCR = false;
      lastLineStart = charCount;
      if (charPosition != charCount) {
        // Don't clear if the last line is the one we want
        result.clear();
      }
    } else if (foundCR) {
      foundCR = false;
    }
  }

  columnCount = charPosition - lastLineStart + 1;
  if (lineString != nullptr) {
    *lineString = "Prev: " + lastLine + "\r\n Line: " + result;
  }
}

void Lexer::Open(shared_ptr<istream> stream) {
  m_consumedCharacters = 0;
  m_deepestFailure = -1;
  m_errorMessage = "";
  m_stream = stream;
  m_transactionDepth = 0;
}

Ref<Symbol> Lexer::Peek() {
  char character = m_stream->peek();
  if (!(character == EOF)) {
    TraceString3("{0}Lexer::Peek: '{1}', Consumed: {2}",
                 SystemTraceType::Parsing, TraceDetail::Diagnostic,
                 String((size_t)(TransactionDepth() * 3), ' '), character,
                 m_consumedCharacters);
    return shared_ptr<LexerSymbol>(new LexerSymbol(character));
  } else {
    TraceString2("{0}Lexer::Peek: '<EOF>', Consumed: {1}",
                 SystemTraceType::Parsing, TraceDetail::Diagnostic,
                 String((size_t)(TransactionDepth() * 3), ' '),
                 m_consumedCharacters);
    return EofSymbol::defaultValue;
  }
}

Ref<Symbol> Lexer::Read() {
  char character;
  if (m_stream->read(&character, 1)) {
    m_consumedCharacters++;
    TraceString3("{0}Lexer::Read: '{1}', Consumed: {2}",
                 SystemTraceType::Parsing, TraceDetail::Diagnostic,
                 String((size_t)(TransactionDepth() * 3), ' '), character,
                 m_consumedCharacters);
    return shared_ptr<LexerSymbol>(new LexerSymbol(character));
  } else {
    TraceString2("{0}Lexer::Read: '<EOF>', Consumed: {1}",
                 SystemTraceType::Parsing, TraceDetail::Diagnostic,
                 String((size_t)(TransactionDepth() * 3), ' '),
                 m_consumedCharacters);
    return EofSymbol::defaultValue;
  }
}

void Lexer::ReportFailure(const String &errorMessage) {
  // Assume the last one to hit this length is going to be the best message
  // because often a token will fail first (like whitespace) and then the good
  // error message token will fail
  if (m_consumedCharacters >= m_deepestFailure) {
    TraceString2("{0}Lexer::ReportFailure New deepest failure at char {1}",
                 SystemTraceType::Parsing, TraceDetail::Diagnostic,
                 String((size_t)(TransactionDepth() * 3), ' '),
                 m_consumedCharacters);
    m_deepestFailure = m_consumedCharacters;
    m_errorMessage = errorMessage;
  }
}

void Lexer::Seek(long position) {
  if (!m_stream->good()) {
    m_stream->clear();
  }

  if (position > -1) {
    m_consumedCharacters = position;
  }

  m_stream->seekg(position);
  FailFastAssert(Position() == position);
}

int Lexer::TransactionDepth() { return m_transactionDepth; }

Ref<Symbol> EofSymbol::defaultValue = Ref<Symbol>(new EofSymbol());

char AmpersandString[] = "&";
char AsterixString[] = "*";
char AtString[] = "@";
char Chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
char CharsAndNumbers[] =
    "1234567890ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
char ColonString[] = ":";
char CommaString[] = ",";
char DashString[] = "-";
char DefaultErrorMessage[] = "";
char DoubleQuoteString[] = "\"";
char EqualString[] = "=";
char ExclamationPointString[] = "!";
char ForwardSlashString[] = "/";
char GreaterThanString[] = ">";
char HexNumbers[] = "1234567890ABCDEFabcdef";
char HyphenString[] = "-";
char LeftBraceString[] = "{";
char LeftBracketString[] = "[";
char LeftParenthesisString[] = "(";
char LessThanString[] = "<";
char MathString[] = "+-<>=/*\\";
char NullString[] = "";
char Numbers[] = "1234567890";
char PercentString[] = "%";
char PeriodString[] = ".";
char PlusString[] = "+";
char PipeString[] = "|";
char PoundString[] = "#";
char QuestionMarkString[] = "?";
char RightBracketString[] = "]";
char RightBraceString[] = "}";
char RightParenthesisString[] = ")";
char SemicolonString[] = ";";
char SingleArrowString[] = "->";
char SingleQuoteString[] = "'";
char Underscore[] = "_";
char WhitespaceChars[] = "\r\n\t ";

bool SymbolID::IsCharacterSymbol(Ref<Symbol> symbol) {
  return symbol->symbolID() <= 255;
}

bool SymbolID::AllCharacterSymbols(vector<Ref<Symbol>> symbolVector) {
  bool allSymbols = true;
  for_each(symbolVector.begin(), symbolVector.end(), [&](Ref<Symbol> child) {
    if (!IsCharacterSymbol(child)) {
      allSymbols = false;
      return;
    }
  });

  return allSymbols;
}

#include "core/object/reference.h"
#include "core_bind.h" #pragma once
#include <Vector>
#include <algorithm>
#include <cstring>
#include <limits.h>
#include <list>

//
/*
 This is a PEG (Parsing Expression Grammar) parser.

 Summary: The goal is to parse a string into a tree of Symbol objects using a
tree of rules. The tree of rules is written by the developer to parse a
particular format of document. The parser visits each node of the tree and
allows it to consume characters from the string as it tries to match the rule's
pattern. If a rule doesn't match, the parser backtracks and tries other branches
of the tree until it gets to the end of document with a successful rule (or
fails).

 A rule is any object derived from Symbol that has the following method on it:
        static Ref<Symbol> TryParse(Ref<Lexer> lexer, const string
&errorMessage) that returns a Symbol if success or null if failure. They read
characters from the Lexer in a (potentially nested) transaction. If they
succeed, they "commit" the transaction which means they "consume" the
characters, removing them from the string so that other rules can't see them If
they fail, the transaction rolls back which allows something else to see them
    They can call nested rules which contain their own transactions.
    Only when the outermost transaction is committed is the string consumed
        Rules form a tree, with a single rule at the root that is followed to
interpret a document.

The rules are all written using C++ Templates so that they end up forming a
"Domain Specific Language" which makes it easier to read and write the rules. It
obviously makes it harder to debug compile errors though.

Each of the rule templates have some standard arguments that can be tweaked when
they are used:
    - most allow you to specify the ID of the symbol returned on success
    - most allow you to control "flattening" which controls what the tree looks
like:
        - Flatten means take this symbol out of the tree and reparent its
children to its parent Useful for things you really don't care about when you
are interpreting the tree later like the < and > in an XML document. Useful for
processing, but only care about what is in them.
        - Delete means remove this symbol and all children completely
                        Whitespaces is a good example of this.
        - most have an error string that is used if this rule happens to be the
deepest rule that fails.  It is the error that will be returned to the user as
the "parser error"

 */

class EmptyClass;
class EofSymbol;
class Lexer;
class LexerReader;
class NonTerminalSymbol;
class Symbol;

extern char AmpersandString[];
extern char AsterixString[];
extern char AtString[];
extern char Chars[];
extern char CharsAndNumbers[];
extern char ColonString[];
extern char CommaString[];
extern char DashString[];
extern char DefaultErrorMessage[];
extern char DoubleQuoteString[];
extern char EqualString[];
extern char ExclamationPointString[];
extern char ForwardSlashString[];
extern char HexNumbers[];
extern char HyphenString[];
extern char LessThanString[];
extern char LeftBracketString[];
extern char LeftBraceString[];
extern char LeftParenthesisString[];
extern char GreaterThanString[];
extern char MathString[];
extern char NullString[];
extern char Numbers[];
extern char PipeString[];
extern char PlusString[];
extern char PercentString[];
extern char PeriodString[];
extern char PoundString[];
extern char QuestionMarkString[];
extern char RightBracketString[];
extern char RightBraceString[];
extern char RightParenthesisString[];
extern char SemicolonString[];
extern char SingleArrowString[];
extern char SingleQuoteString[];
extern char Underscore[];
extern char WhitespaceChars[];

#define SymbolDef(name, ID) static const unsigned short name = ID;
#define CustomSymbolStart 16000
class SymbolID {
public:
  static bool IsCharacterSymbol(Ref<Symbol> symbol);
  static bool AllCharacterSymbols(Vector<Ref<Symbol>> symbolVector);

  SymbolDef(carriageReturn, '\r');
  SymbolDef(lineFeed, '\n');
  SymbolDef(tab, '\t');
  SymbolDef(space, ' ');
  SymbolDef(eof, 256);
  SymbolDef(whitespace, 257);
  SymbolDef(nOrMoreExpression, 259);
  SymbolDef(identifierCharacters, 260);
  SymbolDef(orExpression, 261);
  SymbolDef(oneOrMoreExpression, 262);
  SymbolDef(optionalWhitespace, 263);
  SymbolDef(andExpression, 264);
  SymbolDef(zeroOrMoreExpression, 265);
  SymbolDef(literalExpression, 266);
  SymbolDef(atLeastAndAtMostExpression, 267);
  SymbolDef(notLiteralExpression, 268);
  SymbolDef(optionalExpression, 269);
  SymbolDef(notPeekExpression, 270);
  SymbolDef(peekExpression, 271);
  SymbolDef(integerExpression, 272);
  SymbolDef(notUnmatchedBlockExpression, 273);
  SymbolDef(floatExpression, 274);
};

// Allow you to control "flattening" which controls what the tree looks like if
// the node is successful:
//  - Flatten means take this symbol out of the tree and reparent its children
//  to its parent (which is useful for nodes that are there for mechanics, not
//  the meaning of the parse tree)
//  - Delete means remove this symbol and all children completely (which you
//  might want to do for a comment)
//  - None means leave this node in the tree (for when the node is meaningful
//  like a name of something)
enum class FlattenType { None, Delete, Flatten };

#define GetError(myErrorMessage, passedErrorMessage)                           \
  (string(myErrorMessage) == "" ? passedErrorMessage : myErrorMessage)

#define Spaces() string((size_t)(lexer->TransactionDepth() * 3), ' ')

// This is the base class that is used by all the rules and the primary thing
// the rules (and thus the Parser) generate
class Symbol : public Reference {
  GDCLASS(Symbol, Reference);

public:
  Symbol(unsigned short symbolID)
      : m_flattenType(FlattenType::None), m_symbolID(symbolID) {}

  Symbol(unsigned short symbolID, FlattenType flattenType)
      : m_flattenType(flattenType), m_symbolID(symbolID) {}

  virtual ~Symbol() {}

  void AddSubsymbol(Ref<Symbol> symbol) {
    FailFastAssert(symbol != nullptr);
    m_subSymbols.push_back(symbol);
  }

  void AddSubsymbol(Vector<Ref<Symbol>>::iterator begin,
                    Vector<Ref<Symbol>>::iterator end) {
    for_each(begin, end, [&](Ref<Symbol> &symbol) {
      FailFastAssert(symbol != nullptr);
      this->m_subSymbols.push_back(symbol);
    });
  }

  virtual void AddToStream(stringstream &stream) {
    for_each(m_subSymbols.begin(), m_subSymbols.end(),
             [&stream](Ref<Symbol> &symbol) { symbol->AddToStream(stream); });
  }

  const Vector<Ref<Symbol>> &children() { return m_subSymbols; }

  virtual void FlattenInto(Vector<Ref<Symbol>> &symbolVector) {
    switch (m_flattenType) {
    case FlattenType::None:
      NoFlatten(symbolVector);
      break;
    case FlattenType::Delete:
      // Don't add this symbol or any children to the tree
      break;
    case FlattenType::Flatten:
      FlattenChildren(symbolVector);
      break;
    }
  }

  void FlattenChildren(Vector<Ref<Symbol>> &symbolVector) {
    for_each(m_subSymbols.begin(), m_subSymbols.end(), [&](Ref<Symbol> child) {
      FailFastAssert(child != nullptr);
      child->FlattenInto(symbolVector);
    });
  }

  void NoFlatten(Vector<Ref<Symbol>> &symbolVector) {
    Vector<Ref<Symbol>> newChildren;
    FlattenChildren(newChildren);
    m_subSymbols.swap(newChildren);
    symbolVector.push_back(shared_from_this());
  }

  int HasSubsymbols() { return m_subSymbols.size() > 0; }
  int SubsymbolCount() { return (int)m_subSymbols.size(); }
  bool operator==(const Symbol &other) const {
    return m_symbolID == other.m_symbolID;
  }
  bool operator!=(const Symbol &other) const { return !(*this == other); }

  string ToString() {
    stringstream stream;
    AddToStream(stream);
    return stream.str();
  }

  unsigned short symbolID() { return m_symbolID; };
  FlattenType flattenType() { return m_flattenType; };

protected:
  FlattenType m_flattenType;
  // SymbolID needs to be a number that is unique for the entire parser
  // so that you know what got generated later
  unsigned short m_symbolID;
  Vector<Ref<Symbol>> m_subSymbols;
};

// Symbols returned by the Lexer are simple characters (except for EOF) and will
// be of this class
class LexerSymbol : public Symbol {
public:
  LexerSymbol(char character) : Symbol(character, FlattenType::None) {}

  virtual void AddToStream(stringstream &stream) { stream << (char)m_symbolID; }

  static Ref<Symbol> TryParse(Ref<Lexer> lexer, const string &errorMessage) {
    lexer->ReportFailure(GetError(DefaultErrorMessage, errorMessage));
    return nullptr;
  }
};

// Matches a single character. The SymbolID used is the ascii value of the
// character.
template <char *character, FlattenType flatten = FlattenType::Delete,
          char *staticErrorMessage = DefaultErrorMessage>
class CharacterSymbol : public Symbol {
public:
  typedef CharacterSymbol<character, flatten, staticErrorMessage> ThisType;
  CharacterSymbol() : Symbol(*character, flatten) {}

  CharacterSymbol(char c) : Symbol(c, flatten) {}

  virtual void AddToStream(stringstream &stream) { stream << (char)m_symbolID; }

  static Ref<Symbol> TryParse(Ref<Lexer> lexer, const string &errorMessage) {
    LexerReader reader(lexer);
    reader.Begin();
    Ref<Symbol> streamSymbol = reader.Read();

    if (SymbolID::IsCharacterSymbol(streamSymbol) &&
        streamSymbol->symbolID() == *character) {
      TraceString3("{0}{1}(Succ) - CharacterSymbol::Parse found '{2}'",
                   SystemTraceType::Parsing, TraceDetail::Diagnostic, Spaces(),
                   GetError(staticErrorMessage, errorMessage),
                   streamSymbol->ToString());
      reader.Commit();
      return shared_ptr<ThisType>(new ThisType());
    } else {
      TraceString4(
          "{0}{1}(FAIL) - CharacterSymbol::Parse found '{2}', wanted '{3}'",
          SystemTraceType::Parsing, TraceDetail::Diagnostic, Spaces(),
          GetError(staticErrorMessage, errorMessage), streamSymbol->ToString(),
          character);
      lexer->ReportFailure(GetError(staticErrorMessage, errorMessage));
      return nullptr;
    }
  }
};

// Matches the end of a document. The SymbolID used is SymbolID::eof.
class EofSymbol : public Symbol {
public:
  EofSymbol() : Symbol(SymbolID::eof, FlattenType::Delete) {}

  static Ref<Symbol> TryParse(Ref<Lexer> lexer, const string &errorMessage) {
    LexerReader reader(lexer);
    reader.Begin();
    Ref<Symbol> streamSymbol = reader.Read();
    if (streamSymbol->symbolID() == SymbolID::eof) {
      TraceString2("{0}{1}(Succ) - EofSymbol::Parse", SystemTraceType::Parsing,
                   TraceDetail::Diagnostic, Spaces(), errorMessage);
      reader.Commit();
      return streamSymbol;
    } else {
      TraceString3("{0}{1}(FAIL) - EofSymbol::Parse, found {2}",
                   SystemTraceType::Parsing, TraceDetail::Diagnostic, Spaces(),
                   errorMessage, streamSymbol->ToString());
      lexer->ReportFailure(errorMessage);
      return nullptr;
    }
  }

  virtual void AddToStream(stringstream &stream) { stream << "<EOF>"; }

  static Ref<Symbol> defaultValue;
};

// Matches any single character *except* the disallowedCharacters. The SymbolID
// used is the ascii value of the character.
template <char *disallowedCharacters, FlattenType flatten = FlattenType::None,
          char *staticErrorMessage = DefaultErrorMessage>
class CharacterSetExceptSymbol
    : public CharacterSymbol<NullString, flatten, staticErrorMessage> {
public:
  typedef CharacterSetExceptSymbol<disallowedCharacters, flatten,
                                   staticErrorMessage>
      ThisType;
  CharacterSetExceptSymbol(char character)
      : CharacterSymbol<NullString, flatten, staticErrorMessage>(character) {}

  static Ref<Symbol> TryParse(Ref<Lexer> lexer, const string &errorMessage) {
    LexerReader reader(lexer);
    reader.Begin();
    Ref<Symbol> streamSymbol = reader.Read();

    if (SymbolID::IsCharacterSymbol(streamSymbol) &&
        strchr(disallowedCharacters, streamSymbol->symbolID()) == nullptr) {
      TraceString4("{0}{1}(Succ) - CharacterSetExceptSymbol::Parse found '{2}' "
                   "expected not any of '{3}'",
                   SystemTraceType::Parsing, TraceDetail::Diagnostic, Spaces(),
                   GetError(staticErrorMessage, errorMessage),
                   streamSymbol->ToString(), disallowedCharacters);
      reader.Commit();
      return shared_ptr<ThisType>(new ThisType((char)streamSymbol->symbolID()));
    } else {
      // not found
      TraceString4("{0}{1}(FAIL) - CharacterSetExceptSymbol::Parse found '{2}' "
                   "expected not any of'{3}'",
                   SystemTraceType::Parsing, TraceDetail::Diagnostic, Spaces(),
                   GetError(staticErrorMessage, errorMessage),
                   streamSymbol->ToString(), disallowedCharacters);
      lexer->ReportFailure(GetError(staticErrorMessage, errorMessage));
      return nullptr;
    }
  }
};

// Matches any single character in the set specified by allowedCharacters. The
// SymbolID used is the ascii value of the character.
template <char *allowedCharacters, FlattenType flatten = FlattenType::None,
          char *staticErrorMessage = DefaultErrorMessage>
class CharacterSetSymbol
    : public CharacterSymbol<NullString, flatten, staticErrorMessage> {
public:
  typedef CharacterSetSymbol<allowedCharacters, flatten, staticErrorMessage>
      ThisType;
  CharacterSetSymbol(char character)
      : CharacterSymbol<NullString, flatten, staticErrorMessage>(character) {}

  static Ref<Symbol> TryParse(Ref<Lexer> lexer, const string &errorMessage) {
    LexerReader reader(lexer);
    reader.Begin();
    Ref<Symbol> streamSymbol = reader.Read();

    if ((streamSymbol->symbolID() == SymbolID::eof) ||
        strchr(allowedCharacters, streamSymbol->symbolID()) == nullptr) {
      // not found
      TraceString4("{0}{1}(FAIL) - CharacterSetSymbol::Parse found '{2}', "
                   "wanted one of '{3}'",
                   SystemTraceType::Parsing, TraceDetail::Diagnostic, Spaces(),
                   GetError(staticErrorMessage, errorMessage),
                   streamSymbol->ToString(), allowedCharacters);
      lexer->ReportFailure(GetError(staticErrorMessage, errorMessage));
      return nullptr;
    } else {
      TraceString4("{0}{1}(Succ) - CharacterSetSymbol::Parse found '{2}', "
                   "wanted one of '{3}'",
                   SystemTraceType::Parsing, TraceDetail::Diagnostic, Spaces(),
                   GetError(staticErrorMessage, errorMessage),
                   streamSymbol->ToString(), allowedCharacters);
      reader.Commit();
      return shared_ptr<ThisType>(new ThisType((char)streamSymbol->symbolID()));
    }
  }
};

// Some typical character sets that are used often
typedef CharacterSetSymbol<Chars> CharSymbol;
typedef CharacterSetSymbol<MathString> MathSymbol;
typedef CharacterSetSymbol<CharsAndNumbers> CharOrNumberSymbol;
typedef CharacterSetSymbol<Numbers> NumberSymbol;
typedef CharacterSetSymbol<HexNumbers> HexNumberSymbol;
typedef CharacterSetSymbol<WhitespaceChars> WhitespaceCharSymbol;

// Matches a specific string of characters.
template <char *literalString, FlattenType flatten = FlattenType::None,
          unsigned short ID = SymbolID::literalExpression,
          char *staticErrorMessage = DefaultErrorMessage>
class LiteralExpression : public Symbol {
public:
  typedef LiteralExpression<literalString, flatten, ID, staticErrorMessage>
      ThisType;
  LiteralExpression() : Symbol(ID, flatten) {}

  static Ref<Symbol> TryParse(Ref<Lexer> lexer, const string &errorMessage) {
    LexerReader reader(lexer);
    reader.Begin();
    Ref<Symbol> streamSymbol;
    shared_ptr<ThisType> literalSymbol = shared_ptr<ThisType>(new ThisType());

    int position = 0;
    while (literalString[position] != '\0' &&
           reader.Read(streamSymbol) != SymbolID::eof) {
      if (SymbolID::IsCharacterSymbol(streamSymbol) &&
          streamSymbol->symbolID() == literalString[position]) {
        literalSymbol->AddSubsymbol(streamSymbol);
      } else {
        TraceString4(
            "{0}{1}(FAIL) - LiteralExpression::Parse found '{2}', wanted '{3}'",
            SystemTraceType::Parsing, TraceDetail::Diagnostic, Spaces(),
            GetError(staticErrorMessage, errorMessage),
            streamSymbol->ToString(), literalString[position]);
        lexer->ReportFailure(GetError(staticErrorMessage, errorMessage));
        return nullptr;
      }

      position++;
    }

    if (position == strlen(literalString)) {
      TraceString3("{0}{1}(Succ) - LiteralExpression::Parse found '{2}'",
                   SystemTraceType::Parsing, TraceDetail::Diagnostic, Spaces(),
                   GetError(staticErrorMessage, errorMessage), literalString);
      reader.Commit();
      return literalSymbol;
    } else {
      TraceString3("{0}{1}(FAIL) - LiteralExpression::Parse wanted '{2}'",
                   SystemTraceType::Parsing, TraceDetail::Diagnostic, Spaces(),
                   GetError(staticErrorMessage, errorMessage), literalString);
      return nullptr;
    }
  }
};

// Used when you want to match *everything* (including delimiters and including
// zero characters) that appears within a block delimited by characters like ().
// Will stop when it finds an unmatched right delimiter. Allows parsing of
// string like "(foo(bar))", if you want to match everything inside the outer ()
// in a single token even if it includes more of the delimiters (as long as they
// are matched). Example of use:
//		CharacterSymbol<LeftParenthesisString, FlattenType::Delete,
// errExpectedParenthesis>,
// NotUnmatchedBlockExpression<LeftParenthesisString, RightParenthesisString,
// FlattenType::None, StoreObjectSymbolID::initialValue>,
//		CharacterSymbol<RightParenthesisString, FlattenType::Delete,
// errExpectedParenthesis>,
// Only supports single character blocks like () [] {}
template <char *startBlockChar, char *endBlockChar,
          FlattenType flatten = FlattenType::None,
          unsigned short ID = SymbolID::notUnmatchedBlockExpression,
          char *staticErrorMessage = DefaultErrorMessage>
class NotUnmatchedBlockExpression : public Symbol {
public:
  typedef NotUnmatchedBlockExpression<startBlockChar, endBlockChar, flatten, ID,
                                      staticErrorMessage>
      ThisType;
  NotUnmatchedBlockExpression() : Symbol(ID, flatten) {}

  static Ref<Symbol> TryParse(Ref<Lexer> lexer, const string &errorMessage) {
    // start with blockLevel = 1
    // Loop through grabbing characters adding 1 if we see startBlockChar,
    // subtracting 1 if we see endBlockChar When blockLevel == 0 we are done,
    // rollback so the ending character is still in the stream and exit
    LexerReader reader(lexer);
    Ref<Symbol> streamSymbol;
    shared_ptr<ThisType> symbol = shared_ptr<ThisType>(new ThisType());
    int blockLevel = 1;

    reader.Begin();
    while (reader.Peek(streamSymbol) != SymbolID::eof) {
      if (SymbolID::IsCharacterSymbol(streamSymbol) &&
          streamSymbol->symbolID() == startBlockChar[0]) {
        blockLevel++;
      } else if (SymbolID::IsCharacterSymbol(streamSymbol) &&
                 streamSymbol->symbolID() == endBlockChar[0]) {
        blockLevel--;
        if (blockLevel == 0) {
          // Success! Don't consume the final symbol and return
          TraceString3(
              "{0}{1}(Succ) - NotUnmatchedBlockExpression::Parse found '{2}'",
              SystemTraceType::Parsing, TraceDetail::Diagnostic, Spaces(),
              GetError(staticErrorMessage, errorMessage), endBlockChar);
          reader.Commit();
          return symbol;
        }
      }

      reader.Read();
      symbol->AddSubsymbol(streamSymbol);
    }

    // We're at EOF.  If all the blocks have been closed but one, this is a
    // success
    if (blockLevel == 1) {
      TraceString3(
          "{0}{1}(Succ) - NotUnmatchedBlockExpression::Parse found '<EOF>'",
          SystemTraceType::Parsing, TraceDetail::Diagnostic, Spaces(),
          GetError(staticErrorMessage, errorMessage), endBlockChar);
      reader.Commit();
      return symbol;
    } else {
      TraceString5("{0}{1}(FAIL) - NotUnmatchedBlockExpression::Parse still "
                   "{4} deep within nested '{2}{3}'",
                   SystemTraceType::Parsing, TraceDetail::Diagnostic, Spaces(),
                   GetError(staticErrorMessage, errorMessage), startBlockChar,
                   endBlockChar, blockLevel);
      lexer->ReportFailure(GetError(staticErrorMessage, errorMessage));
      return nullptr;
    }
  }
};

// Matches any sequence of characters (including zero) except the specified
// literal
template <char *literalString, FlattenType flatten = FlattenType::None,
          unsigned short ID = SymbolID::notLiteralExpression,
          char *staticErrorMessage = DefaultErrorMessage>
class NotLiteralExpression : public Symbol {
public:
  typedef NotLiteralExpression<literalString, flatten, ID, staticErrorMessage>
      ThisType;
  NotLiteralExpression() : Symbol(ID, flatten) {}

  static Ref<Symbol> TryParse(Ref<Lexer> lexer, const string &errorMessage) {
    LexerReader reader(lexer);
    Ref<Symbol> streamSymbol;
    shared_ptr<ThisType> symbol = shared_ptr<ThisType>(new ThisType());
    Vector<Ref<Symbol>> partialLiteral;

    // Each stream of characters is a different transaction
    // We commit the transaction and start a new one if
    // we see a new beginning of the literal
    reader.Begin();
    while (reader.Peek(streamSymbol) != SymbolID::eof) {
      // If we see the beginning of the literal, commit the characters we've
      // read so far and try to read the literal
      if (SymbolID::IsCharacterSymbol(streamSymbol) &&
          streamSymbol->symbolID() == literalString[0]) {
        reader.Commit();
        reader.Begin();
        int position = 0;
        Vector<Ref<Symbol>> partialLiteral;
        while (literalString[position] != '\0' &&
               reader.Peek(streamSymbol) != SymbolID::eof) {
          if (streamSymbol->symbolID() == literalString[position]) {
            // Consume the symbol
            reader.Read();
            partialLiteral.push_back(streamSymbol);
            position++;
          } else {
            // Not a complete literal, commit and push symbols into outer symbol
            reader.Commit();
            symbol->AddSubsymbol(partialLiteral.begin(), partialLiteral.end());
            partialLiteral.clear();
            reader.Begin();
            break;
          }
        }

        if (literalString[position] == '\0') {
          // We found the entire literal, rollback so it is still there and exit
          TraceString3("{0}{1}(Succ) - NotLiteralExpression::Parse found '{2}'",
                       SystemTraceType::Parsing, TraceDetail::Diagnostic,
                       Spaces(), GetError(staticErrorMessage, errorMessage),
                       literalString);
          reader.Abort();
          return symbol;
        } else if (streamSymbol->symbolID() == SymbolID::eof) {
          // We are at EOF
          // There was a partial symbol at the end, push into outer sumbol
          TraceString2("{0}{1}(Succ) - NotLiteralExpression::Parse found <EOF>",
                       SystemTraceType::Parsing, TraceDetail::Diagnostic,
                       Spaces(), GetError(staticErrorMessage, errorMessage));
          reader.Commit();
          symbol->AddSubsymbol(partialLiteral.begin(), partialLiteral.end());
          partialLiteral.clear();
          return symbol;
        }
      } else {
        // Not part of our literal, consume and continue
        reader.Read();
        symbol->AddSubsymbol(streamSymbol);
      }
    }

    TraceString2("{0}{1}(Succ) - NotLiteralExpression::Parse found <EOF>",
                 SystemTraceType::Parsing, TraceDetail::Diagnostic, Spaces(),
                 GetError(staticErrorMessage, errorMessage));
    reader.Commit();
    return symbol;
  }
};

// Matches the SymbolType rule at least N and at most M times.
// This is not designed to be used directly, use the wrapper classes instead:
// NOrMoreExpression, OneOrMoreExpression, ZeroOrMoreExpression
template <class SymbolType, int AtLeast, int AtMost,
          FlattenType flatten = FlattenType::Flatten,
          unsigned short ID = SymbolID::atLeastAndAtMostExpression,
          char *staticErrorMessage = DefaultErrorMessage>
class AtLeastAndAtMostExpression : public Symbol {
public:
  typedef AtLeastAndAtMostExpression<SymbolType, AtLeast, AtMost, flatten, ID,
                                     staticErrorMessage>
      ThisType;

  AtLeastAndAtMostExpression() : Symbol(ID, flatten) {}

  static Ref<Symbol> TryParse(Ref<Lexer> lexer, const string &errorMessage) {
    LexerReader reader(lexer);
    shared_ptr<ThisType> expression = shared_ptr<ThisType>(new ThisType());
    Ref<Symbol> streamSymbol;

    reader.Begin();
    Ref<Symbol> newSymbol;
    do {
      newSymbol = SymbolType::TryParse(
          lexer, GetError(staticErrorMessage, errorMessage));
      if (newSymbol != nullptr) {
        expression->AddSubsymbol(newSymbol);
      }

      if (expression->SubsymbolCount() > AtMost) {
        TraceString5("{0}{1}(FAIL) - {2}to{3}Expression::Parse count= {4}",
                     SystemTraceType::Parsing, TraceDetail::Diagnostic,
                     Spaces(), GetError(staticErrorMessage, errorMessage),
                     AtLeast, AtMost, expression->SubsymbolCount());
        lexer->ReportFailure(GetError(staticErrorMessage, errorMessage));
        return nullptr;
      }
    } while (newSymbol != nullptr);

    if (expression->SubsymbolCount() >= AtLeast) {
      TraceString5("{0}{1}(Succ) - {2}to{3}Expression::Parse count= {4}",
                   SystemTraceType::Parsing, TraceDetail::Diagnostic, Spaces(),
                   GetError(staticErrorMessage, errorMessage), AtLeast, AtMost,
                   expression->SubsymbolCount());
      reader.Commit();
      return expression;
    } else {
      TraceString5("{0}{1}(FAIL) - {2}to{3}Expression::Parse count= {4}",
                   SystemTraceType::Parsing, TraceDetail::Diagnostic, Spaces(),
                   GetError(staticErrorMessage, errorMessage), AtLeast, AtMost,
                   expression->SubsymbolCount());
      lexer->ReportFailure(GetError(staticErrorMessage, errorMessage));
      return nullptr;
    }
  }
};

// Matches the SymbolType rule at least N times.
template <class SymbolType, int N, FlattenType flatten = FlattenType::Flatten,
          unsigned short ID = SymbolID::nOrMoreExpression,
          char *staticErrorMessage = DefaultErrorMessage>
class NOrMoreExpression
    : public AtLeastAndAtMostExpression<SymbolType, N, INT_MAX, flatten, ID,
                                        staticErrorMessage> {
public:
  static Ref<Symbol> TryParse(Ref<Lexer> lexer, const string &errorMessage) {
    return AtLeastAndAtMostExpression<
        SymbolType, N, INT_MAX, flatten, ID,
        staticErrorMessage>::TryParse(lexer, GetError(staticErrorMessage,
                                                      errorMessage));
  }
};

// Matches the SymbolType rule at least 1 times.
template <class SymbolType, FlattenType flatten = FlattenType::Flatten,
          unsigned short ID = SymbolID::oneOrMoreExpression,
          char *staticErrorMessage = DefaultErrorMessage>
class OneOrMoreExpression
    : public AtLeastAndAtMostExpression<SymbolType, 1, INT_MAX, flatten, ID,
                                        staticErrorMessage> {
public:
  static Ref<Symbol> TryParse(Ref<Lexer> lexer, const string &errorMessage) {
    return AtLeastAndAtMostExpression<
        SymbolType, 1, INT_MAX, flatten, ID,
        staticErrorMessage>::TryParse(lexer, GetError(staticErrorMessage,
                                                      errorMessage));
  }
};

// Matches the SymbolType rule zero or more times.
template <class SymbolType, FlattenType flatten = FlattenType::Flatten,
          unsigned short ID = SymbolID::zeroOrMoreExpression,
          char *staticErrorMessage = DefaultErrorMessage>
class ZeroOrMoreExpression
    : public AtLeastAndAtMostExpression<SymbolType, 0, INT_MAX, flatten, ID,
                                        staticErrorMessage> {
public:
  static Ref<Symbol> TryParse(Ref<Lexer> lexer, const string &errorMessage) {
    return AtLeastAndAtMostExpression<
        SymbolType, 0, INT_MAX, flatten, ID,
        staticErrorMessage>::TryParse(lexer, GetError(staticErrorMessage,
                                                      errorMessage));
  }
};

// Used when two things need to parse to the same symbol:
// If the SymbolType node matches, throws it away and replaces with the SymbolID
// specified that outputs replacementString as its text
template <class SymbolType, char *replacementString,
          FlattenType flatten = FlattenType::None,
          unsigned short ID = SymbolID::optionalExpression,
          char *staticErrorMessage = DefaultErrorMessage>
class ReplaceExpression : public Symbol {
public:
  typedef ReplaceExpression<SymbolType, replacementString, flatten, ID,
                            staticErrorMessage>
      ThisType;

  ReplaceExpression() : Symbol(ID, flatten) {}

  virtual void AddToStream(stringstream &stream) {
    stream << replacementString;
  }

  static Ref<Symbol> TryParse(Ref<Lexer> lexer, const string &errorMessage) {
    Ref<Symbol> newSymbol =
        SymbolType::TryParse(lexer, GetError(staticErrorMessage, errorMessage));
    if (newSymbol != nullptr) {
      TraceString2(
          "{0}{1}(Succ) - ReplaceNodeExpression subexpression succeeded",
          SystemTraceType::Parsing, TraceDetail::Diagnostic, Spaces(),
          GetError(staticErrorMessage, errorMessage));
      shared_ptr<ThisType> expression = shared_ptr<ThisType>(new ThisType());
      return expression;
    } else {
      TraceString2("{0}{1}(FAIL) - ReplaceNodeExpression::subexpression failed",
                   SystemTraceType::Parsing, TraceDetail::Diagnostic, Spaces(),
                   GetError(staticErrorMessage, errorMessage));
      lexer->ReportFailure(GetError(staticErrorMessage, errorMessage));
      return nullptr;
    }
  }
};

// If SymbolType succeeds, it is added as a child of this Symbol.
// Used when a structure is expected that doesn't naturally fall out of the text
template <class SymbolType, FlattenType flatten = FlattenType::Flatten,
          unsigned short ID = SymbolID::optionalExpression,
          char *staticErrorMessage = DefaultErrorMessage>
class GroupExpression : public Symbol {
public:
  typedef GroupExpression<SymbolType, flatten, ID, staticErrorMessage> ThisType;

  GroupExpression() : Symbol(ID, flatten) {}

  static Ref<Symbol> TryParse(Ref<Lexer> lexer, const string &errorMessage) {
    Ref<Symbol> newSymbol =
        SymbolType::TryParse(lexer, GetError(staticErrorMessage, errorMessage));
    if (newSymbol != nullptr) {
      TraceString2("{0}{1}(Succ) - GroupExpression subexpression succeeded",
                   SystemTraceType::Parsing, TraceDetail::Diagnostic, Spaces(),
                   GetError(staticErrorMessage, errorMessage));
      shared_ptr<ThisType> expression = shared_ptr<ThisType>(new ThisType());
      expression->AddSubsymbol(newSymbol);
      return expression;
    } else {
      TraceString2("{0}{1}(FAIL) - GroupExpression::subexpression failed",
                   SystemTraceType::Parsing, TraceDetail::Diagnostic, Spaces(),
                   GetError(staticErrorMessage, errorMessage));
      lexer->ReportFailure(GetError(staticErrorMessage, errorMessage));
      return nullptr;
    }
  }
};

// Doesn't fail if SymbolType fails
template <class SymbolType, FlattenType flatten = FlattenType::Flatten,
          unsigned short ID = SymbolID::optionalExpression,
          char *staticErrorMessage = DefaultErrorMessage>
class OptionalExpression
    : public AtLeastAndAtMostExpression<SymbolType, 0, 1, flatten, ID,
                                        staticErrorMessage> {
public:
  static Ref<Symbol> TryParse(Ref<Lexer> lexer, const string &errorMessage) {
    return AtLeastAndAtMostExpression<
        SymbolType, 0, 1, flatten, ID,
        staticErrorMessage>::TryParse(lexer, GetError(staticErrorMessage,
                                                      errorMessage));
  }
};

// Used as a bogus default argument for the Args class below
class EmptyClass {
public:
  static Ref<Symbol> TryParse(Ref<Lexer> lexer, const string &errorMessage) {
    StaticFailFastAssert(false);
    return nullptr;
  }
};

// Args is a Template hack that allows for adding a variable number of arguments
// that are symbols. It is used by rules like And and Or that accept many rules.
// They assume the class you pass in is of type "Args"
template <class Symbol1 = EmptyClass, class Symbol2 = EmptyClass,
          class Symbol3 = EmptyClass, class Symbol4 = EmptyClass,
          class Symbol5 = EmptyClass, class Symbol6 = EmptyClass,
          class Symbol7 = EmptyClass, class Symbol8 = EmptyClass,
          class Symbol9 = EmptyClass, class Symbol10 = EmptyClass,
          class Symbol11 = EmptyClass, class Symbol12 = EmptyClass,
          class Symbol13 = EmptyClass, class Symbol14 = EmptyClass,
          class Symbol15 = EmptyClass, class Symbol16 = EmptyClass,
          class Symbol17 = EmptyClass, class Symbol18 = EmptyClass,
          class Symbol19 = EmptyClass, class Symbol20 = EmptyClass>
class Args {
public:
  static int Count() {
    int count = (!std::is_empty<Symbol1>::value ? 1 : 0) +
                (!std::is_empty<Symbol2>::value ? 1 : 0) +
                (!std::is_empty<Symbol3>::value ? 1 : 0) +
                (!std::is_empty<Symbol4>::value ? 1 : 0) +
                (!std::is_empty<Symbol5>::value ? 1 : 0) +
                (!std::is_empty<Symbol6>::value ? 1 : 0) +
                (!std::is_empty<Symbol7>::value ? 1 : 0) +
                (!std::is_empty<Symbol8>::value ? 1 : 0) +
                (!std::is_empty<Symbol9>::value ? 1 : 0) +
                (!std::is_empty<Symbol10>::value ? 1 : 0) +
                (!std::is_empty<Symbol11>::value ? 1 : 0) +
                (!std::is_empty<Symbol12>::value ? 1 : 0) +
                (!std::is_empty<Symbol13>::value ? 1 : 0) +
                (!std::is_empty<Symbol14>::value ? 1 : 0) +
                (!std::is_empty<Symbol15>::value ? 1 : 0) +
                (!std::is_empty<Symbol16>::value ? 1 : 0) +
                (!std::is_empty<Symbol17>::value ? 1 : 0) +
                (!std::is_empty<Symbol18>::value ? 1 : 0) +
                (!std::is_empty<Symbol19>::value ? 1 : 0) +
                (!std::is_empty<Symbol20>::value ? 1 : 0);

    return count;
  }

  static Ref<Symbol> TryParse(int symbolIndex, Ref<Lexer> lexer,
                              const string &errorMessage) {
    switch (symbolIndex) {
    case 0:
      StaticFailFastAssert(!std::is_empty<Symbol1>::value);
      return Symbol1::TryParse(lexer, errorMessage);
    case 1:
      StaticFailFastAssert(!std::is_empty<Symbol2>::value);
      return Symbol2::TryParse(lexer, errorMessage);
    case 2:
      StaticFailFastAssert(!std::is_empty<Symbol3>::value);
      return Symbol3::TryParse(lexer, errorMessage);
    case 3:
      StaticFailFastAssert(!std::is_empty<Symbol4>::value);
      return Symbol4::TryParse(lexer, errorMessage);
    case 4:
      StaticFailFastAssert(!std::is_empty<Symbol5>::value);
      return Symbol5::TryParse(lexer, errorMessage);
    case 5:
      StaticFailFastAssert(!std::is_empty<Symbol6>::value);
      return Symbol6::TryParse(lexer, errorMessage);
    case 6:
      StaticFailFastAssert(!std::is_empty<Symbol7>::value);
      return Symbol7::TryParse(lexer, errorMessage);
    case 7:
      StaticFailFastAssert(!std::is_empty<Symbol8>::value);
      return Symbol8::TryParse(lexer, errorMessage);
    case 8:
      StaticFailFastAssert(!std::is_empty<Symbol9>::value);
      return Symbol9::TryParse(lexer, errorMessage);
    case 9:
      StaticFailFastAssert(!std::is_empty<Symbol10>::value);
      return Symbol10::TryParse(lexer, errorMessage);
    case 10:
      StaticFailFastAssert(!std::is_empty<Symbol11>::value);
      return Symbol11::TryParse(lexer, errorMessage);
    case 11:
      StaticFailFastAssert(!std::is_empty<Symbol12>::value);
      return Symbol12::TryParse(lexer, errorMessage);
    case 12:
      StaticFailFastAssert(!std::is_empty<Symbol13>::value);
      return Symbol13::TryParse(lexer, errorMessage);
    case 13:
      StaticFailFastAssert(!std::is_empty<Symbol14>::value);
      return Symbol14::TryParse(lexer, errorMessage);
    case 14:
      StaticFailFastAssert(!std::is_empty<Symbol15>::value);
      return Symbol15::TryParse(lexer, errorMessage);
    case 15:
      StaticFailFastAssert(!std::is_empty<Symbol16>::value);
      return Symbol16::TryParse(lexer, errorMessage);
    case 16:
      StaticFailFastAssert(!std::is_empty<Symbol17>::value);
      return Symbol17::TryParse(lexer, errorMessage);
    case 17:
      StaticFailFastAssert(!std::is_empty<Symbol18>::value);
      return Symbol18::TryParse(lexer, errorMessage);
    case 18:
      StaticFailFastAssert(!std::is_empty<Symbol19>::value);
      return Symbol19::TryParse(lexer, errorMessage);
    case 19:
      StaticFailFastAssert(!std::is_empty<Symbol20>::value);
      return Symbol20::TryParse(lexer, errorMessage);
    default:
      StaticFailFastAssert(false);
      return nullptr;
    }
  }
};

// Requires that first argument be an Args class, which allows multiple rules to
// be children. Matches (and stops processing) as soon as one of them succeeds.
// Fails otherwise
template <class Args, FlattenType flatten = FlattenType::Flatten,
          unsigned short ID = SymbolID::orExpression,
          char *staticErrorMessage = DefaultErrorMessage>
class OrExpression : public Symbol {
public:
  typedef OrExpression<Args, flatten, ID, staticErrorMessage> ThisType;
  OrExpression() : Symbol(ID, flatten) {}

  static Ref<Symbol> TryParse(Ref<Lexer> lexer, const string &errorMessage) {
    LexerReader reader(lexer);
    shared_ptr<ThisType> expression = shared_ptr<ThisType>(new ThisType());

    // Loop through the symbols and succeed the first time one works
    reader.Begin();
    int argsCount = Args::Count();
    for (int symbolIndex = 0; symbolIndex < argsCount; ++symbolIndex) {
      Ref<Symbol> streamSymbol = Args::TryParse(
          symbolIndex, lexer, GetError(staticErrorMessage, errorMessage));
      if (streamSymbol != nullptr) {
        TraceString4(
            "{0}{1}(Succ) - OrExpression::Parse symbol #{3} found '{2}'",
            SystemTraceType::Parsing, TraceDetail::Diagnostic, Spaces(),
            GetError(staticErrorMessage, errorMessage),
            streamSymbol->ToString(), symbolIndex);
        expression->AddSubsymbol(streamSymbol);
        reader.Commit();
        return expression;
      }
    }

    TraceString2("{0}{1}(FAIL) - OrExpression::Parse", SystemTraceType::Parsing,
                 TraceDetail::Diagnostic, Spaces(),
                 GetError(staticErrorMessage, errorMessage));
    lexer->ReportFailure(GetError(staticErrorMessage, errorMessage));
    return nullptr;
  }
};

// Requires that first argument be an Args class, which allows multiple rules to
// be children. Fails (and stops processing) as soon as one of the symbols in
// Args fails.  Succeeds otherwise
template <class Args, FlattenType flatten = FlattenType::Flatten,
          unsigned short ID = SymbolID::andExpression,
          char *staticErrorMessage = DefaultErrorMessage>
class AndExpression : public Symbol {
public:
  typedef AndExpression<Args, flatten, ID, staticErrorMessage> ThisType;

  AndExpression() : Symbol(ID, flatten) {}

  static Ref<Symbol> TryParse(Ref<Lexer> lexer, const string &errorMessage) {
    LexerReader reader(lexer);
    shared_ptr<ThisType> expression = shared_ptr<ThisType>(new ThisType());

    reader.Begin();
    int argsCount = Args::Count();
    for (int symbolIndex = 0; symbolIndex < argsCount; ++symbolIndex) {
      Ref<Symbol> streamSymbol = Args::TryParse(
          symbolIndex, lexer, GetError(staticErrorMessage, errorMessage));
      if (streamSymbol != nullptr) {
        expression->AddSubsymbol(streamSymbol);
      } else {
        TraceString3("{0}{1}(FAIL) - AndExpression::Parse symbol #{2}",
                     SystemTraceType::Parsing, TraceDetail::Diagnostic,
                     Spaces(), GetError(staticErrorMessage, errorMessage),
                     symbolIndex);
        lexer->ReportFailure(GetError(staticErrorMessage, errorMessage));
        return nullptr;
      }
    }

    TraceString3("{0}{1}(Succ) - AndExpression::Parse found {2}",
                 SystemTraceType::Parsing, TraceDetail::Diagnostic, Spaces(),
                 GetError(staticErrorMessage, errorMessage),
                 expression->ToString());
    reader.Commit();
    return expression;
  }
};

// Matches any series of charactrs in WhitespaceCharSymbol
template <FlattenType flatten = FlattenType::Delete,
          unsigned short ID = SymbolID::whitespace,
          char *staticErrorMessage = DefaultErrorMessage>
class WhitespaceSymbol
    : public AtLeastAndAtMostExpression<WhitespaceCharSymbol, 1, INT_MAX,
                                        flatten, ID, staticErrorMessage> {};

// Same as WhitespaceSymbol but doesn't fail if there aren't any
template <FlattenType flatten = FlattenType::Delete,
          unsigned short ID = SymbolID::whitespace,
          char *staticErrorMessage = DefaultErrorMessage>
class OptionalWhitespaceSymbol
    : public AtLeastAndAtMostExpression<WhitespaceCharSymbol, 0, INT_MAX,
                                        flatten, ID, staticErrorMessage> {};

// Matches text that is *not* the symbol specified by SymbolType
// AND don't consume anything
template <class SymbolType, FlattenType flatten = FlattenType::Delete,
          unsigned short ID = SymbolID::notPeekExpression,
          char *staticErrorMessage = DefaultErrorMessage>
class NotPeekExpression : public Symbol {
public:
  typedef NotPeekExpression<SymbolType, flatten, ID, staticErrorMessage>
      ThisType;

  NotPeekExpression() : Symbol(ID, flatten) {}

  static Ref<Symbol> TryParse(Ref<Lexer> lexer, const string &errorMessage) {
    LexerReader reader(lexer);
    Ref<Symbol> streamSymbol;

    reader.Begin();
    Ref<Symbol> newSymbol =
        SymbolType::TryParse(lexer, GetError(staticErrorMessage, errorMessage));
    if (newSymbol != nullptr) {
      TraceString3("{0}{1}(FAIL) - NotPeekExpression::Parse found '{2}'",
                   SystemTraceType::Parsing, TraceDetail::Diagnostic, Spaces(),
                   GetError(staticErrorMessage, errorMessage),
                   newSymbol->ToString());
      lexer->ReportFailure(GetError(staticErrorMessage, errorMessage));
      return nullptr;
    } else {
      // Even though this was successful, this is only peeking, so abort
      // anything that happened but succeed
      reader.Abort();
      TraceString2("{0}{1}(Succ) - NotPeekExpression::Parse ",
                   SystemTraceType::Parsing, TraceDetail::Diagnostic, Spaces(),
                   GetError(staticErrorMessage, errorMessage));
      return shared_ptr<ThisType>(new ThisType());
    }
  }
};

// Matches text that *is* the symbol specified by SymbolType
// BUT doesn't consume it
template <class SymbolType, FlattenType flatten = FlattenType::Delete,
          unsigned short ID = SymbolID::peekExpression,
          char *staticErrorMessage = DefaultErrorMessage>
class PeekExpression : public Symbol {
public:
  typedef PeekExpression<SymbolType, flatten, ID, staticErrorMessage> ThisType;

  PeekExpression() : Symbol(ID, flatten) {}

  static Ref<Symbol> TryParse(Ref<Lexer> lexer, const string &errorMessage) {
    LexerReader reader(lexer);
    Ref<Symbol> streamSymbol;

    reader.Begin();
    Ref<Symbol> newSymbol =
        SymbolType::TryParse(lexer, GetError(staticErrorMessage, errorMessage));
    if (newSymbol != nullptr) {
      // Even though this was successful, this is only peeking, so abort
      // anything that happened but succeed
      reader.Abort();
      TraceString3("{0}{1}(Succ) - PeekExpression::Parse found '{2}'",
                   SystemTraceType::Parsing, TraceDetail::Diagnostic, Spaces(),
                   GetError(staticErrorMessage, errorMessage),
                   newSymbol->ToString());
      return shared_ptr<ThisType>(new ThisType());
    } else {
      TraceString2("{0}{1}(FAIL) - PeekExpression::Parse",
                   SystemTraceType::Parsing, TraceDetail::Diagnostic, Spaces(),
                   GetError(staticErrorMessage, errorMessage));
      lexer->ReportFailure(GetError(staticErrorMessage, errorMessage));
      return nullptr;
    }
  }
};

// Matches an integer, including those with a + or - in front
template <FlattenType flatten = FlattenType::None,
          unsigned short ID = SymbolID::integerExpression,
          char *staticErrorMessage = DefaultErrorMessage>
class Integer
    : public AndExpression<
          Args<OptionalExpression<OrExpression<
                   Args<CharacterSymbol<PlusString>,
                        CharacterSymbol<DashString, FlattenType::None>>>>,
               OneOrMoreExpression<NumberSymbol>>,
          flatten, ID, staticErrorMessage> {};

// Matches a float with an optional - in front
// [-] Integer "." Integer
template <FlattenType flatten = FlattenType::None,
          unsigned short ID = SymbolID::floatExpression,
          char *staticErrorMessage = DefaultErrorMessage>
class Float
    : public AndExpression<
          Args<
              OptionalExpression<CharacterSymbol<DashString, FlattenType::None>,
                                 FlattenType::Flatten>,
              Integer<>, CharacterSymbol<PeriodString, FlattenType::None>,
              Integer<>>,
          flatten, ID, staticErrorMessage> {};

// Functions that are useful for debugging parsing
class ParserDebug {
public:
  // Make sure tree has a child in position level0Index that has a certain
  // symbolID AND that that child matches the string toString
  static bool CheckTree(Vector<Ref<Symbol>> &tree, int level0Index,
                        unsigned short symbolID, const string &toString) {
    string foo = PrintTree(tree);

    if (tree.size() >= (size_t)level0Index + 1) {
      if (tree[level0Index]->symbolID() == symbolID) {
        if (tree[level0Index]->ToString() == toString) {
          return true;
        }
      }
    }

    return false;
  }

  // Same as above but walks from root to child at index level0Index and then
  // its child at level1Index
  static bool CheckTree(Vector<Ref<Symbol>> &tree, int level0Index,
                        int level1Index, unsigned short symbolID,
                        const string &toString) {
    string foo = PrintTree(tree);

    if (tree.size() >= (size_t)level0Index + 1) {
      if (tree[level0Index]->children().size() >= (size_t)level1Index + 1) {
        if (tree[level0Index]->children()[level1Index]->symbolID() ==
            symbolID) {
          string level1Value =
              tree[level0Index]->children()[level1Index]->ToString();
          if (level1Value == toString) {
            return true;
          }
        }
      }
    }

    return false;
  }

  // Same as above, one level deeper
  static bool CheckTree(Vector<Ref<Symbol>> &tree, int level0Index,
                        int level1Index, int level2Index,
                        unsigned short symbolID, const string &toString) {
    string foo = PrintTree(tree);

    if (tree.size() >= (size_t)level0Index + 1) {
      if (tree[level0Index]->children().size() >= (size_t)level1Index + 1) {
        if (tree[level0Index]->children()[level1Index]->children().size() >=
            (size_t)level2Index + 1) {
          if (tree[level0Index]
                  ->children()[level1Index]
                  ->children()[level2Index]
                  ->symbolID() == symbolID) {
            if (tree[level0Index]
                    ->children()[level1Index]
                    ->children()[level2Index]
                    ->ToString() == toString) {
              return true;
            }
          }
        }
      }
    }

    return false;
  }

  // Attempts to parse rule, has options whether the entire string should be
  // consumed (requireEOF)
  template <class rule>
  static Ref<Symbol> TestTryParse(string testString, int &deepestFailure,
                                  string &errorMessage,
                                  bool requireEOF = true) {
    TraceString1("**** Begin Test: ", SystemTraceType::Parsing,
                 TraceDetail::Detailed, testString);

    deepestFailure = 0;
    Ref<Lexer> lexer = Ref<Lexer>(new Lexer());
    shared_ptr<istream> stream =
        shared_ptr<istream>(new stringstream(testString));
    //	    stream->setf(ios::binary);
    lexer->Open(stream);
    Ref<Symbol> symbol;

    if (requireEOF) {
      symbol = AndExpression<Args<rule, EofSymbol>>::TryParse(
          lexer, "Test Error Message");
    } else {
      symbol = rule::TryParse(lexer, "Test Error Message");
    }

    if (symbol == nullptr) {
      deepestFailure = lexer->DeepestFailure();
      errorMessage = lexer->ErrorMessage();
    }

    return symbol;
  }

  // returns true if the flattened tree returns a single symbolID
  static bool
  CheckFlattenedSingleSymbolResult(Ref<Symbol> rule, unsigned short ID,
                                   const string &result,
                                   Vector<Ref<Symbol>> &flattenedTree) {
    if (rule == nullptr) {
      return false;
    }

    flattenedTree.clear();
    rule->FlattenInto(flattenedTree);
    string foo = PrintTree(flattenedTree);

    if (!(flattenedTree.size() == 1)) {
      return false;
    }

    if (!(flattenedTree[0]->symbolID() == ID)) {
      return false;
    }

    string thisResult = flattenedTree[0]->ToString();
    if (!(thisResult == result)) {
      return false;
    }

    return true;
  }

  static string PrintSymbol(Ref<Symbol> symbol, int level) {
    stringstream stream;

    stream << string((size_t)(level * 3), ' ') << symbol->symbolID();
    switch (symbol->flattenType()) {
    case FlattenType::None:
      stream << " (None)";
      break;
    case FlattenType::Flatten:
      stream << " (Flatten)";
      break;
    case FlattenType::Delete:
      stream << " (Delete)";
      break;
    }

    stream << ": " << symbol->ToString() << "\r\n";

    return stream.str();
  }

  static string PrintTree(Ref<Symbol> &symbol, int level) {
    stringstream stream;
    stream << PrintSymbol(symbol, level);

    for_each(
        symbol->children().begin(), symbol->children().end(),
        [&](Ref<Symbol> symbol) { stream << PrintTree(symbol, level + 1); });

    return stream.str();
  }

  static string PrintTree(Vector<Ref<Symbol>> &symbols) {
    stringstream stream;
    for_each(symbols.begin(), symbols.end(),
             [&](Ref<Symbol> symbol) { stream << PrintTree(symbol, 1); });
    return stream.str();
  }

  static int MaxDepth(const Vector<Ref<Symbol>> &symbols) {
    int maxDepth = 0;
    if (symbols.size() > 0) {
      for_each(symbols.begin(), symbols.end(), [&](Ref<Symbol> symbol) {
        int childMax = MaxDepth(symbol->children());
        if (childMax > maxDepth) {
          maxDepth = childMax;
        }
      });
    }

    return maxDepth + 1;
  }
};

class Symbol;

class CompileError {
public:
  CompileError(int line, int column, const String &errorString)
      : m_column(column), m_error(errorString), m_line(line) {}

  String ToString() {
    return "Line " + lexical_cast<String>(line()) + ", Column " +
           lexical_cast<String>(column()) + ": " + error();
  }

  Property(public, int, column);
  Property(public, String, error);
  ValueProperty(public, String, file);
  Property(public, int, line);
};

// Base class for a compiler that uses a specific parser
// Usage:
//  1. create a class that derives from Compiler<baserule> where baserule is a
//  rule that parses an entire document
//  2. add members to that class which contain whatever you are compiling into.
//  I.e. whatever the document is supposed to become
//  3. override virtual bool ProcessAst(shared_ptr<CompileResultType> ast) and
//  turn the symbols created by the rules into whatever they become
template <class parser> class Compiler {
public:
  typedef Vector<Ref<Symbol>> CompileResultType;
  virtual ~Compiler() {}

  bool CompileDocument(const String &fullPath) {
    shared_ptr<ifstream> stream = shared_ptr<ifstream>(new ifstream());
    StartTimingOnly(Compiler_CompileDocument_LoadFile, SystemTraceType::HTML,
                    TraceDetail::Detailed);
    stream->open(fullPath, ios::binary);
    EndTimingOnly(Compiler_CompileDocument_LoadFile, SystemTraceType::HTML,
                  TraceDetail::Detailed);
    if (stream->good()) {
      return Compile(stream);
    } else {
      this->errors().push_back(
          CompileError(-1, -1, "error loading file '" + fullPath + "'"));
      return false;
    }
  }

  bool Compile(const String document) {
    shared_ptr<istream> stream =
        shared_ptr<istream>(new Stringstream(document));
    return Compile(stream);
  }

  bool Compile(shared_ptr<istream> stream) {
    Initialize();
    shared_ptr<CompileResultType> result =
        shared_ptr<CompileResultType>(new CompileResultType());
    shared_ptr<CompileError> error = this->Compile(stream, result);

    if (error != nullptr) {
      errors().push_back(*error);
      TraceString1("Compiler::Compile Error {0}", SystemTraceType::System,
                   TraceDetail::Normal, GetErrorString());
      return false;
    }

    return ProcessAst(result);
  }

  static Ref<Symbol> GetChild(Ref<Symbol> symbol, int level0Index, int ID0) {
    if (symbol->children().size() >= (size_t)level0Index + 1) {
      Ref<Symbol> found = symbol->children()[level0Index];
      if (ID0 == -1 || found->symbolID() == ID0) {
        return found;
      }
    }

    return nullptr;
  }

  static Ref<Symbol> GetChild(Ref<Symbol> symbol, int level0Index, int ID0,
                              int level1Index, int ID1) {
    Ref<Symbol> level0Symbol = GetChild(symbol, level0Index, ID0);
    if (level0Symbol == nullptr) {
      return nullptr;
    }
    return GetChild(level0Symbol, level1Index, ID1);
  }

  static Ref<Symbol> GetChild(Ref<Symbol> rootSymbol, int level0Index, int ID0,
                              int level1Index, int ID1, int level2Index,
                              int ID2) {
    Ref<Symbol> level0Symbol = GetChild(rootSymbol, level0Index, ID0);
    if (level0Symbol == nullptr) {
      return nullptr;
    }
    return GetChild(level0Symbol, level1Index, ID1, level2Index, ID2);
  }

  String GetErrorString() {
    Stringstream stream;

    for (vector<CompileError>::iterator iter = m_errors.begin();
         iter != m_errors.end(); ++iter) {
      stream << iter->ToString();
    }

    return stream.str();
  }

  bool HasErrors() { return m_errors.size() > 0; }
  virtual void Initialize() { m_errors.clear(); }

  Property(private, vector<CompileError>, errors);

protected:
  shared_ptr<CompileError> Compile(shared_ptr<istream> stream,
                                   shared_ptr<CompileResultType> &flattened) {
    Ref<Lexer> lexer = Ref<Lexer>(new Lexer());
    lexer->Open(stream);

    StartTimingOnly(Compiler_Compile_Parse, SystemTraceType::HTML,
                    TraceDetail::Detailed);
    Ref<Symbol> result = parser::TryParse(lexer, "");
    EndTimingOnly(Compiler_Compile_Parse, SystemTraceType::HTML,
                  TraceDetail::Detailed);

    if (result != nullptr) {
      // No Errors, flatten now

      flattened = shared_ptr<vector<shared_ptr<Symbol>>>(
          new vector<shared_ptr<Symbol>>());
      StartTimingOnly(Compiler_Compile_Flatten, SystemTraceType::HTML,
                      TraceDetail::Detailed);
      result->FlattenInto(*flattened);
      EndTimingOnly(Compiler_Compile_Flatten, SystemTraceType::HTML,
                    TraceDetail::Detailed);
      return nullptr;
    } else {
      // Got an error, figure out what line it is on
      int lineCount;
      int columnCount;
      String errorLine;
      Lexer::GetLineAndColumn(lexer->DeepestFailure(), stream, lineCount,
                              columnCount, &errorLine);
      return shared_ptr<CompileError>(
          new CompileError(lineCount, columnCount,
                           lexer->ErrorMessage() + ", \r\n" + errorLine));
    }
  }

  // This must be overridden to process the tree that got parsed
  virtual bool ProcessAst(shared_ptr<CompileResultType> ast) = 0;
};