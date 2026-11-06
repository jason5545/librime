//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-07-12 Zou Xu <zouivex@gmail.com>
// 2012-02-11 GONG Chen <chen.sst@gmail.com>
//
#include <algorithm>
#include <cstring>
#include <queue>
#include <boost/range/adaptor/reversed.hpp>
#include <atomic>
#include <new>
#include <rime/algo/syllabifier.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <rime/dict/corrector.h>
#include <rime/dict/prism.h>
#include "syllabifier.h"

namespace rime {

// 蓬萊：EdgeArena（syllabifier.h）。每個執行緒一塊目前的區塊，依序往後切；每一塊記著還活著的配置數
//（目前那塊另外算一個），數到 0 就還給 malloc。目前那塊裡的都釋放了，就從頭再切。太大的（超過一塊）直接跟 malloc 要
namespace {
constexpr size_t kEdgeChunkSize = 32 * 1024;
constexpr size_t kEdgeAlign = 16;
struct EdgeChunk {
  std::atomic<size_t> live;
  size_t used;
};
struct EdgeBlock {
  EdgeChunk* chunk;  // nullptr：直接跟 malloc 要的
  size_t reserved;   // 補到 16 位元組，資料照 16 對齊
};
constexpr size_t kEdgeChunkHeader =
    (sizeof(EdgeChunk) + kEdgeAlign - 1) / kEdgeAlign * kEdgeAlign;
static_assert(sizeof(EdgeBlock) % kEdgeAlign == 0, "EdgeBlock alignment");

void ReleaseEdgeChunk(EdgeChunk* c) noexcept {
  if (c->live.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    c->~EdgeChunk();
    ::operator delete(c);
  }
}

struct EdgeChunkHolder {
  EdgeChunk* current = nullptr;
  ~EdgeChunkHolder() {
    if (current)
      ReleaseEdgeChunk(current);
  }
};
thread_local EdgeChunkHolder t_edge_chunk;
}  // namespace

void* EdgeArena::Allocate(size_t bytes) {
  const size_t need =
      (sizeof(EdgeBlock) + bytes + kEdgeAlign - 1) / kEdgeAlign * kEdgeAlign;
  if (need > kEdgeChunkSize - kEdgeChunkHeader) {
    auto* b = static_cast<EdgeBlock*>(::operator new(need));
    b->chunk = nullptr;
    return b + 1;
  }
  EdgeChunk*& c = t_edge_chunk.current;
  if (c && c->live.load(std::memory_order_acquire) == 1)
    c->used = kEdgeChunkHeader;  // 這塊切出去的都還回來了：從頭再切
  if (!c || c->used + need > kEdgeChunkSize) {
    if (c)
      ReleaseEdgeChunk(c);
    c = new (::operator new(kEdgeChunkSize)) EdgeChunk;
    c->live.store(1, std::memory_order_relaxed);
    c->used = kEdgeChunkHeader;
  }
  auto* b = reinterpret_cast<EdgeBlock*>(reinterpret_cast<char*>(c) + c->used);
  c->used += need;
  c->live.fetch_add(1, std::memory_order_relaxed);
  b->chunk = c;
  return b + 1;
}

void EdgeArena::Deallocate(void* p) noexcept {
  if (!p)
    return;
  auto* b = static_cast<EdgeBlock*>(p) - 1;
  if (!b->chunk) {
    ::operator delete(b);
    return;
  }
  ReleaseEdgeChunk(b->chunk);
}
using namespace corrector;

using Vertex = pair<size_t, SpellingType>;
using VertexQueue =
    std::priority_queue<Vertex, vector<Vertex>, std::greater<Vertex>>;

// 權重階梯：
// 1. 全拼：用戶完整輸入了所有編碼。Penalty = 0
// 2. 簡拼：用戶輸入了縮寫，明確希望匹配某個字。Penalty ≈ -2.3
// 3. 補全 (Completion)：用戶還沒打完，算法瞎猜的。Penalty ≈ -3.0
const double kCompletionPenalty = -2.995732273553991;      // log(0.05)
const double kCorrectionCredibility = -4.605170185988091;  // log(0.01)
// 蓬萊：跟 algo/calculus.cc 的 abbrev 同一個值（log(0.5)）
const double kGlueAbbreviationPenalty = -0.6931471805599453;

// 蓬萊：罕用黏合音節。方案檔的 abbrev 規則在部署時對這個拼法底下的每個音節做 SpellingProperties::Compose：
// 類型取比較模糊的、分數加 log(0.5)，存進 prism 時分數是 float。這裡照同樣的順序算、一樣存成 float 再讀回來，
// 結果跟部署出來的 prism 逐位元組相同
bool Syllabifier::Glue(SyllableId spelling_id, SpellingProperties* props) const {
  if (!glue_spellings_ || glue_spellings_->find(spelling_id) == glue_spellings_->end())
    return false;
  if (props->type < kAbbreviation)
    props->type = kAbbreviation;
  props->credibility = static_cast<float>(props->credibility +
                                          kGlueAbbreviationPenalty);
  return true;
}

int Syllabifier::BuildSyllableGraph(const string& input,
                                    Prism& prism,
                                    SyllableGraph* graph) {
  if (input.empty())
    return 0;

  size_t farthest = 0;
  VertexQueue queue;
  queue.push(Vertex{0, kNormalSpelling});  // start
  // 蓬萊：一個拼法的音節先收集在這裡，再放進 SpellingMap（flat_map；見下面）
  vector<pair<SyllableId, EdgeProperties>> collected;
  vector<pair<SyllableId, size_t>> order;

  while (!queue.empty()) {
    Vertex vertex(queue.top());
    queue.pop();
    size_t current_pos = vertex.first;

    // record a visit to the vertex
    if (graph->vertices.find(current_pos) == graph->vertices.end()) {
      graph->vertices.insert(vertex);  // preferred spelling type comes first
    } else {
      //      graph->vertices[current_pos] =
      //          std::min(vertex.second, graph->vertices[current_pos]);
      continue;  // discard worse spelling types
    }

    if (current_pos > farthest)
      farthest = current_pos;

    // consume leading delimiters
    size_t begin_pos = current_pos;
    while (begin_pos < input.length() &&
           delimiters_.find(input[begin_pos]) != string::npos)
      ++begin_pos;
    DLOG(INFO) << "current_pos: " << current_pos
               << ", begin_pos: " << begin_pos;

    // see where we can go by advancing a syllable
    vector<Prism::Match> matches;
    // 蓬萊：完全照打的拼法記（拼法, 長度）：按鍵模型的多按一下會讓同一個拼法吃不同長度
    set<pair<SyllableId, size_t>> exact_match_syllables;
    // 蓬萊：按鍵模型的容錯分數，key 是 Corrections::Key（拼法, 長度）
    hash_map<uint64_t, Correction> scored_corrections;
    auto current_input = input.substr(begin_pos);
    prism.CommonPrefixSearch(current_input, &matches);
    const bool scored = corrector_ && corrector_->Scored();
    // 蓬萊：照打就有完整音節的長度（按鍵模型：這種長度的容錯另外扣 ValidPenalty）
    set<size_t> exact_normal_lengths;
    if (corrector_) {
      for (auto& m : matches) {
        exact_match_syllables.insert({m.value, m.length});
        if (scored) {
          // 沒打聲調的完整音節（ㄈㄡ）在方案裡是簡拼，也算照打合法：沒打聲調的模糊音容錯照樣只當建議。
          // 認法：這段有聲母以外的注音鍵（大千鍵位），只打聲母、聲母加聲調的不算
          bool untoned_full = false;
          for (size_t k = 0; k < m.length; ++k) {
            char ch = current_input[k];
            if (!std::strchr("1qaz2wsxedcrfv5tgbyhn 6347", ch)) {
              untoned_full = true;
              break;
            }
          }
          for (auto a = prism.QuerySpelling(m.value); !a.exhausted(); a.Next()) {
            auto type = a.properties().type;
            if (type == kNormalSpelling ||
                (untoned_full && type == kAbbreviation)) {
              exact_normal_lengths.insert(m.length);
              break;
            }
          }
        }
      }
      Corrections corrections;
      if (scored)
        corrector_->ScoredSearch(prism, input, begin_pos, &corrections);
      else
        corrector_->ToleranceSearch(prism, current_input, &corrections, 5);
      for (const auto& m : corrections) {
        const auto& c = m.second;
        if (exact_match_syllables.count({c.syllable, c.length}))
          continue;
        // 蓬萊：沒打聲調的模糊音只修照打本身也是完整音節的（ㄈㄡ → ㄈㄛ）。照打不成音節的多半是只打前兩個注音
        // （ㄕ ㄛ 是「是哦」，不是 ㄕㄡ）
        if (scored && !c.closed && corrector_->RequiresTone() &&
            !exact_normal_lengths.count(c.length))
          continue;
        // 蓬萊：照打的那段就是完整音節、設定成照打合法時不修
        if (scored && exact_normal_lengths.count(c.length) &&
            !std::isfinite(corrector_->ValidPenalty()))
          continue;
        for (auto accessor = prism.QuerySpelling(c.syllable);
             !accessor.exhausted(); accessor.Next()) {
          auto props = accessor.properties();
          Glue(c.syllable, &props);
          if (props.is_correction)
            continue;
          bool ok = props.type == kNormalSpelling;
          if (!ok && scored && corrector_->AbbreviationAllows(c.ops, c.closed) &&
              props.type <= kAbbreviation)
            ok = true;
          if (ok) {
            matches.push_back({c.syllable, c.length});
            if (scored)
              scored_corrections[m.first] = c;
            static const bool km_debug = std::getenv("PENGLAI_KM_DEBUG") != nullptr;
            if (scored && km_debug)
              fprintf(stderr, "km %zu+%zu spell=%d type=%d ops=%u score=%.2f cred=%.2f\n",
                      begin_pos, c.length, c.syllable, (int)props.type, c.ops,
                      c.score, props.credibility);
            break;
          }
        }
      }
    }

    size_t leading_gap = begin_pos - current_pos;
    if (!matches.empty()) {
      auto& end_vertices(graph->edges[current_pos]);
      for (const auto& m : matches) {
        if (m.length == 0)
          continue;
        size_t end_pos = current_pos + leading_gap + m.length;
        // consume trailing delimiters
        while (end_pos < input.length() &&
               delimiters_.find(input[end_pos]) != string::npos)
          ++end_pos;
        DLOG(INFO) << "end_pos: " << end_pos;
        bool matches_input = (current_pos == 0 && end_pos == input.length());
        SpellingMap& spellings(end_vertices[end_pos]);
        SpellingType end_vertex_type = kInvalidSpelling;
        // when spelling algebra is enabled,
        // a spelling evaluates to a set of syllables;
        // otherwise, it resembles exactly the syllable itself.
        SpellingAccessor accessor(prism.QuerySpelling(m.value));
        collected.clear();
        bool ascending = true;  // 收集到的音節編號由小到大、沒有重複
        while (!accessor.exhausted()) {
          SyllableId syllable_id = accessor.syllable_id();
          EdgeProperties props(accessor.properties());
          Glue(m.value, &props);
          if (strict_spelling_ && matches_input &&
              props.type != kNormalSpelling) {
            // disqualify fuzzy spelling or abbreviation as single word
          } else {
            props.end_pos = end_pos;
            // add a syllable with properties to the edge's
            // spelling-to-syllable map
            if (corrector_ && !exact_match_syllables.count({m.value, m.length})) {
              if (scored) {
                // 蓬萊：按鍵模型的分數加在拼法原本的分數上（簡拼的扣分照扣）；補了漏打的鍵只給完整音節；
                // 簡拼不容錯時只給完整音節
                auto sc = scored_corrections.find(Corrections::Key(m.value, m.length));
                if (sc == scored_corrections.end() || props.is_correction ||
                    (props.type != kNormalSpelling &&
                     !corrector_->AbbreviationAllows(sc->second.ops, sc->second.closed))) {
                  accessor.Next();
                  continue;
                }
                props.is_correction = true;
                props.credibility += sc->second.score;
                if (exact_normal_lengths.count(m.length)) {
                  props.credibility += corrector_->ValidPenalty();
                  if (corrector_->ValidSuggest())
                    graph->weak.insert(std::make_tuple(current_pos, end_pos, syllable_id));
                  if (corrector_->ValidSuggest() && !sc->second.closed)
                    graph->weak_untoned.insert(std::make_tuple(current_pos, end_pos, syllable_id));
                }
              } else {
                props.is_correction = true;
                props.credibility = kCorrectionCredibility;
              }
            }
            // let end_vertex_type be the best (smaller) type of spelling
            // that ends at the vertex
            // 蓬萊：按鍵模型修好的完整音節（以照打的聲調鍵結尾）當成照打的完整音節算類型
            bool closed_correction = false;
            if (scored && props.is_correction && corrector_->ClosedType()) {
              auto sc = scored_corrections.find(Corrections::Key(m.value, m.length));
              closed_correction = sc != scored_corrections.end() &&
                                  sc->second.closed &&
                                  props.type == kNormalSpelling;
            }
            if (end_vertex_type > props.type &&
                (!props.is_correction || closed_correction)) {
              end_vertex_type = props.type;
            }
            if (!collected.empty() && collected.back().first >= syllable_id)
              ascending = false;
            collected.emplace_back(syllable_id, std::move(props));
          }
          accessor.Next();
        }
        // 蓬萊：原本逐一 find／insert（已經有的只把 type 換成比較小的）。拼法的音節清單是部署時拼寫運算
        // 產生的順序、不一定照編號，flat_map 逐一插在中間會一直搬。這條邊還是空的時候（幾乎都是），
        // 照（編號, 先後）排好依序接在後面：同一個音節第一個放進去、後面的只更新 type，跟原本相同
        if (spellings.empty() && ascending) {
          spellings.reserve(collected.size());
          for (auto& x : collected)
            spellings.emplace_hint(spellings.end(), x.first,
                                   std::move(x.second));
        } else if (spellings.empty()) {
          order.clear();
          for (size_t c = 0; c < collected.size(); ++c)
            order.emplace_back(collected[c].first, c);
          std::sort(order.begin(), order.end());
          spellings.reserve(collected.size());
          for (const auto& o : order) {
            auto& x = collected[o.second];
            if (!spellings.empty() && spellings.rbegin()->first == x.first) {
              auto& last = spellings.rbegin()->second;
              last.type = (std::min)(last.type, x.second.type);
            } else {
              spellings.emplace_hint(spellings.end(), x.first,
                                     std::move(x.second));
            }
          }
        } else {
          for (auto& x : collected) {
            auto it = spellings.find(x.first);
            if (it == spellings.end()) {
              spellings.insert({x.first, std::move(x.second)});
            } else {
              it->second.type = (std::min)(it->second.type, x.second.type);
            }
          }
        }
        if (spellings.empty()) {
          DLOG(INFO) << "not spelled.";
          end_vertices.erase(end_pos);
          continue;
        }
        // find the best common type in a path up to the end vertex
        // eg. pinyin "shurfa" has vertex type kNormalSpelling at position 3,
        // kAbbreviation at position 4 and kAbbreviation at position 6
        if (end_vertex_type < vertex.second) {
          end_vertex_type = vertex.second;
        }
        queue.push(Vertex{end_pos, end_vertex_type});
        DLOG(INFO) << "added to syllable graph, edge: [" << current_pos << ", "
                   << end_pos << ")";
      }
    }
  }

  DLOG(INFO) << "remove stale vertices and edges";
  set<int> good;
  good.insert(farthest);
  // fuzzy spellings are immune to invalidation by normal spellings
  SpellingType last_type =
      (std::max)(graph->vertices[farthest], kFuzzySpelling);
  for (int i = farthest - 1; i >= 0; --i) {
    if (graph->vertices.find(i) == graph->vertices.end())
      continue;
    // remove stale edges
    for (auto j = graph->edges[i].begin(); j != graph->edges[i].end();) {
      if (good.find(j->first) == good.end()) {
        // not connected
        graph->edges[i].erase(j++);
        continue;
      }
      // remove disqualified syllables (eg. matching abbreviated spellings)
      // when there is a path of more favored type
      SpellingType edge_type = kInvalidSpelling;
      // 蓬萊：SpellingMap 是 flat_map，一個一個 erase 每次都要搬後面的（按下聲調鍵時簡拼整批刪掉）。
      // 判斷照原本的順序做，留下來的照順序搬到前面，最後一次刪掉
      size_t kept = 0;
      for (auto k = j->second.begin(); k != j->second.end(); ++k) {
        bool keep = true;
        if (k->second.is_correction) {
          // Don't care correction edges
        } else if (k->second.type > last_type) {
          keep = false;
        } else if (k->second.type < edge_type) {
          edge_type = k->second.type;
        }
        if (keep) {
          auto dest = j->second.begin() + kept;
          if (dest != k)
            *dest = boost::move(*k);
          ++kept;
        }
      }
      j->second.erase(j->second.begin() + kept, j->second.end());
      if (j->second.empty()) {
        graph->edges[i].erase(j++);
      } else {
        if (edge_type < kAbbreviation)
          CheckOverlappedSpellings(graph, i, j->first);
        ++j;
      }
    }
    // 蓬萊：按鍵模型的容錯邊不參與決定音節類型，只靠容錯走得到的點類型是 kInvalidSpelling，不因為類型被刪
    bool correction_only = corrector_ && corrector_->Scored() &&
                           graph->vertices[i] == kInvalidSpelling;
    if ((graph->vertices[i] > last_type && !correction_only) ||
        graph->edges[i].empty()) {
      DLOG(INFO) << "remove stale vertex at " << i;
      graph->vertices.erase(i);
      graph->edges.erase(i);
      continue;
    }
    // keep the valid vertex
    good.insert(i);
  }

  if (enable_completion_ && farthest < input.length()) {
    DLOG(INFO) << "completion enabled";
    const size_t kExpandSearchLimit = 512;
    vector<Prism::Match> keys;
    prism.ExpandSearch(input.substr(farthest), &keys, kExpandSearchLimit);
    if (!keys.empty()) {
      size_t current_pos = farthest;
      size_t end_pos = input.length();
      size_t code_length = end_pos - current_pos;
      auto& end_vertices(graph->edges[current_pos]);
      auto& spellings(end_vertices[end_pos]);
      // 蓬萊：最多 512 個鍵、音節順序不一定，flat_map 逐一插入會一直搬。先照原本的順序收集，
      // 照音節編號穩定排序、同一個音節留第一個（原本 insert 已經有的不換），再照順序放進去
      vector<pair<SyllableId, SpellingProperties>> collected;
      for (const auto& m : keys) {
        if (m.length < code_length)
          continue;
        // when spelling algebra is enabled,
        // a spelling evaluates to a set of syllables;
        // otherwise, it resembles exactly the syllable itself.
        SpellingAccessor accessor(prism.QuerySpelling(m.value));
        while (!accessor.exhausted()) {
          SyllableId syllable_id = accessor.syllable_id();
          SpellingProperties props = accessor.properties();
          Glue(m.value, &props);
          if (props.type < kAbbreviation) {
            props.type = kCompletion;
            props.credibility += kCompletionPenalty;
            props.end_pos = end_pos;
            // add a syllable with properties to the edge's
            // spelling-to-syllable map
            collected.emplace_back(syllable_id, props);
          }
          accessor.Next();
        }
      }
      std::stable_sort(collected.begin(), collected.end(),
                       [](const pair<SyllableId, SpellingProperties>& a,
                          const pair<SyllableId, SpellingProperties>& b) {
                         return a.first < b.first;
                       });
      for (size_t c = 0; c < collected.size(); ++c) {
        if (c > 0 && collected[c].first == collected[c - 1].first)
          continue;
        spellings.insert(spellings.end(),
                         {collected[c].first, collected[c].second});
      }
      if (spellings.empty()) {
        DLOG(INFO) << "no completion could be made.";
        end_vertices.erase(end_pos);
      } else {
        DLOG(INFO) << "added to syllable graph, completion: [" << current_pos
                   << ", " << end_pos << ")";
        farthest = end_pos;
      }
    }
  }

  graph->input_length = input.length();
  graph->interpreted_length = farthest;
  DLOG(INFO) << "input length: " << graph->input_length;
  DLOG(INFO) << "syllabified length: " << graph->interpreted_length;

  Transpose(graph);

  return farthest;
}

void Syllabifier::CheckOverlappedSpellings(SyllableGraph* graph,
                                           size_t start,
                                           size_t end) {
  if (!graph || graph->edges.find(start) == graph->edges.end())
    return;
  // if "Z" = "YX", mark the vertex between Y and X an ambiguous syllable joint
  auto& y_end_vertices(graph->edges[start]);
  // enumerate Ys
  for (const auto& y : y_end_vertices) {
    size_t joint = y.first;
    if (joint >= end)
      break;
    // test X
    if (graph->edges.find(joint) == graph->edges.end())
      continue;
    auto& x_end_vertices(graph->edges[joint]);
    for (auto& x : x_end_vertices) {
      if (x.first < end)
        continue;
      if (x.first == end) {
        // discourage syllables at an ambiguous joint
        // bad cases include pinyin syllabification "niju'ede"
        for (auto& spelling : x.second) {
          // 這條邊（X）相對於起點構成歧義
          spelling.second.ambiguous_source_positions.insert(start);
        }
        graph->vertices[joint] = kAmbiguousSpelling;
        DLOG(INFO) << "ambiguous syllable joint at position " << joint << ".";
      }
      break;
    }
  }
}

void Syllabifier::Transpose(SyllableGraph* graph) {
  // 蓬萊：SpellingIndex 是連續的陣列。原本照「終點由大到小、每個終點裡音節由小到大」逐一
  // index[syll_id].push_back；每個終點的音節本來就排好了，這裡合併這幾串（同一個音節先拿排在前面的終點），
  // 每個音節的邊順序跟原本相同
  using Run = pair<SpellingMap::const_iterator, SpellingMap::const_iterator>;
  vector<Run> runs;
  vector<pair<SyllableId, const EdgeProperties*>> existing;
  for (const auto& start : graph->edges) {
    auto& index(graph->indices[start.first]);
    // 圖裡已經有的放前面（原本是接在後面 push_back）；只有同一張圖建兩次才會有
    existing.clear();
    for (const auto& x : index)
      for (auto props : x.second)
        existing.emplace_back(x.first, props);
    runs.clear();
    size_t total = existing.size();
    for (const auto& end : boost::adaptors::reverse(start.second)) {
      runs.emplace_back(end.second.begin(), end.second.end());
      total += end.second.size();
    }
    index.Reset(total);
    size_t e = 0;
    while (true) {
      int best = -1;
      SyllableId best_id = 0;
      if (e < existing.size()) {
        best = static_cast<int>(runs.size());
        best_id = existing[e].first;
      }
      for (size_t i = 0; i < runs.size(); ++i) {
        if (runs[i].first == runs[i].second)
          continue;
        // 同一個音節：已經有的、排在前面的終點先（嚴格小於才換）
        if (best < 0 || runs[i].first->first < best_id) {
          best = static_cast<int>(i);
          best_id = runs[i].first->first;
        }
      }
      if (best < 0)
        break;
      if (best == static_cast<int>(runs.size())) {
        index.Append(existing[e].first, existing[e].second);
        ++e;
      } else {
        index.Append(best_id, &runs[best].first->second);
        ++runs[best].first;
      }
    }
  }
}

void Syllabifier::EnableCorrection(Corrector* corrector) {
  corrector_ = corrector;
}

}  // namespace rime
