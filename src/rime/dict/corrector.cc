//
// Copyright RIME Developers
// Distributed under the BSD License
//
// Created by nameoverflow on 2018/11/14.
//

#include "corrector.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <numeric>
#include <queue>
#include <rime/config.h>
#include <rime/schema.h>
#include <rime/service.h>
#include <rime/ticket.h>

using namespace rime;
using namespace corrector;

static hash_map<char, hash_set<char>> keyboard_map = {
    {'1', {'2', 'q', 'w'}},
    {'2', {'1', '3', 'q', 'w', 'e'}},
    {'3', {'2', '4', 'w', 'e', 'r'}},
    {'4', {'3', '5', 'e', 'r', 't'}},
    {'5', {'4', '6', 'r', 't', 'y'}},
    {'6', {'5', '7', 't', 'y', 'u'}},
    {'7', {'6', '8', 'y', 'u', 'i'}},
    {'8', {'7', '9', 'u', 'i', 'o'}},
    {'9', {'8', '0', 'i', 'o', 'p'}},
    {'0', {'9', '-', 'o', 'p', '['}},
    {'-', {'0', '=', 'p', '[', ']'}},
    {'=', {'-', '[', ']', '\\'}},
    {'q', {'w'}},
    {'w', {'q', 'e'}},
    {'e', {'w', 'r'}},
    {'r', {'e', 't'}},
    {'t', {'r', 'y'}},
    {'y', {'t', 'u'}},
    {'u', {'y', 'i'}},
    {'i', {'u', 'o'}},
    {'o', {'i', 'p'}},
    {'p', {'o', '['}},
    {'[', {'p', ']'}},
    {']', {'[', '\\'}},
    {'\\', {']'}},
    {'a', {'s'}},
    {'s', {'a', 'd'}},
    {'d', {'s', 'f'}},
    {'f', {'d', 'g'}},
    {'g', {'f', 'h'}},
    {'h', {'g', 'j'}},
    {'j', {'h', 'k'}},
    {'k', {'j', 'l'}},
    {'l', {'k', ';'}},
    {';', {'l', '\''}},
    {'\'', {';'}},
    {'z', {'x'}},
    {'x', {'z', 'c'}},
    {'c', {'x', 'v'}},
    {'v', {'c', 'b'}},
    {'b', {'v', 'n'}},
    {'n', {'b', 'm'}},
    {'m', {'n', ','}},
    {',', {'m', '.'}},
    {'.', {',', '/'}},
    {'/', {'.'}},
};

void DFSCollect(const string& origin,
                const string& current,
                size_t ed,
                Script& result);

Script SymDeleteCollector::Collect(size_t edit_distance) {
  // TODO: specifically for 1 length str
  Script script;

  for (auto& v : syllabary_) {
    DFSCollect(v, v, edit_distance, script);
  }

  return script;
}

void DFSCollect(const string& origin,
                const string& current,
                size_t ed,
                Script& result) {
  if (ed <= 0)
    return;
  for (size_t i = 0; i < current.size(); i++) {
    string temp = current;
    temp.erase(i, 1);
    Spelling spelling(origin);
    spelling.properties.tips = origin;
    result[temp].push_back(spelling);
    DFSCollect(origin, temp, ed - 1, result);
  }
}

void EditDistanceCorrector::ToleranceSearch(const Prism& prism,
                                            const string& key,
                                            Corrections* results,
                                            size_t threshold) {
  if (key.empty())
    return;
  size_t key_len = key.length();

  vector<size_t> jump_pos(key_len);

  auto match_next = [&](size_t& node, size_t& point) -> bool {
    auto res_val = trie_->traverse(key.c_str(), node, point, point + 1);
    if (res_val == -2)
      return false;
    if (res_val >= 0) {
      for (auto accessor = QuerySpelling(res_val); !accessor.exhausted();
           accessor.Next()) {
        auto origin = accessor.properties().tips;
        auto current_input = key.substr(0, point);
        if (origin == current_input) {
          continue;  // early termination: this comparison is O(n)
        }
        auto distance = RestrictedDistance(origin, current_input, threshold);
        if (distance <= threshold) {  // only trace near words
          SyllableId corrected;
          if (prism.GetValue(origin, &corrected)) {
            results->Alter(corrected, {distance, corrected, point});
          }
        }
      }
    }
    return true;
  };

  // pass through origin key, cache trie nodes
  size_t max_match = 0;
  for (size_t next_node = 0; max_match < key_len;) {
    jump_pos[max_match] = next_node;
    if (!match_next(next_node, max_match))
      break;
  }

  // start at the next position of deleted char
  for (size_t del_pos = 0; del_pos <= max_match; del_pos++) {
    size_t next_node = jump_pos[del_pos];
    for (size_t key_point = del_pos + 1; key_point < key_len;) {
      if (!match_next(next_node, key_point))
        break;
    }
  }
}

inline uint8_t SubstCost(char left, char right) {
  if (left == right)
    return 0;
  if (keyboard_map[left].find(right) != keyboard_map[left].end()) {
    return 1;
  }
  return 4;
}

// This nice O(min(m, n)) implementation is from
// https://en.wikibooks.org/wiki/Algorithm_Implementation/Strings/Levenshtein_distance#C++
Distance EditDistanceCorrector::LevenshteinDistance(const std::string& s1,
                                                    const std::string& s2) {
  // To change the type this function manipulates and returns, change
  // the return type and the types of the two variables below.
  auto s1len = (size_t)s1.size();
  auto s2len = (size_t)s2.size();

  auto column_start = (decltype(s1len))1;

  auto column = new decltype(s1len)[s1len + 1];
  std::iota(column + column_start - 1, column + s1len + 1, column_start - 1);

  for (auto x = column_start; x <= s2len; x++) {
    column[0] = x;
    auto last_diagonal = x - column_start;
    for (auto y = column_start; y <= s1len; y++) {
      auto old_diagonal = column[y];
      auto possibilities = {column[y] + 1, column[y - 1] + 1,
                            last_diagonal + SubstCost(s1[y - 1], s2[x - 1])};

      column[y] = (std::min)(possibilities);
      last_diagonal = old_diagonal;
    }
  }
  auto result = column[s1len];
  delete[] column;
  return result;
}

// L's distance with transposition allowed
Distance EditDistanceCorrector::RestrictedDistance(const std::string& s1,
                                                   const std::string& s2,
                                                   Distance threshold) {
  auto len1 = s1.size(), len2 = s2.size();
  vector<size_t> d((len1 + 1) * (len2 + 1));

  auto index = [len1, len2](size_t i, size_t j) { return i * (len2 + 1) + j; };

  d[0] = 0;
  for (size_t i = 1; i <= len1; ++i)
    d[index(i, 0)] = i * 2;
  for (size_t i = 1; i <= len2; ++i)
    d[index(0, i)] = i * 2;

  for (size_t i = 1; i <= len1; ++i) {
    auto min_d = threshold + 1;
    for (size_t j = 1; j <= len2; ++j) {
      d[index(i, j)] = (std::min)(
          {d[index(i - 1, j)] + 2, d[index(i, j - 1)] + 2,
           d[index(i - 1, j - 1)] + SubstCost(s1[i - 1], s2[j - 1])});
      if (i > 1 && j > 1 && s1[i - 2] == s2[j - 1] && s1[i - 1] == s2[j - 2]) {
        d[index(i, j)] = (std::min)(d[index(i, j)], d[index(i - 2, j - 2)] + 2);
      }
      min_d = (std::min)(min_d, d[index(i, j)]);
    }
    // early termination: do not continue if too far
    if (min_d > threshold)
      return min_d;
  }
  return (uint8_t)d[index(len1, len2)];
}
bool EditDistanceCorrector::Build(const Syllabary& syllabary,
                                  const Script* script,
                                  uint32_t dict_file_checksum,
                                  uint32_t schema_file_checksum) {
  Syllabary correct_syllabary;
  if (script && !script->empty()) {
    for (auto& v : *script) {
      correct_syllabary.insert(v.first);
    }
  } else {
    correct_syllabary = syllabary;
  }

  SymDeleteCollector collector(correct_syllabary);
  auto correction_script = collector.Collect((size_t)1);

  return Prism::Build(syllabary, &correction_script, dict_file_checksum,
                      schema_file_checksum);
}
EditDistanceCorrector::EditDistanceCorrector(const path& file_path)
    : Prism(file_path) {}

void NearSearchCorrector::ToleranceSearch(const Prism& prism,
                                          const string& key,
                                          Corrections* results,
                                          size_t threshold) {
  if (key.empty())
    return;

  using record = struct {
    size_t node_pos;
    size_t idx;
    size_t distance;
    char ch;
  };

  std::queue<record> queue;
  queue.push({0, 0, 0, key[0]});
  for (auto subst : keyboard_map[key[0]]) {
    queue.push({0, 0, 1, subst});
  }
  for (; !queue.empty(); queue.pop()) {
    auto& rec = queue.front();
    char ch = rec.ch;
    char& exchange(const_cast<char*>(key.c_str())[rec.idx]);
    std::swap(ch, exchange);
    auto val =
        prism.trie().traverse(key.c_str(), rec.node_pos, rec.idx, rec.idx + 1);
    std::swap(ch, exchange);

    if (val == -2)
      continue;
    if (val >= 0) {
      results->Alter(val, {rec.distance, val, rec.idx});
    }
    if (rec.idx < key.size()) {
      queue.push({rec.node_pos, rec.idx, rec.distance, key[rec.idx]});
      if (rec.distance < threshold) {
        for (auto subst : keyboard_map[key[rec.idx]]) {
          queue.push({rec.node_pos, rec.idx, rec.distance + 1, subst});
        }
      }
    }
  }
}
void CorrectorComponent::Unified::ToleranceSearch(const Prism& prism,
                                                  const string& key,
                                                  Corrections* results,
                                                  size_t tolerance) {
  for (auto& c : contents) {
    c->ToleranceSearch(prism, key, results, tolerance);
  }
}
CorrectorComponent::CorrectorComponent()
    : resolver_(Service::instance().CreateDeployedResourceResolver(
          {"corrector", "", ".correction.bin"})) {}

// 蓬萊：按鍵模型 ------------------------------------------------------------

namespace {

// 美式鍵盤（ANSI）上的鍵中心，鍵寬為 1。數字列 1 在 x=1；QWERTY 列往右錯 0.5、ASDF 列 0.75、ZXCV 列 1.25
struct KeyPos {
  char ch;
  double x, y;
};
const vector<KeyPos>& KeyPositions() {
  static const vector<KeyPos> positions = [] {
    vector<KeyPos> v;
    const char* rows[] = {"1234567890-=", "qwertyuiop[]\\", "asdfghjkl;'",
                          "zxcvbnm,./"};
    const double offsets[] = {1.0, 1.5, 1.75, 2.25};
    for (int r = 0; r < 4; ++r) {
      const char* row = rows[r];
      for (int c = 0; row[c]; ++c)
        v.push_back({row[c], offsets[r] + c, static_cast<double>(r)});
    }
    return v;
  }();
  return positions;
}

// 大千鍵位：注音 → 鍵（tools/zhuyin.py 的 ZY、KEY）
const char* const kZhuyin[] = {
    "ㄅ", "ㄆ", "ㄇ", "ㄈ", "ㄉ", "ㄊ", "ㄋ", "ㄌ", "ㄍ", "ㄎ", "ㄏ", "ㄐ", "ㄑ",
    "ㄒ", "ㄓ", "ㄔ", "ㄕ", "ㄖ", "ㄗ", "ㄘ", "ㄙ", "ㄧ", "ㄨ", "ㄩ", "ㄚ", "ㄛ",
    "ㄜ", "ㄝ", "ㄞ", "ㄟ", "ㄠ", "ㄡ", "ㄢ", "ㄣ", "ㄤ", "ㄥ", "ㄦ"};
const char kZhuyinKeys[] = "1qaz2wsxedcrfv5tgbyhnujm8ik,9ol.0p;/-";
const char kInitialKeys[] = "1qaz2wsxedcrfv5tgbyhn";
const char kMedialKeys[] = "ujm";

bool IsIn(const char* set, char c) {
  return c && std::strchr(set, c) != nullptr;
}

// 一個注音字串（UTF-8）拆成鍵；認不得的回空字串
string ZhuyinToKeys(const string& zhuyin) {
  string keys;
  size_t i = 0;
  while (i < zhuyin.size()) {
    bool found = false;
    for (size_t z = 0; z < sizeof(kZhuyin) / sizeof(kZhuyin[0]); ++z) {
      size_t len = std::strlen(kZhuyin[z]);
      if (zhuyin.compare(i, len, kZhuyin[z]) == 0) {
        keys += kZhuyinKeys[z];
        i += len;
        found = true;
        break;
      }
    }
    if (!found)
      return string();
  }
  return keys;
}

}  // namespace

namespace {
thread_local const vector<string>* g_touches = nullptr;
thread_local size_t g_touch_offset = 0;
thread_local bool g_touch_full_style = false;
}  // namespace

TouchScope::TouchScope(const vector<string>* touches, size_t offset, bool full_style)
    : saved_touches_(g_touches),
      saved_offset_(g_touch_offset),
      saved_full_style_(g_touch_full_style) {
  g_touches = touches;
  g_touch_offset = offset;
  g_touch_full_style = full_style;
}

TouchScope::~TouchScope() {
  g_touches = saved_touches_;
  g_touch_offset = saved_offset_;
  g_touch_full_style = saved_full_style_;
}

bool TouchScope::FullStyle() {
  return g_touch_full_style;
}

const string* TouchScope::At(size_t pos) {
  if (!g_touches)
    return nullptr;
  size_t i = g_touch_offset + pos;
  if (i >= g_touches->size() || (*g_touches)[i].empty())
    return nullptr;
  return &(*g_touches)[i];
}

// 觸控字串：空白隔開的「鍵:距離平方」，距離以鍵寬為 1（例：q:0.04 a:0.81 w:0.9）。要有打出來的那個鍵
bool PenglaiKeyCorrector::TouchSubstitutes(size_t pos,
                                           char typed,
                                           vector<TouchAlt>* out) const {
  const string* touch = TouchScope::At(pos);
  if (!touch || options_.touch_prior <= 0)
    return false;
  vector<pair<char, double>> keys;
  const char* p = touch->c_str();
  while (*p) {
    while (*p == ' ')
      ++p;
    if (!*p)
      break;
    char k = *p++;
    if (*p != ':')
      return false;
    ++p;
    char* end = nullptr;
    double d2 = std::strtod(p, &end);
    if (end == p)
      return false;
    p = end;
    keys.emplace_back(k, d2);
  }
  double typed_d2 = -1;
  for (const auto& k : keys)
    if (k.first == typed)
      typed_d2 = k.second;
  if (typed_d2 < 0)
    return false;
  const double sigma = options_.touch_sigma > 0 ? options_.touch_sigma : 0.35;
  const double lp = std::log(options_.touch_prior);
  out->clear();
  for (const auto& k : keys) {
    if (k.first == typed)
      continue;
    double cost = lp - (k.second - typed_d2) / (2 * sigma * sigma);
    // 容錯一定要扣分（step 用 cost < 0 算錯了幾次）；太不可能的不走
    cost = (std::min)(cost, -1e-4);
    if (cost < -12.0)
      continue;
    const double diff = k.second - typed_d2;
    // 只打完整注音的人，聲調鍵按偏落在旁邊的聲母（ㄉ、ㄓ）上多半就是按偏，照 touch_near 算打完音節；
    // 其他人那一鍵多半就是下一個字的首注音，要按得更靠近聲調鍵（touch_close）
    const double close = TouchScope::FullStyle()
                             ? options_.touch_near
                             : (std::min)(options_.touch_near, options_.touch_close);
    out->push_back({k.first, cost, diff <= options_.touch_near, diff <= close});
  }
  return true;
}

bool PenglaiKeyCorrector::IsInitialKey(char c) {
  return IsIn(kInitialKeys, c);
}

bool PenglaiKeyCorrector::IsToneKey(char c) {
  return IsIn(" 6347", c);
}

PenglaiKeyCorrector::PenglaiKeyCorrector(const Options& options)
    : options_(options) {
  auto safe_log = [](double p) {
    return p > 0 ? std::log(p) : -std::numeric_limits<double>::infinity();
  };
  log_extra_ = safe_log(options_.extra);
  log_double_ = safe_log(options_.double_press);
  log_omit_initial_ = safe_log(options_.omit_initial);
  log_omit_medial_ = safe_log(options_.omit_medial);
  const auto& positions = KeyPositions();
  const double sigma = options_.sigma > 0 ? options_.sigma : 0.6;
  const double cutoff = 1.5;
  if (options_.substitution > 0) {
    // 本來要按 intended，按到 typed 的機率 = substitution × w(intended, typed) / Σ w(intended, 附近每個鍵)
    for (const auto& a : positions) {
      double total = 0;
      vector<pair<char, double>> near;
      for (const auto& b : positions) {
        if (a.ch == b.ch)
          continue;
        double dx = a.x - b.x, dy = a.y - b.y;
        double d = std::sqrt(dx * dx + dy * dy);
        if (d > cutoff)
          continue;
        double w = std::exp(-d * d / (2 * sigma * sigma));
        total += w;
        near.emplace_back(b.ch, w);
      }
      if (total <= 0)
        continue;
      for (const auto& n : near) {
        double p = options_.substitution * n.second / total;
        // 存在 typed 鍵底下：打出 n.first、本來要按 a.ch
        substitutes_[n.first].emplace_back(a.ch, std::log(p));
      }
    }
  }
  if (options_.fuzzy > 0) {
    double lp = std::log(options_.fuzzy);
    for (const auto& pair_text : options_.fuzzy_pairs) {
      string keys = ZhuyinToKeys(pair_text);
      if (keys.size() != 2)
        continue;
      fuzzy_[keys[0]].emplace_back(keys[1], lp);
      fuzzy_[keys[1]].emplace_back(keys[0], lp);
    }
  }
}

// 沒打聲調的音節收不收：require_tone 關著都收；開著時只收只換了模糊音、拼法裡有韻母或介音的
bool PenglaiKeyCorrector::Untoned(unsigned ops, bool has_final) const {
  if (!options_.require_tone)
    return true;
  return options_.fuzzy_untoned && ops == kOpFuzzy && has_final;
}

void PenglaiKeyCorrector::ToleranceSearch(const Prism& prism,
                                          const string& key,
                                          Corrections* results,
                                          size_t tolerance) {
  ScoredSearch(prism, key, 0, results);
}

// 在 prism 的拼法 trie 上做加權的編輯搜尋：每一步可以照打的鍵走（0 分）、換成附近的鍵或模糊音的另一半、
// 跳過打的鍵（多按）、或不吃輸入補一個鍵（漏打聲母、介音）。走到有值的節點就是一個拼法，記下吃了多少輸入、
// 總分（log 機率）。完全照打的（0 次錯）由 CommonPrefixSearch 處理，這裡不記
void PenglaiKeyCorrector::ScoredSearch(const Prism& prism,
                                       const string& input,
                                       size_t begin,
                                       Corrections* results) {
  if (begin >= input.size())
    return;
  const auto& trie = prism.trie();
  const int max_edits = options_.max_edits;
  // 觸控靠近另一鍵的換鍵（kOpTouch）另外算，一個音節總共最多 touch_max_edits 次；其他錯照舊最多 max_edits 次
  const int total_max = (std::max)(max_edits, options_.touch_max_edits);
  const double kFloor = -20.0;
  // 沒打聲調的模糊音，整串裡有單獨一個介音或韻母的零聲母字（開頭、聲調後或韻母後的一個鍵，後面接聲母或
  // 結尾：ㄨ ㄗ ㄚ 是「我在啊」，ㄗㄚ 不是一個音節）就不修。打完整注音時「一」「啊」這種一個鍵的字也會被當成
  // 首注音、不修，只是少修，不會修錯。單獨的聲母（ㄈㄡ ㄓ 的 ㄓ）原本也擋（0054），11/6 晚上拿掉：
  // 修過的詞不再打亂照打的排序之後（0057、0060），只打首注音全量、混合題第一名開關相同
  auto lone_final_free = [](const string& in) {
    for (size_t i = 0; i < in.size(); ++i) {
      char c = in[i];
      char next = i + 1 < in.size() ? in[i + 1] : 0;
      bool next_starts = !next || IsIn(kInitialKeys, next);
      if (IsIn(kInitialKeys, c))
        continue;
      if (IsIn(" 6347", c) || !next_starts)
        continue;
      char prev = i > 0 ? in[i - 1] : 0;
      bool prev_ends = !prev || IsIn(" 6347", prev) ||
                       (!IsIn(kInitialKeys, prev) && !IsIn(kMedialKeys, prev));
      if (prev_ends)
        return false;
    }
    return true;
  };
  const bool untoned_ok = lone_final_free(input);
  struct State {
    size_t node;
    size_t pos;  // 下一個要讀的輸入位置（整串輸入的索引）
    double score;
    int edits;
    int depth;  // 拼法裡已經走了幾個鍵
    int value;  // 最後一步走到的節點的值（拼法編號），沒有是 -1
    bool omission;
    unsigned ops;
    bool closed;  // 最後一步是照打的聲調鍵
    bool has_final;  // 拼法裡有聲母以外的注音（介音、韻母）
    bool initial;    // 拼法的第一個鍵是聲母
    int touch_edits = 0;  // 觸控靠近另一鍵的換鍵次數（不算在 edits 裡）
    char keys[8] = {};    // 拼法走過的鍵（前 8 個）：只照觸控換鍵的路，拿來把輸入換成修過的樣子
  };
  vector<State> stack;
  stack.push_back({0, begin, 0.0, 0, 0, -1, false, 0, false, false, false});
  map<pair<size_t, size_t>, double> best;  // (節點, 位置) → 最佳分數（同樣錯幾次以內）
  vector<TouchAlt> touch_alts;

  auto step = [&](const State& from, char c, size_t next_pos, double cost,
                  unsigned op, vector<State>* out) {
    bool omission = op == kOpOmission;
    size_t node = from.node;
    size_t kp = 0;
    char buf[2] = {c, 0};
    auto val = trie.traverse(buf, node, kp, 1);
    if (val == -2)
      return;
    State s{node, next_pos, from.score + cost,
            from.edits + (cost < 0 && !(op & kOpTouch) ? 1 : 0), from.depth + 1,
            static_cast<int>(val >= 0 ? val : -1), from.omission || omission,
            from.ops | op, (op == 0 || (op & kOpTouchClose)) && IsIn(" 6347", c),
            from.has_final || (!IsIn(kInitialKeys, c) && !IsIn(" 6347", c)),
            from.depth == 0 ? IsIn(kInitialKeys, c) : from.initial};
    s.touch_edits = from.touch_edits + ((op & kOpTouch) ? 1 : 0);
    std::memcpy(s.keys, from.keys, sizeof(s.keys));
    if (from.depth < 8)
      s.keys[from.depth] = c;
    if (s.edits > max_edits || s.edits + s.touch_edits > total_max ||
        s.score < kFloor)
      return;
    // 觸控的簡拼：一個鍵的聲母照觸控換成附近的聲母（打的那一鍵也是聲母）
    const bool touch_abbrev = s.depth == 1 && TouchAbbreviation(s.ops) &&
                              IsIn(kInitialKeys, c) &&
                              IsIn(kInitialKeys, input[from.pos]);
    // 觸控沒打聲調的完整音節（0071，11/8 手機 tk0：想打 ㄔㄨㄢ，ㄨ 按偏到 ㄜ）：只照觸控換鍵、有聲母也有
    // 介音或韻母時照收（require_tone 原本只收模糊音）。上面「單獨一個韻母就不修」照修過的輸入重看：
    // ㄔㄜ ㄢ 的 ㄢ 像單獨的「安」，是按偏造出來的，ㄜ 換回 ㄨ 之後就不是。
    // 要按得夠靠近換過去的鍵（touch_close，離兩鍵交界 0.2 鍵寬以內）：只打首注音的人聲母後面常接 ㄧ ㄨ ㄩ 開頭的字
    // （一、有、用），不限的話按在鍵中間也會被併成兩個鍵的音節（ㄅ ㄩ 的「備用」變 ㄅㄨ「補」）。真的按到隔壁鍵的，
    // 大多落在交界附近（手指偏 σ 0.22 鍵寬、瞄準隔壁鍵中心卻落在這一鍵的，九成以上在交界 0.2 鍵寬內）
    bool touch_untoned = false;
    if (val >= 0 && !s.closed && !touch_abbrev && s.has_final && s.initial &&
        TouchAbbreviation(s.ops) && (s.ops & kOpTouchClose) && s.depth <= 8 &&
        s.pos - begin == static_cast<size_t>(s.depth) &&
        !(untoned_ok && Untoned(s.ops, true))) {
      string fixed = input;
      for (int i = 0; i < s.depth; ++i)
        fixed[begin + i] = s.keys[i];
      touch_untoned = lone_final_free(fixed);
    }
    if (val >= 0 && s.edits + s.touch_edits > 0 &&
        (s.closed || touch_abbrev || touch_untoned ||
         (untoned_ok && Untoned(s.ops, s.has_final && s.initial)))) {
      Correction c{static_cast<size_t>(s.edits + s.touch_edits), static_cast<SyllableId>(val),
                   s.pos - begin, s.score, true};
      c.closed = s.closed;
      c.omission = s.omission;
      c.ops = s.ops;
      results->Alter(val, c);
    }
    out->push_back(s);
  };

  while (!stack.empty()) {
    State st = stack.back();
    stack.pop_back();
    auto key = std::make_pair(st.node, st.pos);
    auto it = best.find(key);
    if (it != best.end() && it->second >= st.score)
      continue;
    best[key] = st.score;
    vector<State> next;
    bool has_input = st.pos < input.size();
    char typed = has_input ? input[st.pos] : 0;
    if (has_input) {
      // 照打的鍵走
      step(st, typed, st.pos + 1, 0.0, 0, &next);
      if (st.edits + st.touch_edits < total_max) {
        // 按到旁邊的鍵：有觸控照按下去的位置，沒有照鍵距表
        if (TouchSubstitutes(st.pos, typed, &touch_alts)) {
          for (const auto& alt : touch_alts)
            step(st, alt.key, st.pos + 1, alt.cost,
                 kOpSubstitution | kOpTouchSub | (alt.near ? kOpTouch : 0) |
                     (alt.close ? kOpTouchClose : 0),
                 &next);
        } else {
          auto sub = substitutes_.find(typed);
          if (sub != substitutes_.end())
            for (const auto& alt : sub->second)
              step(st, alt.first, st.pos + 1, alt.second, kOpSubstitution,
                   &next);
        }
        // 模糊音
        auto fz = fuzzy_.find(typed);
        if (fz != fuzzy_.end())
          for (const auto& alt : fz->second)
            step(st, alt.first, st.pos + 1, alt.second, kOpFuzzy, &next);
        // 多按一鍵：同一個鍵連按兩下在哪裡都算；其他的只算在拼法中間（不能是一個音節的第一個鍵）
        char prev = st.pos > 0 ? input[st.pos - 1] : 0;
        bool is_double = prev && prev == typed && typed != ' ';
        double cost = is_double ? log_double_ : (st.depth > 0 ? log_extra_ : -INFINITY);
        if (std::isfinite(cost)) {
          State s = st;
          s.pos += 1;
          s.score += cost;
          s.edits += 1;
          s.ops |= is_double ? kOpDouble : kOpExtra;
          if (s.edits <= max_edits && s.edits + s.touch_edits <= total_max &&
              s.score >= kFloor) {
            // 多按的那一鍵在拼法最後（ㄐㄐ 當一個 ㄐ）：剛走到的節點就是拼法。
            // 多按的是聲調鍵（ˋˋ）時，前一步照打的聲調鍵還算數
            if (is_double && st.value >= 0 &&
                (st.closed || (untoned_ok && Untoned(s.ops, st.has_final && st.initial)))) {
              Correction c{static_cast<size_t>(s.edits + s.touch_edits),
                           static_cast<SyllableId>(st.value), s.pos - begin,
                           s.score, true};
              c.omission = s.omission;
              c.ops = s.ops;
              c.closed = st.closed;
              results->Alter(st.value, c);
            }
            next.push_back(s);
          }
        }
      }
    }
    if (st.edits < max_edits && st.edits + st.touch_edits < total_max &&
        has_input) {
      // 漏打聲母：音節的第一個鍵，打的是韻母或介音；前一個鍵是聲母時不算（那個聲母多半就是這個音節的，
      // 或是只打第一個注音的字，照打的就讀得通）
      char before = st.pos > 0 ? input[st.pos - 1] : 0;
      if (st.depth == 0 && std::isfinite(log_omit_initial_) &&
          !IsIn(kInitialKeys, typed) && !IsIn(" 6347", typed) &&
          !IsIn(kInitialKeys, before)) {
        for (const char* c = kInitialKeys; *c; ++c)
          step(st, *c, st.pos, log_omit_initial_, kOpOmission, &next);
      }
      // 漏打介音：聲母後面，打的是韻母
      if (st.depth == 1 && std::isfinite(log_omit_medial_) &&
          !IsIn(kMedialKeys, typed) && !IsIn(kInitialKeys, typed) &&
          !IsIn(" 6347", typed)) {
        for (const char* c = kMedialKeys; *c; ++c)
          step(st, *c, st.pos, log_omit_medial_, kOpOmission, &next);
      }
    }
    for (auto& s : next)
      stack.push_back(s);
  }
}

Corrector* CorrectorComponent::Create(const Ticket& ticket) noexcept {
  // 蓬萊：translator/key_model 打開時用按鍵模型
  if (ticket.schema) {
    Config* config = ticket.schema->config();
    bool enabled = false;
    // 蓬萊：鍵名寫全（tools/gen_settings_catalog.py 照「ticket.name_space + "/…"」掃設定鍵）
    if (config->GetBool(ticket.name_space + "/key_model/enable", &enabled) &&
        enabled) {
      PenglaiKeyCorrector::Options o;
      config->GetDouble(ticket.name_space + "/key_model/substitution", &o.substitution);
      config->GetDouble(ticket.name_space + "/key_model/sigma", &o.sigma);
      config->GetDouble(ticket.name_space + "/key_model/extra", &o.extra);
      config->GetDouble(ticket.name_space + "/key_model/double_press", &o.double_press);
      config->GetDouble(ticket.name_space + "/key_model/omit_initial", &o.omit_initial);
      config->GetDouble(ticket.name_space + "/key_model/omit_medial", &o.omit_medial);
      config->GetDouble(ticket.name_space + "/key_model/fuzzy", &o.fuzzy);
      config->GetInt(ticket.name_space + "/key_model/max_edits", &o.max_edits);
      config->GetString(ticket.name_space + "/key_model/abbreviations", &o.abbreviations);
      config->GetBool(ticket.name_space + "/key_model/require_tone", &o.require_tone);
      config->GetBool(ticket.name_space + "/key_model/fuzzy_untoned", &o.fuzzy_untoned);
      config->GetBool(ticket.name_space + "/key_model/show_reading", &o.show_reading);
      config->GetDouble(ticket.name_space + "/key_model/valid", &o.valid);
      config->GetBool(ticket.name_space + "/key_model/valid_suggest", &o.valid_suggest);
      config->GetBool(ticket.name_space + "/key_model/closed_type", &o.closed_type);
      config->GetBool(ticket.name_space + "/key_model/valid_first", &o.valid_first);
      config->GetDouble(ticket.name_space + "/key_model/touch_sigma", &o.touch_sigma);
      config->GetDouble(ticket.name_space + "/key_model/touch_prior", &o.touch_prior);
      config->GetDouble(ticket.name_space + "/key_model/touch_valid", &o.touch_valid);
      config->GetDouble(ticket.name_space + "/key_model/touch_near", &o.touch_near);
      config->GetDouble(ticket.name_space + "/key_model/touch_close", &o.touch_close);
      config->GetBool(ticket.name_space + "/key_model/touch_abbreviations", &o.touch_abbreviations);
      config->GetInt(ticket.name_space + "/key_model/touch_max_edits", &o.touch_max_edits);
      if (auto list = config->GetList(ticket.name_space + "/key_model/fuzzy_pairs")) {
        for (size_t i = 0; i < list->size(); ++i) {
          if (auto v = list->GetValueAt(i))
            o.fuzzy_pairs.push_back(v->str());
        }
      }
      return new PenglaiKeyCorrector(o);
    }
  }
  // Don't use edit distance based correction for now.
#if 0
  if (!ticket.schema) return nullptr;
  Config* config = ticket.schema->config();
  string prism_name;
  if (!config->GetString(ticket.name_space + "/prism", &prism_name)) {
    config->GetString(ticket.name_space + "/dictionary", &prism_name);
  }

  auto file_path = resolver_->ResolvePath(prism_name);

  auto ed_corrector = New<EditDistanceCorrector>(file_path);
  if (ed_corrector->Load()) {
    return Combine(New<NearSearchCorrector>(), ed_corrector);
  } else {
    return new NearSearchCorrector();
  }
#endif
  return new NearSearchCorrector();
}
