#ifndef CPYPDICT_H_
#define CPYPDICT_H_

#include <string>
#include <algorithm>
#include <iostream>
#include <cassert>
#include <fstream>
#include <sstream>
#include <vector>
#include <set>
#include <unordered_map>
#include <functional>
#include <vector>
#include <map>
#include <string>

namespace cpyp {

class ParserVocabulary {
  friend class Corpus;
public:
  typedef std::map<std::string, unsigned> StrToIntMap;

  // String literals
  static constexpr const char* UNK = "<UNK>";
  static constexpr const char* BAD0 = "<BAD0>";

  StrToIntMap wordsToInt;
  std::vector<std::string> intToWords;

  StrToIntMap posToInt;
  std::vector<std::string> intToPos;

  StrToIntMap charsToInt;
  std::vector<std::string> intToChars;

  std::vector<std::string> actions;

  ParserVocabulary() {
    AddEntry(BAD0, &wordsToInt, &intToWords);
    AddEntry(UNK, &wordsToInt, &intToWords);
    // For some reason, original LSTM parser had char and POS lists starting at
    // index 1.
    AddEntry("", &posToInt, &intToPos);
    AddEntry("", &charsToInt, &intToChars);
    AddEntry(BAD0, &charsToInt, &intToChars);
  }

  inline unsigned CountPOS() { return posToInt.size(); }
  inline unsigned CountWords() { return wordsToInt.size(); }
  inline unsigned CountChars() { return charsToInt.size(); }
  inline unsigned CountActions() { return actions.size(); }

private:
  static inline int AddEntry(const std::string& str, StrToIntMap* map,
                             std::vector<std::string>* indexed_list) {
    int new_id = indexed_list->size();
    map->insert({str, new_id});
    indexed_list->push_back(str);
    return new_id;
  }

  static inline unsigned GetOrAddEntry(const std::string& str, StrToIntMap* map,
                                       std::vector<std::string>* indexed_list) {
    auto entry_iter = map->find(str);
    if (entry_iter == map->end()) {
      return AddEntry(str, map, indexed_list);
    } else {
      return entry_iter->second;
    }
  }
};

class Corpus {
  //typedef std::unordered_map<std::string, unsigned, std::hash<std::string> > Map;
  // typedef std::unordered_map<unsigned,std::string, std::hash<std::string> > ReverseMap;
public:
  bool USE_SPELLING = false;

  std::map<int, std::vector<unsigned>> correct_act_sent;
  std::map<int, std::vector<unsigned>> sentences;
  std::map<int, std::vector<unsigned>> sentencesPos;
  std::map<int, std::vector<std::string>> sentencesSurfaceForms;

  /*
  std::map<unsigned,unsigned>* headsTraining;
  std::map<unsigned,std::string>* labelsTraining;

  std::map<unsigned,unsigned>*  headsParsing;
  std::map<unsigned,std::string>* labelsParsing;
  //*/

  ParserVocabulary *vocab;

public:
  Corpus(const std::string& file, ParserVocabulary* vocab) : vocab(vocab) {
    load_correct_actions(file);
  }


  inline unsigned UTF8Len(unsigned char x) {
    if (x < 0x80) return 1;
    else if ((x >> 5) == 0x06) return 2;
    else if ((x >> 4) == 0x0e) return 3;
    else if ((x >> 3) == 0x1e) return 4;
    else if ((x >> 2) == 0x3e) return 5;
    else if ((x >> 1) == 0x7e) return 6;
    else return 0;
  }

  inline unsigned get_or_add_word(const std::string& word) {
    return vocab->GetOrAddEntry(word, &vocab->wordsToInt, &vocab->intToWords);
  }

private:
  void load_correct_actions(const std::string& file);
  void load_correct_actionsDev(const std::string &file);

  static inline void ReplaceStringInPlace(std::string& subject,
                                          const std::string& search,
                                          const std::string& replace) {
    size_t pos = 0;
    while ((pos = subject.find(search, pos)) != std::string::npos) {
      subject.replace(pos, search.length(), replace);
      pos += replace.length();
    }
  }
};

} // namespace

#endif
